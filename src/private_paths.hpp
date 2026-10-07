#pragma once

#include <filesystem>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace roc::vehicle::detail {
inline bool link_free_directory_prefix(const std::filesystem::path& path) {
#ifndef _WIN32
    std::error_code error;const auto absolute=std::filesystem::absolute(path,error).lexically_normal();
    if (error) return false;
    int current=::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (current<0) return false;
    for (const auto& component:absolute.relative_path()) {
        const int next=::openat(current,component.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        const int saved=errno;static_cast<void>(::close(current));
        if (next<0) return saved==ENOENT;
        current=next;
    }
    static_cast<void>(::close(current));return true;
#else
    static_cast<void>(path);return true;
#endif
}
// Trusted local configuration only. Reject symlinks at every existing component;
// never chmod ancestors or change process umask. Not an adversarial same-UID
// concurrent-directory-replacement guarantee for later path-based operations.
inline bool private_directory(const std::filesystem::path& path,bool create) {
    std::error_code error;
    const auto absolute=std::filesystem::absolute(path,error).lexically_normal();
    if (path.empty() || error || absolute==absolute.root_path()) return false;
    const auto temporary=std::filesystem::temp_directory_path(error).lexically_normal();
    if (error || absolute==temporary) return false;
#ifndef _WIN32
    if (absolute=="/var/tmp") return false;
    int current=::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (current<0) return false;
    const auto relative=absolute.relative_path();
    bool valid=true;
    for (auto component=relative.begin();component!=relative.end();++component) {
        const auto name=component->string();
        int next=::openat(current,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if (next<0 && errno==ENOENT && create) {
            if (::mkdirat(current,name.c_str(),0700)!=0 && errno!=EEXIST) { valid=false;break; }
            next=::openat(current,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        }
        if (next<0 || (create && ::fsync(current)!=0)) {
            if (next>=0) static_cast<void>(::close(next));
            valid=false;break;
        }
        static_cast<void>(::close(current));current=next;
    }
    struct stat data {};
    valid=valid && ::fstat(current,&data)==0 && S_ISDIR(data.st_mode) &&
        data.st_uid==::geteuid() && (data.st_mode&07777)==0700;
    static_cast<void>(::close(current));return valid;
#else
    if (create) std::filesystem::create_directories(absolute,error);
    const auto status=std::filesystem::symlink_status(absolute,error);
    return !error && std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status);
#endif
}

inline bool private_regular_file(const std::filesystem::path& path,bool missing_ok,bool harden) {
#ifndef _WIN32
    const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) return missing_ok && errno==ENOENT;
    struct stat data {};
    bool valid=::fstat(fd,&data)==0 && S_ISREG(data.st_mode) &&
        data.st_uid==::geteuid() && data.st_nlink==1;
    if (valid && harden) valid=::fchmod(fd,0600)==0;
    valid=valid && ::fstat(fd,&data)==0 && (data.st_mode&07777)==0600;
    static_cast<void>(::close(fd));return valid;
#else
    static_cast<void>(harden);
    std::error_code error;const auto status=std::filesystem::symlink_status(path,error);
    return (!error && std::filesystem::is_regular_file(status)) ||
        (missing_ok && error==std::errc::no_such_file_or_directory);
#endif
}
}
