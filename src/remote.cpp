#include "remote.h"

#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <vector>

#include "paths.h"
#include "process.h"
#include "support.h"
#include "values.h"

namespace {

std::vector<std::string> in(const std::string &cache, const std::vector<std::string> &args) {
    std::vector<std::string> argv = {"git", "--git-dir", cache};
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
}

// Output attached, for the commands whose progress or prompts the user should
// see: the clone, the fetch's errors, the push.
void git(const std::string &cache, const std::vector<std::string> &args) {
    if (run(in(cache, args)) != 0) fail("git " + join(args, " ") + " failed");
}

// One line of output, the way the plumbing commands answer.
std::string ask(const std::string &cache, const std::vector<std::string> &args) {
    std::string out;
    if (run_capture(in(cache, args), &out) != 0) fail("git " + join(args, " ") + " failed");
    return rstrip(out);
}

}  // namespace

std::string cache_path(const std::string &url) {
    const char *xdg = std::getenv("XDG_CACHE_HOME");
    const std::string home = (xdg && *xdg) ? std::string(xdg) : expanduser("~/.cache");
    return path_join(path_join(home, "deployment-repo-tool"), repo_name(url) + ".git");
}

std::string repo_name(const std::string &url) {
    std::string name = url;
    while (ends_with(name, "/")) name.pop_back();
    if (ends_with(name, ".git")) name.resize(name.size() - 4);
    // scp-style URLs (host:group/name) have no slash before a top-level name.
    const size_t cut = name.find_last_of("/:");
    return cut == std::string::npos ? name : name.substr(cut + 1);
}

Snapshot fetch_values(const std::string &url) {
    Snapshot read;
    read.cache = cache_path(url);
    if (!is_dir(read.cache)) {
        std::cout << "cloning " << url << " into " << read.cache << std::endl;
        // git makes the leading directories. A bare clone records the remote's
        // default branch as HEAD, which is the branch every edit goes to.
        if (run({"git", "clone", "--bare", "--quiet", url, read.cache}) != 0) {
            fail("cannot clone " + url);
        }
    }
    // Always by URL rather than a configured remote, so the cache follows
    // builtin.ini if a URL there changes. Every branch, force-updated and
    // pruned: the cache mirrors the server and holds no work of its own.
    git(read.cache, {"fetch", "--quiet", "--prune", url, "+refs/heads/*:refs/heads/*"});

    read.branch = ask(read.cache, {"symbolic-ref", "--short", "HEAD"});
    read.commit = ask(read.cache, {"rev-parse", "--verify", "refs/heads/" + read.branch});
    if (run_capture(in(read.cache, {"show", read.commit + ":" + VALUES}), &read.text) != 0) {
        fail(format("%s has no %s on %s", url.c_str(), VALUES, read.branch.c_str()));
    }
    std::cout << "branch  " << read.branch << " at " << read.commit.substr(0, 8) << "\n";
    return read;
}

void push_values(const std::string &url, const Snapshot &read, const std::string &text,
                 const std::string &message) {
    // The blob goes in through a file, as run() has no stdin to give it.
    const std::string scratch = path_join(read.cache, "ioc-values");
    write_file(scratch, text);
    const std::string blob = ask(read.cache, {"hash-object", "-w", "--", scratch});
    unlink(scratch.c_str());

    // A private index, so the tree is built without a working tree and without
    // anything left in the cache's own index affecting it.
    const std::string index = path_join(read.cache, "ioc-index");
    unlink(index.c_str());
    setenv("GIT_INDEX_FILE", index.c_str(), 1);
    git(read.cache, {"read-tree", read.commit});
    git(read.cache, {"update-index", "--cacheinfo",
                     "100644," + blob + "," + VALUES});
    const std::string tree = ask(read.cache, {"write-tree"});
    unsetenv("GIT_INDEX_FILE");
    unlink(index.c_str());

    const std::string commit =
        ask(read.cache, {"commit-tree", tree, "-p", read.commit, "-m", message});
    // A fast-forward only, as any push without --force is: if the branch has
    // moved since the fetch, the server says so and nothing changes.
    if (run(in(read.cache, {"push", "--quiet", url,
                            commit + ":refs/heads/" + read.branch})) != 0) {
        fail("push refused: if someone else has pushed since, run the command again");
    }
    git(read.cache, {"update-ref", "refs/heads/" + read.branch, commit, read.commit});
    std::cout << "pushed  " << commit.substr(0, 8) << " to " << read.branch << std::endl;
}
