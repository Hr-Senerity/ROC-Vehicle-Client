#include "roc/vehicle/device_client.hpp"

#include "roc/vehicle/endpoint.hpp"
#include "roc/vehicle/state_store.hpp"
#include "roc/vehicle/version.hpp"
#include "session_core.hpp"
#include "websocket_transport.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace roc::vehicle {
namespace {

namespace asio = boost::asio;

Error lifecycle_error(std::string message) {
    return Error{ErrorDomain::internal, ErrorCode::invalid_state, false,
                 std::move(message)};
}

void cancel_timer(asio::steady_timer& timer) noexcept {
    try {
        timer.cancel();
    } catch (...) {
    }
}

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - seconds).count();
    const std::time_t value = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    char buffer[32]{};
    const int written = std::snprintf(
        buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
        utc.tm_min, utc.tm_sec, static_cast<long long>(milliseconds));
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
        return {};
    }
    return buffer;
}

}  // namespace

struct DeviceClient::Impl {
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;
    enum class WriteKind { heartbeat, telemetry };

    struct PendingWrite {
        std::shared_ptr<const std::string> message;
        WriteKind kind{WriteKind::heartbeat};
    };

    Impl(ClientConfig client_config,
         DeviceEndpoint device_endpoint,
         DeviceTokenProvider provider,
         DeviceClientCallbacks client_callbacks,
         std::unique_ptr<DurableStateStore> durable_state)
        : config(std::move(client_config)),
          endpoint(std::move(device_endpoint)),
          token_provider(std::move(provider)),
          callbacks(std::move(client_callbacks)),
          state_store(std::move(durable_state)),
          session(*state_store, config.task_queue_capacity, {}, config.expected_vehicle_id),
          reconnect_timer(io_context),
          hello_timer(io_context),
          heartbeat_timer(io_context),
          random_engine(std::random_device{}()),
          next_reconnect_delay(config.reconnect.initial_delay) {}

    ~Impl() {
        stop();
    }

    Result<void> start() {
        std::lock_guard<std::mutex> lock{lifecycle_mutex};
        if (running.load()) {
            return lifecycle_error("device client is already started");
        }
        stopping = false;
        reconnect_scheduled = false;
        io_context.restart();
        work_guard.emplace(io_context.get_executor());
        running.store(true);
        accepting_posts.store(true);
        worker = std::thread([this] { io_context.run(); });
        asio::post(io_context, [this] { begin_connect(); });
        return {};
    }

    void stop() noexcept {
        std::unique_lock<std::mutex> lock{lifecycle_mutex};
        if (!running.load()) {
            return;
        }
        accepting_posts.store(false);
        if (worker.get_id() == std::this_thread::get_id()) {
            shutdown_on_io_thread();
            return;
        }
        if (!stopping) {
            asio::post(io_context, [this] { shutdown_on_io_thread(); });
        }
        lock.unlock();
        if (worker.joinable()) {
            worker.join();
        }
        running.store(false);
    }

    void shutdown_on_io_thread() noexcept {
        stopping = true;
        cancel_timer(reconnect_timer);
        cancel_timer(hello_timer);
        cancel_timer(heartbeat_timer);
        if (transport) {
            transport->cancel();
            transport.reset();
        }
        writes.clear();
        write_active = false;
        heartbeat_pending = false;
        telemetry_pending = false;
        session.reset_connection();
        set_state(ConnectionState::stopped);
        if (work_guard) {
            work_guard->reset();
            work_guard.reset();
        }
    }

