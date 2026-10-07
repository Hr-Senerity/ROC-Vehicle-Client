#include "roc/vehicle/task_http_client.hpp"

#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/endpoint.hpp"
#include "roc/vehicle/resource_failure.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roc::vehicle {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using BoostError = boost::system::error_code;

constexpr std::uint64_t kMaximumJsonBytes = 1024U * 1024U;
// Beast deadlines apply to asynchronous operations only. Drive the private
// context here to preserve the public blocking API without unbounded socket I/O.
template <typename Stream, typename Start>
BoostError run_io(Stream& stream, Start start) {
    auto& context = static_cast<asio::io_context&>(stream.get_executor().context());
    BoostError result;
    context.restart();
    start([&](BoostError error, auto...) { result = error; });
    context.run();
    return result;
}

struct JsonHttpResponse {
    unsigned status{0};
    std::string content_type;
    std::string body;
};

Error canceled_error() {
    return Error{ErrorDomain::canceled, ErrorCode::canceled, false,
                 "task HTTP operation was canceled"};
}

Error transport_error(const BoostError& error, ErrorDomain domain = ErrorDomain::transport) {
    return Error{domain, error == beast::error::timeout ? ErrorCode::timeout : ErrorCode::unavailable, true,
                 "task HTTP transport failure: " + error.message()};
}

bool canceled(const CancellationSignal* signal) {
    return signal != nullptr && signal->is_canceled();
}

std::string media_type(std::string_view value) {
    const auto separator = value.find(';');
    auto type = value.substr(0, separator);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
        type.remove_suffix(1);
    }
    std::size_t start = 0;
    while (start < type.size() && (type[start] == ' ' || type[start] == '\t')) {
        ++start;
    }
    std::string result{type.substr(start)};
    std::transform(result.begin(), result.end(), result.begin(), [](char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    });
    return result;
}

Error classify_http_error(unsigned status, std::string_view body,
                           bool artifact_endpoint = false) {
    std::string platform_code;
    const auto parsed = parse_http_error_response(body, status, artifact_endpoint);
    if (!parsed) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "device error response violates the fixed contract"};
    }
    if (parsed) {
        platform_code = parsed.value().code;
    }
    if (status == 401 && platform_code == "authentication_failed") {
        return Error{ErrorDomain::authentication, ErrorCode::permission_denied, false,
                     "device HTTP authentication or authorization was rejected",
                     std::move(platform_code)};
    }
    if (status == 401 || status == 409) {
        return Error{ErrorDomain::lease, ErrorCode::invalid_state, false,
                     "device task request was rejected due to task or lease state",
                     std::move(platform_code)};
    }
    if (status == 429 || status >= 500) {
        return Error{ErrorDomain::http, ErrorCode::unavailable, true,
                     "device task service is temporarily unavailable",
                     std::move(platform_code)};
    }
    if (status >= 300 && status < 400) {
        return Error{ErrorDomain::http, ErrorCode::unsupported, false,
                     "device task HTTP redirects are not accepted",
                     std::move(platform_code)};
    }
    return Error{ErrorDomain::http, ErrorCode::invalid_argument, false,
                 "device task HTTP request was rejected", std::move(platform_code)};
}

template <typename Stream>
Result<JsonHttpResponse> exchange_json(
    Stream& stream,
    const DeviceEndpoint& endpoint,
    http::verb method,
    std::string_view target,
    std::string_view authorization,
    std::string_view lease,
    std::string_view body,
    std::chrono::milliseconds timeout,
    const CancellationSignal* cancellation) {
    if (canceled(cancellation)) {
        return canceled_error();
    }
    http::request<http::string_body> request{method, std::string{target}, 11};
    request.set(http::field::host, endpoint.host_header);
    request.set(http::field::user_agent, "roc-vehicle-cpp/0.1");
    request.set(http::field::authorization, authorization);
    request.set(http::field::accept, "application/json");
    request.set(http::field::connection, "close");
    if (!lease.empty()) {
        request.set("X-Task-Lease", lease);
    }
    if (!body.empty()) {
        request.set(http::field::content_type, "application/json");
        request.body() = std::string{body};
        request.prepare_payload();
    } else if (method == http::verb::post) {
        request.content_length(0);
    }

    BoostError error;
    beast::get_lowest_layer(stream).expires_after(timeout);
    error = run_io(stream, [&](auto done) { http::async_write(stream, request, done); });
    if (error) {
        return transport_error(error);
    }
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaximumJsonBytes);
    beast::get_lowest_layer(stream).expires_after(timeout);
    error = run_io(stream, [&](auto done) { http::async_read(stream, buffer, parser, done); });
    if (error) {
        return transport_error(error);
    }
    if (canceled(cancellation)) {
        return canceled_error();
    }
    auto response = parser.release();
    return JsonHttpResponse{
        response.result_int(),
        std::string{response[http::field::content_type]},
        std::move(response.body())};
}

