// The unified diff printed before anything is written or committed.
//
// Showing the change is the point of the tool as much as making it: the diff is
// what you read to confirm it picked the services you meant before it pushes.
// python had difflib; this is the same output -- three lines of context, hunks
// merged when they nearly touch -- produced from a Myers diff.
//
// Lines are expected to carry their own trailing newline, as they do everywhere
// else in the tool.
#pragma once

#include <string>
#include <vector>

std::string unified_diff(const std::vector<std::string> &before,
                         const std::vector<std::string> &after,
                         const std::string &from_file, const std::string &to_file);
