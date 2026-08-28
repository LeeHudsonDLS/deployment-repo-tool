#include "cli.h"

#include <cstdlib>
#include <iostream>
#include <vector>

#include "paths.h"
#include "support.h"
#include "values.h"

// The actions, and for the two that act on the cluster the script each runs.
// One table: the choices --help offers, the choices accepted, and which
// actions skip the repo entirely all come from here, so they cannot disagree.
const Action ACTIONS[] = {
    {"start", nullptr},          {"stop", nullptr}, {"deploy", nullptr},
    {"restart", "ioc-restart"},  {"exec", "ioc-exec"},
};
const size_t ACTION_COUNT = sizeof(ACTIONS) / sizeof(ACTIONS[0]);

const Action *find_action(const std::string &name) {
    for (size_t i = 0; i < ACTION_COUNT; i++) {
        if (name == ACTIONS[i].name) return &ACTIONS[i];
    }
    return nullptr;
}

namespace {

std::string program_name = "deployment-repo-tool";

struct LongOption {
    const char *name;
    bool takes_value;
};

const LongOption LONG_OPTIONS[] = {
    {"help", false},   {"repo", true},   {"config", true},
    {"list", false},   {"no-git", false}, {"dry-run", false},
};
const size_t LONG_OPTION_COUNT = sizeof(LONG_OPTIONS) / sizeof(LONG_OPTIONS[0]);

std::string action_choices(const std::string &separator) {
    std::vector<std::string> names;
    for (size_t i = 0; i < ACTION_COUNT; i++) names.push_back(ACTIONS[i].name);
    return join(names, separator);
}

std::string usage_line() {
    // Continuation lines line up under the first argument, as argparse's did.
    const std::string indent(std::string("usage: ").size() + program_name.size() + 1, ' ');
    return "usage: " + program_name + " [-h] [-r REPO] [--config PATH] [--list]\n" +
           indent + "[--no-git] [--dry-run]\n" + indent + "[{" + action_choices(",") +
           "}] [SERVICE] [revision]";
}

// The top of what the python kept in its docstring. Everything below "How it
// works" in that file was reference for whoever opened it rather than help for
// whoever ran it, and printing all of it turned --help into three screens; in
// C++ that half lives in the header comments instead.
const char *DESCRIPTION =
    "Start, stop and deploy services in an ArgoCD \"*-deployment\" repo, and\n"
    "restart or open a shell in a running one.\n"
    "\n"
    "    dep start sr22c-va-ioc-01\n"
    "    dep stop sr22c-va-ioc-01\n"
    "    dep deploy sr22c-va-ioc-01 2026_sd3\n"
    "    dep restart fe22i-mo-ioc-01\n"
    "    dep exec fe22i-mo-ioc-01\n"
    "\n"
    "start, stop and deploy edit apps/values.yaml, then pull, commit and push.\n"
    "restart and exec change nothing at all: they act on the running pod, and are\n"
    "the ArgoCD web UI's pod delete and terminal.\n"
    "\n"
    "For start, stop and deploy the service can be a glob, selecting every matching\n"
    "entry in one commit. Quote it, or the shell expands it first (\"no matches\n"
    "found\" in zsh):\n"
    "\n"
    "    dep stop 'fe15*'    # fe15i-cs-ioc-01, -mo-, -py-\n"
    "\n"
    "Which repo is edited comes from the config (see --list), the first of:\n"
    "\n"
    "    -r, either a config key or a path to a checkout\n"
    "    the current directory, if it is inside a *-deployment checkout\n"
    "    the service name: its shorthand's repo, else the [match] patterns\n"
    "    [general] default\n"
    "\n"
    "so it runs from anywhere, and prints which repo it picked and why before\n"
    "changing anything.";

const char *OPTION_HELP =
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  -r REPO, --repo REPO  which deployment repo: a key from the config, or a\n"
    "                        path to a checkout\n"
    "  --config PATH         config file to use\n"
    "  --list                show the configured repos and exit\n"
    "  --no-git              edit the file only: no pull, commit or push\n"
    "  --dry-run             show the change, write nothing";

// The [<repo>.aliases] sections, for the bottom of --help. Built from the
// config that is actually loaded, so the help cannot drift from it.
std::string shorthand_help(const Config &config) {
    const std::string suffix = ".aliases";
    std::string out;
    for (const std::string &name : config.section_names()) {
        if (!ends_with(name, suffix)) continue;
        const Options &aliases = config.section(name);
        if (aliases.empty()) continue;
        out += "  " + name.substr(0, name.size() - suffix.size()) + ":\n";
        for (const std::pair<std::string, std::string> &alias : aliases) {
            out += format("    %-4s %s\n", alias.first.c_str(), alias.second.c_str());
        }
    }
    if (out.empty()) return std::string();
    return "\nshorthand for SERVICE, by repo:\n" + out;
}

void show_help(const Config &config) {
    std::cout << usage_line() << "\n\n"
              << DESCRIPTION << "\n\n"
              << "positional arguments:\n"
              << "  {" << action_choices(",") << "}\n"
              << "                        restart and exec act on the running pod and\n"
              << "                        change nothing in the repo; the rest edit it\n"
              << "  SERVICE               service name as it appears in " << VALUES
              << ", a\n"
              << "                        quoted glob like 'fe15*' to do several at\n"
              << "                        once, or one of the shorthand names below\n"
              << "  revision              branch or tag; deploy only, and refused by\n"
              << "                        restart and exec\n\n"
              << OPTION_HELP << "\n"
              << shorthand_help(config);
    std::cout.flush();
    std::exit(0);
}

// An unambiguous abbreviation is accepted, as argparse did: --dry is --dry-run.
const LongOption *match_long_option(const std::string &name) {
    const LongOption *found = nullptr;
    for (size_t i = 0; i < LONG_OPTION_COUNT; i++) {
        const std::string candidate = LONG_OPTIONS[i].name;
        if (candidate == name) return &LONG_OPTIONS[i];
        if (!starts_with(candidate, name)) continue;
        if (found) usage_error("ambiguous option: --" + name);
        found = &LONG_OPTIONS[i];
    }
    if (!found) usage_error("unrecognized argument: --" + name);
    return found;
}

void store(Args *args, const std::string &name, const std::string &value) {
    if (name == "repo") args->repo = value;
    else if (name == "config") args->config = value;
    else if (name == "list") args->list = true;
    else if (name == "no-git") args->no_git = true;
    else if (name == "dry-run") args->dry_run = true;
}

}  // namespace

