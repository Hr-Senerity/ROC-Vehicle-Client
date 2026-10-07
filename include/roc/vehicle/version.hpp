#pragma once

#include <string_view>

namespace roc::vehicle {

[[nodiscard]] std::string_view library_version() noexcept;
[[nodiscard]] std::string_view protocol_compatibility_version() noexcept;

}  // namespace roc::vehicle
