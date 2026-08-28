#include "diff.h"

#include <algorithm>

#include "support.h"

namespace {

// How much context each hunk carries, and therefore how far apart two changes
// have to be before they are reported separately. Three is what every diff
// tool uses and what difflib defaulted to.
const size_t CONTEXT = 3;

// An edit script is a run of these. i1..i2 indexes the old lines, j1..j2 the
// new ones; for Equal the two runs are the same length.
enum Tag { Equal, Replace, Delete, Insert };

struct Op {
    Tag tag;
    size_t i1, i2, j1, j2;
};

// The most differing lines the search will look through before giving up. The
// trace it has to keep grows with the square of this, and a diff of more than a
// thousand changed lines is not one anybody reads anyway, so past this point
// the caller falls back to calling the whole region replaced. In practice a
// command changes a handful of lines and the search ends within a few steps.
const long EDIT_LIMIT = 2000;

// Myers' algorithm: walk one diagonal further each round, furthest reaching
// path first, and stop as soon as the end of both files is reached. The work is
// proportional to the number of differing lines rather than to the size of the
// file, which suits this tool exactly.
//
// `matches` comes back as the pairs of line numbers that line up. False means
// no edit script was found within EDIT_LIMIT.
bool myers_matches(const std::vector<std::string> &a, const std::vector<std::string> &b,
                   size_t from_a, size_t from_b, size_t to_a, size_t to_b,
                   std::vector<std::pair<size_t, size_t>> *matches) {
    const long n = static_cast<long>(to_a - from_a);
    const long m = static_cast<long>(to_b - from_b);
    if (n == 0 && m == 0) return true;
    const long limit = std::min(n + m, EDIT_LIMIT);

    // The furthest x reached on each diagonal k, indexed by k + offset. Only
    // |k| <= limit + 1 is ever touched, so the array is sized by the limit
    // rather than by the file.
    const long offset = limit + 1;
    std::vector<long> v(static_cast<size_t>(2 * limit + 3), 0);
    // One snapshot per round, holding just the diagonals that round can reach
    // and their two neighbours: trace[d] covers k in [-d-1, d+1], indexed by
    // k + d + 1. Keeping the whole array each round would cost the square of
    // the file length instead of the square of the number of changes.
    std::vector<std::vector<long>> trace;

    for (long d = 0; d <= limit; d++) {
        trace.push_back(std::vector<long>(v.begin() + (offset - d - 1),
                                          v.begin() + (offset + d + 2)));
        for (long k = -d; k <= d; k += 2) {
            long x;
            if (k == -d || (k != d && v[static_cast<size_t>(k - 1 + offset)] <
                                          v[static_cast<size_t>(k + 1 + offset)])) {
                x = v[static_cast<size_t>(k + 1 + offset)];  // came down: a line inserted
            } else {
                x = v[static_cast<size_t>(k - 1 + offset)] + 1;  // came right: deleted
            }
            long y = x - k;
            // Then follow the diagonal as far as the lines keep agreeing.
            while (x < n && y < m &&
                   a[from_a + static_cast<size_t>(x)] == b[from_b + static_cast<size_t>(y)]) {
                x++;
                y++;
            }
            v[static_cast<size_t>(k + offset)] = x;
            if (x < n || y < m) continue;

            // The end: walk the snapshots back to recover which lines matched.
            long back_x = n, back_y = m;
            for (long step = d; step >= 0; step--) {
                const std::vector<long> &previous = trace[static_cast<size_t>(step)];
                const long back_k = back_x - back_y;
                const long base = step + 1;  // index of diagonal 0 in the snapshot
                long previous_k;
                if (back_k == -step ||
                    (back_k != step && previous[static_cast<size_t>(back_k - 1 + base)] <
                                           previous[static_cast<size_t>(back_k + 1 + base)])) {
                    previous_k = back_k + 1;
                } else {
                    previous_k = back_k - 1;
                }
                const long previous_x = previous[static_cast<size_t>(previous_k + base)];
                const long previous_y = previous_x - previous_k;
                while (back_x > previous_x && back_y > previous_y) {
                    back_x--;
                    back_y--;
                    matches->push_back(std::make_pair(from_a + static_cast<size_t>(back_x),
                                                      from_b + static_cast<size_t>(back_y)));
                }
                back_x = previous_x;
                back_y = previous_y;
            }
            std::reverse(matches->begin(), matches->end());
            return true;
        }
    }
    return false;
}

void add_equal(std::vector<Op> *ops, size_t i, size_t j, size_t length) {
    if (length == 0) return;
    // Extend the run in progress rather than starting another, so that the
    // trimmed prefix and the first matches found join up into one block.
    if (!ops->empty() && ops->back().tag == Equal && ops->back().i2 == i &&
        ops->back().j2 == j) {
        ops->back().i2 += length;
        ops->back().j2 += length;
        return;
    }
    Op op = {Equal, i, i + length, j, j + length};
    ops->push_back(op);
}

void add_change(std::vector<Op> *ops, size_t i1, size_t i2, size_t j1, size_t j2) {
    if (i1 == i2 && j1 == j2) return;
    Tag tag = Replace;
    if (i1 == i2) tag = Insert;
    if (j1 == j2) tag = Delete;
    Op op = {tag, i1, i2, j1, j2};
    ops->push_back(op);
}

std::vector<Op> opcodes(const std::vector<std::string> &a,
                        const std::vector<std::string> &b) {
    // Trim the parts that obviously agree first. For this tool that is almost
    // the whole file, which keeps the search below down to the few lines an
    // edit actually touched.
    size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) prefix++;
    size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        suffix++;
    }

    std::vector<Op> ops;
    add_equal(&ops, 0, 0, prefix);

    std::vector<std::pair<size_t, size_t>> matches;
    if (myers_matches(a, b, prefix, prefix, a.size() - suffix, b.size() - suffix,
                      &matches)) {
        size_t i = prefix, j = prefix;
        for (size_t index = 0; index < matches.size(); index++) {
            const size_t match_i = matches[index].first, match_j = matches[index].second;
            add_change(&ops, i, match_i, j, match_j);
            add_equal(&ops, match_i, match_j, 1);
            i = match_i + 1;
            j = match_j + 1;
        }
        add_change(&ops, i, a.size() - suffix, j, b.size() - suffix);
    } else {
        add_change(&ops, prefix, a.size() - suffix, prefix, b.size() - suffix);
    }

    add_equal(&ops, a.size() - suffix, b.size() - suffix, suffix);
    return ops;
}

