#include "roc/vehicle/package_sink.hpp"
#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include "private_paths.hpp"
#include <jsoncons/json.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <set>
#include <vector>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace roc::vehicle {
namespace {
using Json = jsoncons::json;
Error metadata_failure() { return make_resource_failure(ResourceFailure::artifact_metadata_mismatch); }
Error storage_failure() { return make_resource_failure(ResourceFailure::local_storage_failure); }
Error canceled_failure() { return {ErrorDomain::canceled, ErrorCode::canceled, false, "package canceled"}; }
std::string local_name(const ArtifactFile& file) { return file.role + ".bin"; }
Result<std::string> descriptor(const ArtifactManifest& manifest) {
    if (manifest.package_version != 2)
        return make_resource_failure(ResourceFailure::unsupported_package_version);
    if (manifest.resource_type != ResourceType::map) return metadata_failure();
    if (manifest.files.empty()) return make_resource_failure(ResourceFailure::artifact_file_missing);
    if (manifest.byte_size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return metadata_failure();
    try {
        Json item(jsoncons::json_object_arg);
        item["task_id"]=manifest.task_id; item["batch_id"]=manifest.batch_id;
        item["project_id"]=manifest.project_id; item["resource_type"]="map";
        item["resource_revision_id"]=manifest.resource_revision_id; item["map_id"]=manifest.map_id;
        item["content_type"]=manifest.content_type; item["byte_size"]=manifest.byte_size;
        item["sha256"]=manifest.sha256; item["version"]=manifest.version; item["package_version"]=2;
        item["package_digest_algorithm"]=manifest.package_digest_algorithm;
        item["map_format"]=manifest.map_format; item["coordinate_mode"]=manifest.coordinate_mode;
        const auto optional = [](const auto& value) { return value ? Json(*value) : Json::null(); };
        item["resolution"]=optional(manifest.resolution); item["image_width"]=optional(manifest.image_width);
        item["image_height"]=optional(manifest.image_height);
        Json origin(jsoncons::json_object_arg);
        origin["x"]=optional(manifest.origin_x); origin["y"]=optional(manifest.origin_y);
        origin["theta"]=optional(manifest.origin_theta); item["origin"]=origin;
        item["files"]=Json(jsoncons::json_array_arg);
        auto files=manifest.files;
        std::sort(files.begin(), files.end(), [](const ArtifactFile& a, const ArtifactFile& b) {
            return a.role < b.role;
        });
        for (const auto& file : files) {
            Json entry(jsoncons::json_object_arg);
            entry["id"]=file.id; entry["role"]=file.role; entry["file_name"]=file.file_name;
            entry["content_type"]=file.content_type; entry["byte_size"]=file.byte_size;
            entry["sha256"]=file.sha256; entry["artifact_url"]=file.artifact_url;
            item["files"].push_back(entry);
        }
        Json body(jsoncons::json_object_arg); body["ok"]=true; body["manifest"]=item;
        const auto text=body.to_string();
        TaskAvailableMessage notice;
        notice.task_id=manifest.task_id; notice.batch_id=manifest.batch_id;
        notice.project_id=manifest.project_id; notice.resource_type=manifest.resource_type;
        notice.resource_revision_id=manifest.resource_revision_id;
        ArtifactLimits limits; limits.maximum_map_bytes=static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
        const auto parsed=parse_manifest_response(text,notice,limits);
        if (!parsed) return parsed.error();
        if (!manifest.artifact_url.empty() || manifest.schema_version != 0 || text.size()>65536)
            return metadata_failure();
        return text;
    } catch (const std::exception&) { return metadata_failure(); }
}
Result<std::string> published_name(const ArtifactManifest& manifest, std::string_view text) {
    const auto digest=sha256_hex(text);
    if (!digest) return digest.error();
    return "map-package-"+manifest.task_id+"-"+manifest.resource_revision_id+"-v"+
        std::to_string(manifest.version)+"-"+digest.value();
}

#ifdef __linux__
class Fd {
public:
    explicit Fd(int fd=-1) noexcept : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_=-1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this!=&other) { reset(); fd_=other.fd_; other.fd_=-1; } return *this;
    }
    Fd(const Fd&)=delete; Fd& operator=(const Fd&)=delete;
    ~Fd() { reset(); }
    void reset() noexcept { if (fd_>=0) { static_cast<void>(::close(fd_)); fd_=-1; } }
    bool close_checked() noexcept {
        if (fd_<0) return false;
        const int saved=fd_; fd_=-1; return ::close(saved)==0;
    }
    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_>=0; }
