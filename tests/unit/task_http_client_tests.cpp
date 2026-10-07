#include "roc/vehicle/task_http_client.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

constexpr auto kTaskId = "123e4567-e89b-12d3-a456-426614174010";
constexpr auto kLease = "opaque-loopback-lease";
constexpr auto kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kArtifact = "artifact-data";

struct ServerResult {
    std::mutex mutex;
    std::string failure;
    int requests{0};
    std::vector<std::string> bodies;
    std::vector<std::string> leases;
};

void fail_server(ServerResult& result, std::string message) {
    std::lock_guard<std::mutex> lock{result.mutex};
    if (result.failure.empty()) {
        result.failure = std::move(message);
    }
}

template <typename Body>
void write_response(tcp::socket& socket,
                    http::response<Body>& response,
                    boost::system::error_code& error) {
    response.version(11);
    response.keep_alive(false);
    response.prepare_payload();
    http::write(socket, response, error);
}

std::thread run_success_server(tcp::acceptor& acceptor, ServerResult& result) {
    return std::thread([&acceptor, &result] {
        for (int index = 0; index < 4; ++index) {
            boost::system::error_code error;
            tcp::socket socket{acceptor.get_executor()};
            acceptor.accept(socket, error);
            if (error) {
                fail_server(result, "accept failed: " + error.message());
                return;
            }
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request, error);
            if (error) {
                fail_server(result, "request read failed: " + error.message());
                return;
            }
            const std::string prefix = std::string{"/api/device/tasks/"} + kTaskId;
            const bool auth_ok = request[http::field::authorization] ==
                                 "Device loopback-test-token";
            const bool lease_expected = index != 0;
            const bool lease_ok = lease_expected
                                      ? request["X-Task-Lease"] == kLease
                                      : request["X-Task-Lease"].empty();
            if (!auth_ok || !lease_ok) {
                fail_server(result, "authorization or lease header mismatch");
                return;
            }

            http::response<http::string_body> response{http::status::ok, 11};
            if (index == 0) {
                if (request.method() != http::verb::post ||
                    request.target() != prefix + "/accept") {
                    fail_server(result, "accept request mismatch");
                    return;
                }
                response.set(http::field::content_type, "application/json");
                response.body() = R"JSON({
                  "ok":true,"replayed":false,
                  "lease_token":"opaque-loopback-lease",
                  "lease_expires_at":4102444800,
                  "task":{"id":"123e4567-e89b-12d3-a456-426614174010",
                          "state":"accepted","attempt":1,"max_attempts":3,
                          "progress":20,"batch_id":"123e4567-e89b-12d3-a456-426614174011",
          "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
          "error_code":null,"error_message":null,"offered_at":null,
          "accepted_at":null,"delivered_at":null,
          "updated_at":"2026-10-04T00:00:00Z"}
                })JSON";
            } else if (index == 1) {
                if (request.method() != http::verb::get ||
                    request.target() != prefix + "/manifest") {
                    fail_server(result, "manifest request mismatch");
                    return;
                }
                response.set(http::field::content_type, "application/json; charset=utf-8");
                response.body() = R"JSON({
                  "ok":true,"manifest":{
                    "task_id":"123e4567-e89b-12d3-a456-426614174010",
                    "batch_id":"123e4567-e89b-12d3-a456-426614174011",
                    "project_id":"123e4567-e89b-12d3-a456-426614174012",
                    "resource_type":"map",
                    "resource_revision_id":"123e4567-e89b-12d3-a456-426614174013",
                    "map_id":"123e4567-e89b-12d3-a456-426614174014",
                    "content_type":"image/png","byte_size":13,
                    "sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                    "version":1,
                    "artifact_url":"/api/device/tasks/123e4567-e89b-12d3-a456-426614174010/artifact"
                  }
                })JSON";
            } else if (index == 2) {
                if (request.method() != http::verb::get ||
                    request.target() != prefix + "/artifact" ||
                    !request[http::field::range].empty()) {
                    fail_server(result, "artifact request mismatch");
                    return;
                }
                response.set(http::field::content_type, "image/png");
                response.set("X-Content-SHA256", kSha);
                response.body() = std::string{kArtifact};
            } else {
                if (request.method() != http::verb::post ||
                    request.target() != prefix + "/status" ||
                    request.body().find("\"event_id\"") == std::string::npos ||
                    request.body().find("\"progress\":20") == std::string::npos) {
                    fail_server(result, "status request mismatch");
                    return;
                }
                response.set(http::field::content_type, "application/json");
                response.body() = R"JSON({"ok":true,"replayed":true,
          "project_id":"123e4567-e89b-12d3-a456-426614174012",
          "task":{"id":"123e4567-e89b-12d3-a456-426614174010",
          "state":"delivered","attempt":2,"max_attempts":3,"progress":100,"batch_id":"123e4567-e89b-12d3-a456-426614174011",
          "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
          "error_code":null,"error_message":null,"offered_at":null,
          "accepted_at":null,"delivered_at":null,
          "updated_at":"2026-10-04T00:00:00Z"}})JSON";
            }
            write_response(socket, response, error);
            if (error) {
                fail_server(result, "response write failed: " + error.message());
                return;
            }
            {
                std::lock_guard<std::mutex> lock{result.mutex};
                ++result.requests;
            }
        }
    });
}

roc::vehicle::TaskAvailableMessage notification() {
    return {{"123e4567-e89b-12d3-a456-426614174000", 1,
             "2026-09-26T12:00:00Z"},
            kTaskId,
            "123e4567-e89b-12d3-a456-426614174011",
            "123e4567-e89b-12d3-a456-426614174012",
            roc::vehicle::ResourceType::map,
            "123e4567-e89b-12d3-a456-426614174013",
            0,
            3};
}