    void begin_connect() {
        if (stopping) {
            return;
        }
        if (config.tls.insecure_demo && !insecure_warning_emitted) {
            insecure_warning_emitted = true;
            notify_warning("WARNING: TLS peer and hostname verification are disabled by insecure_demo");
        }
        reconnect_scheduled = false;
        set_state(ConnectionState::connecting);
        if (stopping) {
            return;
        }

        Result<std::string> token = lifecycle_error("credential provider failed");
        try {
            token = token_provider();
        } catch (...) {
            notify_error(Error{ErrorDomain::authentication,
                               ErrorCode::internal_failure, false,
                               "device token provider threw an exception"});
            set_state(ConnectionState::authentication_stopped);
            return;
        }
        if (!token) {
            notify_error(token.error());
            set_state(ConnectionState::authentication_stopped);
            return;
        }
        auto authorization = make_device_authorization(token.value());
        token.value().assign(token.value().size(), '\0');
        if (!authorization) {
            notify_error(authorization.error());
            set_state(ConnectionState::authentication_stopped);
            return;
        }

        auto created = internal::make_beast_websocket_transport(
            io_context, endpoint, config.tls, std::move(authorization.value()));
        if (!created) {
            notify_error(created.error());
            set_state(ConnectionState::halted);
            return;
        }
        transport = std::move(created.value());
        transport->start(
            [this](Result<void> opened) {
                if (!opened) {
                    handle_connection_failure(opened.error());
                    return;
                }
                set_state(ConnectionState::awaiting_hello);
                if (stopping) {
                    return;
                }
                hello_timer.expires_after(config.hello_timeout);
                hello_timer.async_wait([this](const boost::system::error_code& error) {
                    if (!error && current_state.load() == ConnectionState::awaiting_hello) {
                        handle_connection_failure(Error{
                            ErrorDomain::protocol, ErrorCode::timeout, true,
                            "timed out waiting for WebSocket hello"});
                    }
                });
            },
            [this](std::string frame) { handle_frame(std::move(frame)); },
            [this](Error error) { handle_connection_failure(std::move(error)); });
    }

    void handle_frame(std::string frame) {
        const auto handled = session.handle_text_frame(frame);
        if (!handled) {
            handle_connection_failure(handled.error());
            return;
        }
        const auto& event = handled.value();
        if (event.hello.has_value()) {
            cancel_timer(hello_timer);
            next_reconnect_delay = config.reconnect.initial_delay;
            set_state(ConnectionState::online);
            if (stopping) {
                return;
            }
            schedule_heartbeat();
            send_latest_telemetry();
        }
        if (event.task.has_value() && callbacks.on_task_available) {
            try {
                callbacks.on_task_available(*event.task);
            } catch (...) {
                notify_error(Error{ErrorDomain::internal, ErrorCode::internal_failure,
                                   false, "task callback threw an exception"});
            }
        }
        if (event.error.has_value()) {
            notify_error(Error{ErrorDomain::protocol, ErrorCode::invalid_state,
                               event.directive == SessionDirective::reconnect,
                               "server WebSocket error", event.error->code});
            if (stopping) {
                return;
            }
        }
        switch (event.directive) {
            case SessionDirective::none:
            case SessionDirective::reject_outbound:
                break;
            case SessionDirective::reconnect:
                handle_connection_failure(Error{
                    ErrorDomain::transport, ErrorCode::unavailable, true,
                    "server requested WebSocket reconnect"});
                break;
            case SessionDirective::stop_authentication:
                stop_automatic_reconnect(ConnectionState::authentication_stopped);
                break;
            case SessionDirective::stop_replaced:
                stop_automatic_reconnect(ConnectionState::connection_replaced);
                break;
        }
    }

    void schedule_heartbeat() {
        if (current_state.load() != ConnectionState::online) {
            return;
        }
        const auto server_seconds = session.effective_limits().heartbeat_interval_seconds;
        const auto delay_ms = std::max<std::int64_t>(1000, server_seconds * 750);
        heartbeat_timer.expires_after(std::chrono::milliseconds{delay_ms});
        heartbeat_timer.async_wait([this](const boost::system::error_code& error) {
            if (error || current_state.load() != ConnectionState::online) {
                return;
            }
            send_heartbeat();
            schedule_heartbeat();
        });
    }

    void send_heartbeat() {
        if (heartbeat_pending || current_state.load() != ConnectionState::online) {
            return;
        }
        auto message = session.make_heartbeat(
            utc_timestamp(), library_version());
        if (!message) {
            if (message.error().retryable) {
                handle_connection_failure(message.error());
            } else {
                notify_error(message.error());
                if (!stopping) {
                    stop_automatic_reconnect(ConnectionState::halted);
                }
            }
            return;
        }
        heartbeat_pending = true;
        enqueue_write(std::move(message.value().json), WriteKind::heartbeat);
    }