template <typename Stream>
Result<ArtifactHttpMetadata> exchange_artifact(
    Stream& stream,
    const DeviceEndpoint& endpoint,
    std::string_view target,
    std::string_view authorization,
    std::string_view lease,
    const ArtifactManifest& manifest,
    std::size_t chunk_size,
    const ArtifactChunkHandler& on_chunk,
    std::chrono::milliseconds timeout,
    const CancellationSignal* cancellation) {
    if (canceled(cancellation)) {
        return canceled_error();
    }
    http::request<http::empty_body> request{http::verb::get, std::string{target}, 11};
    request.set(http::field::host, endpoint.host_header);
    request.set(http::field::user_agent, "roc-vehicle-cpp/0.1");
    request.set(http::field::authorization, authorization);
    request.set(http::field::accept, manifest.content_type);
    request.set(http::field::connection, "close");
    request.set("X-Task-Lease", lease);

    BoostError error;
    beast::get_lowest_layer(stream).expires_after(timeout);
    error = run_io(stream, [&](auto done) { http::async_write(stream, request, done); });
    if (error) {
        return transport_error(error);
    }

    beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(std::max<std::uint64_t>(manifest.byte_size, kMaximumJsonBytes));
    beast::get_lowest_layer(stream).expires_after(timeout);
    error = run_io(stream, [&](auto done) { http::async_read_header(stream, buffer, parser, done); });
    if (error) {
        // A declared artifact length that already contradicts the frozen
        // manifest is not an ambiguous network truncation, even when Beast's
        // body budget rejects it during header parsing.
        // content_length() requires a completed header. In particular, an
        // operation timeout before any header arrives must remain retryable.
        if (error == http::error::body_limit && parser.get().result_int() == 200) {
            const auto declared = parser.content_length();
            if (declared && declared.value() != manifest.byte_size) {
                return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
            }
        }
        return transport_error(error);
    }

    const auto status = parser.get().result_int();
    ArtifactHttpMetadata metadata;
    metadata.content_type = std::string{parser.get()[http::field::content_type]};
    metadata.sha256 = std::string{parser.get()["X-Content-SHA256"]};
    const auto content_length = parser.content_length();
    if (content_length.has_value()) {
        metadata.content_length = content_length.value();
    }
    if (status == 200 && content_length && content_length.value() != manifest.byte_size) {
        return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    }

    const auto capacity = std::max<std::size_t>(1, chunk_size);
    std::vector<std::uint8_t> chunk(capacity);
    std::string error_body;
    while (!parser.is_done()) {
        if (canceled(cancellation)) {
            return canceled_error();
        }
        parser.get().body().data = chunk.data();
        parser.get().body().size = chunk.size();
        beast::get_lowest_layer(stream).expires_after(timeout);
        error = run_io(stream, [&](auto done) { http::async_read_some(stream, buffer, parser, done); });
        const auto used = chunk.size() - parser.get().body().size;
        if (used > 0) {
            if (status == 200) {
                if (metadata.bytes_received > manifest.byte_size ||
                    used > manifest.byte_size - metadata.bytes_received) {
                    return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
                }
                const auto handled = on_chunk(chunk.data(), used);
                if (!handled) {
                    return handled.error();
                }
                metadata.bytes_received += static_cast<std::uint64_t>(used);
            } else if (error_body.size() + used <= kMaximumJsonBytes) {
                error_body.append(reinterpret_cast<const char*>(chunk.data()), used);
            }
        }
        if (error == http::error::need_buffer) {
            error = {};
            continue;
        }
        if (error) {
            return transport_error(error);
        }
    }
    if (status != 200) {
        return classify_http_error(status, error_body, true);
    }
    if (metadata.bytes_received != manifest.byte_size) {
        return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    }
    return metadata;
}

