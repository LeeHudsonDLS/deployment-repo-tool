#include "values.h"

#include <algorithm>

#include "glob.h"
#include "support.h"

const char *VALUES = "apps/values.yaml";

namespace {
bool ignored(const std::string &line) {
    const auto text = strip(line);
    return text.empty() || text[0] == '#';
}

// Find comments outside quoted scalars. A '#' inside a revision is data.
size_t comment_at(const std::string &line) {
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote == '"' && c == '\\') { ++i; continue; }
        if (quote) {
            if (c == quote) {
                if (quote == '\'' && i + 1 < line.size() && line[i + 1] == '\'') ++i;
                else quote = 0;
            }
        } else if (c == '#' && (i == 0 || is_space(line[i - 1]))) return i;
        else if (c == '\'' || c == '"') quote = c;
    }
    return std::string::npos;
}

struct Entry {
    std::string name;
    size_t start, end;
    bool inline_value;
};

// All callers use the same service depth, including inline entries. Recognising
// an unsupported entry is essential: deploy must not append a duplicate of it.
std::vector<Entry> entries(const Lines &lines) {
    const auto range = block(lines);
    size_t depth = std::string::npos;
    for (size_t i = range.first; i < range.second; ++i) {
        if (!ignored(lines[i])) depth = std::min(depth, indent_of(lines[i]));
    }
    std::vector<Entry> result;
    for (size_t i = range.first; i < range.second; ++i) {
        if (ignored(lines[i]) || indent_of(lines[i]) != depth) continue;
        const auto text = strip(lines[i].substr(0, comment_at(lines[i])));
        const size_t colon = text.find(':');
        if (colon == std::string::npos) fail("unsupported service entry: " + text);
        std::string name = text.substr(0, colon);
        if (name.size() >= 2 && (name.front() == '\'' || name.front() == '"') &&
            name.back() == name.front()) name = name.substr(1, name.size() - 2);
        if (!result.empty()) result.back().end = i;
        result.push_back({name, i, range.second, !strip(text.substr(colon + 1)).empty()});
    }
    // Keep separator comments and blank lines between services where they are.
    for (auto &entry : result) {
        while (entry.end > entry.start + 1 && ignored(lines[entry.end - 1])) --entry.end;
    }
    return result;
}

std::string revision_scalar(const std::string &value) {
    for (unsigned char c : value) {
        if (c < 32 || c == 127) fail("revision must not contain control characters");
    }
    // Leave familiar branch names readable; quote anything which could acquire
    // a YAML type or syntax. Single quotes escape by doubling, not backslashes.
    const auto folded = lower(value);
    const bool word = !value.empty() &&
        ((folded[0] >= 'a' && folded[0] <= 'z') || folded[0] == '_') &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-") == std::string::npos;
    if (word && folded != "true" && folded != "false" && folded != "null" &&
        folded != "yes" && folded != "no" && folded != "on" && folded != "off") return value;
    std::string quoted = "'";
    for (char c : value) { quoted += c; if (c == '\'') quoted += c; }
    return quoted + "'";
}
}

size_t indent_of(const std::string &line) { return line.size() - lstrip(line).size(); }

std::pair<size_t, size_t> block(const Lines &lines) {
    size_t start = 0;
    while (start < lines.size() && rstrip(lines[start]) != "services:") ++start;
    if (start == lines.size()) fail(std::string("no 'services:' block in ") + VALUES);
    size_t end = start + 1;
    while (end < lines.size() && (ignored(lines[end]) || starts_with(lines[end], " "))) ++end;
    while (end > start + 1 && strip(lines[end - 1]).empty()) --end;
    return {start + 1, end};
}

bool find_service(const Lines &lines, const std::string &service, size_t *start,
                  size_t *end) {
    bool found = false;
    for (const auto &entry : entries(lines)) {
        if (entry.name != service) continue;
        if (found) fail("duplicate service entry: " + service);
        if (entry.inline_value) fail("unsupported inline service entry: " + service);
        *start = entry.start;
        *end = entry.end;
        found = true;
    }
    return found;
}

std::vector<std::string> select(const Lines &lines, const std::string &pattern,
                                const Options &aliases) {
    std::string expanded = pattern;
    for (const auto &alias : aliases) {
        if (alias.first == pattern) { expanded = alias.second; break; }
    }
    if (!is_glob(expanded)) return {expanded};
    std::vector<std::string> names;
    for (const auto &entry : entries(lines)) names.push_back(entry.name);
    const auto hits = filter(names, expanded);
    if (hits.empty()) {
        const auto shown = expanded == pattern ? "'" + pattern + "'" :
            "'" + pattern + "' (" + expanded + ")";
        fail(format("nothing in %s matches %s", VALUES, shown.c_str()));
    }
    return hits;
}

void set_key(Lines &lines, const std::string &service, const std::string &key,
             const std::string &value) {
    size_t start, end;
    if (!find_service(lines, service, &start, &end)) fail("'" + service + "' is not in " + VALUES);
    size_t depth = std::string::npos;
    for (size_t i = start + 1; i < end; ++i) {
        if (!ignored(lines[i])) depth = std::min(depth, indent_of(lines[i]));
    }
    if (depth == std::string::npos) depth = indent_of(lines[start]) + 2;
    const auto scalar = key == "targetRevision" ? revision_scalar(value) : value;
    size_t found = end;
    for (size_t i = start + 1; i < end; ++i) {
        if (indent_of(lines[i]) != depth || !starts_with(lstrip(lines[i]), key + ":")) continue;
        if (found != end) fail("duplicate key " + key + " in service " + service);
        found = i;
    }
    const std::string replacement = std::string(depth, ' ') + key + ": " + scalar;
    if (found != end) {
        const auto hash = comment_at(lines[found]);
        const auto note = hash == std::string::npos ? "" : "  " + rstrip(lines[found].substr(hash));
        lines[found] = replacement + note + "\n";
    } else lines.insert(lines.begin() + static_cast<long>(end), replacement + "\n");
}
