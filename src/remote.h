// Editing a deployment repo without a checkout.
//
// Each [repos] entry is a clone URL. The tool keeps a bare clone of it at
// $XDG_CACHE_HOME/deployment-repo-tool/<name>.git (~/.cache when unset), named
// after the repo, which it owns and nobody edits: there is no working tree to
// be dirty, no local branch to diverge and nothing to set up by hand. The first
// run clones it; every run after that fetches.
//
// A change is made the way git itself would make it, minus the files:
//
//   fetch, then read apps/values.yaml straight out of the branch tip
//   write the edited text as a blob, and a tree with it in place of the old one
//   a commit of that tree whose parent is exactly the tip that was read
//   push that commit to the branch
//
// The push is a plain fast-forward. If someone else pushed in between, the
// server refuses it, nothing is changed and the command can simply be run
// again; there is deliberately no automatic retry or rebase.
//
// Everything is git on the command line, authenticated however the user's git
// already is (an SSH key for the gitlab.diamond.ac.uk URLs), and the commit is
// theirs: author and committer come from their git config, as before.
#pragma once

#include <string>

// What was read, so the commit can be made on top of exactly that.
struct Snapshot {
    std::string cache;   // the bare clone
    std::string branch;  // the remote's default branch
    std::string commit;  // its tip when fetched
    std::string text;    // apps/values.yaml at that commit
};

// The repo's name from its URL, without any .git: fe-deployment for
// git@gitlab.diamond.ac.uk:controls/.../fe-deployment.git. Names the cache, and
// the Argo CD parent app is inferred from it.
std::string repo_name(const std::string &url);

// Where the tool keeps its clone of url, whether or not it exists yet.
std::string cache_path(const std::string &url);

// Clone if there is no cache yet, fetch, and read the values file. Read-only
// as far as the remote is concerned, so --dry-run and status use it too.
Snapshot fetch_values(const std::string &url);

// Commit text as the new values file on top of what was read, and push it.
// Stops the tool if the push is refused.
void push_values(const std::string &url, const Snapshot &read, const std::string &text,
                 const std::string &message);
