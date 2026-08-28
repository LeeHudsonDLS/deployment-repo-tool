#include "glob.h"

namespace {

// A bracket expression starting at `open` (which must be a '['). Returns false
// when there is no closing ']' at all, which fnmatch treats as a literal '['
// rather than an error -- so `stop fe[15` looks for a service with a bracket in
// its name and finds nothing, instead of blowing up.
//
// The scan mirrors fnmatch.translate exactly: a '!' immediately after the
// bracket negates, and a ']' immediately after that (or after the '[') is an
// ordinary member rather than the end.
bool parse_class(const std::string &pattern, size_t open, size_t *content_start,
                 size_t *content_end, size_t *after, bool *negate) {
    size_t i = open + 1;
    *negate = false;
    if (i < pattern.size() && pattern[i] == '!') {
        *negate = true;
        i++;
    }
    *content_start = i;
    if (i < pattern.size() && pattern[i] == ']') i++;
    while (i < pattern.size() && pattern[i] != ']') i++;
    if (i >= pattern.size()) return false;
    *content_end = i;
    *after = i + 1;
    return true;
}

// One character against the inside of a bracket expression. `-` is a range only
// when it has a character on each side; at either end it is a literal, which is
// how a regex character class reads it and therefore how fnmatch does.
bool class_matches(const std::string &pattern, size_t start, size_t end, bool negate,
                   char c) {
    const unsigned char target = static_cast<unsigned char>(c);
    bool hit = false;
    size_t i = start;
    while (i < end) {
        if (i + 2 < end && pattern[i + 1] == '-') {
            const unsigned char low = static_cast<unsigned char>(pattern[i]);
            const unsigned char high = static_cast<unsigned char>(pattern[i + 2]);
            if (target >= low && target <= high) hit = true;
            i += 3;
        } else {
            if (static_cast<unsigned char>(pattern[i]) == target) hit = true;
            i += 1;
        }
    }
    return negate ? !hit : hit;
}

}  // namespace

bool fnmatch(const std::string &name, const std::string &pattern) {
    const size_t n = name.size(), m = pattern.size();
    size_t i = 0, j = 0;
    // Where to resume from if the current '*' turns out to have swallowed too
    // little. Backtracking to the most recent star is enough for these
    // patterns and keeps the cost at O(n*m); a recursive matcher on something
    // like `*a*a*a*a*` would not.
    size_t star = m, resume = 0;

    while (i < n) {
        bool matched = false;
        if (j < m) {
            const char p = pattern[j];
            if (p == '*') {
                star = j;
                resume = i;
                j++;
                continue;
            }
            if (p == '?') {
                matched = true;
                j++;
            } else if (p == '[') {
                size_t start, end, after;
                bool negate;
                if (parse_class(pattern, j, &start, &end, &after, &negate)) {
                    if (class_matches(pattern, start, end, negate, name[i])) {
                        matched = true;
                        j = after;
                    }
                } else if (name[i] == '[') {  // unclosed: an ordinary character
                    matched = true;
                    j++;
                }
            } else if (name[i] == p) {
                matched = true;
                j++;
            }
        }
        if (matched) {
            i++;
            continue;
        }
        if (star == m) return false;
        resume++;
        i = resume;
        j = star + 1;
    }

    while (j < m && pattern[j] == '*') j++;
    return j == m;
}

bool is_glob(const std::string &pattern) {
    return pattern.find_first_of("*?[") != std::string::npos;
}

std::vector<std::string> filter(const std::vector<std::string> &names,
                                const std::string &pattern) {
    std::vector<std::string> hits;
    for (const std::string &name : names) {
        if (fnmatch(name, pattern)) hits.push_back(name);
    }
    return hits;
}