roc::vehicle::ClientConfig config_for(std::uint16_t port,
                                      const std::filesystem::path& root) {
    roc::vehicle::ClientConfig config;
    config.base_url = "http://127.0.0.1:" + std::to_string(port);
    config.state_directory = root / "state";
    config.artifact_directory = root / "artifacts";
    config.artifact_limits.stream_chunk_bytes = 4;
    return config;
}

bool write_self_signed_certificate(const std::filesystem::path& certificate,
                                   const std::filesystem::path& private_key) {
    using KeyContext = std::unique_ptr<EVP_PKEY_CTX,
                                       decltype(&EVP_PKEY_CTX_free)>;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
    KeyContext context{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                       &EVP_PKEY_CTX_free};
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0) {
        return false;
    }
    EVP_PKEY* generated = nullptr;
    if (EVP_PKEY_keygen(context.get(), &generated) <= 0) {
        return false;
    }
    Key key{generated, &EVP_PKEY_free};
    Certificate x509{X509_new(), &X509_free};
    if (!x509 || X509_set_version(x509.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(x509.get()), 1) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(x509.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(x509.get()), 3600) == nullptr ||
        X509_set_pubkey(x509.get(), key.get()) != 1) {
        return false;
    }
    auto* name = X509_get_subject_name(x509.get());
    const unsigned char common_name[] = "localhost";
    if (name == nullptr ||
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC, common_name, -1, -1, 0) != 1 ||
        X509_set_issuer_name(x509.get(), name) != 1 ||
        X509_sign(x509.get(), key.get(), EVP_sha256()) <= 0) {
        return false;
    }
    using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    File certificate_file{std::fopen(certificate.string().c_str(), "wb"),
                          &std::fclose};
    if (!certificate_file ||
        PEM_write_X509(certificate_file.get(), x509.get()) != 1) {
        return false;
    }
    File key_file{std::fopen(private_key.string().c_str(), "wb"),
                  &std::fclose};
    return key_file &&
           PEM_write_PrivateKey(key_file.get(), key.get(), nullptr, nullptr, 0,
                                nullptr, nullptr) == 1;
}

roc::vehicle::StatusEventRecord make_status_event(
    std::string event_id,
    roc::vehicle::TaskExecutionState state,
    std::int64_t progress,
    std::int64_t lease_expires_at = 1) {
    roc::vehicle::StatusEventRecord event{
        std::move(event_id), {kTaskId, 1}, state, progress, {}, {}, {}, {},
        kLease, lease_expires_at,
        R"JSON({"receiver":"file","sha256":"verified","fsync":true})JSON"};
    const auto payload = roc::vehicle::make_status_request(event);
    EXPECT(payload);
    if (payload) {
        event.payload_json = payload.value();
    }
    return event;
}

roc::vehicle::StatusEventRecord seed_terminal_outbox(
    roc::vehicle::DurableStateStore& store,
    std::string terminal_event_id,
    std::int64_t lease_expires_at = 1) {
    EXPECT(store.register_task_generation({kTaskId, 1}, 20));
    auto downloading = make_status_event(
        "123e4567-e89b-12d3-a456-426614174030",
        roc::vehicle::TaskExecutionState::downloading, 20, lease_expires_at);
    EXPECT(store.persist_status_event(downloading));
    EXPECT(store.acknowledge_status_event(downloading.event_id));
    auto delivering = make_status_event(
        "123e4567-e89b-12d3-a456-426614174031",
        roc::vehicle::TaskExecutionState::delivering, 80, lease_expires_at);
    EXPECT(store.persist_status_event(delivering));
    EXPECT(store.acknowledge_status_event(delivering.event_id));
    auto delivered = make_status_event(
        std::move(terminal_event_id),
        roc::vehicle::TaskExecutionState::delivered, 100, lease_expires_at);
    EXPECT(store.persist_status_event(delivered));
    return delivered;
}

void test_complete_loopback_flow() {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    ServerResult server_result;
    auto server = run_success_server(acceptor, server_result);

    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-task-http-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto config = config_for(acceptor.local_endpoint().port(), root);
    const auto store_result = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    EXPECT(store_result);
    auto client_result = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(client_result);
    if (!store_result || !client_result) {
        acceptor.close();
        server.join();
        return;
    }
    auto& store = *store_result.value();
    auto& client = *client_result.value();
    const auto task = notification();
    const auto accepted = client.accept_task(task, store);
    EXPECT(accepted);
    if (!accepted) {
        acceptor.close();
        server.join();
        return;
    }
    EXPECT(accepted.value().progress_floor == 20);
    const auto manifest = client.read_manifest(task, accepted.value(), store);
    EXPECT(manifest);
    std::string artifact;
    if (manifest) {
        const auto downloaded = client.download_artifact(
            manifest.value(), accepted.value(), store,
            [&artifact](const std::uint8_t* data, std::size_t size) {
                artifact.append(reinterpret_cast<const char*>(data), size);
                return roc::vehicle::Result<void>{};
            });
        EXPECT(downloaded);
        if (downloaded) {
            EXPECT(downloaded.value().bytes_received == kArtifact.size());
            EXPECT(downloaded.value().content_length.has_value());
        }
    }
    EXPECT(artifact == kArtifact);

    roc::vehicle::StatusEventRecord event{
        "123e4567-e89b-12d3-a456-426614174020", {kTaskId, 1},
        roc::vehicle::TaskExecutionState::downloading, 20, {}, {}, {}, {},
        accepted.value().lease_token, accepted.value().lease_expires_at,
        R"JSON({"phase":"download-started"})JSON"};
    const auto payload = roc::vehicle::make_status_request(event);
    EXPECT(payload);
    if (payload) {
        event.payload_json = payload.value();
    }
    EXPECT(store.persist_status_event(event));
    const auto status = client.report_status(event, accepted.value(), store);
    EXPECT(status);
    if (status) {
        EXPECT(status.value().replayed);
    }
    const auto pending = store.pending_status_events();
    EXPECT(pending);
    if (pending) {
        EXPECT(pending.value().empty());
    }

    server.join();
    {
        std::lock_guard<std::mutex> lock{server_result.mutex};
        EXPECT(server_result.failure.empty());
        EXPECT(server_result.requests == 4);
    }
    std::filesystem::remove_all(root, ignored);
}

