#include "roc/vehicle/file_sink.hpp"
#include "private_paths.hpp"

#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/protocol.hpp"
#include "roc/vehicle/road_network.hpp"
#include "roc/vehicle/resource_failure.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace roc::vehicle {
namespace {

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

Error storage_error(std::string message) {
    return Error{ErrorDomain::storage, ErrorCode::io_failure, false,
                 std::move(message), "local_storage_failure"};
}

Error artifact_error(std::string message) {
    return Error{ErrorDomain::artifact, ErrorCode::integrity_failure, false,
                 std::move(message), "artifact_metadata_mismatch"};
}

std::string normalized_media_type(std::string_view value) {
    const auto separator = value.find(';');
    auto type = value.substr(0, separator);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
        type.remove_suffix(1);
    }
    std::size_t start = 0;
    while (start < type.size() && (type[start] == ' ' || type[start] == '\t')) {
        ++start;
    }
    std::string result{type.substr(start)};
    std::transform(result.begin(), result.end(), result.begin(), [](char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    });
    return result;
}

std::string final_filename(const ArtifactManifest& manifest) {
    const std::string extension = manifest.resource_type == ResourceType::road_network
                                      ? ".json"
                                      : (manifest.content_type == "image/png" ? ".png" : ".jpg");
    const std::string prefix = manifest.resource_type == ResourceType::road_network
                                   ? "road-network-v"
                                   : "map-v";
    return prefix + std::to_string(manifest.version) + "-" +
           manifest.sha256.substr(0, 12) + extension;
}

bool part_name(std::string_view name) {
    return name.size()==5+36+1+36+5 && name.substr(0,5)==".roc-" &&
        name[41]=='-' && name.substr(name.size()-5)==".part" &&
        is_valid_uuid(name.substr(5,36)) && is_valid_uuid(name.substr(42,36));
}

bool valid_sha256(std::string_view value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

Result<void> validate_single_manifest(const ArtifactManifest& manifest,const FileSinkOptions& options) {
    if (manifest.resource_type==ResourceType::road_network &&
        manifest.schema_version!=1 && manifest.schema_version!=2)
        return make_resource_failure(ResourceFailure::unsupported_schema_version);
    if (manifest.package_version!=0 || !is_valid_uuid(manifest.task_id) ||
        !is_valid_uuid(manifest.resource_revision_id) || manifest.version<1 || manifest.byte_size==0 ||
        !valid_sha256(manifest.sha256) ||
        (manifest.resource_type==ResourceType::map && manifest.content_type!="image/png" && manifest.content_type!="image/jpeg") ||
        (manifest.resource_type==ResourceType::road_network &&
            (manifest.content_type!=(manifest.schema_version==1?"application/vnd.roc.road-network+json":"application/vnd.roc.road-network.v2+json") ||
             manifest.byte_size>options.maximum_road_document_bytes)))
        return artifact_error("FileSink configuration is invalid");
    return {};
}

#ifdef _WIN32
int open_exclusive(const std::filesystem::path& path) {
    return _wopen(path.c_str(), _O_BINARY | _O_CREAT | _O_EXCL | _O_WRONLY,
                  _S_IREAD | _S_IWRITE);
}
int close_file(int descriptor) { return _close(descriptor); }
int sync_file(int descriptor) { return _commit(descriptor); }
int write_file(int descriptor, const void* data, unsigned int size) {
    return _write(descriptor, data, size);
}
bool atomic_replace(const std::filesystem::path& from,
                    const std::filesystem::path& to) {
    return MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
bool sync_directory(const std::filesystem::path& path) {
    // Atomic replacement already uses MOVEFILE_WRITE_THROUGH. Windows does not
    // expose a portable directory-fsync equivalent for ordinary applications.
    static_cast<void>(path);
    return true;
}
#else
int open_exclusive(const std::filesystem::path& path) {
    return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  S_IRUSR | S_IWUSR);
}
int close_file(int descriptor) { return ::close(descriptor); }
int sync_file(int descriptor) { return ::fsync(descriptor); }
ssize_t write_file(int descriptor, const void* data, std::size_t size) {
    return ::write(descriptor, data, size);
}
bool atomic_replace(const std::filesystem::path& from,
                    const std::filesystem::path& to) {
    return ::rename(from.c_str(), to.c_str()) == 0;
}
bool sync_directory(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (descriptor < 0) {
        return false;
    }
    const bool result = ::fsync(descriptor) == 0;
    static_cast<void>(::close(descriptor));
    return result;
}
#endif

Result<std::string> hash_file(const std::filesystem::path& path,
                              std::uint64_t expected_size) {
    if (!detail::private_regular_file(path,false,true))
        return storage_error("artifact is not a private singly-linked regular file");
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return storage_error("unable to open artifact for verification");
    }
    DigestContext digest{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        return artifact_error("unable to initialize artifact SHA-256 verification");
    }
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t total = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto amount = static_cast<std::uint64_t>(count);
            if (total > expected_size || amount > expected_size - total) {
                return artifact_error("existing artifact exceeds manifest byte_size");
            }
            if (EVP_DigestUpdate(digest.get(), buffer.data(),
                                 static_cast<std::size_t>(count)) != 1) {
                return artifact_error("unable to update artifact SHA-256 verification");
            }
            total += amount;
        }
    }
    if (!input.eof() || total != expected_size) {
        return artifact_error("existing artifact length does not match manifest");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &size) != 1 || size != 32U) {
        return artifact_error("unable to finalize artifact SHA-256 verification");
    }
    static constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned int index = 0; index < size; ++index) {
        result.push_back(hexadecimal[(bytes[index] >> 4U) & 0x0fU]);
        result.push_back(hexadecimal[bytes[index] & 0x0fU]);
    }
    return result;
}

