#include "websocket_transport.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/ssl.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace roc::vehicle::internal {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using ErrorCode = boost::system::error_code;

Error connection_error(const ErrorCode& error, ErrorDomain domain = ErrorDomain::transport) {
    return Error{domain, roc::vehicle::ErrorCode::unavailable, true,
                 "WebSocket transport failure: " + error.message()};
}

Error authentication_error() {
    return Error{ErrorDomain::authentication, roc::vehicle::ErrorCode::permission_denied,
                 false, "WebSocket authentication was rejected"};
}

Error protocol_frame_error() {
    return Error{ErrorDomain::protocol, roc::vehicle::ErrorCode::invalid_argument,
                 false, "WebSocket binary frames are not accepted"};
}

template <typename Stream>
void configure_websocket(Stream& stream, const std::string& authorization) {
    websocket::stream_base::timeout timeout =
        websocket::stream_base::timeout::suggested(beast::role_type::client);
    timeout.keep_alive_pings = false;
    stream.set_option(timeout);
    stream.read_message_max(65536);
    stream.binary(false);
    stream.set_option(websocket::stream_base::decorator(
        [authorization](websocket::request_type& request) {
            request.set(http::field::authorization, authorization);
            request.set(http::field::user_agent, "roc-vehicle-cpp/0.1");
        }));
}

class PlainTransport final : public WebSocketTransport,
                             public std::enable_shared_from_this<PlainTransport> {
public:
    PlainTransport(asio::io_context& io_context,
                   DeviceEndpoint endpoint,
                   std::string authorization)
        : resolver_(asio::make_strand(io_context)),
          stream_(asio::make_strand(io_context)),
          endpoint_(std::move(endpoint)),
          authorization_(std::move(authorization)) {}

    void start(OpenHandler on_open,
               FrameHandler on_frame,
               CloseHandler on_close) override {
        on_open_ = std::move(on_open);
        on_frame_ = std::move(on_frame);
        on_close_ = std::move(on_close);
        resolver_.async_resolve(
            endpoint_.host, endpoint_.port,
            [self = shared_from_this()](const ErrorCode& error,
                                        const tcp::resolver::results_type& results) {
                if (error) {
                    self->finish_open(connection_error(error));
                    return;
                }
                beast::get_lowest_layer(self->stream_).expires_after(
                    std::chrono::seconds{10});
                beast::get_lowest_layer(self->stream_).async_connect(
                    results,
                    [self](const ErrorCode& connect_error,
                           const tcp::resolver::results_type::endpoint_type&) {
                        if (connect_error) {
                            self->finish_open(connection_error(connect_error));
                            return;
                        }
                        self->handshake();
                    });
            });
    }

    void async_write(std::shared_ptr<const std::string> message,
                     WriteHandler handler) override {
        stream_.async_write(
            asio::buffer(*message),
            [self = shared_from_this(), message,
             handler = std::move(handler)](const ErrorCode& error, std::size_t) mutable {
                if (error) {
                    handler(connection_error(error));
                    return;
                }
                handler(Result<void>{});
            });
    }

    void cancel() noexcept override {
        active_ = false;
        resolver_.cancel();
        ErrorCode ignored;
        beast::get_lowest_layer(stream_).socket().cancel(ignored);
        beast::get_lowest_layer(stream_).socket().close(ignored);
    }

private:
    void handshake() {
        configure_websocket(stream_, authorization_);
        response_ = {};
        stream_.async_handshake(
            response_, endpoint_.host_header, endpoint_.websocket_target,
            [self = shared_from_this()](const ErrorCode& error) {
                if (error) {
                    if (self->response_.result() == http::status::unauthorized ||
                        self->response_.result() == http::status::forbidden) {
                        self->finish_open(authentication_error());
                    } else {
                        self->finish_open(connection_error(error));
                    }
                    return;
                }
                beast::get_lowest_layer(self->stream_).expires_never();
                self->active_ = true;
                self->finish_open(Result<void>{});
                self->read_next();
            });
    }

    void read_next() {
        if (!active_) {
            return;
        }
        stream_.async_read(
            buffer_, [self = shared_from_this()](const ErrorCode& error, std::size_t) {
                if (error) {
                    self->finish_close(connection_error(error));
                    return;
                }
                if (!self->stream_.got_text()) {
                    self->finish_close(protocol_frame_error());
                    self->cancel();
                    return;
                }
                auto frame = beast::buffers_to_string(self->buffer_.data());
                self->buffer_.consume(self->buffer_.size());
                self->on_frame_(std::move(frame));
                self->read_next();
            });
    }

    void finish_open(Result<void> result) {
        if (on_open_) {
            auto handler = std::move(on_open_);
            handler(std::move(result));
        }
    }

    void finish_close(Error error) {
        if (!active_) {
            return;
        }
        active_ = false;
        if (on_close_) {
            on_close_(std::move(error));
        }
    }

    tcp::resolver resolver_;
    websocket::stream<beast::tcp_stream> stream_;
    DeviceEndpoint endpoint_;
    std::string authorization_;
    beast::flat_buffer buffer_;
    websocket::response_type response_;
    OpenHandler on_open_;
    FrameHandler on_frame_;
    CloseHandler on_close_;
    bool active_{false};
};