    void send_latest_telemetry() {
        if (telemetry_pending || !latest_telemetry.has_value() ||
            current_state.load() != ConnectionState::online) {
            return;
        }
        auto snapshot = std::move(*latest_telemetry);
        latest_telemetry.reset();
        auto message = session.make_telemetry(utc_timestamp(), snapshot);
        if (!message) {
            if (message.error().retryable) {
                handle_connection_failure(message.error());
            } else {
                notify_error(message.error());
            }
            return;
        }
        telemetry_pending = true;
        enqueue_write(std::move(message.value().json), WriteKind::telemetry);
    }

    void enqueue_write(std::string json, WriteKind kind) {
        writes.push_back(PendingWrite{
            std::make_shared<const std::string>(std::move(json)), kind});
        pump_writes();
    }

    void pump_writes() {
        if (write_active || writes.empty() || !transport ||
            current_state.load() != ConnectionState::online) {
            return;
        }
        write_active = true;
        const auto pending = writes.front();
        transport->async_write(
            pending.message,
            [this, kind = pending.kind](Result<void> written) {
                write_active = false;
                if (!writes.empty()) {
                    writes.pop_front();
                }
                if (kind == WriteKind::heartbeat) {
                    heartbeat_pending = false;
                } else {
                    telemetry_pending = false;
                }
                if (!written) {
                    handle_connection_failure(written.error());
                    return;
                }
                if (kind == WriteKind::telemetry) {
                    send_latest_telemetry();
                }
                pump_writes();
            });
    }

    void handle_connection_failure(Error error) {
        const auto state = current_state.load();
        if (stopping || state == ConnectionState::backing_off ||
            state == ConnectionState::authentication_stopped ||
            state == ConnectionState::connection_replaced ||
            state == ConnectionState::halted) {
            return;
        }
        notify_error(error);
        if (stopping) {
            return;
        }
        if (error.domain == ErrorDomain::authentication && !error.retryable) {
            stop_automatic_reconnect(ConnectionState::authentication_stopped);
            return;
        }
        if (transport) {
            transport->cancel();
            transport.reset();
        }
        cancel_timer(hello_timer);
        cancel_timer(heartbeat_timer);
        writes.clear();
        write_active = false;
        heartbeat_pending = false;
        telemetry_pending = false;
        session.reset_connection();
        schedule_reconnect();
    }

    void schedule_reconnect() {
        if (stopping || reconnect_scheduled) {
            return;
        }
        reconnect_scheduled = true;
        set_state(ConnectionState::backing_off);
        std::uniform_real_distribution<double> jitter{
            config.reconnect.jitter_min, config.reconnect.jitter_max};
        const auto jittered = static_cast<std::int64_t>(std::llround(
            static_cast<double>(next_reconnect_delay.count()) * jitter(random_engine)));
        reconnect_timer.expires_after(
            std::chrono::milliseconds{std::max<std::int64_t>(1, jittered)});
        next_reconnect_delay = std::min(
            next_reconnect_delay * 2, config.reconnect.maximum_delay);
        reconnect_timer.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                begin_connect();
            }
        });
    }

    void stop_automatic_reconnect(ConnectionState stopped_state) {
        cancel_timer(reconnect_timer);
        cancel_timer(hello_timer);
        cancel_timer(heartbeat_timer);
        if (transport) {
            transport->cancel();
            transport.reset();
        }
        writes.clear();
        write_active = false;
        heartbeat_pending = false;
        telemetry_pending = false;
        session.reset_connection();
        set_state(stopped_state);
    }

    void notify_error(const Error& error) noexcept {
        if (callbacks.on_error) {
            try {
                callbacks.on_error(error);
            } catch (...) {
            }
        }
    }

    void notify_warning(std::string_view warning) noexcept {
        if (callbacks.on_warning) {
            try {
                callbacks.on_warning(warning);
            } catch (...) {
            }
        }
    }

    void set_state(ConnectionState state) noexcept {
        if (current_state.exchange(state) == state || !callbacks.on_state_changed) {
            return;
        }
        try {
            callbacks.on_state_changed(state);
        } catch (...) {
        }
    }

    ClientConfig config;
    DeviceEndpoint endpoint;
    DeviceTokenProvider token_provider;
    DeviceClientCallbacks callbacks;
    std::unique_ptr<DurableStateStore> state_store;
    SessionCore session;
    asio::io_context io_context;
    asio::steady_timer reconnect_timer;
    asio::steady_timer hello_timer;
    asio::steady_timer heartbeat_timer;
    std::optional<WorkGuard> work_guard;
    std::shared_ptr<internal::WebSocketTransport> transport;
    std::deque<PendingWrite> writes;
    std::optional<TelemetrySnapshot> latest_telemetry;
    std::mt19937_64 random_engine;
    std::chrono::milliseconds next_reconnect_delay;
    std::thread worker;
    std::mutex lifecycle_mutex;
    std::atomic<ConnectionState> current_state{ConnectionState::stopped};
    std::atomic_bool running{false};
    std::atomic_bool accepting_posts{false};
    bool stopping{false};
    bool reconnect_scheduled{false};
    bool write_active{false};
    bool heartbeat_pending{false};
    bool telemetry_pending{false};
    bool insecure_warning_emitted{false};
};

