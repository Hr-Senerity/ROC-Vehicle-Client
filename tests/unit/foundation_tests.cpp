#include "roc/vehicle/config.hpp"
#include "roc/vehicle/logging.hpp"
#include "roc/vehicle/result.hpp"
#include "roc/vehicle/version.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
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

roc::vehicle::ClientConfig valid_config() {
    roc::vehicle::ClientConfig config;
    config.base_url = "https://roc.invalid";
    config.state_directory = std::filesystem::path{"state"};
    config.artifact_directory = std::filesystem::path{"artifacts"};
    return config;
}

void test_config_validation() {
    auto config = valid_config();
    EXPECT(roc::vehicle::validate_config(config));

    config.base_url = "ftp://roc.invalid";
    EXPECT(!roc::vehicle::validate_config(config));
    config = valid_config();
    config.base_url = "https://user:secret@roc.invalid";
    EXPECT(!roc::vehicle::validate_config(config));
    config = valid_config();
    config.tls.verify_hostname = false;
    EXPECT(!roc::vehicle::validate_config(config));
    config.tls.verify_peer = false;
    config.tls.insecure_demo = true;
    EXPECT(roc::vehicle::validate_config(config));
    config = valid_config();
    config.reconnect.jitter_min = std::numeric_limits<double>::quiet_NaN();
    EXPECT(!roc::vehicle::validate_config(config));
    config = valid_config();
    config.http_operation_timeout = std::chrono::milliseconds{0};
    EXPECT(!roc::vehicle::validate_config(config));
    config.http_operation_timeout = std::chrono::milliseconds{-1};
    EXPECT(!roc::vehicle::validate_config(config));
    config=valid_config(); config.expected_vehicle_id="not-a-uuid";
    EXPECT(!roc::vehicle::validate_config(config));
    config.expected_vehicle_id="123e4567-e89b-12d3-a456-426614174001";
    EXPECT(roc::vehicle::validate_config(config));
}

void test_redaction() {
    const std::string raw =
        "Authorization: Device roc_dev_example-secret\n"
        "X-Task-Lease: opaque-lease-value\n"
        "{\"lease_token\":\"json-lease\",\"token\":\"another-secret\"}";
    const auto redacted = roc::vehicle::redact_sensitive_text(raw);
    EXPECT(redacted.find("roc_dev_example-secret") == std::string::npos);
    EXPECT(redacted.find("opaque-lease-value") == std::string::npos);
    EXPECT(redacted.find("json-lease") == std::string::npos);
    EXPECT(redacted.find("another-secret") == std::string::npos);
    EXPECT(redacted.find("<redacted>") != std::string::npos);

    roc::vehicle::LogRecord record;
    record.component = "foundation";
    record.message = "safe message";
    record.fields.push_back({"lease", "do-not-print", roc::vehicle::FieldSensitivity::secret});
    const auto formatted = roc::vehicle::format_log_record(record);
    EXPECT(formatted.find("do-not-print") == std::string::npos);
}

void test_result_and_version() {
    roc::vehicle::Result<int> success{42};
    EXPECT(success);
    EXPECT(success.value() == 42);

    roc::vehicle::Result<int> failure{roc::vehicle::Error{
        roc::vehicle::ErrorDomain::protocol,
        roc::vehicle::ErrorCode::invalid_argument,
        false,
        "invalid"}};
    EXPECT(!failure);
    EXPECT(failure.error().domain == roc::vehicle::ErrorDomain::protocol);
    EXPECT(roc::vehicle::library_version() == "0.2.1-rc2");
    EXPECT(roc::vehicle::protocol_compatibility_version() ==
           "roc-system-v0.2.1@a9c601d6977d89000438854a24e3199573004a2f");
}

}  // namespace

int main() {
    test_config_validation();
    test_redaction();
    test_result_and_version();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