void usage_error(const std::string &message) {
    std::cout.flush();
    std::cerr << usage_line() << "\n" << program_name << ": error: " << message
              << std::endl;
    std::exit(2);
}

Args parse_args(int argc, char **argv, const Config &config) {
    if (argc > 0 && argv[0] && *argv[0]) program_name = basename(argv[0]);

    Args args;
    std::vector<std::string> positionals;
    bool options_finished = false;  // set by a bare "--"

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];

        if (options_finished || arg == "-" || arg.size() < 2 || arg[0] != '-') {
            positionals.push_back(arg);
            continue;
        }
        if (arg == "--") {
            options_finished = true;
            continue;
        }

        if (starts_with(arg, "--")) {
            std::string name = arg.substr(2);
            std::string value;
            bool have_value = false;
            const size_t equals = name.find('=');
            if (equals != std::string::npos) {
                value = name.substr(equals + 1);
                name = name.substr(0, equals);
                have_value = true;
            }
            const LongOption *option = match_long_option(name);
            if (std::string(option->name) == "help") show_help(config);
            if (option->takes_value && !have_value) {
                if (i + 1 >= argc) {
                    usage_error(format("argument --%s: expected one argument",
                                       option->name));
                }
                value = argv[++i];
            } else if (!option->takes_value && have_value) {
                usage_error(format("argument --%s: ignored explicit argument '%s'",
                                   option->name, value.c_str()));
            }
            store(&args, option->name, value);
            continue;
        }

        // Short options. Only -h and -r exist; -rva is the same as -r va.
        if (arg[1] == 'h') show_help(config);
        if (arg[1] != 'r') usage_error("unrecognized argument: " + arg);
        if (arg.size() > 2) {
            args.repo = arg.substr(2);
        } else if (i + 1 < argc) {
            args.repo = argv[++i];
        } else {
            usage_error("argument -r/--repo: expected one argument");
        }
    }

    if (positionals.size() > 3) {
        std::vector<std::string> extra(positionals.begin() + 3, positionals.end());
        usage_error("unrecognized arguments: " + join(extra, " "));
    }
    if (positionals.size() > 0) args.action = positionals[0];
    if (positionals.size() > 1) args.service = positionals[1];
    if (positionals.size() > 2) args.revision = positionals[2];

    if (!args.action.empty() && !find_action(args.action)) {
        usage_error(format("argument action: invalid choice: '%s' (choose from %s)",
                           args.action.c_str(), action_choices(", ").c_str()));
    }
    return args;
}
