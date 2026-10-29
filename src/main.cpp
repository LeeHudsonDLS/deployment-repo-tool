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

#include <algorithm>
#include <cstdlib>
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
#include "version.h"

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

// Columns padded to their widest cell, two spaces apart, like kubectl's.
void print_table(const std::vector<std::vector<std::string>> &rows) {
    std::vector<size_t> width;
    for (const auto &row : rows) {
        if (width.size() < row.size()) width.resize(row.size(), 0);
        for (size_t i = 0; i < row.size(); i++) width[i] = std::max(width[i], row[i].size());
    }
    for (const auto &row : rows) {
        std::string line;
        for (size_t i = 0; i < row.size(); i++) {
            line += row[i];
            if (i + 1 < row.size()) line += std::string(width[i] - row[i].size() + 2, ' ');
        }
        std::cout << line << "\n";
    }
    std::cout.flush();
}

// status: where each service's pod is running, next to what the repo says.
//
// Unlike restart and exec it takes a glob or shorthand, because "which node is
// everything on" is the question, and asking it changes nothing. That is also
// why the repo is read but not pulled: a status check that rewrites your
// checkout would be a surprise. What it shows from Git is the local checkout.
//
// The cluster half is ioc-status, which prints one line per pod; this only
// lines those up against the services asked for, so a stopped IOC still gets
// a row saying so rather than silently missing from the table.
void status(const Config &config, const Action &action, const Args &args) {
    if (!args.revision.empty()) usage_error("status takes only a service name");

    std::vector<std::string> names;
    Lines lines;
    std::vector<std::string> script_args;
    if (!args.node.empty()) {
        script_args = {"--node", args.node};
    } else {
        const Repo repo = resolve_repo(config, args.repo, args.service);
        check_repo(repo.root, repo.why);
        std::cout << "repo    " << (repo.key.empty() ? "" : repo.key + "  ") << repo.root
                  << "  (" << repo.why << "; local checkout, not pulled)\n";
        lines = split_lines(read_file(path_join(repo.root, VALUES)));
        Options aliases;
        if (!repo.key.empty()) aliases = config.section(repo.key + ".aliases");
        names = select(lines, args.service, aliases);
        script_args = names;
    }

    const std::string script = helper_script(config, action);
    if (args.dry_run) {
        std::cout << "would run: " << (script.empty() ? action.script : script)
                  << (script.empty() ? " (embedded) " : " ") << join(script_args, " ")
                  << std::endl;
        return;
    }
    std::vector<std::string> argv = script.empty()
        ? embedded_helper(action.script, script_args)
        : std::vector<std::string>{script};
    if (!script.empty()) argv.insert(argv.end(), script_args.begin(), script_args.end());

    // The script has already said what went wrong on stderr; pass its status
    // on so a failed lookup cannot look like an empty, healthy table.
    std::string output;
    const int code = run_capture(argv, &output);
    // 3 is ioc-status saying this machine is off the primary network and it
    // did not try the cluster. The repo half is still a right answer, so show
    // that alone; --node has nothing without the cluster.
    const bool cluster = code == 0;
    if (code != 0 && (code != 3 || !args.node.empty())) {
        std::cout.flush();
        std::exit(code);
    }

    // SERVICE NODE STATUS RESTARTS AGE POD, after a header line.
    std::vector<std::vector<std::string>> pods;
    const std::vector<std::string> printed = split_lines(output);
    for (size_t i = 1; i < printed.size(); i++) {
        const std::vector<std::string> fields = split_list(printed[i]);
        if (fields.size() >= 5) pods.push_back(fields);
    }

    std::vector<std::vector<std::string>> rows;
    if (!args.node.empty()) {
        rows.push_back({"SERVICE", "NODE", "STATUS", "RESTARTS", "AGE"});
        for (const auto &pod : pods) rows.push_back({pod[0], pod[1], pod[2], pod[3], pod[4]});
        if (pods.empty()) {
            std::cout << "no pods on " << args.node << std::endl;
            return;
        }
        print_table(rows);
        return;
    }

    if (cluster) {
        rows.push_back({"SERVICE", "ENABLED", "REVISION", "NODE", "STATUS", "RESTARTS", "AGE"});
    } else {
        rows.push_back({"SERVICE", "ENABLED", "REVISION"});
    }
    // No targetRevision of its own means the service runs the source: default.
    const std::string inherited = default_revision(lines);
    for (const std::string &name : names) {
        size_t start, end;
        const bool listed = find_service(lines, name, &start, &end);
        // No enabled key means running, the same as the editing actions read it.
        std::string enabled = listed ? get_key(lines, name, "enabled") : "-";
        if (enabled.empty()) enabled = "true";
        std::string revision = listed ? get_key(lines, name, "targetRevision") : "-";
        if (revision.empty()) {
            revision = inherited.empty() ? "-" : inherited;
        }
        if (!cluster) {
            rows.push_back({name, enabled, revision});
            continue;
        }

        bool found = false;
        for (const auto &pod : pods) {
            if (pod[0] != name) continue;
            rows.push_back({name, enabled, revision, pod[1], pod[2], pod[3], pod[4]});
            found = true;
        }
        if (!found) {
            const char *why = !listed ? "not in repo" : enabled == "false" ? "stopped" : "no pod";
            rows.push_back({name, enabled, revision, "-", why, "-", "-"});
        }
    }
    print_table(rows);
}

}  // namespace

int main(int argc, char **argv) {
    const Args args = parse_args(argc, argv);
    if (args.version) {
        std::cout << "ioc " << IOC_VERSION << std::endl;
        return 0;
    }
    LoadedConfig loaded = read_config(args.config);
    const Config &config = loaded.config;
    if (args.help) show_help(config);

    if (args.force_sync && (args.no_git || args.list ||
                           args.action == "restart" || args.action == "exec" ||
                           args.action == "status")) {
        usage_error("--force-sync requires start, stop, deploy or no action; incompatible with --no-git and --list");
    }
    if (!args.argocd_app.empty() && !args.force_sync) {
        usage_error("--argocd-app requires --force-sync");
    }

    if (!args.node.empty() && args.action != "status") {
        usage_error("--node is for status only, e.g. status --node NODE");
    }
    if (!args.node.empty() && !args.service.empty()) {
        usage_error("status takes a service or --node, not both");
    }

    if (args.list) {
        show_repos(config, loaded.path);
        return 0;
    }
    const bool sync_only = args.force_sync && args.action.empty();
    if (sync_only && args.service.empty()) {
        usage_error("--force-sync needs a service, e.g. --force-sync sr22c-va-ioc-01");
    }
    if (!sync_only && (args.action.empty() ||
                       (args.service.empty() && args.node.empty()))) {
        usage_error("give an action and a service, e.g. stop sr22c-va-ioc-01");
    }
    if (args.action == "deploy" && args.revision.empty()) {
        usage_error("deploy needs a revision, e.g. deploy sr22c-va-ioc-01 2026_sd3");
    }

    const Action *action = find_action(args.action);

    if (args.action == "status") {
        status(config, *action, args);
        return 0;
    }

    // These act on the cluster, not the repo, so they take none of what
    // follows: no repo to pick, nothing to pull, edit, commit or push. The
    // third positional is deploy's revision and means nothing here; catching it
    // beats silently ignoring a mistyped command.
    if (action && action->script) {
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
    if (sync_only) {
        const auto targets = select(split_lines(read_file(path)), args.service,
                                    config.section(repo.key + ".aliases"));
        check_sync_names(parent, targets);
        if (!args.dry_run) prepare_argocd(parent);
        sync_apps(parent, targets, args.dry_run, false);
        return 0;
    }
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
