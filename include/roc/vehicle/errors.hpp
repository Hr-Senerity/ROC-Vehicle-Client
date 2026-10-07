#pragma once

#include <string>
#include <utility>

namespace roc::vehicle {

enum class ErrorDomain {
    configuration,
    authentication,
    protocol,
    transport,
    tls,
    http,
    lease,
    storage,
    artifact,
    canceled,
    internal
};

enum class ErrorCode {
    invalid_argument = 1,
    invalid_state,
    unavailable,
    timeout,
    permission_denied,
    integrity_failure,
    io_failure,
    unsupported,
    canceled,
    internal_failure
};

struct Error {
    Error() = default;
    Error(ErrorDomain error_domain, ErrorCode error_code, bool can_retry,
          std::string error_message, std::string platform_code = {})
        : domain(error_domain),
          code(error_code),
          retryable(can_retry),
          message(std::move(error_message)),
          server_code(std::move(platform_code)) {}

    ErrorDomain domain{ErrorDomain::internal};
    ErrorCode code{ErrorCode::internal_failure};
    bool retryable{false};
    std::string message;
    // Stable, non-secret platform code, or a documented local resource diagnostic.
    // Local resource codes are never used as platform state-machine responses.
    // The human-readable response body is deliberately not retained here.
    std::string server_code;
};

}  // namespace roc::vehicle
