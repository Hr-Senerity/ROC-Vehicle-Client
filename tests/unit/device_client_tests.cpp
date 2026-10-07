#include "roc/vehicle/device_client.hpp"
#include "roc/vehicle/version.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

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
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

struct LocalServerResult {
    std::mutex mutex;
    std::condition_variable condition;
    bool handshake_ok{false};
    bool telemetry_received{false};
    bool heartbeat_received{false};
    std::string failure;
};

std::thread run_local_server(tcp::acceptor& acceptor, LocalServerResult& result) {
    return std::thread([&acceptor, &result] {
        boost::system::error_code error;
        tcp::socket socket{acceptor.get_executor()};
        acceptor.accept(socket, error);
        if (error) {
            std::lock_guard<std::mutex> lock{result.mutex};
            result.failure = "accept failed: " + error.message();
            result.condition.notify_all();
            return;
        }

        websocket::stream<tcp::socket> stream{std::move(socket)};
        beast::flat_buffer handshake_buffer;
        http::request<http::string_body> request;
        http::read(stream.next_layer(), handshake_buffer, request, error);
        if (error) {
            std::lock_guard<std::mutex> lock{result.mutex};
            result.failure = "HTTP handshake read failed: " + error.message();
            result.condition.notify_all();
            return;
        }
        stream.accept(request, error);
        if (error) {
            std::lock_guard<std::mutex> lock{result.mutex};
            result.failure = "WebSocket handshake failed: " + error.message();
            result.condition.notify_all();
            return;
        }
        {
            std::lock_guard<std::mutex> lock{result.mutex};
            result.handshake_ok = request.target() == "/ws/device" &&
                request[http::field::authorization] == "Device unit-test-token";
            result.condition.notify_all();
        }

        const std::string hello = R"JSON({
          "protocol_version":1,
          "message_id":"123e4567-e89b-12d3-a456-426614174000",
          "type":"hello",
          "sequence":"1",
          "timestamp":"2026-09-26T12:00:00.000Z",
          "payload":{
            "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
            "heartbeat_interval_seconds":1,
            "idle_timeout_seconds":5,
            "max_message_bytes":65536,
            "max_heartbeat_bytes":4096,
            "max_telemetry_bytes":16384,
            "last_client_sequence":"0"
          }
        })JSON";
        stream.text(true);
        stream.write(asio::buffer(hello), error);
        if (error) {
            return;
        }
        const std::string task = R"JSON({
          "protocol_version":1,
          "message_id":"123e4567-e89b-12d3-a456-426614174002",
          "type":"task.available",
          "sequence":"2",
          "timestamp":"2026-09-26T12:00:00.100Z",
          "payload":{
            "task_id":"123e4567-e89b-12d3-a456-426614174010",
            "batch_id":"123e4567-e89b-12d3-a456-426614174011",
            "project_id":"123e4567-e89b-12d3-a456-426614174012",
            "resource_type":"map",
            "resource_revision_id":"123e4567-e89b-12d3-a456-426614174013",
            "attempt":0,
            "max_attempts":3
          }
        })JSON";
        stream.write(asio::buffer(task), error);
        if (error) {
            return;
        }

        for (int message_index = 0; message_index < 2; ++message_index) {
            beast::flat_buffer buffer;
            stream.read(buffer, error);
            if (error || !stream.got_text()) {
                return;
            }
            const auto message = beast::buffers_to_string(buffer.data());
            std::lock_guard<std::mutex> lock{result.mutex};
            if (message_index == 0) {
                result.telemetry_received =
                    message.find("\"type\":\"telemetry\"") != std::string::npos &&
                    message.find("\"sequence\":\"1\"") != std::string::npos;
            } else {
                result.heartbeat_received =
                    message.find("\"type\":\"heartbeat\"") != std::string::npos &&
                    message.find("\"sequence\":\"2\"") != std::string::npos &&
                    message.find("\"library_version\":\"" +
                                 std::string{roc::vehicle::library_version()} + "\"") !=
                        std::string::npos;
            }
            result.condition.notify_all();
        }
        stream.close(websocket::close_code::normal, error);
    });
}

std::string reconnect_hello(int index) {
    return R"JSON({"protocol_version":1,"message_id":"123e4567-e89b-12d3-a456-426614174000","type":"hello","sequence":")JSON" +
        std::to_string(index + 1) +
        R"JSON(","timestamp":"2026-09-26T12:00:00.000Z","payload":{"vehicle_id":"123e4567-e89b-12d3-a456-426614174001","heartbeat_interval_seconds":1,"idle_timeout_seconds":5,"max_message_bytes":65536,"max_heartbeat_bytes":4096,"max_telemetry_bytes":16384,"last_client_sequence":")JSON" +
        std::to_string(index) + R"JSON("}})JSON";
}