class TlsTransport final : public WebSocketTransport,
                           public std::enable_shared_from_this<TlsTransport> {
public:
    TlsTransport(asio::io_context& io_context,
                 DeviceEndpoint endpoint,
                 TlsConfig tls_config,
                 std::string authorization)
        : resolver_(asio::make_strand(io_context)),
          context_(ssl::context::tls_client),
          stream_(asio::make_strand(io_context), context_),
          endpoint_(std::move(endpoint)),
          tls_config_(std::move(tls_config)),
          authorization_(std::move(authorization)) {}

    Result<void> configure_tls() {
        ErrorCode error;
        context_.set_options(ssl::context::default_workarounds, error);
        if (error) {
            return connection_error(error, ErrorDomain::tls);
        }
        if (tls_config_.insecure_demo) {
            context_.set_verify_mode(ssl::verify_none, error);
        } else {
            context_.set_verify_mode(ssl::verify_peer, error);
            if (!error && tls_config_.ca_file.has_value()) {
                context_.load_verify_file(tls_config_.ca_file->string(), error);
            } else if (!error) {
                context_.set_default_verify_paths(error);
            }
            if (!error && tls_config_.verify_hostname) {
                stream_.next_layer().set_verify_callback(
                    ssl::host_name_verification(endpoint_.host));
            }
        }
        if (error) {
            return connection_error(error, ErrorDomain::tls);
        }
        if (SSL_set_tlsext_host_name(stream_.next_layer().native_handle(),
                                     endpoint_.host.c_str()) != 1) {
            return Error{ErrorDomain::tls, roc::vehicle::ErrorCode::unavailable,
                         false, "unable to configure TLS server name"};
        }
        return {};
    }

    void start(OpenHandler on_open,
               FrameHandler on_frame,
               CloseHandler on_close) override {
        on_open_ = std::move(on_open);
        on_frame_ = std::move(on_frame);
        on_close_ = std::move(on_close);
        resolver_.async_resolve(
            endpoint_.host, endpoint_.port,
            [self = shared_from_this()](const ErrorCode& error,
                                        const tcp::resolver::results_type& results) {
                if (error) {
                    self->finish_open(connection_error(error));
                    return;
                }
                beast::get_lowest_layer(self->stream_).expires_after(
                    std::chrono::seconds{10});
                beast::get_lowest_layer(self->stream_).async_connect(
                    results,
                    [self](const ErrorCode& connect_error,
                           const tcp::resolver::results_type::endpoint_type&) {
                        if (connect_error) {
                            self->finish_open(connection_error(connect_error));
                            return;
                        }
                        self->tls_handshake();
                    });
            });
    }

    void async_write(std::shared_ptr<const std::string> message,
                     WriteHandler handler) override {
        stream_.async_write(
            asio::buffer(*message),
            [self = shared_from_this(), message,
             handler = std::move(handler)](const ErrorCode& error, std::size_t) mutable {
                if (error) {
                    handler(connection_error(error));
                    return;
                }
                handler(Result<void>{});
            });
    }

    void cancel() noexcept override {
        active_ = false;
        resolver_.cancel();
        ErrorCode ignored;
        beast::get_lowest_layer(stream_).socket().cancel(ignored);
        beast::get_lowest_layer(stream_).socket().close(ignored);
    }

private:
    void tls_handshake() {
        stream_.next_layer().async_handshake(
            ssl::stream_base::client,
            [self = shared_from_this()](const ErrorCode& error) {
                if (error) {
                    self->finish_open(connection_error(error, ErrorDomain::tls));
                    return;
                }
                self->websocket_handshake();
            });
    }

    void websocket_handshake() {
        configure_websocket(stream_, authorization_);
        response_ = {};
        stream_.async_handshake(
            response_, endpoint_.host_header, endpoint_.websocket_target,
            [self = shared_from_this()](const ErrorCode& error) {
                if (error) {
                    if (self->response_.result() == http::status::unauthorized ||
                        self->response_.result() == http::status::forbidden) {
                        self->finish_open(authentication_error());
                    } else {
                        self->finish_open(connection_error(error));
                    }
                    return;
                }
                beast::get_lowest_layer(self->stream_).expires_never();
                self->active_ = true;
                self->finish_open(Result<void>{});
                self->read_next();
            });
    }

    void read_next() {
        if (!active_) {
            return;
        }
        stream_.async_read(
            buffer_, [self = shared_from_this()](const ErrorCode& error, std::size_t) {
                if (error) {
                    self->finish_close(connection_error(error));
                    return;
                }
                if (!self->stream_.got_text()) {
                    self->finish_close(protocol_frame_error());
                    self->cancel();
                    return;
                }
                auto frame = beast::buffers_to_string(self->buffer_.data());
                self->buffer_.consume(self->buffer_.size());
                self->on_frame_(std::move(frame));
                self->read_next();
            });
    }

    void finish_open(Result<void> result) {
        if (on_open_) {
            auto handler = std::move(on_open_);
            handler(std::move(result));
        }
    }

    void finish_close(Error error) {
        if (!active_) {
            return;
        }
        active_ = false;
        if (on_close_) {
            on_close_(std::move(error));
        }
    }

    tcp::resolver resolver_;
    ssl::context context_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> stream_;
    DeviceEndpoint endpoint_;
    TlsConfig tls_config_;
    std::string authorization_;
    beast::flat_buffer buffer_;
    websocket::response_type response_;
    OpenHandler on_open_;
    FrameHandler on_frame_;
    CloseHandler on_close_;
    bool active_{false};
};

}  // namespace

Result<std::shared_ptr<WebSocketTransport>> make_beast_websocket_transport(
    asio::io_context& io_context,
    DeviceEndpoint endpoint,
    TlsConfig tls_config,
    std::string authorization) {
    if (endpoint.tls) {
        auto transport = std::make_shared<TlsTransport>(
            io_context, std::move(endpoint), std::move(tls_config),
            std::move(authorization));
        const auto configured = transport->configure_tls();
        if (!configured) {
            return configured.error();
        }
        return std::static_pointer_cast<WebSocketTransport>(transport);
    }
    return std::static_pointer_cast<WebSocketTransport>(
        std::make_shared<PlainTransport>(io_context, std::move(endpoint),
                                         std::move(authorization)));
}

}  // namespace roc::vehicle::internal
