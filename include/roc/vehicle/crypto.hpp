#pragma once

#include "roc/vehicle/result.hpp"

#include <string>
#include <string_view>

namespace roc::vehicle {

[[nodiscard]] Result<std::string> sha256_hex(std::string_view bytes);

}  // namespace roc::vehicle