std::thread run_expiry_server(tcp::acceptor& acceptor, ServerResult& result) {
    return std::thread([&acceptor, &result] {
        for (int index = 0; index < 2; ++index) {
            boost::system::error_code error;
            tcp::socket socket{acceptor.get_executor()};
            acceptor.accept(socket, error);
            if (error) {
                fail_server(result, "expiry accept failed");
                return;
            }
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request, error);
            if (error) {
                fail_server(result, "expiry request read failed");
                return;
            }
            http::response<http::string_body> response;
            if (index == 0) {
                response.result(http::status::conflict);
                response.set(http::field::content_type, "application/json");
                response.body() = R"JSON({"ok":false,"code":"lease_expired","message":"expired"})JSON";
            } else {
                response.result(http::status::ok);
                response.set(http::field::content_type, "application/json");
                response.body() = R"JSON({
                  "ok":true,"replayed":false,
                  "lease_token":"opaque-loopback-lease",
                  "lease_expires_at":4102444800,
                  "task":{"id":"123e4567-e89b-12d3-a456-426614174010",
                          "state":"accepted","attempt":2,"max_attempts":3,
                          "progress":20,"batch_id":"123e4567-e89b-12d3-a456-426614174011",
          "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
          "error_code":null,"error_message":null,"offered_at":null,
          "accepted_at":null,"delivered_at":null,
          "updated_at":"2026-10-04T00:00:00Z"}
                })JSON";
            }
            write_response(socket, response, error);
            if (error) {
                fail_server(result, "expiry response write failed");
                return;
            }
            std::lock_guard<std::mutex> lock{result.mutex};
            ++result.requests;
        }
    });
}

void test_lease_expiry_and_new_attempt() {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    ServerResult server_result;
    auto server = run_expiry_server(acceptor, server_result);
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-task-http-expiry-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto config = config_for(acceptor.local_endpoint().port(), root);
    auto store_result = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    auto client_result = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(store_result);
    EXPECT(client_result);
    if (!store_result || !client_result) {
        acceptor.close();
        server.join();
        return;
    }
    auto& store = *store_result.value();
    EXPECT(store.register_task_generation({kTaskId, 1}, 20));
    EXPECT(store.persist_active_task_lease({notification(), {kTaskId,1}, kLease, 4102444800, 20}));
    roc::vehicle::StatusEventRecord pending_event{
        "123e4567-e89b-12d3-a456-426614174021", {kTaskId, 1},
        roc::vehicle::TaskExecutionState::downloading, 20, {}, {}, {}, {},
        kLease, 4102444800, R"JSON({"phase":"download-started"})JSON"};
    const auto pending_payload = roc::vehicle::make_status_request(pending_event);
    EXPECT(pending_payload);
    if (pending_payload) {
        pending_event.payload_json = pending_payload.value();
        EXPECT(store.persist_status_event(pending_event));
    }
    roc::vehicle::AcceptedTask old{false, kLease, 4102444800, kTaskId, 1, 3, 20};
    const auto manifest = client_result.value()->read_manifest(
        notification(), old, store);
    EXPECT(!manifest);
    if (!manifest) {
        EXPECT(manifest.error().server_code == "lease_expired");
        EXPECT(!manifest.error().retryable);
    }
    const auto expired = store.task_generation({kTaskId, 1});
    EXPECT(expired);
    if (expired) {
        EXPECT(expired.value().state == roc::vehicle::TaskExecutionState::expired);
    }
    const auto pending_after_expiry = store.pending_status_events();
    EXPECT(pending_after_expiry);
    if (pending_after_expiry) {
        EXPECT(pending_after_expiry.value().empty());
    }
    const auto accepted = client_result.value()->accept_task(notification(), store);
    EXPECT(accepted);
    if (accepted) {
        EXPECT(accepted.value().attempt == 2);
        const auto next = store.task_generation({kTaskId, 2});
        EXPECT(next);
        if (next) {
            EXPECT(next.value().state == roc::vehicle::TaskExecutionState::accepted);
        }
    }
    server.join();
    {
        std::lock_guard<std::mutex> lock{server_result.mutex};
        EXPECT(server_result.failure.empty());
        EXPECT(server_result.requests == 2);
    }
    std::filesystem::remove_all(root, ignored);
}

