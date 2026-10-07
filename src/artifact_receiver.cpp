#include "roc/vehicle/artifact_receiver.hpp"
#include "roc/vehicle/resource_failure.hpp"

#include <openssl/evp.h>

#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace roc::vehicle {
namespace {

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

Error delivery_error(ErrorCode code, std::string message) {
    return Error{ErrorDomain::artifact, code, false, std::move(message)};
}

Error canceled_error() {
    return Error{ErrorDomain::canceled, ErrorCode::canceled, false,
                 "controlled artifact delivery was canceled"};
}

bool canceled(const CancellationSignal* signal) {
    return signal != nullptr && signal->is_canceled();
}

Result<void> validate_artifact_identity(const DeliveredArtifact& artifact) {
    if (artifact.path.empty() || artifact.sha256.size() != 64 ||
        artifact.byte_size == 0 || artifact.content_type.empty() ||
        !is_valid_uuid(artifact.task_id) ||
        !is_valid_uuid(artifact.resource_revision_id) || artifact.version < 1) {
        return delivery_error(ErrorCode::invalid_argument,
                              "delivered artifact metadata is invalid");
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(artifact.path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        return delivery_error(ErrorCode::integrity_failure,
                              "delivered artifact path is not a regular file");
    }
    return {};
}

Result<std::string> verify_and_hash_file(const DeliveredArtifact& artifact) {
    const auto identity = validate_artifact_identity(artifact);
    if (!identity) {
        return identity.error();
    }
    std::ifstream input{artifact.path, std::ios::binary};
    if (!input) {
        return delivery_error(ErrorCode::io_failure,
                              "unable to open verified artifact");
    }
    DigestContext digest{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        return delivery_error(ErrorCode::internal_failure,
                              "unable to initialize delivery SHA-256");
    }
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t total = 0;
    std::string road_document;
    const bool road=artifact.resource_type==ResourceType::road_network;
    if (road && artifact.byte_size>64U*1024U*1024U)
        return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto amount = static_cast<std::uint64_t>(count);
            if (total > artifact.byte_size || amount > artifact.byte_size - total) {
                return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
            }
            if (EVP_DigestUpdate(digest.get(), buffer.data(),
                                 static_cast<std::size_t>(count)) != 1) {
                return delivery_error(ErrorCode::internal_failure,
                                      "unable to update delivery SHA-256");
            }
            total += amount;
            if (road) road_document.append(buffer.data(),static_cast<std::size_t>(amount));
        }
    }
    if (!input.eof() || total != artifact.byte_size) {
        return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &size) != 1 || size != 32U) {
        return delivery_error(ErrorCode::internal_failure,
                              "unable to finalize delivery SHA-256");
    }
    static constexpr char hexadecimal[] = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (unsigned int index = 0; index < size; ++index) {
        actual.push_back(hexadecimal[(bytes[index] >> 4U) & 0x0fU]);
        actual.push_back(hexadecimal[bytes[index] & 0x0fU]);
    }
    if (actual != artifact.sha256) {
        return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
    }
    if (road) {
        if (artifact.schema_version==2 && artifact.content_type=="application/vnd.roc.road-network.v2+json") {
            const auto parsed=parse_road_network_v2(road_document);
            if (!parsed) return parsed.error();
        } else if ((artifact.schema_version==1 || artifact.schema_version==0) &&
            artifact.content_type=="application/vnd.roc.road-network+json") {
            if (!validate_road_network_v1(road_document))
                return make_resource_failure(ResourceFailure::road_network_invalid);
        } else return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    }
    return actual;
}

class AbortGuard {
public:
    explicit AbortGuard(ArtifactStreamTransaction& transaction)
        : transaction_(transaction) {}
    ~AbortGuard() {
        if (!committed_) {
            transaction_.abort();
        }
    }
    void committed() noexcept { committed_ = true; }

private:
    ArtifactStreamTransaction& transaction_;
    bool committed_{false};
};

}  // namespace

Result<DeliveryReceipt> deliver_to_verified_file_callback(
    const DeliveredArtifact& artifact,
    const VerifiedFileCallback& callback,
    const CancellationSignal* cancellation) {
    if (!callback) {
        return delivery_error(ErrorCode::invalid_argument,
                              "verified-file callback is required");
    }
    if (canceled(cancellation)) {
        return canceled_error();
    }
    const auto verified = verify_and_hash_file(artifact);
    if (!verified) {
        return verified.error();
    }
    if (canceled(cancellation)) {
        return canceled_error();
    }
    Result<void> delivered = [&]() -> Result<void> {
        try {
            return callback(artifact);
        } catch (const std::exception&) {
            return delivery_error(ErrorCode::internal_failure,
                                  "verified-file callback threw an exception");
        } catch (...) {
            return delivery_error(ErrorCode::internal_failure,
                                  "verified-file callback threw a nonstandard exception");
        }
    }();
    if (!delivered) {
        return delivered.error();
    }
    const auto unchanged = verify_and_hash_file(artifact);
    if (!unchanged) {
        return unchanged.error();
    }
    return DeliveryReceipt{true};
}

