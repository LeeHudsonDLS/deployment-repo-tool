#include "repo.h"

#include <iostream>

#include "glob.h"
#include "paths.h"
#include "remote.h"
#include "support.h"

namespace {

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
// Accepting `-r VA` here would find the URL but then look for shorthand in a
// section that does not exist.
bool is_configured(const Config &config, const std::string &key) {
    for (const std::pair<std::string, std::string> &repo : config.section("repos")) {
        if (repo.first == key) return true;
    }
    return false;
}

std::string repo_url(const Config &config, const std::string &key) {
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
            Repo repo = {repo_url(config, wanted), wanted, "-r " + wanted};
            return repo;
        }
        fail(format("no repo '%s' in the config; there is %s", wanted.c_str(),
                    keys.empty() ? "no [repos] section" : join(keys, ", ").c_str()));
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
        Repo repo = {repo_url(config, owners[0]), owners[0],
                     format("'%s' is a %s shorthand", service.c_str(), owners[0].c_str())};
        return repo;
    }

    for (const std::pair<std::string, std::string> &entry : config.section("match")) {
        for (const std::string &pattern : patterns(entry.second)) {
            if (!fnmatch(service, pattern)) continue;
            if (!is_configured(config, entry.first)) {
                fail(format("[match] has '%s' but [repos] gives it no URL",
                            entry.first.c_str()));
            }
            Repo repo = {repo_url(config, entry.first), entry.first,
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
        Repo repo = {repo_url(config, fallback), fallback,
                     "the configured default"};
        return repo;
    }

    fail(format("cannot tell which repo '%s' belongs to - use -r%s",
                service.c_str(),
                keys.empty() ? "" : ("; there is " + join(keys, ", ")).c_str()));
}

std::string shorthand_hint(const Config &config, const std::string &name,
                           const std::string &key) {
    // `stop cs -r va` otherwise gets the flat "no service
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

void show_repos(const Config &config, const std::string &config_path) {
    std::cout << "config  " << (config_path.empty() ? "none found" : config_path) << "\n";
    const Options &repos = config.section("repos");
    if (repos.empty()) {
        std::cout << "no [repos] section" << std::endl;
        return;
    }
    for (const std::pair<std::string, std::string> &repo : repos) {
        // Not fetched is not a problem: the first command that needs it clones.
        const char *state = is_dir(cache_path(repo.second)) ? "cached" : "not cloned";
        std::cout << format("%-10s %-11s %s", repo.first.c_str(), state, repo.second.c_str())
                  << "\n";
    }
    std::cout.flush();
}
