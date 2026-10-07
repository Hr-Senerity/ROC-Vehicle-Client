#include "roc/vehicle/endpoint.hpp"
#include "session_core.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

constexpr auto kTimestamp = "2026-09-26T12:00:00.000Z";
constexpr auto kTaskId = "123e4567-e89b-12d3-a456-426614174010";
constexpr auto kOtherTaskId = "123e4567-e89b-12d3-a456-426614174020";

std::string hello_frame(std::string cursor) {
    return R"JSON({"protocol_version":1,"message_id":"123e4567-e89b-12d3-a456-426614174000","type":"hello","sequence":"1","timestamp":"2026-09-26T12:00:00.000Z","payload":{"vehicle_id":"123e4567-e89b-12d3-a456-426614174001","heartbeat_interval_seconds":30,"idle_timeout_seconds":45,"max_message_bytes":65536,"max_heartbeat_bytes":4096,"max_telemetry_bytes":16384,"last_client_sequence":")JSON" +
        cursor + R"JSON("}})JSON";
}

std::string task_frame(std::string task_id, std::string sequence) {
    return R"JSON({"protocol_version":1,"message_id":"123e4567-e89b-12d3-a456-426614174002","type":"task.available","sequence":")JSON" +
        sequence + R"JSON(","timestamp":"2026-09-26T12:00:01.000Z","payload":{"task_id":")JSON" +
        task_id +
        R"JSON(","batch_id":"123e4567-e89b-12d3-a456-426614174011","project_id":"123e4567-e89b-12d3-a456-426614174012","resource_type":"map","resource_revision_id":"123e4567-e89b-12d3-a456-426614174013","attempt":0,"max_attempts":3}})JSON";
}

std::string ack_frame(const roc::vehicle::OutboundMessage& outbound) {
    return R"JSON({"protocol_version":1,"message_id":"123e4567-e89b-12d3-a456-426614174003","type":"ack","sequence":"3","timestamp":"2026-09-26T12:00:02.000Z","payload":{"ack_message_id":")JSON" +
        outbound.message_id + R"JSON(","ack_sequence":")JSON" +
        std::to_string(outbound.sequence) +
        R"JSON(","accepted_type":"heartbeat","duplicate":true}})JSON";
}

void test_endpoint_derivation_and_authorization() {
    const auto secure = roc::vehicle::derive_device_endpoint(
        "https://vehicle.invalid:8443/");
    EXPECT(secure);
    if (secure) {
        EXPECT(secure.value().tls);
        EXPECT(secure.value().host == "vehicle.invalid");
        EXPECT(secure.value().port == "8443");
        EXPECT(secure.value().host_header == "vehicle.invalid:8443");
        EXPECT(secure.value().websocket_target == "/ws/device");
    }

    const auto ipv6 = roc::vehicle::derive_device_endpoint("http://[::1]");
    EXPECT(ipv6);
    if (ipv6) {
        EXPECT(!ipv6.value().tls);
        EXPECT(ipv6.value().host == "::1");
        EXPECT(ipv6.value().port == "80");
        EXPECT(ipv6.value().host_header == "[::1]");
        EXPECT(ipv6.value().websocket_target == "/ws/device");
    }

    EXPECT(!roc::vehicle::derive_device_endpoint("https://user@vehicle.invalid"));
    EXPECT(!roc::vehicle::derive_device_endpoint("https://vehicle.invalid:0"));
    EXPECT(!roc::vehicle::derive_device_endpoint("https://vehicle.invalid/api"));
    EXPECT(roc::vehicle::make_device_authorization("test-device-token").value() ==
           "Device test-device-token");
    EXPECT(!roc::vehicle::make_device_authorization("bad\r\ntoken"));
}

