#pragma once

#include "roc/vehicle/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace roc::vehicle {

using DeviceTokenProvider = std::function<Result<std::string>()>;

struct TlsConfig {
    bool verify_peer{true};
    bool verify_hostname{true};
    bool insecure_demo{false};
    std::optional<std::filesystem::path> ca_file;
};

struct ReconnectPolicy {
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds maximum_delay{30000};
    double jitter_min{0.8};
    double jitter_max{1.2};
};

struct ArtifactLimits {
    std::uint64_t maximum_map_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_road_network_bytes{64ULL * 1024ULL * 1024ULL};
    std::size_t stream_chunk_bytes{64U * 1024U};
};

struct ClientConfig {
    std::string base_url;
    std::filesystem::path state_directory;
    std::filesystem::path artifact_directory;
    TlsConfig tls;
    ReconnectPolicy reconnect;
    ArtifactLimits artifact_limits;
    std::chrono::milliseconds hello_timeout{10000};
    std::size_t task_queue_capacity{32};
    // Per connect/handshake/read/write deadline, not a total transfer budget.
    std::chrono::milliseconds http_operation_timeout{15000};
    // Optional pinned local identity. Applications such as the driver require it.
    // No identity/capability field is added to upstream protocol messages.
    std::string expected_vehicle_id;
};

[[nodiscard]] Result<void> validate_config(const ClientConfig& config);

}  // namespace roc::vehicle