// Break the edit script into hunks: at most CONTEXT unchanged lines at each
// end, and a run of more than twice that in the middle splits it in two.
std::vector<std::vector<Op>> grouped(const std::vector<Op> &ops) {
    std::vector<std::vector<Op>> groups;
    if (ops.empty()) return groups;

    std::vector<Op> codes = ops;
    if (codes.front().tag == Equal) {
        Op &first = codes.front();
        const size_t keep = std::min(CONTEXT, first.i2 - first.i1);
        first.i1 = first.i2 - keep;
        first.j1 = first.j2 - keep;
    }
    if (codes.back().tag == Equal) {
        Op &last = codes.back();
        const size_t keep = std::min(CONTEXT, last.i2 - last.i1);
        last.i2 = last.i1 + keep;
        last.j2 = last.j1 + keep;
    }

    std::vector<Op> group;
    for (size_t index = 0; index < codes.size(); index++) {
        Op op = codes[index];
        if (op.tag == Equal && op.i2 - op.i1 > 2 * CONTEXT) {
            Op tail = op;
            tail.i2 = tail.i1 + CONTEXT;
            tail.j2 = tail.j1 + CONTEXT;
            group.push_back(tail);
            groups.push_back(group);
            group.clear();
            op.i1 = op.i2 - CONTEXT;
            op.j1 = op.j2 - CONTEXT;
        }
        group.push_back(op);
    }
    // A group holding nothing but context is the tail of the last hunk, already
    // reported.
    if (!group.empty() && !(group.size() == 1 && group.front().tag == Equal)) {
        groups.push_back(group);
    }
    return groups;
}

// "@@ -12,7 +12,7 @@" style ranges: a single line is written without a length,
// and an empty range is numbered from the line before it.
std::string range(size_t start, size_t stop) {
    const size_t length = stop - start;
    if (length == 1) return format("%zu", start + 1);
    if (length == 0) return format("%zu,0", start);
    return format("%zu,%zu", start + 1, length);
}

}  // namespace

std::string unified_diff(const std::vector<std::string> &before,
                         const std::vector<std::string> &after,
                         const std::string &from_file, const std::string &to_file) {
    const std::vector<std::vector<Op>> groups = grouped(opcodes(before, after));
    if (groups.empty()) return std::string();

    std::string out = "--- " + from_file + "\n+++ " + to_file + "\n";
    for (const std::vector<Op> &group : groups) {
        const Op &first = group.front();
        const Op &last = group.back();
        out += "@@ -" + range(first.i1, last.i2) + " +" + range(first.j1, last.j2) + " @@\n";
        for (const Op &op : group) {
            if (op.tag != Insert) {
                for (size_t i = op.i1; i < op.i2; i++) {
                    out += (op.tag == Equal ? " " : "-") + before[i];
                }
            }
            if (op.tag == Replace || op.tag == Insert) {
                for (size_t j = op.j1; j < op.j2; j++) out += "+" + after[j];
            }
        }
    }
    return out;
}