void test_untrusted_terminal_response_retains_outbox(std::string body, unsigned status) {
    asio::io_context context;
    tcp::acceptor acceptor{context, {asio::ip::address_v4::loopback(), 0}};
    auto server = std::thread([&] {
        tcp::socket socket{context};
        acceptor.accept(socket);
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request);
        http::response<http::string_body> response;
        response.result(status);
        response.set(http::field::content_type, "application/json");
        response.body() = body;
        boost::system::error_code error;
        write_response(socket, response, error);
    });
    const auto root = std::filesystem::temp_directory_path()/"roc-vehicle-untrusted-terminal";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto config = config_for(acceptor.local_endpoint().port(), root);
    auto store = std::move(roc::vehicle::DurableStateStore::open(
        config.state_directory/"state.sqlite3").value());
    auto client = std::move(roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; }).value());
    auto delivered = seed_terminal_outbox(*store, "123e4567-e89b-12d3-a456-426614174039");
    EXPECT(!client->replay_status_event(delivered, *store));
    const auto retained = store->status_event(delivered.event_id).value();
    EXPECT(retained.disposition == roc::vehicle::StatusEventDisposition::pending);
    EXPECT(retained.payload_json == delivered.payload_json);
    EXPECT(retained.lease_token == delivered.lease_token);
    EXPECT(retained.local_delivery_evidence == delivered.local_delivery_evidence);
    EXPECT(store->has_unconfirmed_terminal_event(kTaskId).value());
    server.join();
    store.reset();
    std::filesystem::remove_all(root, ignored);
}

void test_package_file_http_path() {
    asio::io_context context;
    tcp::acceptor acceptor{context, {asio::ip::address_v4::loopback(), 0}};
    ServerResult result;
    const std::string file_id = "123e4567-e89b-12d3-a456-426614174040";
    auto server = std::thread([&] {
        boost::system::error_code error;
        tcp::socket socket{context}; acceptor.accept(socket, error);
        beast::flat_buffer buffer; http::request<http::string_body> request;
        http::read(socket, buffer, request, error);
        if (error || request.target() != roc::vehicle::task_artifact_file_target(kTaskId,file_id) ||
            request[http::field::authorization] != "Device loopback-test-token" ||
            request["X-Task-Lease"] != kLease || !request[http::field::range].empty())
            fail_server(result,"package file request mismatch");
        http::response<http::string_body> response{http::status::ok,11};
        response.set(http::field::content_type,"application/octet-stream");
        response.set("X-Content-SHA256",kSha); response.body()=std::string{kArtifact};
        write_response(socket,response,error);
    });
    const auto root = std::filesystem::temp_directory_path()/"roc-vehicle-package-http";
    std::error_code ignored; std::filesystem::remove_all(root,ignored);
    const auto config = config_for(acceptor.local_endpoint().port(),root);
    auto store = std::move(roc::vehicle::DurableStateStore::open(config.state_directory/"state.sqlite3").value());
    auto client = std::move(roc::vehicle::DeviceTaskHttpClient::create(config,
        [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; }).value());
    EXPECT(store->register_task_generation({kTaskId,1},0));
    EXPECT(store->persist_active_task_lease({notification(), {kTaskId,1}, kLease, 4102444800, 0}));
    roc::vehicle::AcceptedTask accepted;
    accepted.task_id=kTaskId; accepted.attempt=1;
    accepted.lease_token=kLease; accepted.lease_expires_at=4102444800;
    roc::vehicle::ArtifactManifest package;
    package.task_id=kTaskId; package.resource_type=roc::vehicle::ResourceType::map;
    package.package_version=2; package.files.push_back({file_id,"pgm","map.pgm",
        "application/octet-stream",kArtifact.size(),kSha,
        roc::vehicle::task_artifact_file_target(kTaskId,file_id)});
    const auto collect=[](const std::uint8_t*,std::size_t) { return roc::vehicle::Result<void>{}; };
    EXPECT(!client->download_artifact(package,accepted,*store,collect));
    EXPECT(!client->download_artifact_file(package,"invalid",accepted,*store,collect));
    std::string received;
    const auto downloaded=client->download_artifact_file(package,file_id,accepted,*store,
        [&](const std::uint8_t* bytes,std::size_t size) {
            received.append(reinterpret_cast<const char*>(bytes),size);
            return roc::vehicle::Result<void>{};
        });
    EXPECT(downloaded); EXPECT(received==kArtifact);
    server.join(); EXPECT(result.failure.empty());
    store.reset(); std::filesystem::remove_all(root,ignored);
}

void test_accepted_live_repeat_preserves_lease() {
    asio::io_context context;
    tcp::acceptor acceptor{context,{asio::ip::address_v4::loopback(),0}};
    ServerResult result;
    auto server=std::thread([&] {
        for (int index=0;index<2;++index) {
            boost::system::error_code error;
            tcp::socket socket{context}; acceptor.accept(socket,error);
            beast::flat_buffer buffer; http::request<http::string_body> request;
            http::read(socket,buffer,request,error);
            if (error || request.target()!=roc::vehicle::task_accept_target(kTaskId) ||
                !request["X-Task-Lease"].empty()) fail_server(result,"repeat accept request mismatch");
            http::response<http::string_body> response{http::status::ok,11};
            response.set(http::field::content_type,"application/json");
            response.body()=std::string{R"({"ok":true,"replayed":)"}+
                (index==0?"false":"true")+R"(,"lease_token":"opaque-loopback-lease",
                "lease_expires_at":4102444800,"task":{
                "id":"123e4567-e89b-12d3-a456-426614174010",
                "batch_id":"123e4567-e89b-12d3-a456-426614174011",
                "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
                "state":"accepted","attempt":1,"max_attempts":3,"progress":20,
                "error_code":null,"error_message":null,"offered_at":null,
                "accepted_at":null,"delivered_at":null,"updated_at":"2026-10-04T00:00:00Z"}})";
            write_response(socket,response,error);
        }
    });
    const auto root=std::filesystem::temp_directory_path()/"roc-vehicle-repeat-accept";
    std::error_code ignored; std::filesystem::remove_all(root,ignored);
    const auto config=config_for(acceptor.local_endpoint().port(),root);
    auto store=std::move(roc::vehicle::DurableStateStore::open(config.state_directory/"state.sqlite3").value());
    auto client=std::move(roc::vehicle::DeviceTaskHttpClient::create(config,
        [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; }).value());
    const auto first=client->accept_task(notification(),*store);
    EXPECT(first);
    const auto repeated=client->accept_task(notification(),*store);
    EXPECT(repeated);
    if(first && repeated) {
        EXPECT(repeated.value().replayed);
        EXPECT(first.value().lease_token==repeated.value().lease_token);
        EXPECT(first.value().attempt==repeated.value().attempt);
        EXPECT(store->active_task_keys().value().size()==1);
    }
    server.join(); EXPECT(result.failure.empty());
    store.reset(); std::filesystem::remove_all(root,ignored);
}