struct ReconnectResult {
    std::mutex mutex;
    std::condition_variable condition;
    bool second_heartbeat{false};
    std::string failure;
};

std::thread run_reconnect_server(tcp::acceptor& acceptor, ReconnectResult& result) {
    return std::thread([&acceptor, &result] {
        for (int index = 0; index < 2; ++index) {
            boost::system::error_code error;
            tcp::socket socket{acceptor.get_executor()};
            acceptor.accept(socket, error);
            if (error) {
                std::lock_guard<std::mutex> lock{result.mutex};
                result.failure = "reconnect accept failed: " + error.message();
                result.condition.notify_all();
                return;
            }
            websocket::stream<tcp::socket> stream{std::move(socket)};
            stream.accept(error);
            if (error) {
                std::lock_guard<std::mutex> lock{result.mutex};
                result.failure = "reconnect handshake failed: " + error.message();
                result.condition.notify_all();
                return;
            }
            const auto hello = reconnect_hello(index);
            stream.text(true);
            stream.write(asio::buffer(hello), error);
            if (error) {
                return;
            }
            beast::flat_buffer buffer;
            stream.read(buffer, error);
            if (error) {
                return;
            }
            const auto heartbeat = beast::buffers_to_string(buffer.data());
            const auto expected =
                "\"sequence\":\"" + std::to_string(index + 1) + "\"";
            const bool valid = heartbeat.find("\"type\":\"heartbeat\"") !=
                    std::string::npos &&
                heartbeat.find(expected) != std::string::npos &&
                heartbeat.find("\"library_version\":\"" +
                               std::string{roc::vehicle::library_version()} + "\"") !=
                    std::string::npos;
            if (!valid) {
                std::lock_guard<std::mutex> lock{result.mutex};
                result.failure = "reconnect heartbeat sequence was invalid";
                result.condition.notify_all();
                return;
            }
            if (index == 1) {
                std::lock_guard<std::mutex> lock{result.mutex};
                result.second_heartbeat = true;
                result.condition.notify_all();
            }
            stream.close(websocket::close_code::normal, error);
        }
    });
}

void test_loopback_websocket_session() {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    const auto port = acceptor.local_endpoint().port();
    LocalServerResult server_result;
    auto server_thread = run_local_server(acceptor, server_result);

    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-device-client-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    bool online = false;
    bool task_received = false;
    std::string client_error;
    roc::vehicle::ClientConfig config;
    config.base_url = "http://127.0.0.1:" + std::to_string(port);
    config.state_directory = root / "state";
    config.artifact_directory = root / "artifacts";
    config.hello_timeout = std::chrono::seconds{2};
    config.reconnect.initial_delay = std::chrono::seconds{2};
    config.reconnect.maximum_delay = std::chrono::seconds{2};
    config.reconnect.jitter_min = 1.0;
    config.reconnect.jitter_max = 1.0;

    roc::vehicle::DeviceClientCallbacks callbacks;
    callbacks.on_state_changed = [&](roc::vehicle::ConnectionState state) {
        std::lock_guard<std::mutex> lock{callback_mutex};
        if (state == roc::vehicle::ConnectionState::online) {
            online = true;
        }
        callback_condition.notify_all();
    };
    callbacks.on_task_available = [&](const roc::vehicle::TaskAvailableMessage& task) {
        std::lock_guard<std::mutex> lock{callback_mutex};
        task_received = task.task_id == "123e4567-e89b-12d3-a456-426614174010";
        callback_condition.notify_all();
    };
    callbacks.on_error = [&](const roc::vehicle::Error& error) {
        std::lock_guard<std::mutex> lock{callback_mutex};
        client_error = error.message;
        callback_condition.notify_all();
    };

    auto created = roc::vehicle::DeviceClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"unit-test-token"}; },
        std::move(callbacks));
    EXPECT(created);
    if (!created) {
        acceptor.close();
        server_thread.join();
        return;
    }
    auto client = std::move(created.value());
    EXPECT(!client->publish_telemetry({}));
    EXPECT(client->start());
    roc::vehicle::TelemetrySnapshot telemetry;
    telemetry.online = true;
    telemetry.cpu_usage = 10.0;
    telemetry.memory_usage = 20.0;
    telemetry.battery_level = 90;
    telemetry.localization_confidence = 95.0;
    telemetry.library_version = "roc-vehicle-cpp-0.1.0";
    EXPECT(client->publish_telemetry(telemetry));
    {
        std::unique_lock<std::mutex> lock{callback_mutex};
        callback_condition.wait_for(lock, std::chrono::seconds{4}, [&] {
            return online && task_received;
        });
        EXPECT(online);
        EXPECT(task_received);
    }
    {
        std::unique_lock<std::mutex> lock{server_result.mutex};
        server_result.condition.wait_for(lock, std::chrono::seconds{4}, [&] {
            return (server_result.telemetry_received &&
                    server_result.heartbeat_received) ||
                !server_result.failure.empty();
        });
        EXPECT(server_result.failure.empty());
        EXPECT(server_result.handshake_ok);
        EXPECT(server_result.telemetry_received);
        EXPECT(server_result.heartbeat_received);
    }
    if (!client_error.empty()) {
        std::cerr << "client error: " << client_error << '\n';
    }
    if (!server_result.failure.empty()) {
        std::cerr << "server error: " << server_result.failure << '\n';
    }
    client->stop();
    server_thread.join();
    EXPECT(client->state() == roc::vehicle::ConnectionState::stopped);

    std::filesystem::remove_all(root, ignored);
}

