#pragma once
#include <string_view>
#include <cstddef>

namespace roc::vehicle::detail {
// Standalone wire constraints and UUID/date-time format assertions. Fail closed
// without copying response bodies or sensitive values into diagnostics.
[[nodiscard]] bool wire_rules_self_test();
[[nodiscard]] bool valid_task_contract(std::string_view body, std::string_view definition);
[[nodiscard]] bool valid_device_contract(std::string_view body, std::string_view definition);
[[nodiscard]] bool valid_road_contract(std::string_view body, int version);
// Cursor preflight before any DOM/typed point allocation. No normalization.
[[nodiscard]] bool bounded_road_document(std::string_view body, std::size_t maximum_bytes);
}