Result<std::string> read_road_document(const std::filesystem::path& path,
                                       std::uint64_t byte_size, std::size_t maximum) {
    if (byte_size > maximum) {
        return artifact_error("road-network document exceeds the local byte budget");
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return storage_error("unable to open road-network part file");
    }
    std::string document(static_cast<std::size_t>(byte_size), '\0');
    input.read(document.data(), static_cast<std::streamsize>(document.size()));
    if (input.gcount() != static_cast<std::streamsize>(document.size())) {
        return storage_error("unable to read complete road-network part file");
    }
    return document;
}

}  // namespace

struct FileSink::Impl {
    std::filesystem::path root;
    std::filesystem::path part;
    std::filesystem::path final;
    ArtifactManifest manifest;
    FileSinkOptions options;
    DigestContext digest{nullptr, &EVP_MD_CTX_free};
    int descriptor{-1};
    std::uint64_t written{0};
    bool sealed{false};
    bool installed{false};
    bool published{false};
    bool digest_finalized{false};
};

FileSink::FileSink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FileSink::~FileSink() { abort(); }

Result<std::unique_ptr<FileSink>> FileSink::create(
    std::filesystem::path artifact_root,
    ArtifactManifest manifest,
    FileSinkOptions options) {
    const auto valid=validate_single_manifest(manifest,options);if (!valid) return valid.error();
    std::error_code error;
    if (!detail::private_directory(artifact_root,true))
        return storage_error("artifact directory is not a private Vehicle directory");
    const auto random = generate_uuid_v4();
    if (!random) {
        return random.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->root = std::move(artifact_root);
    impl->manifest = std::move(manifest);
    impl->options = std::move(options);
    impl->part = impl->root /
        (".roc-" + impl->manifest.task_id + "-" + random.value() + ".part");
    impl->final = impl->root / final_filename(impl->manifest);
    impl->descriptor = open_exclusive(impl->part);
    if (impl->descriptor < 0) {
        return storage_error("unable to create exclusive artifact part file");
    }
    impl->digest.reset(EVP_MD_CTX_new());
    if (!impl->digest ||
        EVP_DigestInit_ex(impl->digest.get(), EVP_sha256(), nullptr) != 1) {
        close_file(impl->descriptor);
        impl->descriptor = -1;
        std::filesystem::remove(impl->part, error);
        return artifact_error("unable to initialize artifact SHA-256");
    }
    return std::unique_ptr<FileSink>{new FileSink{std::move(impl)}};
}

Result<void> FileSink::write(const std::uint8_t* data, std::size_t size) {
    if (impl_->descriptor < 0 || impl_->sealed || impl_->installed ||
        (size > 0 && data == nullptr)) {
        return artifact_error("FileSink is not writable");
    }
    if (impl_->written > impl_->manifest.byte_size ||
        size > impl_->manifest.byte_size - impl_->written) {
        return artifact_error("artifact exceeds manifest byte_size");
    }
    std::size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
        const auto remaining = std::min<std::size_t>(
            size - offset, static_cast<std::size_t>(std::numeric_limits<unsigned int>::max()));
        const int result = write_file(impl_->descriptor, data + offset,
                                      static_cast<unsigned int>(remaining));
#else
        const auto remaining = size - offset;
        const auto result = write_file(impl_->descriptor, data + offset, remaining);
#endif
        if (result < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            return storage_error("unable to write artifact part file");
        }
        if (result == 0) {
            return storage_error("artifact part write made no progress");
        }
        const auto amount = static_cast<std::size_t>(result);
        if (EVP_DigestUpdate(impl_->digest.get(), data + offset, amount) != 1) {
            return artifact_error("unable to update artifact SHA-256");
        }
        offset += amount;
        impl_->written += static_cast<std::uint64_t>(amount);
    }
    return {};
}

