#pragma once

#include "roc/vehicle/config.hpp"
#include "roc/vehicle/errors.hpp"
#include "roc/vehicle/protocol.hpp"
#include "roc/vehicle/result.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace roc::vehicle {

enum class ConnectionState {
    stopped,
    connecting,
    awaiting_hello,
    online,
    backing_off,
    authentication_stopped,
    connection_replaced,
    halted
};

struct DeviceClientCallbacks {
    std::function<void(ConnectionState)> on_state_changed;
    std::function<void(const TaskAvailableMessage&)> on_task_available;
    std::function<void(const Error&)> on_error;
    std::function<void(std::string_view)> on_warning;
};

class DeviceClient {
public:
    static Result<std::unique_ptr<DeviceClient>> create(
        ClientConfig config,
        DeviceTokenProvider token_provider,
        DeviceClientCallbacks callbacks = {});

    DeviceClient(const DeviceClient&) = delete;
    DeviceClient& operator=(const DeviceClient&) = delete;
    DeviceClient(DeviceClient&&) = delete;
    DeviceClient& operator=(DeviceClient&&) = delete;

    ~DeviceClient();

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    [[nodiscard]] Result<void> resume_after_credential_update();
    [[nodiscard]] Result<void> publish_telemetry(TelemetrySnapshot telemetry);
    void release_task_notification(std::string task_id);
    // Generation-scoped release cannot erase a newer reoffer received meanwhile.
    void release_task_notification(std::string task_id, std::int64_t notification_attempt);

    [[nodiscard]] ConnectionState state() const noexcept;

private:
    struct Impl;
    explicit DeviceClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace roc::vehicle