template <typename Operation>
auto with_connection(const DeviceEndpoint& endpoint,
                     const TlsConfig& tls_config,
                     std::chrono::milliseconds timeout,
                     const CancellationSignal* cancellation,
                     Operation operation) -> decltype(operation(
                         std::declval<beast::tcp_stream&>())) {
    using Return = decltype(operation(std::declval<beast::tcp_stream&>()));
    if (canceled(cancellation)) {
        return Return{canceled_error()};
    }
    asio::io_context context;
    tcp::resolver resolver{context};
    BoostError error;
    const auto endpoints = resolver.resolve(endpoint.host, endpoint.port, error);
    if (error) {
        return Return{transport_error(error)};
    }
    if (!endpoint.tls) {
        beast::tcp_stream stream{context};
        stream.expires_after(timeout);
        error = run_io(stream, [&](auto done) { stream.async_connect(endpoints, done); });
        if (error) {
            return Return{transport_error(error)};
        }
        auto result = operation(stream);
        stream.socket().shutdown(tcp::socket::shutdown_both, error);
        return result;
    }

    ssl::context tls_context{ssl::context::tls_client};
    tls_context.set_options(ssl::context::default_workarounds, error);
    if (error) {
        return Return{transport_error(error, ErrorDomain::tls)};
    }
    if (tls_config.insecure_demo) {
        tls_context.set_verify_mode(ssl::verify_none, error);
    } else {
        tls_context.set_verify_mode(ssl::verify_peer, error);
        if (!error && tls_config.ca_file.has_value()) {
            tls_context.load_verify_file(tls_config.ca_file->string(), error);
        } else if (!error) {
            tls_context.set_default_verify_paths(error);
        }
    }
    if (error) {
        return Return{transport_error(error, ErrorDomain::tls)};
    }
    beast::ssl_stream<beast::tcp_stream> stream{context, tls_context};
    if (!tls_config.insecure_demo && tls_config.verify_hostname) {
        stream.set_verify_callback(ssl::host_name_verification(endpoint.host));
    }
    if (SSL_set_tlsext_host_name(stream.native_handle(), endpoint.host.c_str()) != 1) {
        return Return{Error{ErrorDomain::tls, ErrorCode::unavailable, false,
                            "unable to configure TLS server name"}};
    }
    beast::get_lowest_layer(stream).expires_after(timeout);
    error = run_io(stream, [&](auto done) { beast::get_lowest_layer(stream).async_connect(endpoints, done); });
    if (error) {
        return Return{transport_error(error)};
    }
    beast::get_lowest_layer(stream).expires_after(timeout);
    error = run_io(stream, [&](auto done) { stream.async_handshake(ssl::stream_base::client, done); });
    if (error) {
        return Return{transport_error(error, ErrorDomain::tls)};
    }
    auto result = operation(stream);
    beast::get_lowest_layer(stream).expires_after(timeout);
    static_cast<void>(run_io(stream, [&](auto done) { stream.async_shutdown(done); }));
    return result;
}

Error lease_mismatch() {
    return Error{ErrorDomain::lease, ErrorCode::invalid_argument, false,
                 "task lease does not match the requested generation"};
}

bool terminal_status(TaskExecutionState state) {
    return state == TaskExecutionState::delivered ||
           state == TaskExecutionState::failed;
}

bool terminal_reconciliation_code(std::string_view server_code) {
    return server_code == "lease_expired" ||
           server_code == "invalid_state" ||
           server_code == "invalid_transition" ||
           server_code == "invalid_lease" ||
           server_code == "event_conflict";
}