DeviceClient::DeviceClient(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DeviceClient::~DeviceClient() = default;

Result<std::unique_ptr<DeviceClient>> DeviceClient::create(
    ClientConfig config,
    DeviceTokenProvider token_provider,
    DeviceClientCallbacks callbacks) {
    const auto valid = validate_config(config);
    if (!valid) {
        return valid.error();
    }
    if (!token_provider) {
        return Error{ErrorDomain::configuration, ErrorCode::invalid_argument, false,
                     "device token provider is required"};
    }
    const auto endpoint = derive_device_endpoint(config.base_url);
    if (!endpoint) {
        return endpoint.error();
    }
    auto state_store = DurableStateStore::open(
        config.state_directory / "roc-vehicle-state.sqlite3");
    if (!state_store) {
        return state_store.error();
    }
    auto impl = std::make_unique<Impl>(
        std::move(config), endpoint.value(), std::move(token_provider),
        std::move(callbacks), std::move(state_store.value()));
    return std::unique_ptr<DeviceClient>{new DeviceClient{std::move(impl)}};
}

Result<void> DeviceClient::start() {
    return impl_->start();
}

void DeviceClient::stop() noexcept {
    impl_->stop();
}

Result<void> DeviceClient::resume_after_credential_update() {
    if (!impl_->running.load() ||
        impl_->current_state.load() != ConnectionState::authentication_stopped) {
        return lifecycle_error("client is not stopped for authentication");
    }
    asio::post(impl_->io_context, [impl = impl_.get()] {
        impl->next_reconnect_delay = impl->config.reconnect.initial_delay;
        impl->begin_connect();
    });
    return {};
}

Result<void> DeviceClient::publish_telemetry(TelemetrySnapshot telemetry) {
    if (!impl_->accepting_posts.load()) {
        return lifecycle_error("device client is not started");
    }
    asio::post(impl_->io_context,
               [impl = impl_.get(), telemetry = std::move(telemetry)]() mutable {
                   if (!impl->accepting_posts.load()) {
                       return;
                   }
                   impl->latest_telemetry = std::move(telemetry);
                   impl->send_latest_telemetry();
               });
    return {};
}

void DeviceClient::release_task_notification(std::string task_id) {
    if (impl_->accepting_posts.load()) {
        asio::post(impl_->io_context,
                   [impl = impl_.get(), task_id = std::move(task_id)] {
                       if (impl->accepting_posts.load()) {
                           impl->session.release_task_notification(task_id);
                       }
                   });
    }
}

ConnectionState DeviceClient::state() const noexcept {
    return impl_->current_state.load();
}

void DeviceClient::release_task_notification(std::string task_id, std::int64_t notification_attempt) {
    if (impl_->accepting_posts.load()) {
        asio::post(impl_->io_context,
                   [impl = impl_.get(), task_id = std::move(task_id), notification_attempt] {
                       if (impl->accepting_posts.load())
                           impl->session.release_task_notification(task_id, notification_attempt);
                   });
    }
}

}  // namespace roc::vehicle
