// The command line.
//
// This is what python's argparse did for the original, kept to the shape the
// tool actually uses: three optional positionals and a handful of switches,
// with options and positionals allowed in any order. Long options may be
// abbreviated as long as the abbreviation is unambiguous, and `--repo=va`,
// `--repo va`, `-r va` and `-rva` are all accepted, because argparse took all
// of those and fingers remember.
//
// A bad command line exits 2, separate from the 1 the tool uses for anything it
// refuses, so a script can tell "you typed it wrong" from "it would not do it".
#pragma once

#include <cstddef>
#include <string>

#include "config.h"

// What the tool can be asked to do. `script` is null for the actions that edit
// the repo, and names the shell script to hand the service to for the two that
// act on the cluster instead. Adding a cluster action is a line here and a
// script in the build-time embedded helpers.
struct Action {
    const char *name;
    const char *script;
};

extern const Action ACTIONS[];
extern const size_t ACTION_COUNT;

// Null if the name is not an action at all.
const Action *find_action(const std::string &name);

struct Args {
    std::string action;
    std::string service;
    std::string revision;
    std::string repo;    // -r/--repo
    std::string config;  // --config
    bool help = false;
    bool list = false;
    bool no_git = false;
    bool dry_run = false;
    bool force_sync = false;
    std::string argocd_app;
};

Args parse_args(int argc, char **argv);
void show_help(const Config &config);

// Reports a mistyped command the way argparse did -- usage, then the message --
// and exits 2.
[[noreturn]] void usage_error(const std::string &message);