std::thread run_lost_terminal_response_server(tcp::acceptor& acceptor,
                                              ServerResult& result) {
    return std::thread([&acceptor, &result] {
        for (int index = 0; index < 2; ++index) {
            boost::system::error_code error;
            tcp::socket socket{acceptor.get_executor()};
            acceptor.accept(socket, error);
            if (error) {
                fail_server(result, "terminal replay accept failed");
                return;
            }
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request, error);
            if (error) {
                fail_server(result, "terminal replay request read failed");
                return;
            }
            if (request.method() != http::verb::post ||
                request.target() != std::string{"/api/device/tasks/"} +
                                        kTaskId + "/status") {
                fail_server(result, "terminal replay target mismatch");
                return;
            }
            {
                std::lock_guard<std::mutex> lock{result.mutex};
                result.bodies.push_back(request.body());
                result.leases.emplace_back(request["X-Task-Lease"]);
                ++result.requests;
            }
            if (index == 0) {
                socket.close(error);
                continue;
            }
            http::response<http::string_body> response{http::status::ok, 11};
            response.set(http::field::content_type, "application/json");
            response.body() = R"JSON({"ok":true,"replayed":true,
          "project_id":"123e4567-e89b-12d3-a456-426614174012",
          "task":{"id":"123e4567-e89b-12d3-a456-426614174010",
          "state":"delivered","attempt":2,"max_attempts":3,"progress":100,"batch_id":"123e4567-e89b-12d3-a456-426614174011",
          "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
          "error_code":null,"error_message":null,"offered_at":null,
          "accepted_at":null,"delivered_at":null,
          "updated_at":"2026-10-04T00:00:00Z"}})JSON";
            write_response(socket, response, error);
            if (error) {
                fail_server(result, "terminal replay response write failed");
                return;
            }
        }
    });
}

void test_terminal_response_loss_replays_exact_outbox() {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    ServerResult server_result;
    auto server = run_lost_terminal_response_server(acceptor, server_result);
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-terminal-replay-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto config = config_for(acceptor.local_endpoint().port(), root);
    auto opened = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    auto client = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(opened);
    EXPECT(client);
    if (!opened || !client) {
        acceptor.close();
        server.join();
        return;
    }
    auto store = std::move(opened.value());
    auto delivered = seed_terminal_outbox(
        *store, "123e4567-e89b-12d3-a456-426614174032");
    roc::vehicle::AcceptedTask expired{
        false, kLease, 1, kTaskId, 1, 3, 20};
    const auto first = client.value()->report_status(delivered, expired, *store);
    EXPECT(!first);
    if (!first) {
        EXPECT(first.error().domain == roc::vehicle::ErrorDomain::transport);
    }
    const auto blocked = client.value()->accept_task(notification(), *store);
    EXPECT(!blocked);
    if (!blocked) {
        EXPECT(blocked.error().server_code == "terminal_outbox_pending");
    }
    store.reset();

    auto reopened = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    EXPECT(reopened);
    if (reopened) {
        auto& recovered = *reopened.value();
        const auto pending = recovered.pending_status_events();
        EXPECT(pending);
        if (pending && pending.value().size() == 1) {
            const auto& replay = pending.value().front();
            EXPECT(replay.request_sha256.size() == 64);
            EXPECT(replay.lease_token == kLease);
            EXPECT(replay.lease_expires_at == 1);
            const auto response = client.value()->replay_status_event(
                replay, recovered);
            EXPECT(response);
            if (response) {
                EXPECT(response.value().replayed);
            }
            const auto confirmed = recovered.status_event(replay.event_id);
            EXPECT(confirmed);
            if (confirmed) {
                EXPECT(confirmed.value().disposition ==
                       roc::vehicle::StatusEventDisposition::acknowledged);
                EXPECT(confirmed.value().lease_token.empty());
                EXPECT(!confirmed.value().local_delivery_evidence.empty());
            }
        } else if (pending) {
            EXPECT(pending.value().size() == 1);
        }
    }
    server.join();
    {
        std::lock_guard<std::mutex> lock{server_result.mutex};
        EXPECT(server_result.failure.empty());
        EXPECT(server_result.requests == 2);
        EXPECT(server_result.bodies.size() == 2);
        if (server_result.bodies.size() == 2) {
            EXPECT(server_result.bodies[0] == server_result.bodies[1]);
            EXPECT(server_result.leases[0] == server_result.leases[1]);
            EXPECT(server_result.leases[0] == kLease);
        }
    }
    std::filesystem::remove_all(root, ignored);
}

