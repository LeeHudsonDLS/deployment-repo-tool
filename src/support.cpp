#include "support.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

void fail(const std::string &message) {
    // Flushed first: our own progress lines are buffered, and an error that
    // arrives before the output it followed is confusing to read back.
    std::cout.flush();
    std::cerr << "error: " << message << std::endl;
    std::exit(1);
}

std::string format(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list measure;
    va_copy(measure, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, measure);
    va_end(measure);
    if (needed < 0) {
        va_end(args);
        return std::string();
    }
    std::string out(static_cast<size_t>(needed) + 1, '\0');
    std::vsnprintf(&out[0], out.size(), fmt, args);
    va_end(args);
    out.resize(static_cast<size_t>(needed));
    return out;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

std::string lstrip(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && is_space(s[i])) i++;
    return s.substr(i);
}

std::string rstrip(const std::string &s) {
    size_t end = s.size();
    while (end > 0 && is_space(s[end - 1])) end--;
    return s.substr(0, end);
}

std::string strip(const std::string &s) { return lstrip(rstrip(s)); }

bool starts_with(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lower(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::vector<std::string> split_list(const std::string &value) {
    std::vector<std::string> out;
    std::string current;
    for (char c : value) {
        if (c == ',' || is_space(c)) {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string join(const std::vector<std::string> &parts, const std::string &sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start + 1));
        start = nl + 1;
    }
    return lines;
}

std::string read_file(const std::string &path) {
    std::ifstream handle(path.c_str(), std::ios::binary);
    if (!handle) fail("cannot read " + path);
    std::ostringstream buffer;
    buffer << handle.rdbuf();
    if (handle.bad()) fail("cannot read " + path);
    return buffer.str();
}

void write_file(const std::string &path, const std::string &text) {
    std::ofstream handle(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!handle) fail("cannot write " + path);
    handle << text;
    handle.close();
    // Checked rather than assumed: a full disk or a read-only checkout must not
    // leave a truncated values.yaml behind and report success.
    if (!handle) fail("cannot write " + path);
}
