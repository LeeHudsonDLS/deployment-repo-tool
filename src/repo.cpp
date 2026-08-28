#include "repo.h"

#include <iostream>

#include "glob.h"
#include "paths.h"
#include "support.h"
#include "values.h"

namespace {

bool looks_like_a_deployment_repo(const std::string &root) {
    return lower(basename(root)).find("deployment") != std::string::npos;
}

// The deployment repo the current directory is in, or empty. Walks up to the
// top of the checkout so that running it from apps/ works.
std::string repo_from_cwd() {
    std::string root = current_directory();
    while (!path_exists(path_join(root, ".git"))) {
        const std::string parent = dirname(root);
        if (parent == root) return std::string();
        root = parent;
    }
    if (!looks_like_a_deployment_repo(root)) return std::string();
    // A checkout with nothing to edit is not the repo you meant. This tool's
    // own source tree is called deployment-repo-tool and so passes the name
    // test above; standing in it -- which the documented install makes likely,
    // since you clone it and build it there -- used to claim the command and
    // then refuse it. The current directory still beats anything inferred, but
    // only when it is really a deployment repo, and having the file is what
    // makes it one.
    if (!is_file(path_join(root, VALUES))) return std::string();
    return root;
}

// The names of the configured repos, for the "there is fe, va, ..." tail on an
// error. Order is the config's.
std::vector<std::string> repo_keys(const Config &config) {
    std::vector<std::string> keys;
    for (const std::pair<std::string, std::string> &repo : config.section("repos")) {
        keys.push_back(repo.first);
    }
    return keys;
}

// Exact matches only, unlike Config::get: a repo key is also used to build the
// name of its `[<key>.aliases]` section, and section names are case sensitive.
// Accepting `-r VA` here would find the path but then look for shorthand in a
// section that does not exist.
bool is_configured(const Config &config, const std::string &key) {
    for (const std::pair<std::string, std::string> &repo : config.section("repos")) {
        if (repo.first == key) return true;
    }
    return false;
}

std::string repo_path(const Config &config, const std::string &key) {
    for (const std::pair<std::string, std::string> &repo : config.section("repos")) {
        if (repo.first == key) return repo.second;
    }
    return std::string();
}

}  // namespace

Repo resolve_repo(const Config &config, const std::string &wanted,
                  const std::string &service) {
    const std::vector<std::string> keys = repo_keys(config);

    if (!wanted.empty()) {
        if (is_configured(config, wanted)) {
            Repo repo = {expand(repo_path(config, wanted)), wanted, "-r " + wanted};
            return repo;
        }
        if (is_dir(wanted)) {
            Repo repo = {expand(wanted), std::string(), "-r, as a path"};
            return repo;
        }
        fail(format("no repo '%s' in the config; there is %s", wanted.c_str(),
                    keys.empty() ? "no [repos] section" : join(keys, ", ").c_str()));
    }

    // Standing in a checkout beats anything inferred: whoever cd'd in there
    // meant that copy, not whichever one the config happens to point at.
    const std::string here = repo_from_cwd();
    if (!here.empty()) {
        std::string key;
        for (const std::string &candidate : keys) {
            if (expand(repo_path(config, candidate)) == here) {
                key = candidate;
                break;
            }
        }
        Repo repo = {here, key, "current directory"};
        return repo;
    }

    // A shorthand belongs to the repo that defines it, so `stop cs` needs no
    // -r. If two repos define the same one, ask rather than guess.
    std::vector<std::string> owners;
    for (const std::string &key : keys) {
        if (config.has_option(key + ".aliases", service)) owners.push_back(key);
    }
    if (owners.size() > 1) {
        fail(format("'%s' is a shorthand in %s - use -r to say which", service.c_str(),
                    join(owners, " and ").c_str()));
    }
    if (owners.size() == 1) {
        Repo repo = {expand(repo_path(config, owners[0])), owners[0],
                     format("'%s' is a %s shorthand", service.c_str(), owners[0].c_str())};
        return repo;
    }

    for (const std::pair<std::string, std::string> &entry : config.section("match")) {
        for (const std::string &pattern : patterns(entry.second)) {
            if (!fnmatch(service, pattern)) continue;
            if (!is_configured(config, entry.first)) {
                fail(format("[match] has '%s' but [repos] gives it no path",
                            entry.first.c_str()));
            }
            Repo repo = {expand(repo_path(config, entry.first)), entry.first,
                         format("'%s' matches %s", service.c_str(), pattern.c_str())};
            return repo;
        }
    }

    const std::string fallback = config.get("general", "default");
    if (!fallback.empty()) {
        if (!is_configured(config, fallback)) {
            fail(format("[general] default is '%s', which is not in [repos]",
                        fallback.c_str()));
        }
        Repo repo = {expand(repo_path(config, fallback)), fallback,
                     "the configured default"};
        return repo;
    }

    fail(format("cannot tell which repo '%s' belongs to - use -r, or cd into one%s",
                service.c_str(),
                keys.empty() ? "" : ("; there is " + join(keys, ", ")).c_str()));
}

std::string shorthand_hint(const Config &config, const std::string &name,
                           const std::string &key) {
    // Typing `stop cs` in the va checkout otherwise gets the flat "no service
    // 'cs'", which does not say that cs is a real thing somewhere else.
    std::vector<std::string> owners;
    for (const std::string &candidate : repo_keys(config)) {
        if (candidate != key && config.has_option(candidate + ".aliases", name)) {
            owners.push_back(candidate);
        }
    }
    if (owners.empty()) return std::string();
    return format("\n  ('%s' is a shorthand in %s - try -r %s)", name.c_str(),
                  join(owners, ", ").c_str(), owners[0].c_str());
}

void check_repo(const std::string &root, const std::string &why) {
    if (!looks_like_a_deployment_repo(root)) {
        fail(format("%s (%s) is not a deployment repo", root.c_str(), why.c_str()));
    }
    if (!is_file(path_join(root, VALUES))) {
        fail(format("%s (%s) has no %s", root.c_str(), why.c_str(), VALUES));
    }
}

void show_repos(const Config &config, const std::string &config_path) {
    std::cout << "config  " << (config_path.empty() ? "none found" : config_path) << "\n";
    const Options &repos = config.section("repos");
    if (repos.empty()) {
        std::cout << "no [repos] section" << std::endl;
        return;
    }
    for (const std::pair<std::string, std::string> &repo : repos) {
        const std::string root = expand(repo.second);
        const char *state = is_file(path_join(root, VALUES)) ? "ok" : "NOT THERE";
        std::cout << format("%-10s %-10s %s", repo.first.c_str(), state, root.c_str())
                  << "\n";
    }
    std::cout.flush();
}
