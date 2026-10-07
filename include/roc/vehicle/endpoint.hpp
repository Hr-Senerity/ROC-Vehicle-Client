#pragma once

#include "roc/vehicle/result.hpp"

#include <string>
#include <string_view>

namespace roc::vehicle {

struct DeviceEndpoint {
    bool tls{false};
    std::string host;
    std::string port;
    std::string host_header;
    std::string websocket_target;
};

[[nodiscard]] Result<DeviceEndpoint> derive_device_endpoint(
    std::string_view base_url);

[[nodiscard]] Result<std::string> make_device_authorization(
    std::string_view device_token);

}  // namespace roc::vehicle