Result<DeliveryReceipt> deliver_to_transactional_stream(
    const DeliveredArtifact& artifact,
    TransactionalStreamReceiver& receiver,
    std::size_t chunk_size,
    const CancellationSignal* cancellation) {
    if (chunk_size == 0) {
        return delivery_error(ErrorCode::invalid_argument,
                              "stream receiver chunk size must be positive");
    }
    if (canceled(cancellation)) {
        return canceled_error();
    }
    const auto verified = verify_and_hash_file(artifact);
    if (!verified) {
        return verified.error();
    }
    if (canceled(cancellation)) {
        return canceled_error();
    }
    auto begun = [&]() -> Result<std::unique_ptr<ArtifactStreamTransaction>> {
        try {
            return receiver.begin(artifact);
        } catch (const std::exception&) {
            return delivery_error(ErrorCode::internal_failure,
                                  "stream receiver begin threw an exception");
        } catch (...) {
            return delivery_error(ErrorCode::internal_failure,
                                  "stream receiver begin threw a nonstandard exception");
        }
    }();
    if (!begun) {
        return begun.error();
    }
    if (!begun.value()) {
        return delivery_error(ErrorCode::invalid_state,
                              "stream receiver returned no transaction");
    }
    auto transaction = std::move(begun.value());
    AbortGuard guard{*transaction};
    std::ifstream input{artifact.path, std::ios::binary};
    if (!input) {
        return delivery_error(ErrorCode::io_failure,
                              "unable to reopen verified artifact");
    }
    DigestContext stream_digest{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!stream_digest ||
        EVP_DigestInit_ex(stream_digest.get(), EVP_sha256(), nullptr) != 1) {
        return delivery_error(ErrorCode::internal_failure,
                              "unable to initialize stream delivery SHA-256");
    }
    std::vector<std::uint8_t> buffer(chunk_size);
    std::uint64_t total = 0;
    while (input) {
        if (canceled(cancellation)) {
            return canceled_error();
        }
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto amount = static_cast<std::size_t>(count);
            const auto written = [&]() -> Result<void> {
                try {
                    return transaction->write(buffer.data(), amount);
                } catch (const std::exception&) {
                    return delivery_error(ErrorCode::internal_failure,
                                          "stream receiver write threw an exception");
                } catch (...) {
                    return delivery_error(
                        ErrorCode::internal_failure,
                        "stream receiver write threw a nonstandard exception");
                }
            }();
            if (!written) {
                return written.error();
            }
            if (EVP_DigestUpdate(stream_digest.get(), buffer.data(), amount) != 1) {
                return delivery_error(ErrorCode::internal_failure,
                                      "unable to update stream delivery SHA-256");
            }
            total += static_cast<std::uint64_t>(amount);
        }
    }
    if (!input.eof() || total != artifact.byte_size) {
        return delivery_error(ErrorCode::integrity_failure,
                              "artifact changed during stream delivery");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> stream_bytes{};
    unsigned int stream_size = 0;
    if (EVP_DigestFinal_ex(stream_digest.get(), stream_bytes.data(),
                           &stream_size) != 1 || stream_size != 32U) {
        return delivery_error(ErrorCode::internal_failure,
                              "unable to finalize stream delivery SHA-256");
    }
    static constexpr char hexadecimal[] = "0123456789abcdef";
    std::string streamed_hash;
    streamed_hash.reserve(64);
    for (unsigned int index = 0; index < stream_size; ++index) {
        streamed_hash.push_back(hexadecimal[(stream_bytes[index] >> 4U) & 0x0fU]);
        streamed_hash.push_back(hexadecimal[stream_bytes[index] & 0x0fU]);
    }
    if (streamed_hash != artifact.sha256) {
        return delivery_error(ErrorCode::integrity_failure,
                              "artifact changed during stream delivery");
    }
    if (canceled(cancellation)) {
        return canceled_error();
    }
    const auto committed = [&]() -> Result<void> {
        try {
            return transaction->commit();
        } catch (const std::exception&) {
            return delivery_error(ErrorCode::internal_failure,
                                  "stream receiver commit threw an exception");
        } catch (...) {
            return delivery_error(ErrorCode::internal_failure,
                                  "stream receiver commit threw a nonstandard exception");
        }
    }();
    if (!committed) {
        return committed.error();
    }
    guard.committed();
    return DeliveryReceipt{true};
}

}  // namespace roc::vehicle
