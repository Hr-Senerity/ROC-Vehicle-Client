#include "roc/vehicle/version.hpp"
#include "build_version.hpp"

namespace roc::vehicle {

std::string_view library_version() noexcept { return ROC_VEHICLE_FULL_VERSION; }

std::string_view protocol_compatibility_version() noexcept {
    return "roc-system-v0.2.1@" ROC_VEHICLE_SYSTEM_COMMIT;
}

}  // namespace roc::vehicle
