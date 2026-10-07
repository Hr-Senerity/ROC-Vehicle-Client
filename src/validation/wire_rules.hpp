#pragma once
#include <jsoncons/json.hpp>
#include <string_view>

namespace roc::vehicle::detail::wire {
using Json = jsoncons::json;
[[nodiscard]] bool device(const Json& value, std::string_view definition);
[[nodiscard]] bool task(const Json& value, std::string_view definition);
[[nodiscard]] bool road(const Json& value, int version);
}
