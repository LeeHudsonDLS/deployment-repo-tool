#include "paths.h"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <vector>

#include "support.h"

namespace {

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

std::string home_directory() {
    const char *home = std::getenv("HOME");
    if (home && *home) return home;
    // No $HOME: the password database is what os.path.expanduser falls back on,
    // and cron jobs and su sessions really do arrive without it.
    const struct passwd *entry = getpwuid(getuid());
    return (entry && entry->pw_dir) ? entry->pw_dir : std::string();
}

}  // namespace

std::string expandvars(const std::string &path) {
    if (path.find('$') == std::string::npos) return path;

    std::string out;
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] != '$') {
            out += path[i++];
            continue;
        }
        std::string name;
        size_t after = i;
        if (i + 1 < path.size() && path[i + 1] == '{') {
            const size_t close = path.find('}', i + 2);
            if (close != std::string::npos) {
                name = path.substr(i + 2, close - (i + 2));
                after = close + 1;
            }
        } else {
            size_t end = i + 1;
            while (end < path.size() && is_name_char(path[end])) end++;
            if (end > i + 1) {
                name = path.substr(i + 1, end - (i + 1));
                after = end;
            }
        }
        if (after == i) {  // a bare $, or ${ with no closing brace: not a variable
            out += path[i++];
            continue;
        }
        const char *value = std::getenv(name.c_str());
        // An undefined variable is left as it was written. Substituting an
        // empty string would turn a typo into a path that quietly points
        // somewhere else, usually the root of the filesystem.
        out += value ? std::string(value) : path.substr(i, after - i);
        i = after;
    }
    return out;
}

std::string expanduser(const std::string &path) {
    if (path.empty() || path[0] != '~') return path;

    size_t slash = path.find('/', 1);
    if (slash == std::string::npos) slash = path.size();

    std::string userhome;
    if (slash == 1) {
        userhome = home_directory();
    } else {
        const std::string name = path.substr(1, slash - 1);
        const struct passwd *entry = getpwnam(name.c_str());
        if (!entry || !entry->pw_dir) return path;  // no such user: leave it alone
        userhome = entry->pw_dir;
    }
    while (!userhome.empty() && userhome.back() == '/') userhome.pop_back();

    const std::string out = userhome + path.substr(slash);
    return out.empty() ? "/" : out;
}

std::string normpath(const std::string &path) {
    if (path.empty()) return ".";

    // POSIX gives a path beginning with exactly two slashes an
    // implementation-defined meaning, so those two are kept; three or more are
    // the same as one. os.path.normpath makes the same distinction.
    size_t leading = 0;
    if (path[0] == '/') {
        leading = 1;
        if (path.size() > 1 && path[1] == '/' && !(path.size() > 2 && path[2] == '/')) {
            leading = 2;
        }
    }

    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        const size_t next = path.find('/', i);
        const std::string part =
            path.substr(i, next == std::string::npos ? std::string::npos : next - i);
        if (!part.empty() && part != ".") {
            if (part != "..") {
                parts.push_back(part);
            } else if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (leading == 0) {
                // Only a relative path can keep a leading "..": above the root
                // there is nothing, so /../x is just /x.
                parts.push_back(part);
            }
        }
        if (next == std::string::npos) break;
        i = next + 1;
    }

    const std::string out = std::string(leading, '/') + join(parts, "/");
    return out.empty() ? "." : out;
}

std::string abspath(const std::string &path) {
    if (!path.empty() && path[0] == '/') return normpath(path);
    return normpath(path_join(current_directory(), path));
}

std::string expand(const std::string &path) {
    return abspath(expanduser(expandvars(path)));
}

std::string path_join(const std::string &head, const std::string &tail) {
    if (!tail.empty() && tail[0] == '/') return tail;
    if (head.empty()) return tail;
    if (head.back() == '/') return head + tail;
    return head + "/" + tail;
}

std::string basename(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string dirname(const std::string &path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return std::string();
    std::string head = path.substr(0, slash + 1);
    // "/" stays "/", but "/usr/bin/" becomes "/usr/bin".
    if (head.find_first_not_of('/') != std::string::npos) {
        while (head.size() > 1 && head.back() == '/') head.pop_back();
    }
    return head;
}

bool path_exists(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

bool is_file(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool is_dir(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string current_directory() {
    // No fixed buffer: a deployment repo under a deeply nested automount path
    // is exactly the sort of thing that would overflow one.
    for (size_t size = 512; size <= 65536; size *= 2) {
        std::vector<char> buffer(size);
        if (getcwd(buffer.data(), size)) return std::string(buffer.data());
        if (errno != ERANGE) break;
    }
    fail("cannot read the current directory");
}

std::string own_path() {
    std::vector<char> buffer(4096);
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    // Empty rather than fatal: the only caller can carry on and say that the
    // helper script has to be named in the config instead.
    if (length <= 0) return std::string();
    return std::string(buffer.data(), static_cast<size_t>(length));
}
