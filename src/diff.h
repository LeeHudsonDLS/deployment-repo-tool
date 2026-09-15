// The unified diff printed before anything is written or committed.
//
// Showing the change is the point of the tool as much as making it: the diff is
// what you read to confirm it picked the services you meant before it pushes.
// System diff produces unified output with three lines of context. Inputs and
// captured output use anonymous temporary files; errors stop before writing.
//
// Lines are expected to carry their own trailing newline, as they do everywhere
// else in the tool.
#pragma once

#include <string>
#include <vector>

std::string unified_diff(const std::vector<std::string> &before,
                         const std::vector<std::string> &after,
                         const std::string &from_file, const std::string &to_file);