bool same_replay_material(const StatusEventRecord& requested,
                          const StatusEventRecord& stored) {
    return requested.event_id == stored.event_id &&
           requested.generation.task_id == stored.generation.task_id &&
           requested.generation.attempt == stored.generation.attempt &&
           requested.state == stored.state && requested.progress == stored.progress &&
           requested.error_code == stored.error_code &&
           requested.error_message == stored.error_message &&
           requested.payload_json == stored.payload_json &&
           (requested.request_sha256.empty() ||
            requested.request_sha256 == stored.request_sha256) &&
           requested.lease_token == stored.lease_token &&
           requested.lease_expires_at == stored.lease_expires_at &&
           requested.local_delivery_evidence == stored.local_delivery_evidence;
}

Result<void> require_active_lease(const AcceptedTask& accepted,
                                  DurableStateStore& state_store,
                                  bool establishing_binding = false) {
    if (accepted.attempt < 1 || accepted.lease_token.empty()) {
        return lease_mismatch();
    }
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (accepted.is_expired_at(now)) {
        static_cast<void>(state_store.mark_task_generation_expired(
            {accepted.task_id, accepted.attempt}));
        return Error{ErrorDomain::lease, ErrorCode::invalid_state, false,
                     "task lease has expired", "lease_expired"};
    }
    const auto generation = state_store.task_generation({accepted.task_id, accepted.attempt});
    if (!generation) return generation.error();
    if (generation.value().state != TaskExecutionState::accepted &&
        generation.value().state != TaskExecutionState::downloading &&
        generation.value().state != TaskExecutionState::delivering) return lease_mismatch();
    const auto binding = state_store.active_task_lease({accepted.task_id, accepted.attempt});
    if (!binding && (!establishing_binding || binding.error().code == ErrorCode::io_failure)) return binding.error();
    if (binding && (binding.value().lease_token != accepted.lease_token ||
                    binding.value().lease_expires_at != accepted.lease_expires_at)) return lease_mismatch();
    return {};
}

void expire_on_platform_rejection(const Error& error,
                                  const AcceptedTask& accepted,
                                  DurableStateStore& state_store) {
    if (error.domain == ErrorDomain::lease && error.server_code == "lease_expired") {
        static_cast<void>(state_store.mark_task_generation_expired(
            {accepted.task_id, accepted.attempt}));
    }
}

}  // namespace

struct DeviceTaskHttpClient::Impl {
    ClientConfig config;
    DeviceEndpoint endpoint;
    DeviceTokenProvider token_provider;

    Result<std::string> authorization() const {
        auto token = token_provider();
        if (!token) {
            return token.error();
        }
        return make_device_authorization(token.value());
    }

    Result<JsonHttpResponse> json_request(http::verb method,
                                          std::string_view target,
                                          std::string_view lease,
                                          std::string_view body,
                                          const CancellationSignal* cancellation) const {
        if (canceled(cancellation)) {
            return canceled_error();
        }
        const auto auth = authorization();
        if (!auth) {
            return auth.error();
        }
        return with_connection(
            endpoint, config.tls, config.http_operation_timeout, cancellation,
            [&](auto& stream) {
                return exchange_json(stream, endpoint, method, target, auth.value(),
                                     lease, body, config.http_operation_timeout, cancellation);
            });
    }
};

