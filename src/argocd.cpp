#include "argocd.h"

#include <iostream>

#include "process.h"
#include "paths.h"
#include "support.h"

namespace {
int run_argocd(const std::vector<std::string> &args) {
    const std::string helper = path_join(dirname(own_path()), "ioc-argocd");
    if (!is_file(helper)) fail("ioc-argocd is missing; restore it beside the ioc binary");
    std::vector<std::string> command = {"bash", helper};
    command.insert(command.end(), args.begin(), args.end());
    return run(command);
}
bool valid_name(const std::string &name) {
    return !name.empty() && name[0] != '-' && name[0] != '.' &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") == std::string::npos;
}
}

void prepare_argocd(const std::string &parent) {
    if (run_argocd({"preflight", parent}) != 0) {
        fail("Argo CD setup or access check failed. No Git operations have been performed");
    }
}

void check_sync_names(const std::string &parent, const std::vector<std::string> &services) {
    const size_t slash = parent.find('/');
    if (!(slash == std::string::npos ? valid_name(parent) :
          valid_name(parent.substr(0, slash)) && valid_name(parent.substr(slash + 1)))) {
        fail("--force-sync needs --argocd-app APP or [<repo>.argocd] app "
             "(name or namespace/name)");
    }
    for (const auto &service : services) {
        if (!valid_name(service)) fail("invalid Argo CD service app name: " + service);
    }
}

void sync_apps(const std::string &parent, const std::vector<std::string> &services,
               bool dry_run) {
    const size_t slash = parent.find('/');
    const std::string prefix = slash == std::string::npos ? "" : parent.substr(0, slash + 1);
    auto command = [&](const std::vector<std::string> &argv) {
        std::cout << (dry_run ? "would run: " : "running: ") << join(argv, " ") << std::endl;
        if (!dry_run && run_argocd(std::vector<std::string>(argv.begin() + 1, argv.end())) != 0) {
            fail("Argo CD failed; Git changes remain pushed. Check the reported app, "
                 "login, permissions and manual-sync window policy, then rerun "
                 "the same command with --force-sync to retry");
        }
    };
    command({"argocd", "app", "get", parent, "--hard-refresh"});
    // Only apply the selected child definitions. A full parent sync could
    // release pending changes for other IOCs while the machine is running.
    std::vector<std::string> parent_sync = {
        "argocd", "app", "sync", parent, "--timeout", "300"};
    for (const auto &service : services) {
        parent_sync.push_back("--resource");
        parent_sync.push_back("argoproj.io:Application:" + prefix + service);
    }
    command(parent_sync);
    for (const auto &service : services) {
        command({"argocd", "app", "get", prefix + service, "--hard-refresh"});
        command({"argocd", "app", "sync", prefix + service, "--prune", "--timeout", "300"});
    }
}
