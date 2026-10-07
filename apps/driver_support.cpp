#include "driver_support.hpp"
#include "private_paths.hpp"
#include "contract_validation.hpp"
#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/endpoint.hpp"
#include <jsoncons/json.hpp>
#include <openssl/ssl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace roc::vehicle::driver {
namespace {
using Json = jsoncons::json;
bool private_file(int fd) {
    struct stat info{};
    return ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
           info.st_nlink == 1 && (info.st_mode & 07777) == 0600;
}
bool disallowed_root(const std::filesystem::path& p) {
    return p == p.root_path() || p == "/tmp" || p == "/var/tmp" || p == "/dev/shm" ||
           p == "/run" || p == "/build" || p == "/workspace" || p == "/home";
}
bool root_ok(const std::filesystem::path& p) {
    if (!p.is_absolute() || p != p.lexically_normal() || disallowed_root(p) ||
        !detail::link_free_directory_prefix(p)) return false;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(p, ec);
    if (!ec && std::filesystem::exists(status)) return detail::private_directory(p, false);
    if (ec && ec != std::errc::no_such_file_or_directory) return false;
    // Only create one dedicated leaf; never recursively claim or chmod ancestors.
    return std::filesystem::is_directory(p.parent_path(), ec) && !ec;
}
bool nested(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto x = a.begin(); auto y = b.begin();
    for (; x != a.end() && y != b.end() && *x == *y; ++x, ++y) {}
    return x == a.end();
}
bool optional_file_ok(const std::filesystem::path& p) {
    return detail::private_regular_file(p, true, false);
}
int bounded_int(const Json& j, const char* name, int fallback, int low, int high) {
    if (!j.contains(name)) return fallback;
    if (!j[name].is_int64()) throw std::invalid_argument("integer");
    const auto n = j[name].as<std::int64_t>();
    if (n < low || n > high) throw std::invalid_argument("range");
    return static_cast<int>(n);
}
}
Error local_error(const char* code, ErrorDomain domain) {
    return {domain, ErrorCode::invalid_argument, false, "driver operation refused", code};
}
Result<std::string> read_private_file(const std::filesystem::path& path, std::size_t limit) {
    if (!path.is_absolute() || !detail::link_free_directory_prefix(path.parent_path()))
        return local_error("private_file_invalid");
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || !private_file(fd)) {
        if (fd >= 0) static_cast<void>(::close(fd));
        return local_error("private_file_invalid");
    }
    std::string bytes; std::array<char,4096> buffer{}; bool ok = true;
    while (true) {
        const auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (count == 0) break;
        if (bytes.size() + static_cast<std::size_t>(count) > limit) { ok = false; break; }
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    static_cast<void>(::close(fd));
    if (!ok) { std::fill(bytes.begin(), bytes.end(), '\0'); return local_error("private_file_invalid"); }
    return bytes;
}
Result<std::string> read_token(const std::filesystem::path& path) {
    if (!detail::private_directory(path.parent_path(), false)) return local_error("credential_invalid");
    auto token = read_private_file(path, 4096);
    if (!token) return local_error("credential_invalid");
    if (!token.value().empty() && token.value().back() == '\n') {
        token.value().pop_back();
        if (!token.value().empty() && token.value().back() == '\r') token.value().pop_back();
    }
    if (token.value().empty() || !std::all_of(token.value().begin(), token.value().end(), [](char c) {
        return static_cast<unsigned char>(c) >= 33 && static_cast<unsigned char>(c) <= 126;
    })) {
        std::fill(token.value().begin(), token.value().end(), '\0'); return local_error("credential_invalid");
    }
    return token;
}
Result<Settings> load_settings(const std::filesystem::path& path, bool allow_remote) {
    // Config contains no secrets, but a protected file avoids accidental runtime edits.
    auto input = read_private_file(path, 65536);
    if (!input) return local_error("config_invalid");
    try {
        const auto j = Json::parse(input.value());
        if (!j.is_object()) return local_error("config_invalid");
        const std::vector<std::string> allowed{"base_url", "vehicle_id", "token_file", "state_directory",
            "artifact_directory", "transport", "network_scope", "ca_file", "queue_capacity",
            "retry_attempts", "retry_initial_ms", "retry_maximum_ms", "offline_budget_ms",
            "http_operation_timeout_ms", "hello_timeout_ms", "reconnect_initial_ms", "reconnect_maximum_ms", "owning_map_context_file"};
        for (const auto& field : j.object_range())
            if (std::find(allowed.begin(), allowed.end(), field.key()) == allowed.end()) return local_error("config_unknown_field");
        for (const auto* name : {"base_url", "vehicle_id", "token_file", "state_directory", "artifact_directory", "transport"})
            if (!j.contains(name) || !j[name].is_string() || j[name].as<std::string>().empty()) return local_error("config_invalid");
        Settings s;
        s.client.base_url = j["base_url"].as<std::string>();
        s.client.expected_vehicle_id = j["vehicle_id"].as<std::string>();
        s.token_file = j["token_file"].as<std::string>();
        s.client.state_directory = j["state_directory"].as<std::string>();
        s.client.artifact_directory = j["artifact_directory"].as<std::string>();
        const auto transport = j["transport"].as<std::string>();
        const auto endpoint = derive_device_endpoint(s.client.base_url);
        if (!endpoint || (transport != "tls" && transport != "http-ws-demo") ||
            endpoint.value().tls != (transport == "tls")) return local_error("transport_invalid");
        if (j.contains("network_scope") && !j["network_scope"].is_string()) return local_error("network_scope_invalid");
        const auto scope = j.contains("network_scope") ? j["network_scope"].as<std::string>() : "loopback";
        s.remote = scope == "remote";
        if ((scope != "loopback" && scope != "remote") || (s.remote && !allow_remote) ||
            (!s.remote && endpoint.value().host != "127.0.0.1" && endpoint.value().host != "::1"))
            return local_error("network_scope_invalid");
        if (j.contains("ca_file")) {
            if (!j["ca_file"].is_string()) return local_error("config_invalid");
            s.client.tls.ca_file = j["ca_file"].as<std::string>();
        }
        s.client.task_queue_capacity = static_cast<std::size_t>(bounded_int(j, "queue_capacity", 32, 1, 1024));
        s.retry_attempts = bounded_int(j, "retry_attempts", 3, 1, 10);
        s.retry_initial_ms = bounded_int(j, "retry_initial_ms", 500, 10, 60000);
        s.retry_maximum_ms = bounded_int(j, "retry_maximum_ms", 5000, s.retry_initial_ms, 60000);
        s.offline_budget_ms = bounded_int(j, "offline_budget_ms", 60000, 100, 3600000);
        s.client.http_operation_timeout = std::chrono::milliseconds{bounded_int(j, "http_operation_timeout_ms", 15000, 50, 60000)};
        s.client.hello_timeout = std::chrono::milliseconds{bounded_int(j, "hello_timeout_ms", 10000, 50, 60000)};
        s.client.reconnect.initial_delay = std::chrono::milliseconds{bounded_int(j, "reconnect_initial_ms", 1000, 10, 60000)};
        s.client.reconnect.maximum_delay = std::chrono::milliseconds{bounded_int(j, "reconnect_maximum_ms", 30000,
            static_cast<int>(s.client.reconnect.initial_delay.count()), 60000)};
        const auto valid = validate_config(s.client);
        if (!valid || s.client.expected_vehicle_id.empty()) return local_error("config_invalid");
        if (j.contains("owning_map_context_file")) {
            if (!j["owning_map_context_file"].is_string()) return local_error("owning_map_context_invalid");
            auto contexts=read_owning_maps(j["owning_map_context_file"].as<std::string>(),s.client.expected_vehicle_id);
            if (!contexts) return contexts.error();
            s.owning_maps=std::move(contexts.value()); s.owning_maps_configured=true;
        }
        if (s.remote && !s.owning_maps_configured) return local_error("owning_map_context_required");
        return s;
    } catch (...) { return local_error("config_invalid"); }
}
Result<void> preflight(const Settings& s) {
    if (!root_ok(s.client.state_directory) || !root_ok(s.client.artifact_directory) ||
        nested(s.client.state_directory, s.client.artifact_directory) ||
        nested(s.client.artifact_directory, s.client.state_directory)) return local_error("root_invalid");
    auto token = read_token(s.token_file);
    if (!token) return token.error();
    std::fill(token.value().begin(), token.value().end(), '\0');
    for (const auto* suffix : {"", "-wal", "-shm"})
        if (!optional_file_ok(s.client.state_directory / (std::string{"roc-vehicle-state.sqlite3"} + suffix)))
            return local_error("state_file_invalid");
    for (const auto& root : {s.client.state_directory, s.client.artifact_directory})
        if (!optional_file_ok(root / ".roc-driver.lock")) return local_error("lock_file_invalid");
    if (s.client.tls.ca_file) {
        const auto cert = read_private_file(*s.client.tls.ca_file, 1024U*1024U);
        if (!cert || cert.value().find("PRIVATE KEY") != std::string::npos) return local_error("ca_file_invalid");
        auto context=std::unique_ptr<SSL_CTX,decltype(&SSL_CTX_free)>(SSL_CTX_new(TLS_client_method()),SSL_CTX_free);
        if (!context || SSL_CTX_load_verify_locations(context.get(),s.client.tls.ca_file->c_str(),nullptr)!=1)
            return local_error("ca_file_invalid");
    }
    if (!detail::wire_rules_self_test()) return local_error("wire_rules_invalid");
    return {};
}
RootLocks::~RootLocks() { for (const auto fd : descriptors_) static_cast<void>(::close(fd)); }
Result<std::unique_ptr<RootLocks>> RootLocks::acquire(const Settings& s) {
    auto locks = std::unique_ptr<RootLocks>(new RootLocks);
    // All roots were checked before creating even the first lock.
    std::array<std::filesystem::path,2> roots{s.client.state_directory, s.client.artifact_directory};
    std::sort(roots.begin(), roots.end());
    for (const auto& root : roots) {
        if (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST) return local_error("root_create_failed", ErrorDomain::storage);
        if (!detail::private_directory(root, false)) return local_error("root_invalid");
        const auto path = root / ".roc-driver.lock";
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0 && errno == EEXIST) fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0 || !private_file(fd)) {
            if (fd >= 0) static_cast<void>(::close(fd));
            return local_error("lock_file_invalid");
        }
        locks->descriptors_.push_back(fd);
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) return local_error("root_in_use", ErrorDomain::storage);
        const int directory = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        const bool synced = directory >= 0 && ::fsync(fd) == 0 && ::fsync(directory) == 0;
        if (directory >= 0) static_cast<void>(::close(directory));
        if (!synced) return local_error("root_sync_failed", ErrorDomain::storage);
    }
    return locks;
}
Result<void> save_immutable(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::symlink_status(path, ec))) {
        const auto old = read_private_file(path, 65536);
        if (!old || old.value() != bytes) return local_error("journal_conflict", ErrorDomain::storage);
        return {};
    }
    const auto id = generate_uuid_v4(); if (!id) return id.error();
    const auto part = path.parent_path() / (".driver-journal-" + id.value() + ".part");
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return local_error("journal_write_failed", ErrorDomain::storage);
    std::size_t offset = 0; bool ok = true;
    while (offset < bytes.size()) {
        const auto n = ::write(fd, bytes.data()+offset, bytes.size()-offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        offset += static_cast<std::size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    if (ok) ok = ::rename(part.c_str(), path.c_str()) == 0;
    if (!ok) { static_cast<void>(::unlink(part.c_str())); return local_error("journal_write_failed", ErrorDomain::storage); }
    const int parent = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ok = parent >= 0 && ::fsync(parent) == 0;
    if (parent >= 0) static_cast<void>(::close(parent));
    if (!ok) return local_error("journal_sync_failed", ErrorDomain::storage);
    return {};
}
std::filesystem::path manifest_path(const Settings& s, const TaskGenerationKey& key) {
    return s.client.state_directory / ("driver-manifest-" + key.task_id + "-" + std::to_string(key.attempt) + ".json");
}
std::string manifest_document(const ArtifactManifest& m) {
    Json j(jsoncons::json_object_arg);
    j["task_id"]=m.task_id; j["batch_id"]=m.batch_id; j["project_id"]=m.project_id;
    j["resource_type"]=m.resource_type==ResourceType::map?"map":"road_network";
    j["resource_revision_id"]=m.resource_revision_id; j["map_id"]=m.map_id;
    j["content_type"]=m.content_type; j["byte_size"]=m.byte_size; j["sha256"]=m.sha256; j["version"]=m.version;
    if (m.schema_version) j["schema_version"]=m.schema_version;
    if (m.package_version) {
        j["package_version"]=m.package_version; j["package_digest_algorithm"]=m.package_digest_algorithm;
        j["map_format"]=m.map_format; j["coordinate_mode"]=m.coordinate_mode;
        const auto optional=[](const auto& v) { return v?Json(*v):Json::null(); };
        j["image_width"]=optional(m.image_width); j["image_height"]=optional(m.image_height);
        j["resolution"]=optional(m.resolution); Json origin(jsoncons::json_object_arg);
        origin["x"]=optional(m.origin_x); origin["y"]=optional(m.origin_y); origin["theta"]=optional(m.origin_theta); j["origin"]=origin;
        j["files"]=Json(jsoncons::json_array_arg);
        for (const auto& file : m.files) {
            Json f(jsoncons::json_object_arg); f["id"]=file.id; f["role"]=file.role; f["file_name"]=file.file_name;
            f["content_type"]=file.content_type; f["byte_size"]=file.byte_size; f["sha256"]=file.sha256; f["artifact_url"]=file.artifact_url;
            j["files"].push_back(f);
        }
    } else j["artifact_url"]=m.artifact_url;
    Json body(jsoncons::json_object_arg); body["ok"]=true; body["manifest"]=j; return body.to_string();
}
}
