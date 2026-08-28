// Small string helpers and the one way this tool gives up.
//
// The python this was converted from leant on the standard library for all of
// this; C++ has no equivalents, so each of the handful of operations the tool
// actually needs is spelled out once here rather than open-coded at every call
// site. Everything is deliberately narrow: these are not general utilities.
#pragma once

#include <string>
#include <vector>

// Anything the tool refuses: the message on stderr, exit status 1. The "error:"
// prefix and the wording of each message are what the test suite matches on, so
// they are part of the interface, not decoration.
[[noreturn]] void fail(const std::string &message);

// printf into a std::string. Used to keep the message text next to the format
// it came from, exactly as the python's % formatting did.
std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Whitespace here means what python's str.strip() means -- space, tab, newline,
// carriage return, vertical tab, form feed -- so that a values.yaml written
// with CRLF line endings is read the same way python read it.
bool is_space(char c);
std::string lstrip(const std::string &s);
std::string rstrip(const std::string &s);
std::string strip(const std::string &s);

bool starts_with(const std::string &s, const std::string &prefix);
bool ends_with(const std::string &s, const std::string &suffix);

// ASCII lowercase. Config option names are folded with this, because that is
// what configparser did to them and the shipped config relies on nothing else.
std::string lower(const std::string &s);

// A comma or space separated list, empties dropped: python's
// re.split(r"[,\s]+", value.strip()) without the regex engine.
std::vector<std::string> split_list(const std::string &value);

std::string join(const std::vector<std::string> &parts, const std::string &sep);

// The whole file as lines, each keeping its trailing newline, the way python's
// readlines() hands them over. The editing code depends on that: a line is
// rewritten by replacing it wholesale, newline included.
std::vector<std::string> split_lines(const std::string &text);

// Reading and writing are wrapped so that an unreadable or unwritable file
// stops the tool with a message naming it, rather than silently producing an
// empty document or a half written one.
std::string read_file(const std::string &path);
void write_file(const std::string &path, const std::string &text);
