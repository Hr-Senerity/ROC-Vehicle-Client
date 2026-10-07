#pragma once
#include "roc/vehicle/artifact_receiver.hpp"
#include "roc/vehicle/road_network.hpp"

namespace roc::vehicle {
struct DeliveredRoadNetworkV2 {
    DeliveredArtifact artifact;
    RoadNetworkV2 network;
};
using VerifiedRoadNetworkV2Callback=std::function<Result<void>(const DeliveredRoadNetworkV2&)>;
// Reads under a local budget, hashes exact bytes, then parses frozen Schema and
// semantics. Metadata has no robot-loaded meaning; artifact binding stays intact.
[[nodiscard]] Result<DeliveredRoadNetworkV2> load_verified_road_network_v2(
    const DeliveredArtifact& artifact, RoadValidationOptions options = {});
// Borrowed input only during this synchronous callback. It must commit before
// returning success; source must remain unchanged, no forced callback cancellation.
[[nodiscard]] Result<DeliveryReceipt> deliver_road_network_v2_to_callback(
    const DeliveredArtifact& artifact,const VerifiedRoadNetworkV2Callback& callback,
    RoadValidationOptions options = {},const CancellationSignal* cancellation = nullptr);
}