DeviceTaskHttpClient::DeviceTaskHttpClient(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DeviceTaskHttpClient::~DeviceTaskHttpClient() = default;

Result<std::unique_ptr<DeviceTaskHttpClient>> DeviceTaskHttpClient::create(
    ClientConfig config,
    DeviceTokenProvider token_provider) {
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
    auto impl = std::make_unique<Impl>();
    impl->config = std::move(config);
    impl->endpoint = endpoint.value();
    impl->token_provider = std::move(token_provider);
    return std::unique_ptr<DeviceTaskHttpClient>{
        new DeviceTaskHttpClient{std::move(impl)}};
}

Result<AcceptedTask> DeviceTaskHttpClient::accept_task(
    const TaskAvailableMessage& notification,
    DurableStateStore& state_store,
    const CancellationSignal* cancellation) const {
    const auto unresolved = state_store.has_unconfirmed_terminal_event(
        notification.task_id);
    if (!unresolved) {
        return unresolved.error();
    }
    if (unresolved.value()) {
        return Error{ErrorDomain::storage, ErrorCode::invalid_state, false,
                     "task has an unconfirmed terminal outbox event",
                     "terminal_outbox_pending"};
    }
    const auto missing_active = state_store.has_unrecoverable_active_generation(notification.task_id);
    if (!missing_active) return missing_active.error();
    if (missing_active.value()) return Error{ErrorDomain::storage, ErrorCode::invalid_state, false,
        "active generation lacks original recovery material; accept is forbidden",
        "active_task_recovery_missing"};
    const auto keys = state_store.active_task_keys();
    if (!keys) return keys.error();
    for (const auto& key : keys.value()) {
        if (key.task_id != notification.task_id) continue;
        const auto lease = state_store.active_task_lease(key);
        const auto generation = state_store.task_generation(key);
        if (!lease || !generation) return lease_mismatch();
        if (static_cast<std::int64_t>(std::time(nullptr)) > lease.value().lease_expires_at) {
            const auto expired = state_store.mark_task_generation_expired(key);
            if (!expired) return expired.error();
        } else if (generation.value().state != TaskExecutionState::accepted) {
            return Error{ErrorDomain::storage, ErrorCode::invalid_state, false,
                         "active task must recover its original lease before accept",
                         "active_task_requires_recovery"};
        }
    }
    const auto target = task_accept_target(notification.task_id);
    if (target.empty()) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "task notification has an invalid task_id"};
    }
    const auto response = impl_->json_request(http::verb::post, target, {}, {}, cancellation);
    if (!response) {
        return response.error();
    }
    if (response.value().status != 200) {
        return classify_http_error(response.value().status, response.value().body);
    }
    if (media_type(response.value().content_type) != "application/json") {
        return Error{ErrorDomain::http, ErrorCode::invalid_argument, false,
                     "accept response Content-Type is not application/json"};
    }
    const auto accepted = parse_accept_response(response.value().body, notification.task_id);
    if (!accepted) {
        return accepted.error();
    }
    if (accepted.value().batch_id != notification.batch_id)
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "accept response batch identity differs"};
    if (!impl_->config.expected_vehicle_id.empty() &&
        accepted.value().vehicle_id != impl_->config.expected_vehicle_id)
        return Error{ErrorDomain::authentication, ErrorCode::permission_denied, false,
                     "accept vehicle identity differs from pinned identity", "vehicle_identity_mismatch"};
    const auto registered = state_store.register_task_generation(
        {accepted.value().task_id, accepted.value().attempt},
        accepted.value().progress_floor);
    if (!registered) {
        return registered.error();
    }
    if (registered.value().state == TaskExecutionState::delivered ||
        registered.value().state == TaskExecutionState::failed ||
        registered.value().state == TaskExecutionState::reconciliation_required ||
        registered.value().state == TaskExecutionState::expired ||
        registered.value().state == TaskExecutionState::canceled) {
        return Error{ErrorDomain::storage, ErrorCode::invalid_state, false,
                     "accepted task generation is already terminal",
                     "generation_not_active"};
    }
    const auto active = require_active_lease(accepted.value(), state_store, true);
    if (!active) {
        return active.error();
    }
    auto stored_notification = notification;
    stored_notification.max_attempts = accepted.value().max_attempts;
    const auto saved = state_store.persist_active_task_lease({
        stored_notification, {accepted.value().task_id, accepted.value().attempt},
        accepted.value().lease_token, accepted.value().lease_expires_at,
        accepted.value().progress_floor});
    if (!saved) return saved.error();
    return accepted.value();
}

