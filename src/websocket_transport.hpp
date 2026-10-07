#pragma once

#include "roc/vehicle/config.hpp"
#include "roc/vehicle/endpoint.hpp"
#include "roc/vehicle/result.hpp"

#include <boost/asio/io_context.hpp>

#include <functional>
#include <memory>
#include <string>

namespace roc::vehicle::internal {

class WebSocketTransport {
public:
    using OpenHandler = std::function<void(Result<void>)>;
    using FrameHandler = std::function<void(std::string)>;
    using CloseHandler = std::function<void(Error)>;
    using WriteHandler = std::function<void(Result<void>)>;

    virtual ~WebSocketTransport() = default;
    virtual void start(OpenHandler on_open,
                       FrameHandler on_frame,
                       CloseHandler on_close) = 0;
    virtual void async_write(std::shared_ptr<const std::string> message,
                             WriteHandler handler) = 0;
    virtual void cancel() noexcept = 0;
};

[[nodiscard]] Result<std::shared_ptr<WebSocketTransport>>
make_beast_websocket_transport(boost::asio::io_context& io_context,
                               DeviceEndpoint endpoint,
                               TlsConfig tls_config,
                               std::string authorization);

}  // namespace roc::vehicle::internal
