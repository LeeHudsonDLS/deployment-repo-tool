// Start, stop and deploy services in an ArgoCD "*-deployment" repo, and
// restart or open a shell in a running one.
//
// start, stop and deploy edit the services block of apps/values.yaml, then
// pull, commit and push. restart and exec change nothing: they hand the service
// name to a shell script that talks to the cluster, and are the ArgoCD web UI's
// pod delete and terminal.
//
// This file is the flow -- what happens in what order. The parts it is made of
// each live in their own header, and the reasoning behind them is written down
// there: config.h for the ini file, repo.h for choosing a checkout, values.h
// for the editing, glob.h and diff.h for the two standard library pieces python
// had and C++ does not.

#include <iostream>
#include <string>
#include <vector>

#include "cli.h"
#include "argocd.h"
#include "config.h"
#include "diff.h"
#include "glob.h"
#include "helpers.h"
#include "paths.h"
#include "process.h"
#include "repo.h"
#include "support.h"
#include "values.h"

namespace {

// How each action reads in a commit message.
const char *verb(const std::string &action) {
    if (action == "start") return "Starting";
    if (action == "stop") return "Stopping";
    return "Deploying";
}

// Run git in the repo; any failure stops the tool. Output stays attached to the
// terminal so progress and any credential prompt reach the user.
void git(const std::string &root, const std::vector<std::string> &args) {
    std::vector<std::string> argv;
    argv.push_back("git");
    argv.push_back("-C");
    argv.push_back(root);
    argv.insert(argv.end(), args.begin(), args.end());
    if (run(argv) != 0) fail("git " + join(args, " ") + " failed");
}

// Name the services for a message, without printing an essay for 'fe*'.
std::string summarise(const std::vector<std::string> &names, const std::string &pattern) {
    const std::string joined = join(names, ", ");
    if (joined.size() > 60) {
        return format("%zu services (%s)", names.size(), pattern.c_str());
    }
    return joined;
}

// An explicit override keeps its existing executable/shebang semantics.
// Otherwise use the script embedded when this binary was built.
std::string helper_script(const Config &config, const Action &action) {
    const std::string key = std::string(action.name) + "_script";

    const std::string configured = config.get("general", key);
    if (!configured.empty()) {
        const std::string script = expand(configured);
        if (!is_file(script)) {
            fail(format("[general] %s is %s, which is not there", key.c_str(),
                        script.c_str()));
        }
        return script;
    }

    return std::string();
}

// Carry out a cluster action by handing the service name to its script.
//
// Nothing here touches the deployment repo: restarting a pod or opening a shell
// in one changes no file, so there is nothing to commit and no repo to resolve.
// The scripts own everything about reaching the cluster -- finding kubectl,
// sourcing the klogin setup, checking the service is really there -- and this
// only locates the right one.
void on_cluster(const Config &config, const Action &action, const std::string &service,
                bool dry_run) {
    // One at a time on purpose. Restarting a glob's worth of IOCs is not
    // something to make easy to do by accident, and a shell in several pods at
    // once is not a thing.
    if (is_glob(service)) {
        fail(format("%s takes one service, not a glob", action.name));
    }

    const std::string script = helper_script(config, action);
    if (dry_run) {
        std::cout << "would run: " << (script.empty() ? action.script : script)
                  << (script.empty() ? " (embedded) " : " ") << service << std::endl;
        return;
    }

    // The script replaces this process, so its exit status is ours and a
    // failure cannot look like a success. The terminal is left alone: rollout
    // progress has to be readable, and for exec the remote shell needs the tty.
    const std::vector<std::string> argv = script.empty()
        ? embedded_helper(action.script, {service})
        : std::vector<std::string>{script, service};
    run_replacing_self(argv);
}

}  // namespace