std::thread run_reconciliation_server(tcp::acceptor& acceptor,
                                      ServerResult& result,
                                      std::string platform_code) {
    return std::thread([&acceptor, &result, platform_code = std::move(platform_code)] {
        for (int index = 0; index < 2; ++index) {
            boost::system::error_code error;
            tcp::socket socket{acceptor.get_executor()};
            acceptor.accept(socket, error);
            if (error) {
                fail_server(result, "reconciliation accept failed");
                return;
            }
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request, error);
            if (error) {
                fail_server(result, "reconciliation request read failed");
                return;
            }
            http::response<http::string_body> response;
            response.set(http::field::content_type, "application/json");
            if (index == 0) {
                response.result(platform_code == "invalid_lease"
                                    ? http::status::unauthorized
                                    : http::status::conflict);
                response.body() = std::string{"{\"ok\":false,\"code\":\""} +
                                  platform_code +
                                  "\",\"message\":\"not recorded\"}";
            } else {
                const auto target = std::string{"/api/device/tasks/"} +
                                    kTaskId + "/accept";
                if (request.method() != http::verb::post ||
                    request.target() != target ||
                    !request["X-Task-Lease"].empty()) {
                    fail_server(result, "resolved task reoffer request mismatch");
                    return;
                }
                response.result(http::status::ok);
                response.body() = R"JSON({
                  "ok":true,"replayed":false,
                  "lease_token":"opaque-loopback-lease",
                  "lease_expires_at":4102444800,
                  "task":{"id":"123e4567-e89b-12d3-a456-426614174010",
                          "state":"accepted","attempt":2,"max_attempts":3,
                          "progress":20,"batch_id":"123e4567-e89b-12d3-a456-426614174011",
          "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
          "error_code":null,"error_message":null,"offered_at":null,
          "accepted_at":null,"delivered_at":null,
          "updated_at":"2026-10-04T00:00:00Z"}
                })JSON";
            }
            write_response(socket, response, error);
            if (error) {
                fail_server(result, "reconciliation response write failed");
                return;
            }
            std::lock_guard<std::mutex> lock{result.mutex};
            ++result.requests;
        }
    });
}

void test_terminal_replay_rejection_requires_reconciliation(
    std::string platform_code,
    std::string terminal_event_id,
    std::string directory_suffix) {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    ServerResult server_result;
    auto server = run_reconciliation_server(
        acceptor, server_result, platform_code);
    const auto root = std::filesystem::temp_directory_path() /
                      ("roc-vehicle-reconciliation-" + directory_suffix);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto config = config_for(acceptor.local_endpoint().port(), root);
    auto store = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    auto client = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(store);
    EXPECT(client);
    if (!store || !client) {
        acceptor.close();
        server.join();
        return;
    }
    auto delivered = seed_terminal_outbox(
        *store.value(), std::move(terminal_event_id));
    const auto rejected = client.value()->replay_status_event(
        delivered, *store.value());
    EXPECT(!rejected);
    if (!rejected) {
        EXPECT(rejected.error().server_code == platform_code);
    }
    const auto reconciliations = store.value()->reconciliation_required_events();
    EXPECT(reconciliations);
    if (reconciliations && reconciliations.value().size() == 1) {
        const auto& event = reconciliations.value().front();
        EXPECT(event.event_id == delivered.event_id);
        EXPECT(event.lease_token == kLease);
        EXPECT(event.payload_json == delivered.payload_json);
        EXPECT(event.request_sha256.size() == 64);
        EXPECT(event.local_delivery_evidence ==
               delivered.local_delivery_evidence);
        EXPECT(event.reconciliation_reason == platform_code);
    }
    const auto generation = store.value()->task_generation({kTaskId, 1});
    EXPECT(generation);
    if (generation) {
        EXPECT(generation.value().state ==
               roc::vehicle::TaskExecutionState::reconciliation_required);
    }
    const auto blocked = client.value()->accept_task(notification(), *store.value());
    EXPECT(!blocked);
    if (!blocked) {
        EXPECT(blocked.error().server_code == "terminal_outbox_pending");
    }
    EXPECT(!store.value()->task_generation({kTaskId, 2}));
    EXPECT(store.value()->resolve_status_event_reconciliation(delivered.event_id));
    const auto resolved = store.value()->status_event(delivered.event_id);
    EXPECT(resolved);
    if (resolved) {
        EXPECT(resolved.value().disposition ==
               roc::vehicle::StatusEventDisposition::resolved);
        EXPECT(resolved.value().lease_token.empty());
    }
    const auto unblocked = client.value()->accept_task(
        notification(), *store.value());
    EXPECT(unblocked);
    if (unblocked) {
        EXPECT(unblocked.value().attempt == 2);
        const auto reoffered = store.value()->task_generation({kTaskId, 2});
        EXPECT(reoffered);
        if (reoffered) {
            EXPECT(reoffered.value().state ==
                   roc::vehicle::TaskExecutionState::accepted);
        }
    }
    server.join();
    {
        std::lock_guard<std::mutex> lock{server_result.mutex};
        EXPECT(server_result.failure.empty());
        EXPECT(server_result.requests == 2);
    }
    std::filesystem::remove_all(root, ignored);
}