Result<ArtifactManifest> DeviceTaskHttpClient::read_manifest(
    const TaskAvailableMessage& notification,
    const AcceptedTask& accepted,
    DurableStateStore& state_store,
    const CancellationSignal* cancellation) const {
    if (accepted.task_id != notification.task_id) {
        return lease_mismatch();
    }
    const auto active = require_active_lease(accepted, state_store);
    if (!active) {
        return active.error();
    }
    const auto target = task_manifest_target(notification.task_id);
    const auto response = impl_->json_request(http::verb::get, target,
                                              accepted.lease_token, {}, cancellation);
    if (!response) {
        return response.error();
    }
    if (response.value().status != 200) {
        auto error = classify_http_error(response.value().status, response.value().body);
        expire_on_platform_rejection(error, accepted, state_store);
        return error;
    }
    if (media_type(response.value().content_type) != "application/json") {
        return Error{ErrorDomain::http, ErrorCode::invalid_argument, false,
                     "manifest response Content-Type is not application/json"};
    }
    return parse_manifest_response(response.value().body, notification,
                                   impl_->config.artifact_limits);
}

Result<ArtifactHttpMetadata> DeviceTaskHttpClient::download_artifact(
    const ArtifactManifest& manifest,
    const AcceptedTask& accepted,
    DurableStateStore& state_store,
    ArtifactChunkHandler on_chunk,
    const CancellationSignal* cancellation) const {
    if (manifest.package_version != 0 || accepted.task_id != manifest.task_id || !on_chunk ||
        manifest.artifact_url != task_artifact_target(manifest.task_id)) {
        return lease_mismatch();
    }
    const auto active = require_active_lease(accepted, state_store);
    if (!active) {
        return active.error();
    }
    if (canceled(cancellation)) {
        return canceled_error();
    }
    const auto auth = impl_->authorization();
    if (!auth) {
        return auth.error();
    }
    auto result = with_connection(
        impl_->endpoint, impl_->config.tls, impl_->config.http_operation_timeout, cancellation,
        [&](auto& stream) {
            return exchange_artifact(
                stream, impl_->endpoint, manifest.artifact_url, auth.value(),
                accepted.lease_token, manifest,
                impl_->config.artifact_limits.stream_chunk_bytes, on_chunk,
                impl_->config.http_operation_timeout, cancellation);
        });
    if (!result) {
        expire_on_platform_rejection(result.error(), accepted, state_store);
    }
    return result;
}

Result<StatusResponse> DeviceTaskHttpClient::report_status(
    const StatusEventRecord& event,
    const AcceptedTask& accepted,
    DurableStateStore& state_store,
    const CancellationSignal* cancellation) const {
    if (accepted.task_id != event.generation.task_id ||
        accepted.attempt != event.generation.attempt ||
        accepted.lease_token != event.lease_token ||
        accepted.lease_expires_at != event.lease_expires_at) {
        return lease_mismatch();
    }
    return replay_status_event(event, state_store, cancellation);
}

