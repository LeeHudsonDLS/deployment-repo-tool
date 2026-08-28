#include "values.h"

#include "glob.h"
#include "support.h"

const char *VALUES = "apps/values.yaml";

namespace {

// The three shapes of line the tool recognises. In the python these were
// regular expressions; written out they are no longer than the patterns were,
// and each one says plainly what it will and will not accept.

// How many leading spaces, or npos if the line is all whitespace. Only spaces
// count: YAML forbids tabs for indentation, and treating one as an indent would
// mean silently agreeing with a file no parser will read.
size_t leading_spaces(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') i++;
    return i;
}

// `  name:` with nothing after it but whitespace -- a service entry, or a key
// with a block under it.  (regex: " +NAME:\s*$")
bool is_named_entry(const std::string &line, const std::string &name) {
    const size_t indent = leading_spaces(line);
    if (indent == 0) return false;
    if (line.size() - indent < name.size() + 1) return false;
    if (line.compare(indent, name.size(), name) != 0) return false;
    size_t i = indent + name.size();
    if (line[i] != ':') return false;
    for (i++; i < line.size(); i++) {
        if (!is_space(line[i])) return false;
    }
    return true;
}

// `    key:` with anything at all after it -- a key/value line to rewrite.
// (regex: " +KEY:")
bool is_key_line(const std::string &line, const std::string &key) {
    const size_t indent = leading_spaces(line);
    if (indent == 0) return false;
    if (line.size() - indent < key.size() + 1) return false;
    if (line.compare(indent, key.size(), key) != 0) return false;
    return line[indent + key.size()] == ':';
}

// Any `  something:` line, giving back its indent and the name. Used to list
// what is in the block, so a comment line must not look like an entry.
// (regex: "( +)([^#\s]\S*):\s*$")
bool entry_name(const std::string &line, size_t *indent, std::string *name) {
    const size_t start = leading_spaces(line);
    if (start == 0 || start >= line.size()) return false;

    size_t end = start;
    while (end < line.size() && !is_space(line[end])) end++;
    // Anything after the first run of whitespace means this is a `key: value`
    // line or a comment, not the head of an entry.
    for (size_t i = end; i < line.size(); i++) {
        if (!is_space(line[i])) return false;
    }

    const std::string token = line.substr(start, end - start);
    if (token.size() < 2 || token.back() != ':' || token[0] == '#') return false;

    *indent = start;
    *name = token.substr(0, token.size() - 1);
    return true;
}

}  // namespace

size_t indent_of(const std::string &line) { return line.size() - lstrip(line).size(); }

std::pair<size_t, size_t> block(const Lines &lines) {
    size_t start = lines.size();
    for (size_t i = 0; i < lines.size(); i++) {
        if (rstrip(lines[i]) == "services:") {
            start = i;
            break;
        }
    }
    if (start == lines.size()) fail(std::string("no 'services:' block in ") + VALUES);

    size_t end = start + 1;
    while (end < lines.size() &&
           (strip(lines[end]).empty() || starts_with(lines[end], " "))) {
        end++;
    }
    while (end > start + 1 && strip(lines[end - 1]).empty()) end--;
    return std::make_pair(start + 1, end);
}

bool find_service(const Lines &lines, const std::string &service, size_t *start,
                  size_t *end) {
    const std::pair<size_t, size_t> range = block(lines);
    for (size_t i = range.first; i < range.second; i++) {
        if (!is_named_entry(lines[i], service)) continue;
        const std::string deeper(indent_of(lines[i]) + 1, ' ');
        size_t j = i + 1;
        while (j < range.second && starts_with(lines[j], deeper)) j++;
        *start = i;
        *end = j;
        return true;
    }
    return false;
}

std::vector<std::string> select(const Lines &lines, const std::string &pattern,
                                const Options &aliases) {
    std::string expanded = pattern;
    for (const std::pair<std::string, std::string> &alias : aliases) {
        if (alias.first == pattern) {
            expanded = alias.second;
            break;
        }
    }
    if (!is_glob(expanded)) return std::vector<std::string>(1, pattern);

    // Keys nested under an entry (`labels:`) look just like an entry, so only
    // lines at the indent of the first one count as a service.
    const std::pair<size_t, size_t> range = block(lines);
    std::vector<std::string> names;
    size_t depth = 0;
    bool have_depth = false;
    for (size_t i = range.first; i < range.second; i++) {
        size_t indent;
        std::string name;
        if (!entry_name(lines[i], &indent, &name)) continue;
        if (!have_depth) {
            depth = indent;
            have_depth = true;
        }
        if (indent == depth) names.push_back(name);
    }

    const std::vector<std::string> hits = filter(names, expanded);
    if (hits.empty()) {
        // Name the pattern the alias stands for, or "nothing matches cs" is a
        // puzzle rather than a message.
        const std::string shown = (expanded == pattern)
                                      ? format("'%s'", pattern.c_str())
                                      : format("'%s' (%s)", pattern.c_str(),
                                               expanded.c_str());
        fail(format("nothing in %s matches %s", VALUES, shown.c_str()));
    }
    return hits;
}

void set_key(Lines &lines, const std::string &service, const std::string &key,
             const std::string &value) {
    size_t start, end;
    if (!find_service(lines, service, &start, &end)) {
        fail(format("'%s' is not in %s", service.c_str(), VALUES));
    }

    for (size_t k = start + 1; k < end; k++) {
        if (!is_key_line(lines[k], key)) continue;
        // Keep any trailing comment: `enabled: true  # kept off for ops` is a
        // note about the service, not about the value it happened to have.
        const size_t hash = lines[k].find('#');
        std::string note;
        if (hash != std::string::npos) note = "  " + rstrip(lines[k].substr(hash));
        lines[k] = format("%s%s: %s%s\n", std::string(indent_of(lines[k]), ' ').c_str(),
                          key.c_str(), value.c_str(), note.c_str());
        return;
    }

    lines.insert(lines.begin() + static_cast<long>(end),
                 format("%s%s: %s\n", std::string(indent_of(lines[start]) + 2, ' ').c_str(),
                        key.c_str(), value.c_str()));
}
