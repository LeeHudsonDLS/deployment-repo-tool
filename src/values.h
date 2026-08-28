// Editing the services block of apps/values.yaml.
//
// The file is treated as lines of text and never loaded by a YAML parser, on
// purpose: re-dumping the document would throw away the comments, key order and
// spacing that make these files readable, and turn every commit into a diff
// nobody can review. Only the lines that have to change are rewritten, so
// everything else in the file comes out byte for byte as it went in.
//
// Each line keeps its trailing newline, the way python's readlines() handed
// them over, so rewriting a line means replacing the whole string.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "config.h"

using Lines = std::vector<std::string>;

// The file this tool edits, relative to the root of a deployment repo.
extern const char *VALUES;

size_t indent_of(const std::string &line);

// Line range of the service entries: the first one, and one past the last.
// Everything indented belongs to the block, so it ends at the next line
// starting in column 0. Trailing blank lines are left out so that a new entry
// is appended tight against the last one rather than after a gap.
std::pair<size_t, size_t> block(const Lines &lines);

// Where one service is: its `  name:` line, and one past its last child line. A
// service owns every following line indented deeper than its own name, so a
// `labels:` block and anything nested under it comes along too.
//
// False when the name is not there, which for deploy means "add it". An entry
// written on one line (`name: {enabled: true}`) does not match, so it is
// reported as missing rather than mangled -- the safe direction to be wrong in.
bool find_service(const Lines &lines, const std::string &service, size_t *start,
                  size_t *end);

// The services a pattern picks out, in the order the file lists them.
//
// A shorthand from the repo's aliases becomes its pattern first. A plain name
// is passed straight through even if the file has never heard of it, so that
// deploy can add it; a glob only ever selects entries that are already there.
std::vector<std::string> select(const Lines &lines, const std::string &pattern,
                                const Options &aliases);

// Set `key: value` for the service, rewriting its line or adding one. A key
// that is not there yet goes at the end of the entry, indented one level deeper
// than the service name. Rewriting an existing key keeps any trailing `#`
// comment, so a note beside `enabled:` is not lost.
void set_key(Lines &lines, const std::string &service, const std::string &key,
             const std::string &value);
