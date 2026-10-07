#include "roc/vehicle/endpoint.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <string>

namespace roc::vehicle {
namespace {

Result<DeviceEndpoint> invalid_endpoint(std::string message) {
    return Error{ErrorDomain::configuration, ErrorCode::invalid_argument, false,
                 std::move(message)};
}

bool valid_dns_host(std::string_view host) {
    return !host.empty() && std::all_of(host.begin(), host.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '.' || character == '-';
    });
}

bool valid_ipv6_host(std::string_view host) {
    return !host.empty() && std::all_of(host.begin(), host.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isxdigit(value) != 0 || character == ':' || character == '.';
    });
}

bool valid_port(std::string_view port) {
    if (port.empty()) {
        return false;
    }
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() &&
           value > 0 && value <= 65535;
}

}  // namespace

Result<DeviceEndpoint> derive_device_endpoint(std::string_view base_url) {
    bool tls = false;
    std::size_t scheme_size = 0;
    if (base_url.substr(0, 7) == "http://") {
        scheme_size = 7;
    } else if (base_url.substr(0, 8) == "https://") {
        tls = true;
        scheme_size = 8;
    } else {
        return invalid_endpoint("base_url must use http or https");
    }
    if (base_url.find_first_of("?#\r\n\t ") != std::string_view::npos ||
        base_url.find('@', scheme_size) != std::string_view::npos) {
        return invalid_endpoint("base_url contains forbidden URL components");
    }

    const auto path_offset = base_url.find('/', scheme_size);
    const auto authority = base_url.substr(
        scheme_size, path_offset == std::string_view::npos
                         ? std::string_view::npos
                         : path_offset - scheme_size);
    std::string_view path = path_offset == std::string_view::npos
                                ? std::string_view{"/"}
                                : base_url.substr(path_offset);
    if (authority.empty() || path.empty() || path.front() != '/') {
        return invalid_endpoint("base_url authority or path is invalid");
    }

    std::string_view host;
    std::string_view port;
    bool ipv6 = false;
    bool explicit_port = false;
    if (authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos) {
            return invalid_endpoint("base_url IPv6 host is invalid");
        }
        ipv6 = true;
        host = authority.substr(1, closing - 1);
        const auto suffix = authority.substr(closing + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':' || suffix.size() == 1) {
                return invalid_endpoint("base_url port is invalid");
            }
            port = suffix.substr(1);
            explicit_port = true;
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) {
                return invalid_endpoint("IPv6 base_url hosts must use brackets");
            }
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
            explicit_port = true;
        } else {
            host = authority;
        }
    }
    if ((ipv6 && !valid_ipv6_host(host)) || (!ipv6 && !valid_dns_host(host)) ||
        (explicit_port && !valid_port(port))) {
        return invalid_endpoint("base_url host or port is invalid");
    }
    if (!explicit_port) {
        port = tls ? std::string_view{"443"} : std::string_view{"80"};
    }

    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    if (path != "/") {
        return invalid_endpoint("base_url must be an origin without a path");
    }
    std::string target{"/ws/device"};

    std::string host_header;
    if (ipv6) {
        host_header = "[" + std::string{host} + "]";
    } else {
        host_header = std::string{host};
    }
    if (explicit_port) {
        host_header += ":" + std::string{port};
    }
    return DeviceEndpoint{tls, std::string{host}, std::string{port},
                          std::move(host_header), std::move(target)};
}

Result<std::string> make_device_authorization(std::string_view device_token) {
    if (device_token.empty() || device_token.size() > 4096 ||
        device_token.find_first_of("\r\n") != std::string_view::npos) {
        return Error{ErrorDomain::authentication, ErrorCode::invalid_argument, false,
                     "device token is empty, oversized, or contains a line break"};
    }
    return "Device " + std::string{device_token};
}

}  // namespace roc::vehicle