int main(int argc, char **argv) {
    const Args args = parse_args(argc, argv);
    LoadedConfig loaded = read_config(args.config);
    const Config &config = loaded.config;
    if (args.help) show_help(config);

    if (args.force_sync && (args.no_git || args.list ||
                           args.action == "restart" || args.action == "exec")) {
        usage_error("--force-sync requires start, stop or deploy with git enabled");
    }
    if (!args.argocd_app.empty() && !args.force_sync) {
        usage_error("--argocd-app requires --force-sync");
    }

    if (args.list) {
        show_repos(config, loaded.path);
        return 0;
    }
    if (args.action.empty() || args.service.empty()) {
        usage_error("give an action and a service, e.g. stop sr22c-va-ioc-01");
    }
    if (args.action == "deploy" && args.revision.empty()) {
        usage_error("deploy needs a revision, e.g. deploy sr22c-va-ioc-01 2026_sd3");
    }

    const Action *action = find_action(args.action);

    // These act on the cluster, not the repo, so they take none of what
    // follows: no repo to pick, nothing to pull, edit, commit or push. The
    // third positional is deploy's revision and means nothing here; catching it
    // beats silently ignoring a mistyped command.
    if (action->script) {
        if (!args.revision.empty()) {
            usage_error(format("%s takes only a service name", action->name));
        }
        on_cluster(config, *action, args.service, args.dry_run);
        return 0;
    }

    const Repo repo = resolve_repo(config, args.repo, args.service);
    check_repo(repo.root, repo.why);
    std::cout << "repo    " << (repo.key.empty() ? "" : repo.key + "  ") << repo.root
              << "  (" << repo.why << ")\n";
    const std::string path = path_join(repo.root, VALUES);
    std::string parent = !args.argocd_app.empty() ? args.argocd_app :
        config.get(repo.key + ".argocd", "app");
    // Infer only from the checkout selected and validated above, never from
    // a service pattern or a config key which may name a different directory.
    if (args.force_sync && parent.empty()) {
        const std::string directory = basename(repo.root);
        const std::string suffix = "-deployment";
        if (ends_with(directory, suffix) && directory.size() > suffix.size()) {
            parent = "accelerator/" + directory.substr(0, directory.size() - suffix.size());
        }
    }
    if (args.force_sync) check_sync_names(parent, {});
    if (args.force_sync && !args.dry_run) prepare_argocd(parent);

    // Pull before reading, so the edit is made against what is really deployed.
    // --ff-only: if the branch has diverged, stop and let a human sort it out
    // rather than quietly building a merge on top of someone else's work.
    if (!(args.no_git || args.dry_run)) git(repo.root, {"pull", "--ff-only"});

    if (args.force_sync && !args.dry_run &&
        run({"git", "-C", repo.root, "diff", "--quiet", "HEAD", "--", VALUES}) != 0) {
        fail("--force-sync requires apps/values.yaml to have no uncommitted changes");
    }

    Lines lines = split_lines(read_file(path));
    // If the file does not end in a newline, appending an entry would join it
    // onto the last line.
    if (!lines.empty() && !ends_with(lines.back(), "\n")) lines.back() += "\n";
    const Lines before = lines;

    Options aliases;
    if (!repo.key.empty()) aliases = config.section(repo.key + ".aliases");
    const std::vector<std::string> targets = select(lines, args.service, aliases);
    if (args.force_sync) check_sync_names(parent, targets);
    if (targets.size() > 1) {
        std::cout << args.action << ": " << join(targets, ", ") << "\n";
    }

    for (const std::string &service : targets) {
        size_t start, end;
        // deploy is the only action allowed to introduce a service; start and
        // stop on an unknown name are far more likely to be a typo. A bare
        // `  name:` line is a complete entry, and set_key fills in the rest.
        if (!find_service(lines, service, &start, &end)) {
            if (args.action != "deploy") {
                fail(format("no service '%s' in %s%s", service.c_str(), VALUES,
                            shorthand_hint(config, service, repo.key).c_str()));
            }
            lines.insert(lines.begin() + static_cast<long>(block(lines).second),
                         "  " + service + ":\n");
            std::cout << "adding a new entry for " << service << "\n";
        }

        // A service with no `enabled` key is running, so start could just
        // delete the key. Writing it out explicitly matches how the file is
        // already kept, and leaves no doubt about the state it was put in.
        set_key(lines, service, "enabled", args.action == "stop" ? "false" : "true");
        if (args.action == "deploy") set_key(lines, service, "targetRevision", args.revision);
    }

    // Already in the wanted state: that is a success, not an error. It keeps the
    // tool safe to re-run and avoids an empty commit.
    if (lines == before) {
        std::cout << "nothing to do - " << summarise(targets, args.service)
                  << " already as asked for" << std::endl;
        if (args.force_sync) {
            // Retry a failed push too, without making an empty commit.
            if (!args.dry_run) git(repo.root, {"push"});
            sync_apps(parent, targets, args.dry_run);
        }
        return 0;
    }
    std::cout << unified_diff(before, lines, VALUES, VALUES);

    if (args.dry_run) {
        if (args.force_sync) sync_apps(parent, targets, true);
        std::cout.flush();
        return 0;
    }
    write_file(path, join(lines, ""));
    if (args.no_git) {
        std::cout.flush();
        return 0;
    }

    // Both commands name the file, so anything else staged in the repo is left
    // out of this commit.
    std::string message =
        format("%s %s", verb(args.action), summarise(targets, args.service).c_str());
    if (args.action == "deploy") message += " on " + args.revision;
    git(repo.root, {"add", "--", VALUES});
    git(repo.root, {"commit", "-m", message, "--", VALUES});
    git(repo.root, {"push"});
    if (args.force_sync) sync_apps(parent, targets, false);
    return 0;
}
