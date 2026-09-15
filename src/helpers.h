#pragma once

#include <string>
#include <vector>

// bash -c preserves stdin/TTY and takes arguments separately from script text.
// The helper name becomes $0 and args become $1 onwards. No temporary files.
std::vector<std::string> embedded_helper(const std::string &name,
                                         const std::vector<std::string> &args);