void test_disconnect_reconnect_sequence_alignment() {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    const auto port = acceptor.local_endpoint().port();
    ReconnectResult server_result;
    auto server_thread = run_reconnect_server(acceptor, server_result);

    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-reconnect-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    roc::vehicle::ClientConfig config;
    config.base_url = "http://127.0.0.1:" + std::to_string(port);
    config.state_directory = root / "state";
    config.artifact_directory = root / "artifacts";
    config.hello_timeout = std::chrono::seconds{2};
    config.reconnect.initial_delay = std::chrono::milliseconds{50};
    config.reconnect.maximum_delay = std::chrono::milliseconds{50};
    config.reconnect.jitter_min = 1.0;
    config.reconnect.jitter_max = 1.0;

    auto created = roc::vehicle::DeviceClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"unit-test-token"}; });
    EXPECT(created);
    if (!created) {
        acceptor.close();
        server_thread.join();
        return;
    }
    auto client = std::move(created.value());
    EXPECT(client->start());
    {
        std::unique_lock<std::mutex> lock{server_result.mutex};
        server_result.condition.wait_for(lock, std::chrono::seconds{5}, [&] {
            return server_result.second_heartbeat || !server_result.failure.empty();
        });
        EXPECT(server_result.failure.empty());
        EXPECT(server_result.second_heartbeat);
    }
    client->stop();
    server_thread.join();
    std::filesystem::remove_all(root, ignored);
}

void test_authentication_rejection_stops_reconnect() {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context, {asio::ip::address_v4::loopback(), 0}};
    const auto port = acceptor.local_endpoint().port();
    std::thread server_thread{[&acceptor] {
        boost::system::error_code error;
        tcp::socket socket{acceptor.get_executor()};
        acceptor.accept(socket, error);
        if (error) {
            return;
        }
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request, error);
        if (error) {
            return;
        }
        http::response<http::empty_body> response{http::status::unauthorized,
                                                  request.version()};
        response.set(http::field::content_length, "0");
        response.keep_alive(false);
        http::write(socket, response, error);
    }};

    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-auth-stop-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    roc::vehicle::ClientConfig config;
    config.base_url = "http://127.0.0.1:" + std::to_string(port);
    config.state_directory = root / "state";
    config.artifact_directory = root / "artifacts";
    config.reconnect.initial_delay = std::chrono::milliseconds{20};
    config.reconnect.maximum_delay = std::chrono::milliseconds{20};
    config.reconnect.jitter_min = 1.0;
    config.reconnect.jitter_max = 1.0;

    std::mutex state_mutex;
    std::condition_variable state_condition;
    bool authentication_stopped = false;
    roc::vehicle::DeviceClientCallbacks callbacks;
    callbacks.on_state_changed = [&](roc::vehicle::ConnectionState state) {
        std::lock_guard<std::mutex> lock{state_mutex};
        authentication_stopped =
            state == roc::vehicle::ConnectionState::authentication_stopped;
        state_condition.notify_all();
    };
    auto created = roc::vehicle::DeviceClient::create(
        config, [] { return roc::vehicle::Result<std::string>{"rejected-test-token"}; },
        std::move(callbacks));
    EXPECT(created);
    if (created) {
        auto client = std::move(created.value());
        EXPECT(client->start());
        std::unique_lock<std::mutex> lock{state_mutex};
        state_condition.wait_for(lock, std::chrono::seconds{2}, [&] {
            return authentication_stopped;
        });
        EXPECT(authentication_stopped);
        lock.unlock();
        EXPECT(client->state() ==
               roc::vehicle::ConnectionState::authentication_stopped);
        client->stop();
    }
    server_thread.join();
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    test_loopback_websocket_session();
    test_disconnect_reconnect_sequence_alignment();
    test_authentication_rejection_stops_reconnect();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
