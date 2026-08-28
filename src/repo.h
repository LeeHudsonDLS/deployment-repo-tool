// Working out which deployment repo a command means.
//
// The tool edits repos you are not necessarily standing in, so the reason it
// picked one is carried back alongside the path and printed before anything
// changes. The order the sources are tried in is deliberate and is the thing to
// leave alone: the current directory beats anything inferred from the service
// name, because standing in your own clone and having the tool quietly edit and
// push from a different one is a foot-gun.
#pragma once

#include <string>

#include "config.h"

struct Repo {
    std::string root;  // absolute path to the checkout
    std::string key;   // the [repos] name, empty if it was given as a path
    std::string why;   // how it was chosen, for the line printed to the user
};

// -r, then the current directory, then the service name (its shorthand's repo,
// else the [match] patterns), then [general] default. Stops the tool if none of
// them answers -- a name that matches nothing is usually a typo, and an error
// naming the configured repos is more use than quietly picking one.
Repo resolve_repo(const Config &config, const std::string &wanted,
                  const std::string &service);

// Where else a name is a shorthand, for when it means nothing in this repo.
// Empty unless some other repo defines it.
std::string shorthand_hint(const Config &config, const std::string &name,
                           const std::string &key);

// Refuse anything that is not a deployment repo, however we got here.
void check_repo(const std::string &root, const std::string &why);

// --list: what is configured, and whether it is actually there.
void show_repos(const Config &config, const std::string &config_path);
