// Working out which deployment repo a command means.
//
// The reason it picked one is carried back alongside the URL and printed
// before anything changes, since the repo is never one you are standing in:
// the tool works on its own clone of it (remote.h), wherever it is run from.
#pragma once

#include <string>

#include "config.h"

struct Repo {
    std::string url;  // what to clone, fetch and push
    std::string key;  // the [repos] name
    std::string why;  // how it was chosen, for the line printed to the user
};

// -r, then the service name (its shorthand's repo, else the [match] patterns),
// then [general] default. Stops the tool if none of them answers -- a name that
// matches nothing is usually a typo, and an error naming the configured repos
// is more use than quietly picking one.
Repo resolve_repo(const Config &config, const std::string &wanted,
                  const std::string &service);

// Where else a name is a shorthand, for when it means nothing in this repo.
// Empty unless some other repo defines it.
std::string shorthand_hint(const Config &config, const std::string &name,
                           const std::string &key);

// --list: what is configured, and whether the tool has a clone of it yet.
void show_repos(const Config &config, const std::string &config_path);