Result<void> FileSink::seal(const ArtifactHttpMetadata& metadata) {
    if (!detail::private_directory(impl_->root,false) ||
        !detail::private_regular_file(impl_->part,false,false))
        return storage_error("artifact staging path is not private");
    if (impl_->descriptor < 0 || impl_->sealed || impl_->installed ||
        impl_->digest_finalized) {
        return artifact_error("FileSink cannot be sealed in its current state");
    }
    if (normalized_media_type(metadata.content_type) != impl_->manifest.content_type ||
        (metadata.content_length.has_value() &&
         metadata.content_length.value() != impl_->manifest.byte_size) ||
        metadata.bytes_received != impl_->manifest.byte_size ||
        impl_->written != impl_->manifest.byte_size) {
        return artifact_error("artifact HTTP metadata or length does not match manifest");
    }
    if (metadata.sha256 != impl_->manifest.sha256)
        return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    impl_->digest_finalized = true;
    if (EVP_DigestFinal_ex(impl_->digest.get(), bytes.data(), &size) != 1 ||
        size != 32U) {
        return artifact_error("unable to finalize artifact SHA-256");
    }
    static constexpr char hexadecimal[] = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (unsigned int index = 0; index < size; ++index) {
        actual.push_back(hexadecimal[(bytes[index] >> 4U) & 0x0fU]);
        actual.push_back(hexadecimal[bytes[index] & 0x0fU]);
    }
    if (actual != impl_->manifest.sha256) {
        return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
    }
    const int sync_result = sync_file(impl_->descriptor);
    const int close_result = close_file(impl_->descriptor);
    impl_->descriptor = -1;
    if (sync_result != 0 || close_result != 0) {
        return storage_error("unable to fsync or close artifact part file");
    }
    impl_->descriptor = -1;
    if (impl_->manifest.resource_type == ResourceType::road_network) {
        const auto document = read_road_document(impl_->part, impl_->manifest.byte_size,
            impl_->options.maximum_road_document_bytes);
        if (!document) {
            return document.error();
        }
        if (impl_->manifest.schema_version==2) {
            const auto validation=parse_road_network_v2(document.value(),
                {impl_->options.expected_coordinate_mode,impl_->options.maximum_road_document_bytes});
            if (!validation) return validation.error();
        } else {
            const auto validation = validate_road_network_v1(
                document.value(), impl_->options.expected_coordinate_mode);
            if (!validation) return validation.error();
        }
    }
    impl_->sealed = true;
    return {};
}

Result<DeliveredArtifact> FileSink::commit() {
    if (!detail::private_directory(impl_->root,false))
        return storage_error("artifact directory is not private");
    if (!impl_->sealed || impl_->installed || impl_->descriptor >= 0) {
        return artifact_error("FileSink is not ready to commit");
    }
    std::error_code error;
    if (!impl_->published) {
        const auto part_status=std::filesystem::symlink_status(impl_->part,error);
        if (error || !std::filesystem::is_regular_file(part_status))
            return storage_error("sealed artifact part is not a regular file");
        const auto part_digest=hash_file(impl_->part,impl_->manifest.byte_size);
        if (!part_digest) return part_digest.error();
        if (part_digest.value()!=impl_->manifest.sha256)
            return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
    }
    const auto status = std::filesystem::symlink_status(impl_->final, error);
    if (!error && status.type() != std::filesystem::file_type::not_found) {
        if (!std::filesystem::is_regular_file(status)) {
            return storage_error("artifact destination is not a regular file");
        }
        const auto existing = hash_file(impl_->final, impl_->manifest.byte_size);
        if (existing && existing.value() == impl_->manifest.sha256) {
            const bool part_exists = std::filesystem::exists(impl_->part, error);
            if (error ||
                (part_exists &&
                 (!std::filesystem::remove(impl_->part, error) || error))) {
                return storage_error("unable to durably remove redundant part file");
            }
            impl_->published=true;
            if (!sync_directory(impl_->root)) return storage_error("unable to sync artifact parent directory");
            impl_->installed = true;
            return DeliveredArtifact{impl_->final, impl_->manifest.sha256,
                                     impl_->manifest.byte_size,
                                     impl_->manifest.content_type,
                                     impl_->manifest.task_id,
                                     impl_->manifest.resource_type,
                                     impl_->manifest.resource_revision_id,
                                     impl_->manifest.version,impl_->manifest.schema_version};
        }
        if (!existing && existing.error().domain == ErrorDomain::storage) {
            return existing.error();
        }
    } else if (error && error != std::errc::no_such_file_or_directory) {
        return storage_error("unable to inspect artifact destination");
    }
    if (!atomic_replace(impl_->part, impl_->final)) {
        return storage_error("unable to atomically install artifact");
    }
    impl_->published=true;
    if (!sync_directory(impl_->root)) {
        return storage_error("unable to sync artifact parent directory");
    }
    impl_->installed = true;
    return DeliveredArtifact{impl_->final, impl_->manifest.sha256,
                             impl_->manifest.byte_size,
                             impl_->manifest.content_type,
                             impl_->manifest.task_id,
                             impl_->manifest.resource_type,
                             impl_->manifest.resource_revision_id,
                             impl_->manifest.version,impl_->manifest.schema_version};
}

