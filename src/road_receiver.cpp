#include "roc/vehicle/road_receiver.hpp"
#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include <fstream>
#include <limits>

namespace roc::vehicle {
Result<DeliveredRoadNetworkV2> load_verified_road_network_v2(
    const DeliveredArtifact& artifact,RoadValidationOptions options) {
    try {
    if (artifact.schema_version!=2)
        return make_resource_failure(ResourceFailure::unsupported_schema_version);
    std::error_code error;
    const auto status=std::filesystem::symlink_status(artifact.path,error);
    if (error || !std::filesystem::is_regular_file(status) || !is_valid_uuid(artifact.task_id) ||
        !is_valid_uuid(artifact.resource_revision_id) || artifact.version<1 || artifact.byte_size==0 ||
        artifact.byte_size>options.maximum_document_bytes ||
        artifact.byte_size>static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        artifact.resource_type!=ResourceType::road_network ||
        artifact.content_type!="application/vnd.roc.road-network.v2+json")
        return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    std::ifstream input(artifact.path,std::ios::binary);
    if (!input) return make_resource_failure(ResourceFailure::local_storage_failure);
    std::string text(static_cast<std::size_t>(artifact.byte_size),'\0');
    input.read(text.data(),static_cast<std::streamsize>(text.size()));
    if (input.gcount()!=static_cast<std::streamsize>(text.size()) || input.peek()!=std::char_traits<char>::eof())
        return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    const auto digest=sha256_hex(text);
    if (!digest || digest.value()!=artifact.sha256)
        return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
    auto parsed=parse_road_network_v2(text,std::move(options)); if (!parsed) return parsed.error();
    return DeliveredRoadNetworkV2{artifact,std::move(parsed.value())};
    } catch (const std::exception&) { return make_resource_failure(ResourceFailure::local_storage_failure); }
}
Result<DeliveryReceipt> deliver_road_network_v2_to_callback(const DeliveredArtifact& artifact,
    const VerifiedRoadNetworkV2Callback& callback,RoadValidationOptions options,const CancellationSignal* cancel) {
    if (!callback) return make_resource_failure(ResourceFailure::receiver_commit_failed);
    return deliver_to_verified_file_callback(artifact,[&](const DeliveredArtifact& verified) -> Result<void> {
        auto loaded=load_verified_road_network_v2(verified,options); if (!loaded) return loaded.error();
        if (cancel && cancel->is_canceled()) return Error{ErrorDomain::canceled,ErrorCode::canceled,false,"road canceled"};
        try {
            if (!callback(loaded.value())) return make_resource_failure(ResourceFailure::receiver_commit_failed);
            return {};
        } catch (...) { return make_resource_failure(ResourceFailure::receiver_commit_failed); }
    },cancel);
}
}