private: int fd_;
};
bool owned_mode(int fd, bool directory) {
    struct stat data {};
    return ::fstat(fd,&data)==0 && data.st_uid==::geteuid() &&
        (directory ? S_ISDIR(data.st_mode) : (S_ISREG(data.st_mode) && data.st_nlink==1)) &&
        (data.st_mode & 07777)==(directory ? 0700 : 0600);
}
Result<Fd> open_root(const std::filesystem::path& root, bool create) {
    if (root.empty()) return storage_failure();
    std::error_code ec;
    const auto absolute=std::filesystem::absolute(root,ec).lexically_normal();
    if (ec || absolute==absolute.root_path() || absolute==std::filesystem::temp_directory_path() ||
        absolute=="/var/tmp" || !detail::link_free_directory_prefix(absolute)) return storage_failure();
    if (create) {
        // Persist ancestor links too: syncing only the package root cannot make
        // its own entry durable. Include existing ancestors on retry because a
        // prior mkdir chain may have failed before its ancestor sync completed.
        std::vector<std::filesystem::path> creation_parents;
        auto parent=absolute.parent_path();
        for (;;) {
            creation_parents.push_back(parent);
            if (parent==parent.root_path()) break;
            parent=parent.parent_path();
        }
        std::filesystem::create_directories(absolute.parent_path(),ec);
        if (ec) return storage_failure();
        const int created=::mkdir(absolute.c_str(),0700);
        if (created!=0 && errno!=EEXIST) return storage_failure();
        // Also sync on retry after an earlier mkdir succeeded but its sync failed.
        for (const auto& path : creation_parents) {
            Fd fd(::open(path.c_str(),O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW));
            if (!fd || ::fsync(fd.get())!=0) return storage_failure();
        }
    }
    Fd fd(::open(absolute.c_str(),O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW));
    if (!fd || !owned_mode(fd.get(),true) || !detail::private_directory(absolute,false)) return storage_failure();
    return fd;
}
Result<void> write_all(int fd, const std::uint8_t* bytes, std::size_t size) {
    std::size_t offset=0;
    while (offset<size) {
        const auto count=::write(fd,bytes+offset,size-offset);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) return storage_failure();
        offset+=static_cast<std::size_t>(count);
    }
    return {};
}
Result<std::string> hash_fd(int fd, std::uint64_t size, const PackageChunkHandler* handler=nullptr,
    std::string_view file_id={}, std::size_t chunk_size=65536, const CancellationSignal* cancel=nullptr) {
    struct stat data {};
    if (!owned_mode(fd,false) || ::fstat(fd,&data)!=0 || data.st_size<0 ||
        static_cast<std::uint64_t>(data.st_size)!=size || ::lseek(fd,0,SEEK_SET)<0)
        return metadata_failure();
    using Digest=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>;
    Digest digest(EVP_MD_CTX_new(),&EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(),EVP_sha256(),nullptr)!=1) return storage_failure();
    std::vector<std::uint8_t> buffer(chunk_size);
    std::uint64_t total=0;
    for (;;) {
        if (cancel && cancel->is_canceled()) return canceled_failure();
        const auto count=::read(fd,buffer.data(),buffer.size());
        if (count<0 && errno==EINTR) continue;
        if (count<0) return storage_failure();
        if (count==0) break;
        const auto amount=static_cast<std::size_t>(count);
        if (total>size || amount>size-total) return metadata_failure();
        if (EVP_DigestUpdate(digest.get(),buffer.data(),amount)!=1) return storage_failure();
        if (handler) {
            const auto result=(*handler)(file_id,buffer.data(),amount);
            if (!result) return result.error();
        }
        total+=static_cast<std::uint64_t>(amount);
    }
    if (total!=size) return metadata_failure();
    std::array<unsigned char,EVP_MAX_MD_SIZE> bytes{}; unsigned int count=0;
    if (EVP_DigestFinal_ex(digest.get(),bytes.data(),&count)!=1 || count!=32) return storage_failure();
    std::string text; text.reserve(64);
    constexpr char hex[]="0123456789abcdef";
    for (unsigned int index=0;index<count;++index) {
        text.push_back(hex[bytes[index]>>4U]); text.push_back(hex[bytes[index]&0xfU]);
    }
    return text;
}
Result<std::set<std::string>> children(int fd) {
    // Independent open file description: duplicated descriptors share directory offsets.
    Fd copy(::openat(fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    if (!copy) return storage_failure();
    const int duplicate=::dup(copy.get());
    if (duplicate<0) return storage_failure();
    DIR* raw=::fdopendir(duplicate);
    if (!raw) { static_cast<void>(::close(duplicate)); return storage_failure(); }
    struct DirectoryCloser { void operator()(DIR* value) const noexcept { static_cast<void>(::closedir(value)); } };
    std::unique_ptr<DIR,DirectoryCloser> directory(raw);
    std::set<std::string> result;
    errno=0;
    while (const auto* entry=::readdir(directory.get())) {
        const std::string name=entry->d_name;
        if (name!="." && name!="..") result.insert(name);
        errno=0;
    }
    if (errno!=0) return storage_failure();
    return result;
}
bool staging_name(std::string_view name) {
    constexpr std::string_view prefix=".roc-package-";
    return name.size()==prefix.size()+36+1+36+5 && name.substr(0,prefix.size())==prefix &&
        name.substr(name.size()-5)==".part" && name[prefix.size()+36]=='-' &&
        is_valid_uuid(name.substr(prefix.size(),36)) && is_valid_uuid(name.substr(prefix.size()+37,36));
}
Result<void> remove_staging(int root_fd, const std::string& name) {
    if (!staging_name(name)) return storage_failure();
    Fd stage(::openat(root_fd,name.c_str(),O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW));
    if (!stage || !owned_mode(stage.get(),true)) return storage_failure();
    const auto names=children(stage.get()); if (!names) return names.error();
    const std::set<std::string> permitted{"image.bin","pgm.bin","yaml.bin","manifest.json"};
    for (const auto& entry : names.value()) {
        struct stat data {};
        if (!permitted.count(entry) || ::fstatat(stage.get(),entry.c_str(),&data,AT_SYMLINK_NOFOLLOW)!=0 ||
            !S_ISREG(data.st_mode) || data.st_uid!=::geteuid() || data.st_nlink!=1) return storage_failure();
    }
    for (const auto& entry : names.value())
        if (::unlinkat(stage.get(),entry.c_str(),0)!=0) return storage_failure();
    if (::unlinkat(root_fd,name.c_str(),AT_REMOVEDIR)!=0) return storage_failure();
    return {};
}
Result<void> verify_directory(int directory_fd, const ArtifactManifest& manifest, const std::string& text,
    const PackageChunkHandler* handler=nullptr, std::size_t chunk_size=65536,
    const CancellationSignal* cancel=nullptr) {
    if (!owned_mode(directory_fd,true)) return storage_failure();
    const auto names=children(directory_fd); if (!names) return names.error();
    std::set<std::string> expected{"manifest.json"};
    for (const auto& file : manifest.files) expected.insert(local_name(file));
    if (names.value()!=expected) return make_resource_failure(ResourceFailure::artifact_file_missing);
    Fd descriptor_fd(::openat(directory_fd,"manifest.json",O_RDONLY|O_NOFOLLOW|O_CLOEXEC));
    if (!descriptor_fd) return storage_failure();
    const auto actual=hash_fd(descriptor_fd.get(),text.size());
    const auto expected_hash=sha256_hex(text);
    if (!actual || !expected_hash || actual.value()!=expected_hash.value()) return metadata_failure();
    for (const auto& file : manifest.files) {
        Fd fd(::openat(directory_fd,local_name(file).c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC));
        if (!fd) return storage_failure();
        const auto hash=hash_fd(fd.get(),file.byte_size,handler,file.id,chunk_size,cancel);
        if (!hash) return hash.error();
        if (hash.value()!=file.sha256) return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
    }
    return {};
}
#endif
}

struct PackageSink::Impl {
    std::filesystem::path root, staging;
    ArtifactManifest manifest;
    std::string text, staging_name, final_name;
    bool ready{false}, published{false}, done{false}, aborted{false};
#ifdef __linux__
    struct File { ArtifactFile metadata; Fd fd; std::uint64_t written{0}; bool sealed{false}; };
    Fd root_fd, stage_fd;
    std::vector<File> files;
#endif
};
PackageSink::PackageSink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PackageSink::~PackageSink() { abort(); }
Result<std::unique_ptr<PackageSink>> PackageSink::create(
    std::filesystem::path root, ArtifactManifest manifest, ArtifactLimits limits) {
    const auto text=descriptor(manifest); if (!text) return text.error();
    if (manifest.byte_size>limits.maximum_map_bytes) return metadata_failure();
#ifdef __linux__
    auto directory=open_root(root,true); if (!directory) return directory.error();
    auto random=generate_uuid_v4(); if (!random) return random.error();
    auto name=published_name(manifest,text.value()); if (!name) return name.error();
    auto impl=std::make_unique<Impl>();
    std::error_code ec; impl->root=std::filesystem::absolute(root,ec).lexically_normal();
    if (ec) return storage_failure();
    impl->manifest=std::move(manifest); impl->text=text.value(); impl->final_name=name.value();
    impl->root_fd=std::move(directory.value());
    impl->staging_name=".roc-package-"+impl->manifest.task_id+"-"+random.value()+".part";
    impl->staging=impl->root/impl->staging_name;
    if (::mkdirat(impl->root_fd.get(),impl->staging_name.c_str(),0700)!=0) return storage_failure();
    impl->stage_fd=Fd(::openat(impl->root_fd.get(),impl->staging_name.c_str(),
        O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    auto sink=std::unique_ptr<PackageSink>(new PackageSink(std::move(impl)));
    if (!sink->impl_->stage_fd || !owned_mode(sink->impl_->stage_fd.get(),true)) return storage_failure();
    for (const auto& file : sink->impl_->manifest.files) {
        Fd fd(::openat(sink->impl_->stage_fd.get(),local_name(file).c_str(),
            O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
        if (!fd || !owned_mode(fd.get(),false)) return storage_failure();
        sink->impl_->files.push_back({file,std::move(fd),0,false});
    }
    return sink;
#else
    static_cast<void>(root); return Error{ErrorDomain::storage,ErrorCode::unsupported,false,
        "durable package publication requires Linux"};
#endif
}
Result<void> PackageSink::write(std::string_view id, const std::uint8_t* data, std::size_t size) {
#ifdef __linux__
    if (impl_->ready || impl_->published || impl_->aborted || (size>0 && !data)) return metadata_failure();
    for (auto& file : impl_->files) if (file.metadata.id==id) {
        if (file.sealed || !file.fd || file.written>file.metadata.byte_size ||
            size>file.metadata.byte_size-file.written) return metadata_failure();
        const auto written=write_all(file.fd.get(),data,size); if (!written) return written.error();
        file.written+=static_cast<std::uint64_t>(size); return {};
    }
#else
    static_cast<void>(id); static_cast<void>(data); static_cast<void>(size);
#endif
    return metadata_failure();
}
Result<void> PackageSink::seal_file(std::string_view id, const ArtifactHttpMetadata& metadata) {
#ifdef __linux__
    if (impl_->ready || impl_->published || impl_->aborted) return metadata_failure();
    for (auto& file : impl_->files) if (file.metadata.id==id) {
        auto media=[](std::string type) {
            type=type.substr(0,type.find(';'));
            while (!type.empty() && (type.back()==' ' || type.back()=='\t')) type.pop_back();
            const auto start=type.find_first_not_of(" \t");
            type=start==std::string::npos ? std::string{} : type.substr(start);
            std::transform(type.begin(),type.end(),type.begin(),[](unsigned char ch) {
                return static_cast<char>(ch>='A' && ch<='Z' ? ch+('a'-'A') : ch);
            }); return type;
        };
        if (file.sealed || !file.fd || file.written!=file.metadata.byte_size ||
            media(metadata.content_type)!=media(file.metadata.content_type) ||
            metadata.bytes_received!=file.metadata.byte_size ||
            (metadata.content_length && *metadata.content_length!=file.metadata.byte_size)) return metadata_failure();
        if (metadata.sha256!=file.metadata.sha256)
            return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
        const auto hash=hash_fd(file.fd.get(),file.metadata.byte_size); if (!hash) return hash.error();
        if (hash.value()!=file.metadata.sha256) return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
        if (::fsync(file.fd.get())!=0) return storage_failure();
        if (!file.fd.close_checked()) return storage_failure();
        file.sealed=true; return {};
    }
#else
    static_cast<void>(id); static_cast<void>(metadata);
#endif
    return metadata_failure();
}
Result<void> PackageSink::seal() {
#ifdef __linux__
    if (impl_->ready || impl_->published || impl_->aborted) return metadata_failure();
    for (const auto& file : impl_->files) if (!file.sealed)
        return make_resource_failure(ResourceFailure::artifact_file_missing);
    Fd fd(::openat(impl_->stage_fd.get(),"manifest.json",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    if (!fd || !owned_mode(fd.get(),false)) return storage_failure();
    const auto written=write_all(fd.get(),reinterpret_cast<const std::uint8_t*>(impl_->text.data()),impl_->text.size());
    if (!written) return written.error();
    if (::fsync(fd.get())!=0 || !fd.close_checked() || ::fsync(impl_->stage_fd.get())!=0) return storage_failure();
    impl_->ready=true; return {};
#else
    return storage_failure();
#endif
}
Result<DeliveredMapPackage> PackageSink::commit() {
#ifdef __linux__
    if (!impl_->ready || impl_->done || impl_->aborted) return metadata_failure();
    if (!impl_->published) {
        const auto valid=verify_directory(impl_->stage_fd.get(),impl_->manifest,impl_->text);
        if (!valid) return valid.error();
        if (::renameat2(impl_->root_fd.get(),impl_->staging_name.c_str(),
            impl_->root_fd.get(),impl_->final_name.c_str(),RENAME_NOREPLACE)!=0) {
            if (errno!=EEXIST) return storage_failure();
            const auto existing=inspect_map_package(impl_->root,impl_->manifest);
            if (!existing) return existing.error();
            const auto removed=remove_staging(impl_->root_fd.get(),impl_->staging_name);
            if (!removed) return removed.error();
        }
        impl_->published=true;
    }
    if (::fsync(impl_->root_fd.get())!=0) return storage_failure();
    const auto result=inspect_map_package(impl_->root,impl_->manifest);
    if (!result) return result.error();
    impl_->done=true; return result.value();
#else
    return storage_failure();
#endif
}
void PackageSink::abort() noexcept {
    if (!impl_ || impl_->published || impl_->aborted) return;
#ifdef __linux__
    for (auto& file : impl_->files) file.fd.reset();
    if (impl_->root_fd && !impl_->staging_name.empty()) {
        try { static_cast<void>(remove_staging(impl_->root_fd.get(),impl_->staging_name)); }
        catch (...) { /* startup recovery retains unrecognized entries for inspection */ }
        static_cast<void>(::fsync(impl_->root_fd.get()));
    }
#endif
    impl_->aborted=true;
}
const std::filesystem::path& PackageSink::staging_path() const noexcept { return impl_->staging; }
Result<DeliveredMapPackage> inspect_map_package(const std::filesystem::path& root, const ArtifactManifest& manifest) {
    const auto text=descriptor(manifest); if (!text) return text.error();
    const auto name=published_name(manifest,text.value()); if (!name) return name.error();
#ifdef __linux__
    auto parent=open_root(root,false); if (!parent) return parent.error();
    Fd directory(::openat(parent.value().get(),name.value().c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if (!directory) return make_resource_failure(ResourceFailure::artifact_file_missing);
    const auto valid=verify_directory(directory.get(),manifest,text.value()); if (!valid) return valid.error();
    if (::fsync(directory.get())!=0 || ::fsync(parent.value().get())!=0) return storage_failure();
    std::error_code ec; const auto absolute=std::filesystem::absolute(root,ec).lexically_normal();
    if (ec) return storage_failure();
    return DeliveredMapPackage{absolute/name.value(),manifest,sha256_hex(text.value()).value()};
#else
    static_cast<void>(root); return storage_failure();
#endif
}
Result<void> verify_map_package(const DeliveredMapPackage& package) {
    const auto valid=inspect_map_package(package.directory.parent_path(),package.manifest);
    if (!valid) return valid.error();
    if (valid.value().directory!=package.directory || valid.value().descriptor_sha256!=package.descriptor_sha256)
        return metadata_failure();
    return {};
}
Result<std::size_t> recover_package_staging(const std::filesystem::path& root) {
#ifdef __linux__
    auto parent=open_root(root,false); if (!parent) return parent.error();
    const auto entries=children(parent.value().get()); if (!entries) return entries.error();
    std::size_t count=0;
    for (const auto& name : entries.value()) if (staging_name(name)) {
        const auto removed=remove_staging(parent.value().get(),name);
        if (!removed) return removed.error();
        ++count;
    }
    if (::fsync(parent.value().get())!=0) return storage_failure();
    return count;
#else
    static_cast<void>(root); return storage_failure();
#endif
}
Result<std::string> make_map_package_evidence(const DeliveredMapPackage& package, const TaskGenerationKey& key) {
    if (key.task_id!=package.manifest.task_id || key.attempt<1) return metadata_failure();
    const auto valid=verify_map_package(package); if (!valid) return valid.error();
    Json evidence(jsoncons::json_object_arg);
    evidence["evidence_version"]=1; evidence["receiver"]="map_package"; evidence["task_id"]=key.task_id;
    evidence["attempt"]=key.attempt; evidence["resource_revision_id"]=package.manifest.resource_revision_id;
    evidence["package_digest"]=package.manifest.sha256; evidence["descriptor_digest"]=package.descriptor_sha256;
    evidence["directory"]=package.directory.filename().string(); evidence["durable"]=true;
    evidence["files"]=Json(jsoncons::json_array_arg);
    for (const auto& file : package.manifest.files) {
        Json entry(jsoncons::json_object_arg); entry["id"]=file.id; entry["role"]=file.role;
        entry["name"]=local_name(file); entry["sha256"]=file.sha256; entry["byte_size"]=file.byte_size;
        evidence["files"].push_back(entry);
    }
    auto text=evidence.to_string();
    if (text.size()>16U*1024U) return storage_failure();
    return text;
}
Result<void> stream_map_package(const DeliveredMapPackage& package, const PackageChunkHandler& handler,
    std::size_t chunk_size, const CancellationSignal* cancel) {
    if (!handler || chunk_size==0 || chunk_size>1024U*1024U) return metadata_failure();
    if (cancel && cancel->is_canceled()) return canceled_failure();
    const auto valid=verify_map_package(package); if (!valid) return valid.error();
#ifdef __linux__
    auto parent=open_root(package.directory.parent_path(),false); if (!parent) return parent.error();
    Fd directory(::openat(parent.value().get(),package.directory.filename().c_str(),
        O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    if (!directory) return storage_failure();
    const auto text=descriptor(package.manifest); if (!text) return text.error();
    return verify_directory(directory.get(),package.manifest,text.value(),&handler,chunk_size,cancel);
#else
    return storage_failure();
#endif
}
}