void FileSink::abort() noexcept {
    if (!impl_ || impl_->installed) {
        return;
    }
    if (impl_->descriptor >= 0) {
        static_cast<void>(close_file(impl_->descriptor));
        impl_->descriptor = -1;
    }
    std::error_code ignored;
    if (detail::private_directory(impl_->root,false) &&
        detail::private_regular_file(impl_->part,false,false))
        std::filesystem::remove(impl_->part, ignored);
}

const std::filesystem::path& FileSink::part_path() const noexcept {
    return impl_->part;
}

const std::filesystem::path& FileSink::final_path() const noexcept {
    return impl_->final;
}

std::uint64_t FileSink::bytes_written() const noexcept {
    return impl_->written;
}

Result<std::size_t> recover_file_sink_parts(
    const std::filesystem::path& artifact_root) {
    if (artifact_root.empty()) {
        return storage_error("artifact recovery root is empty");
    }
    std::error_code error;
    const auto root_status=std::filesystem::symlink_status(artifact_root,error);
    if (std::filesystem::is_symlink(root_status)) return storage_error("artifact recovery root is a link");
    if (error==std::errc::no_such_file_or_directory) return std::size_t{0};
    if (!std::filesystem::exists(root_status)) {
        if (error) {
            return storage_error("unable to inspect artifact recovery root");
        }
        return std::size_t{0};
    }
    if (!detail::private_directory(artifact_root,false))
        return storage_error("artifact recovery root is not a private Vehicle directory");
    std::size_t removed = 0;
    for (std::filesystem::directory_iterator iterator{artifact_root, error}, end;
         iterator != end && !error; iterator.increment(error)) {
        const auto name = iterator->path().filename().string();
        if (!part_name(name)) {
            continue;
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            break;
        }
        if (!detail::private_regular_file(iterator->path(),false,false))
            return storage_error("artifact recovery entry is not private and singly-linked");
        if (std::filesystem::is_regular_file(status)) {
            if (!std::filesystem::remove(iterator->path(), error) || error) {
                break;
            }
            ++removed;
        }
    }
    if (error) {
        return storage_error("unable to recover artifact part files");
    }
    if (removed > 0 && !sync_directory(artifact_root)) {
        return storage_error("unable to sync artifact directory after recovery");
    }
    return removed;
}

Result<DeliveredArtifact> inspect_file_artifact(const std::filesystem::path& root,
    const ArtifactManifest& manifest,FileSinkOptions options) {
    const auto valid=validate_single_manifest(manifest,options);if (!valid) return valid.error();
    if (!detail::private_directory(root,false)) return storage_error("artifact recovery directory is not private");
    const auto final=root/final_filename(manifest);
    const auto digest=hash_file(final,manifest.byte_size);if (!digest) return digest.error();
    if (digest.value()!=manifest.sha256) return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
    if (manifest.resource_type==ResourceType::road_network) {
        const auto text=read_road_document(final,manifest.byte_size,options.maximum_road_document_bytes);
        if (!text) return text.error();
        if (manifest.schema_version==2) {
            const auto parsed=parse_road_network_v2(text.value(),{options.expected_coordinate_mode,options.maximum_road_document_bytes});
            if (!parsed) return parsed.error();
        } else {
            const auto parsed=validate_road_network_v1(text.value(),options.expected_coordinate_mode);
            if (!parsed) return parsed.error();
        }
    }
#ifndef _WIN32
    const int fd=::open(final.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return storage_error("unable to open artifact for recovery sync");
    const int synchronized=::fsync(fd);const int closed=::close(fd);
    if (synchronized!=0 || closed!=0 || !sync_directory(root)) return storage_error("unable to resync recovered artifact");
#else
    return storage_error("durable file inspection is not implemented on Windows");
#endif
    return DeliveredArtifact{final,manifest.sha256,manifest.byte_size,manifest.content_type,manifest.task_id,
        manifest.resource_type,manifest.resource_revision_id,manifest.version,manifest.schema_version};
}

}  // namespace roc::vehicle