void test_session_protocol_and_recovery() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-session-core-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    auto opened = roc::vehicle::DurableStateStore::open(root / "state.sqlite3", 64);
    EXPECT(opened);
    if (!opened) {
        return;
    }
    auto store = std::move(opened.value());
    roc::vehicle::SessionLimits local_limits;
    local_limits.heartbeat_bytes = 512;
    roc::vehicle::SessionCore session{*store, 1, local_limits};

    EXPECT(!session.make_heartbeat(kTimestamp, "roc-vehicle-cpp-0.1.0"));
    EXPECT(!session.handle_text_frame(task_frame(kTaskId, "2")));

    const std::string auth_error = R"JSON({
      "protocol_version":1,
      "message_id":"123e4567-e89b-12d3-a456-426614174005",
      "type":"error",
      "sequence":"2",
      "timestamp":"2026-09-26T12:00:00Z",
      "payload":{"code":"authentication_failed","message":"credential rejected"}
    })JSON";
    const auto authentication = session.handle_text_frame(auth_error);
    EXPECT(authentication);
    if (authentication) {
        EXPECT(authentication.value().directive ==
               roc::vehicle::SessionDirective::stop_authentication);
    }
    for (const auto* code : {"authentication_required", "credential_revoked", "credential_rotated"}) {
        auto frame = auth_error;
        frame.replace(frame.find("authentication_failed"),
                      std::string{"authentication_failed"}.size(), code);
        const auto stopped = session.handle_text_frame(frame);
        EXPECT(stopped);
        if (stopped) EXPECT(stopped.value().directive ==
                           roc::vehicle::SessionDirective::stop_authentication);
    }

    const auto hello = session.handle_text_frame(hello_frame("40"));
    EXPECT(hello);
    EXPECT(session.ready());
    EXPECT(session.vehicle_id() == "123e4567-e89b-12d3-a456-426614174001");
    EXPECT(session.effective_limits().heartbeat_bytes == 512);

    const auto heartbeat = session.make_heartbeat(
        kTimestamp, "roc-vehicle-cpp-0.1.0");
    EXPECT(heartbeat);
    if (heartbeat) {
        EXPECT(heartbeat.value().sequence == 41);
        EXPECT(session.outstanding_count() == 1);
        const auto ack = session.handle_text_frame(ack_frame(heartbeat.value()));
        EXPECT(ack);
        if (ack) {
            EXPECT(ack.value().ack.has_value());
            EXPECT(ack.value().ack->duplicate);
        }
        EXPECT(session.outstanding_count() == 0);
    }

    const auto first_task = session.handle_text_frame(task_frame(kTaskId, "4"));
    EXPECT(first_task);
    if (first_task) {
        EXPECT(first_task.value().task.has_value());
        EXPECT(!first_task.value().duplicate_task);
    }
    const auto duplicate_task = session.handle_text_frame(task_frame(kTaskId, "5"));
    EXPECT(duplicate_task);
    if (duplicate_task) {
        EXPECT(!duplicate_task.value().task.has_value());
        EXPECT(duplicate_task.value().duplicate_task);
    }
    const auto full = session.handle_text_frame(task_frame(kOtherTaskId, "6"));
    EXPECT(!full);
    if (!full) {
        EXPECT(full.error().retryable);
    }
    session.release_task_notification(kTaskId);
    EXPECT(session.handle_text_frame(task_frame(kOtherTaskId, "7")));

    session.reset_connection();
    EXPECT(!session.ready());
    EXPECT(session.outstanding_count() == 0);
    EXPECT(session.handle_text_frame(hello_frame("45")));
    const auto after_reconnect = session.make_heartbeat(kTimestamp);
    EXPECT(after_reconnect);
    if (after_reconnect) {
        EXPECT(after_reconnect.value().sequence == 46);
    }

    const std::string replaced = R"JSON({
      "protocol_version":1,
      "message_id":"123e4567-e89b-12d3-a456-426614174006",
      "type":"error",
      "sequence":"8",
      "timestamp":"2026-09-26T12:00:03Z",
      "payload":{"code":"connection_replaced","message":"new connection won"}
    })JSON";
    const auto replacement = session.handle_text_frame(replaced);
    EXPECT(replacement);
    if (replacement) {
        EXPECT(replacement.value().directive ==
               roc::vehicle::SessionDirective::stop_replaced);
    }

    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    test_endpoint_derivation_and_authorization();
    test_session_protocol_and_recovery();
    {
        const auto root=std::filesystem::temp_directory_path()/"roc-session-pinned-test";
        std::error_code ec; std::filesystem::remove_all(root,ec);
        auto store=roc::vehicle::DurableStateStore::open(root/"state.sqlite3");
        EXPECT(store);
        if (store) {
            roc::vehicle::SessionCore pinned(*store.value(),1,{},"123e4567-e89b-12d3-a456-426614174009");
            const auto rejected=pinned.handle_text_frame(hello_frame("400"));
            EXPECT(!rejected);
            if (!rejected) EXPECT(rejected.error().server_code=="vehicle_identity_mismatch" && !rejected.error().retryable);
            EXPECT(!pinned.ready()); EXPECT(store.value()->reserved_through()==0);
            roc::vehicle::SessionCore session(*store.value(),1);
            EXPECT(session.handle_text_frame(hello_frame("0")));
            EXPECT(session.handle_text_frame(task_frame(kTaskId,"2")).value().task);
            auto higher=task_frame(kTaskId,"3");
            higher.replace(higher.find("\"attempt\":0"),11,"\"attempt\":1");
            EXPECT(session.handle_text_frame(higher).value().task);
            session.release_task_notification(kTaskId,0);
            EXPECT(session.pending_task_count()==1);
            EXPECT(session.handle_text_frame(higher).value().duplicate_task);
            session.release_task_notification(kTaskId,1);
            EXPECT(session.pending_task_count()==0);
            EXPECT(session.handle_text_frame(higher).value().task);
        }
        std::filesystem::remove_all(root,ec);
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