Result<StatusResponse> DeviceTaskHttpClient::replay_status_event(
    const StatusEventRecord& event,
    DurableStateStore& state_store,
    const CancellationSignal* cancellation) const {
    const auto persisted = state_store.status_event(event.event_id);
    if (!persisted) {
        return persisted.error();
    }
    const auto& stored = persisted.value();
    if (stored.disposition != StatusEventDisposition::pending ||
        !same_replay_material(event, stored)) {
        return Error{ErrorDomain::storage, ErrorCode::integrity_failure, false,
                     "status replay does not match the pending persisted event"};
    }
    const auto digest = sha256_hex(stored.payload_json);
    if (!digest || digest.value() != stored.request_sha256) {
        return Error{ErrorDomain::storage, ErrorCode::integrity_failure, false,
                     "persisted status request digest verification failed"};
    }
    const auto canonical = make_status_request(stored);
    if (!canonical) {
        return canonical.error();
    }
    if (stored.payload_json != canonical.value()) {
        return Error{ErrorDomain::storage, ErrorCode::integrity_failure, false,
                     "persisted status payload does not match its event fields"};
    }
    if (!terminal_status(stored.state)) {
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        if (now > stored.lease_expires_at) {
            static_cast<void>(state_store.mark_task_generation_expired(
                stored.generation));
            return Error{ErrorDomain::lease, ErrorCode::invalid_state, false,
                         "task lease has expired", "lease_expired"};
        }
    }
    const auto response = impl_->json_request(
        http::verb::post, task_status_target(stored.generation.task_id),
        stored.lease_token, stored.payload_json, cancellation);
    if (!response) {
        return response.error();
    }
    if (response.value().status != 200) {
        auto error = classify_http_error(response.value().status, response.value().body);
        if (terminal_status(stored.state) &&
            terminal_reconciliation_code(error.server_code)) {
            const auto reconciled =
                state_store.mark_status_event_reconciliation_required(
                    stored.event_id, error.server_code);
            if (!reconciled) {
                return reconciled.error();
            }
        } else if (!terminal_status(stored.state) &&
                   error.domain == ErrorDomain::lease &&
                   error.server_code == "lease_expired") {
            static_cast<void>(state_store.mark_task_generation_expired(
                stored.generation));
        }
        return error;
    }
    if (media_type(response.value().content_type) != "application/json") {
        return Error{ErrorDomain::http, ErrorCode::invalid_argument, false,
                     "status response Content-Type is not application/json"};
    }
    const auto parsed = parse_status_response(response.value().body, stored.generation.task_id);
    if (!parsed) {
        return parsed.error();
    }
    const auto acknowledged = state_store.acknowledge_status_event(stored.event_id);
    if (!acknowledged) {
        return acknowledged.error();
    }
    return parsed.value();
}

Result<AcceptedTask> DeviceTaskHttpClient::recover_accepted_task(
    const TaskGenerationKey& key, DurableStateStore& state_store) const {
    const auto blocked = state_store.has_unconfirmed_terminal_event(key.task_id);
    if (!blocked) return blocked.error();
    if (blocked.value()) return Error{ErrorDomain::storage, ErrorCode::invalid_state, false,
        "terminal replay or reconciliation must finish before active recovery"};
    const auto saved = state_store.active_task_lease(key);
    if (!saved) return saved.error();
    AcceptedTask result;
    result.replayed = true;
    result.lease_token = saved.value().lease_token;
    result.lease_expires_at = saved.value().lease_expires_at;
    result.task_id = key.task_id;
    result.attempt = key.attempt;
    result.max_attempts = saved.value().notification.max_attempts;
    result.progress_floor = saved.value().progress_floor;
    result.batch_id = saved.value().notification.batch_id;
    const auto active = require_active_lease(result, state_store);
    if (!active) return active.error();
    return result;
}

Result<ArtifactHttpMetadata> DeviceTaskHttpClient::download_artifact_file(
    const ArtifactManifest& package, std::string_view file_id,
    const AcceptedTask& accepted, DurableStateStore& state_store,
    ArtifactChunkHandler on_chunk, const CancellationSignal* cancellation) const {
    if (package.package_version != 2 || package.resource_type != ResourceType::map ||
        package.task_id != accepted.task_id || !on_chunk) return lease_mismatch();
    const auto file = std::find_if(package.files.begin(), package.files.end(),
        [&](const ArtifactFile& item) { return item.id == file_id; });
    if (file == package.files.end() ||
        file->artifact_url != task_artifact_file_target(package.task_id, file_id))
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "package file identity differs"};
    const auto active = require_active_lease(accepted, state_store);
    if (!active) return active.error();
    const auto auth = impl_->authorization();
    if (!auth) return auth.error();
    ArtifactManifest manifest;
    manifest.content_type = file->content_type;
    manifest.byte_size = file->byte_size;
    manifest.sha256 = file->sha256;
    auto result = with_connection(impl_->endpoint, impl_->config.tls, impl_->config.http_operation_timeout, cancellation,
        [&](auto& stream) { return exchange_artifact(stream, impl_->endpoint,
            file->artifact_url, auth.value(), accepted.lease_token, manifest,
            impl_->config.artifact_limits.stream_chunk_bytes, on_chunk, impl_->config.http_operation_timeout, cancellation); });
    if (!result) expire_on_platform_rejection(result.error(), accepted, state_store);
    return result;
}

}  // namespace roc::vehicle
