#include "roc/vehicle/device_client.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
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
        std::cerr << "line " << line << ": expectation failed: "
                  << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

constexpr int kReconnectCycles = 24;

struct StressResult {
    std::mutex mutex;
    std::condition_variable condition;
    int completed_cycles{0};
    int online_callbacks{0};
    std::string failure;
};

void fail(StressResult& result, std::string message) {
    std::lock_guard<std::mutex> lock{result.mutex};
    if (result.failure.empty()) {
        result.failure = std::move(message);
    }
    result.condition.notify_all();
}

std::string hello_frame(int cycle) {
    return R"JSON({"protocol_version":1,"message_id":"123e4567-e89b-12d3-a456-426614174000","type":"hello","sequence":")JSON" +
        std::to_string(cycle + 1) +
        R"JSON(","timestamp":"2026-09-28T00:00:00.000Z","payload":{"vehicle_id":"123e4567-e89b-12d3-a456-426614174001","heartbeat_interval_seconds":30,"idle_timeout_seconds":45,"max_message_bytes":65536,"max_heartbeat_bytes":4096,"max_telemetry_bytes":16384,"last_client_sequence":"0"}})JSON";
}

std::thread run_stress_server(tcp::acceptor& acceptor, StressResult& result) {
    return std::thread([&acceptor, &result] {
        for (int cycle = 0; cycle < kReconnectCycles; ++cycle) {
            boost::system::error_code error;
            tcp::socket socket{acceptor.get_executor()};
            acceptor.accept(socket, error);
            if (error) {
                fail(result, "accept failed: " + error.message());
                return;
            }
            websocket::stream<tcp::socket> stream{std::move(socket)};
            stream.accept(error);
            if (error) {
                fail(result, "WebSocket handshake failed: " + error.message());
                return;
            }
            const auto hello = hello_frame(cycle);
            stream.text(true);
            stream.write(asio::buffer(hello), error);
            if (error) {
                fail(result, "hello write failed: " + error.message());
                return;
            }
            {
                std::unique_lock<std::mutex> lock{result.mutex};
                const bool online = result.condition.wait_for(
                    lock, std::chrono::seconds{1}, [&] {
                        return result.online_callbacks >= cycle + 1 ||
                               !result.failure.empty();
                    });
                if (!online || !result.failure.empty()) {
                    if (result.failure.empty()) {
                        result.failure = "client did not reach online state";
                    }
                    result.condition.notify_all();
                    return;
                }
            }
            stream.next_layer().set_option(asio::socket_base::linger{true, 0},
                                           error);
            stream.next_layer().close(error);
            {
                std::lock_guard<std::mutex> lock{result.mutex};
                result.completed_cycles = cycle + 1;
                result.condition.notify_all();
            }
        }
    });
}

roc::vehicle::TelemetrySnapshot telemetry_for(int sample) {
    roc::vehicle::TelemetrySnapshot telemetry;
    telemetry.online = true;
    telemetry.cpu_usage = static_cast<double>(sample % 100);
    telemetry.memory_usage = 25.0;
    telemetry.battery_level = 80;
    telemetry.localization_confidence = 90.0;
    telemetry.library_version = "roc-vehicle-cpp-0.1.0";
    return telemetry;
}

void test_reconnect_and_stop_races() {
    asio::io_context server_context;
    tcp::acceptor acceptor{server_context,
                           {asio::ip::address_v4::loopback(), 0}};
    StressResult server_result;
    auto server = run_stress_server(acceptor, server_result);

    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-device-client-stress-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    roc::vehicle::ClientConfig config;
    config.base_url = "http://127.0.0.1:" +
                      std::to_string(acceptor.local_endpoint().port());
    config.state_directory = root / "state";
    config.artifact_directory = root / "artifacts";
    config.hello_timeout = std::chrono::milliseconds{500};
    config.reconnect.initial_delay = std::chrono::milliseconds{2};
    config.reconnect.maximum_delay = std::chrono::milliseconds{8};
    config.reconnect.jitter_min = 1.0;
    config.reconnect.jitter_max = 1.0;

    std::atomic<int> backing_off_count{0};
    roc::vehicle::DeviceClientCallbacks callbacks;
    callbacks.on_state_changed = [&](roc::vehicle::ConnectionState state) {
        if (state == roc::vehicle::ConnectionState::online) {
            std::lock_guard<std::mutex> lock{server_result.mutex};
            ++server_result.online_callbacks;
            server_result.condition.notify_all();
        } else if (state == roc::vehicle::ConnectionState::backing_off) {
            backing_off_count.fetch_add(1);
        }
    };

    auto created = roc::vehicle::DeviceClient::create(
        config,
        [] { return roc::vehicle::Result<std::string>{"stress-test-token"}; },
        std::move(callbacks));
    EXPECT(created);
    if (!created) {
        acceptor.close();
        server.join();
        return;
    }
    auto client = std::move(created.value());
    const auto started = client->start();
    EXPECT(started);
    bool completed = false;
    {
        std::unique_lock<std::mutex> lock{server_result.mutex};
        server_result.condition.wait_for(lock, std::chrono::seconds{10}, [&] {
            return server_result.completed_cycles == kReconnectCycles ||
                   !server_result.failure.empty();
        });
        EXPECT(server_result.failure.empty());
        EXPECT(server_result.completed_cycles == kReconnectCycles);
        EXPECT(server_result.online_callbacks == kReconnectCycles);
        completed = server_result.failure.empty() &&
                    server_result.completed_cycles == kReconnectCycles;
    }
    if (!started || !completed) {
        client->stop();
        boost::system::error_code wake_error;
        tcp::socket wake_socket{server_context};
        wake_socket.connect(acceptor.local_endpoint(), wake_error);
        wake_socket.close(wake_error);
    }
    server.join();
    if (!started || !completed) {
        std::filesystem::remove_all(root, ignored);
        return;
    }

    std::atomic_bool churn{true};
    std::thread publisher{[&] {
        int sample = 0;
        while (churn.load()) {
            static_cast<void>(client->publish_telemetry(
                telemetry_for(++sample)));
            client->release_task_notification(
                "123e4567-e89b-12d3-a456-426614174010");
            std::this_thread::sleep_for(std::chrono::microseconds{50});
        }
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    client->stop();
    churn.store(false);
    publisher.join();

    EXPECT(client->state() == roc::vehicle::ConnectionState::stopped);
    EXPECT(backing_off_count.load() >= kReconnectCycles - 1);
    EXPECT(!client->publish_telemetry(telemetry_for(0)));
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    test_reconnect_and_stop_races();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
