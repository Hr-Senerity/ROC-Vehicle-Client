#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace roc::vehicle {

enum class LogLevel { trace, debug, info, warning, error, critical };
enum class FieldSensitivity { normal, secret };

struct LogField {
    std::string key;
    std::string value;
    FieldSensitivity sensitivity{FieldSensitivity::normal};
};

struct LogRecord {
    LogLevel level{LogLevel::info};
    std::string component;
    std::string message;
    std::vector<LogField> fields;
};

class Logger {
public:
    virtual ~Logger() = default;
    virtual void write(const LogRecord& record) noexcept = 0;
};

[[nodiscard]] std::string redact_sensitive_text(std::string_view text);
[[nodiscard]] std::string format_log_record(const LogRecord& record);

}  // namespace roc::vehicle
