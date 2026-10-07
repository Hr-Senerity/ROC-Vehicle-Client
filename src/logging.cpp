#include "roc/vehicle/logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace roc::vehicle {
namespace {

constexpr std::string_view kRedacted = "<redacted>";

char lower(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

std::size_t find_case_insensitive(std::string_view text, std::string_view needle,
                                  std::size_t start) {
    if (needle.empty() || text.size() < needle.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = start; i + needle.size() <= text.size(); ++i) {
        bool equal = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lower(text[i + j]) != lower(needle[j])) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return i;
        }
    }
    return std::string_view::npos;
}

void redact_after_marker(std::string& value, std::string_view marker) {
    std::size_t search_from = 0;
    while (true) {
        const auto found = find_case_insensitive(value, marker, search_from);
        if (found == std::string::npos) {
            return;
        }
        std::size_t begin = found + marker.size();
        while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
            ++begin;
        }
        const bool quoted = begin < value.size() && value[begin] == '"';
        if (quoted) {
            ++begin;
        }
        std::size_t end = begin;
        while (end < value.size()) {
            const char current = value[end];
            if ((quoted && current == '"') || (!quoted &&
                (current == '\r' || current == '\n' || current == ',' ||
                 current == '}' || std::isspace(static_cast<unsigned char>(current)) != 0))) {
                break;
            }
            ++end;
        }
        value.replace(begin, end - begin, kRedacted);
        search_from = begin + kRedacted.size();
    }
}

std::string_view level_name(LogLevel level) {
    switch (level) {
        case LogLevel::trace: return "trace";
        case LogLevel::debug: return "debug";
        case LogLevel::info: return "info";
        case LogLevel::warning: return "warning";
        case LogLevel::error: return "error";
        case LogLevel::critical: return "critical";
    }
    return "unknown";
}

std::string single_line(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const char value : input) {
        output.push_back(value == '\r' || value == '\n' ? ' ' : value);
    }
    return output;
}

}  // namespace

std::string redact_sensitive_text(std::string_view text) {
    std::string output{text};
    constexpr std::array<std::string_view, 6> markers{
        "authorization:", "x-task-lease:", "\"lease_token\":",
        "\"device_token\":", "\"token\":", "roc_dev_"};
    for (const auto marker : markers) {
        redact_after_marker(output, marker);
    }
    return output;
}

std::string format_log_record(const LogRecord& record) {
    std::ostringstream stream;
    stream << '[' << level_name(record.level) << "] "
           << single_line(record.component) << ": "
           << single_line(redact_sensitive_text(record.message));
    for (const auto& field : record.fields) {
        const auto value = field.sensitivity == FieldSensitivity::secret
                               ? std::string{kRedacted}
                               : redact_sensitive_text(field.value);
        stream << ' ' << single_line(field.key) << '=' << single_line(value);
    }
    return stream.str();
}

}  // namespace roc::vehicle