void test_pre_canceled_request() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-task-http-cancel-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    auto config = config_for(9, root);
    auto store = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    auto client = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(store);
    EXPECT(client);
    roc::vehicle::CancellationSignal cancellation;
    cancellation.cancel();
    if (store && client) {
        const auto accepted = client.value()->accept_task(
            notification(), *store.value(), &cancellation);
        EXPECT(!accepted);
        if (!accepted) {
            EXPECT(accepted.error().domain == roc::vehicle::ErrorDomain::canceled);
        }
    }
    std::filesystem::remove_all(root, ignored);
}

void test_locally_expired_lease_stops_before_network() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-task-http-local-expiry-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    auto config = config_for(9, root);
    auto store = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    auto client = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(store);
    EXPECT(client);
    if (store && client) {
        EXPECT(store.value()->register_task_generation({kTaskId, 1}, 0));
        roc::vehicle::AcceptedTask expired{false, kLease, 1, kTaskId, 1, 3, 0};
        const auto manifest = client.value()->read_manifest(
            notification(), expired, *store.value());
        EXPECT(!manifest);
        if (!manifest) {
            EXPECT(manifest.error().server_code == "lease_expired");
            EXPECT(!manifest.error().retryable);
        }
        const auto generation = store.value()->task_generation({kTaskId, 1});
        EXPECT(generation);
        if (generation) {
            EXPECT(generation.value().state ==
                   roc::vehicle::TaskExecutionState::expired);
        }
    }
    std::filesystem::remove_all(root, ignored);
}

void test_truncated_artifact_response_is_not_success() {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    std::thread server{[&acceptor] {
        boost::system::error_code error;
        tcp::socket socket{acceptor.get_executor()};
        acceptor.accept(socket, error);
        if (error) {
            return;
        }
        beast::flat_buffer buffer;
        http::request<http::empty_body> request;
        http::read(socket, buffer, request, error);
        if (error) {
            return;
        }
        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/png\r\n"
            "Content-Length: 10\r\n"
            "X-Content-SHA256: " + std::string{kSha} + "\r\n"
            "Connection: close\r\n\r\n12345";
        asio::write(socket, asio::buffer(response), error);
        socket.close(error);
    }};
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-truncated-artifact-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto config = config_for(acceptor.local_endpoint().port(), root);
    auto store = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    auto client = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(store);
    EXPECT(client);
    if (store && client) {
        EXPECT(store.value()->register_task_generation({kTaskId, 1}, 0));
        roc::vehicle::ArtifactManifest manifest;
        manifest.task_id = kTaskId;
        manifest.content_type = "image/png";
        manifest.byte_size = 10;
        manifest.sha256 = kSha;
        manifest.artifact_url = std::string{"/api/device/tasks/"} +
                                kTaskId + "/artifact";
        roc::vehicle::AcceptedTask accepted{
            false, kLease, 4102444800, kTaskId, 1, 3, 0};
        EXPECT(store.value()->persist_active_task_lease({notification(), {kTaskId,1}, kLease, 4102444800, 0}));
        std::size_t received = 0;
        const auto downloaded = client.value()->download_artifact(
            manifest, accepted, *store.value(),
            [&received](const std::uint8_t*, std::size_t size) {
                received += size;
                return roc::vehicle::Result<void>{};
            });
        EXPECT(!downloaded);
        EXPECT(received == 5);
    }
    server.join();
    std::filesystem::remove_all(root, ignored);
}

void test_tls_handshake_failure_is_classified() {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    std::thread server{[&acceptor] {
        boost::system::error_code error;
        tcp::socket socket{acceptor.get_executor()};
        acceptor.accept(socket, error);
        if (error) {
            return;
        }
        const std::string invalid_tls = "not-a-tls-server";
        asio::write(socket, asio::buffer(invalid_tls), error);
        socket.close(error);
    }};
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-tls-failure-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    auto config = config_for(acceptor.local_endpoint().port(), root);
    config.base_url = "https://127.0.0.1:" +
                      std::to_string(acceptor.local_endpoint().port());
    auto store = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    auto client = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(store);
    EXPECT(client);
    if (store && client) {
        const auto accepted = client.value()->accept_task(
            notification(), *store.value());
        EXPECT(!accepted);
        if (!accepted) {
            EXPECT(accepted.error().domain == roc::vehicle::ErrorDomain::tls);
        }
    }
    server.join();
    std::filesystem::remove_all(root, ignored);
}

void test_connection_refusal_is_classified() {
    asio::io_context context;
    tcp::acceptor reservation{context,
                              {asio::ip::address_v4::loopback(), 0}};
    const auto port = reservation.local_endpoint().port();
    boost::system::error_code close_error;
    reservation.close(close_error);
    EXPECT(!close_error);

    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-connection-refusal-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto config = config_for(port, root);
    auto store = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    auto client = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(store);
    EXPECT(client);
    if (store && client) {
        const auto accepted = client.value()->accept_task(
            notification(), *store.value());
        EXPECT(!accepted);
        if (!accepted) {
            EXPECT(accepted.error().domain ==
                   roc::vehicle::ErrorDomain::transport);
        }
    }
    std::filesystem::remove_all(root, ignored);
}

