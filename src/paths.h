// Path handling, matching what python's os.path did for the original.
//
// These are all *lexical* operations: nothing here needs the disk's view of a
// path, only its spelling. <filesystem> is avoided -- it is a separate library
// on the gcc 8 that ships with RHEL8, and the tool being buildable with nothing
// but a compiler is the whole reason it is not python any more.
#pragma once

#include <string>

// $VAR and ${VAR}; an undefined name is left alone, as os.path.expandvars does.
std::string expandvars(const std::string &path);

// A leading ~ or ~user. $HOME first, then the password database; a user who
// cannot be looked up leaves the path untouched.
std::string expanduser(const std::string &path);

// Lexical cleanup: collapse repeated slashes, drop "." and resolve ".."
// without touching the disk.
std::string normpath(const std::string &path);

std::string abspath(const std::string &path);

// The full treatment a configured path gets: variables, then ~, then made
// absolute. Same order as the python's expand().
std::string expand(const std::string &path);

std::string path_join(const std::string &head, const std::string &tail);
std::string basename(const std::string &path);
std::string dirname(const std::string &path);

bool path_exists(const std::string &path);
bool is_file(const std::string &path);
bool is_dir(const std::string &path);

std::string current_directory();