void test_untrusted_tls_certificate_is_rejected() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-untrusted-tls-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);
    const auto certificate = root / "test-certificate.pem";
    const auto private_key = root / "test-private-key.pem";
    EXPECT(write_self_signed_certificate(certificate, private_key));

    asio::io_context server_context;
    ssl::context tls_context{ssl::context::tls_server};
    boost::system::error_code tls_error;
    tls_context.use_certificate_chain_file(certificate.string(), tls_error);
    EXPECT(!tls_error);
    tls_context.use_private_key_file(
        private_key.string(), ssl::context::pem, tls_error);
    EXPECT(!tls_error);
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    std::thread server{[&acceptor, &tls_context] {
        boost::system::error_code error;
        tcp::socket socket{acceptor.get_executor()};
        acceptor.accept(socket, error);
        if (error) {
            return;
        }
        ssl::stream<tcp::socket> stream{std::move(socket), tls_context};
        stream.handshake(ssl::stream_base::server, error);
    }};

    auto config = config_for(acceptor.local_endpoint().port(), root);
    config.base_url = "https://127.0.0.1:" +
                      std::to_string(acceptor.local_endpoint().port());
    auto store = roc::vehicle::DurableStateStore::open(
        config.state_directory / "state.sqlite3");
    auto client = roc::vehicle::DeviceTaskHttpClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"loopback-test-token"}; });
    EXPECT(store);
    EXPECT(client);
    if (store && client) {
        const auto accepted = client.value()->accept_task(
            notification(), *store.value());
        EXPECT(!accepted);
        if (!accepted) {
            EXPECT(accepted.error().domain == roc::vehicle::ErrorDomain::tls);
        }
    }
    server.join();
    std::filesystem::remove_all(root, ignored);
}

void test_missing_active_binding_refuses_resource_before_authorization() {
    const auto root=std::filesystem::temp_directory_path()/"roc-http-missing-active-binding";
    std::error_code ec;std::filesystem::remove_all(root,ec);
    for(int stage=0;stage<3;++stage) {
        const auto config=config_for(9,root/std::to_string(stage));
        auto store=roc::vehicle::DurableStateStore::open(config.state_directory/"state.sqlite3");
        int tokens=0;
        auto client=roc::vehicle::DeviceTaskHttpClient::create(config,[&] {
            ++tokens;return roc::vehicle::Result<std::string>{"synthetic-unused-token"};});
        EXPECT(store && client);if(!store || !client)continue;
        EXPECT(store.value()->register_task_generation({kTaskId,1},20));
        for(int i=0;i<stage;++i) {
            auto e=make_status_event(i==0?"123e4567-e89b-12d3-a456-426614174080":
                "123e4567-e89b-12d3-a456-426614174081",i==0?roc::vehicle::TaskExecutionState::downloading:
                roc::vehicle::TaskExecutionState::delivering,20,4102444800);
            EXPECT(store.value()->persist_status_event(e));EXPECT(store.value()->acknowledge_status_event(e.event_id));
        }
        roc::vehicle::AcceptedTask handle{false,kLease,4102444800,kTaskId,1,3,20};
        const auto manifest=client.value()->read_manifest(notification(),handle,*store.value());EXPECT(!manifest);
        if(!manifest)EXPECT(manifest.error().domain==roc::vehicle::ErrorDomain::storage);
        roc::vehicle::ArtifactManifest m;m.task_id=kTaskId;m.artifact_url=roc::vehicle::task_artifact_target(kTaskId);
        const auto download=client.value()->download_artifact(m,handle,*store.value(),
            [](const std::uint8_t*,std::size_t)->roc::vehicle::Result<void>{EXPECT(false);return {};});
        EXPECT(!download);EXPECT(tokens==0);
    }
    std::filesystem::remove_all(root,ec);
}
}  // namespace

int main() {
    test_missing_active_binding_refuses_resource_before_authorization();
    test_package_file_http_path();
    test_accepted_live_repeat_preserves_lease();
    test_untrusted_terminal_response_retains_outbox(
        R"({"ok":true,"replayed":true,"task":{}})", 200);
    test_untrusted_terminal_response_retains_outbox(
        R"({"ok":false,"code":"future_error","message":"synthetic"})", 409);
    test_untrusted_terminal_response_retains_outbox("{", 200);
    test_untrusted_terminal_response_retains_outbox(R"({"ok":true,"replayed":true,
        "project_id":"123e4567-e89b-12d3-a456-426614174012","task":{
        "id":"123e4567-e89b-12d3-a456-426614174099",
        "batch_id":"123e4567-e89b-12d3-a456-426614174011",
        "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
        "state":"delivered","attempt":2,"max_attempts":3,"progress":100,
        "error_code":null,"error_message":null,"offered_at":null,
        "accepted_at":null,"delivered_at":null,"updated_at":"2026-10-04T00:00:00Z"}})",200);
    test_complete_loopback_flow();
    test_lease_expiry_and_new_attempt();
    test_terminal_response_loss_replays_exact_outbox();
    test_terminal_replay_rejection_requires_reconciliation(
        "invalid_state", "123e4567-e89b-12d3-a456-426614174033",
        "invalid-state-test");
    test_terminal_replay_rejection_requires_reconciliation(
        "lease_expired", "123e4567-e89b-12d3-a456-426614174034",
        "lease-expired-test");
    test_terminal_replay_rejection_requires_reconciliation(
        "invalid_transition", "123e4567-e89b-12d3-a456-426614174035",
        "invalid-transition-test");
    test_terminal_replay_rejection_requires_reconciliation(
        "invalid_lease", "123e4567-e89b-12d3-a456-426614174036",
        "invalid-lease-test");
    test_pre_canceled_request();
    test_terminal_replay_rejection_requires_reconciliation(
        "event_conflict", "123e4567-e89b-12d3-a456-426614174037",
        "event-conflict-test");
    test_locally_expired_lease_stops_before_network();
    test_truncated_artifact_response_is_not_success();
    test_connection_refusal_is_classified();
    test_tls_handshake_failure_is_classified();
    test_untrusted_tls_certificate_is_rejected();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
