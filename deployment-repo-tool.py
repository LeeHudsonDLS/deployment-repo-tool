#!/usr/bin/env python3
"""Start, stop and deploy services in an ArgoCD "*-deployment" repo, and
restart or open a shell in a running one.

    deployment-repo-tool.py start sr22c-va-ioc-01
    deployment-repo-tool.py stop sr22c-va-ioc-01
    deployment-repo-tool.py deploy sr22c-va-ioc-01 2026_sd3
    deployment-repo-tool.py restart fe22i-mo-ioc-01
    deployment-repo-tool.py exec fe22i-mo-ioc-01

start, stop and deploy edit apps/values.yaml, then pull, commit and push.
restart and exec change nothing at all: they act on the running pod, and are
the ArgoCD web UI's pod delete and terminal.

For start, stop and deploy the service can be a glob, selecting every matching
entry in one commit. Quote it, or the shell expands it first ("no matches
found" in zsh):

    deployment-repo-tool.py stop 'fe15*'    # fe15i-cs-ioc-01, -mo-, -py-

Which repo is edited comes from the config (see --list), the first of:

    -r, either a config key or a path to a checkout
    the current directory, if it is inside a *-deployment checkout
    the service name: its shorthand's repo, else the [match] patterns
    [general] default

so it runs from anywhere, and prints which repo it picked and why before
changing anything.

How it works
------------

--help stops at the line above: the rest of this is for reading in the file,
not at a prompt.

It edits the services block of apps/values.yaml, which looks like this:

    services:
      sr06c-va-ioc-01:            # no keys: chart defaults, i.e. running
      sr22c-va-ioc-01:
        enabled: true             # false stops the service
        targetRevision: 2026_sd3  # overrides source.targetRevision above

start and stop set `enabled`. deploy sets `enabled: true` and the revision,
creating the entry if the service is not listed yet. The file is then pulled,
added, committed and pushed.

restart and exec each hand the service name to a shell script shipped beside
this one: ioc-restart deletes the running pod and lets the StatefulSet
recreate it, ioc-exec opens a shell inside it. They are shell scripts because
reaching the cluster means sourcing the site's kubectl setup, which python
cannot do to itself. There is no file to edit, so no repo is resolved either.

values.yaml is edited as lines of text rather than loaded with a YAML library:
there is no PyYAML on the machines this runs on, and re-dumping the document
would throw away the comments and ordering that make the file readable. Only
the standard library is used, so it runs on the system python.
"""

import argparse
import configparser
import difflib
import fnmatch
import os
import re
import subprocess
import sys

VALUES = "apps/values.yaml"
VERBS = {"start": "Starting", "stop": "Stopping", "deploy": "Deploying"}
CONFIG_NAME = "config.ini"
# Actions carried out on the cluster instead of the repo, and the script that
# does each. They are shell scripts because getting to the cluster means
# sourcing the site's kubectl setup, which python cannot do to itself.
HELPERS = {"restart": "ioc-restart", "exec": "ioc-exec"}


def fail(message):
    sys.exit("error: " + message)


def git(root, *args):
    """Run git in the repo; any failure stops the tool.

    Output stays attached to the terminal so progress and any credential
    prompt reach the user. Flushing first stops our own prints from arriving
    out of order when stdout is redirected to a file or a pipe.
    """
    sys.stdout.flush()
    if subprocess.call(["git", "-C", root] + list(args)) != 0:
        fail("git %s failed" % " ".join(args))


# ----------------------------------------------------------------- the config


def read_config(given):
    """(parsed config, path it came from). Missing config is not an error.

    With no config at all every section below comes back empty, which leaves
    the tool working the way it did before there was one: on the repo the
    current directory is in.
    """
    home = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    here = os.path.dirname(os.path.abspath(__file__))
    for path in (given, os.environ.get("DEPLOYMENT_REPO_CONFIG"),
                 os.path.join(home, "deployment-repo-tool", CONFIG_NAME),
                 os.path.join(here, CONFIG_NAME)):
        if path and os.path.isfile(path):
            # interpolation off: a % in a path or a glob is just a %.
            config = configparser.ConfigParser(interpolation=None)
            try:
                config.read(path)
            except configparser.Error as exc:
                fail("%s: %s" % (path, exc))
            return config, path
    if given:
        fail("no config file at %s" % given)
    return configparser.ConfigParser(interpolation=None), None


def section(config, name):
    """A config section as a plain dict, empty if it is not there."""
    return dict(config[name]) if config.has_section(name) else {}


def expand(path):
    return os.path.abspath(os.path.expanduser(os.path.expandvars(path)))


def patterns(value):
    """A comma or space separated list of globs."""
    return [glob for glob in re.split(r"[,\s]+", value.strip()) if glob]


# ------------------------------------------------------------- finding a repo


def repo_from_cwd():
    """The deployment repo the current directory is in, or None."""
    root = os.getcwd()
    while not os.path.exists(os.path.join(root, ".git")):
        parent = os.path.dirname(root)
        if parent == root:
            return None
        root = parent
    return root if "deployment" in os.path.basename(root).lower() else None


def resolve_repo(config, wanted, service):
    """Work out which repo to edit: (path, config key or None, why).

    The reason is carried back so it can be printed: this now edits repos the
    user is not standing in, so it has to say which one it picked and how.
    """
    repos = section(config, "repos")

    if wanted:
        if wanted in repos:
            return expand(repos[wanted]), wanted, "-r %s" % wanted
        if os.path.isdir(wanted):
            return expand(wanted), None, "-r, as a path"
        fail("no repo '%s' in the config; there is %s"
             % (wanted, ", ".join(repos) if repos else "no [repos] section"))

    # Standing in a checkout beats anything inferred: whoever cd'd in there
    # meant that copy, not whichever one the config happens to point at.
    here = repo_from_cwd()
    if here:
        key = next((k for k in repos if expand(repos[k]) == here), None)
        return here, key, "current directory"

    # A shorthand belongs to the repo that defines it, so `stop cs` needs no
    # -r. If two repos define the same one, ask rather than guess.
    owners = [key for key in repos if config.has_option("%s.aliases" % key, service)]
    if len(owners) > 1:
        fail("'%s' is a shorthand in %s - use -r to say which"
             % (service, " and ".join(owners)))
    if owners:
        return (expand(repos[owners[0]]), owners[0],
                "'%s' is a %s shorthand" % (service, owners[0]))

    for key, value in section(config, "match").items():
        for glob in patterns(value):
            if fnmatch.fnmatchcase(service, glob):
                if key not in repos:
                    fail("[match] has '%s' but [repos] gives it no path" % key)
                return expand(repos[key]), key, "'%s' matches %s" % (service, glob)

    fallback = config.get("general", "default", fallback=None)
    if fallback:
        if fallback not in repos:
            fail("[general] default is '%s', which is not in [repos]" % fallback)
        return expand(repos[fallback]), fallback, "the configured default"

    fail("cannot tell which repo '%s' belongs to - use -r, or cd into one%s"
         % (service, ("; there is " + ", ".join(repos)) if repos else ""))


def shorthand_hint(config, name, key):
    """Where a name is a shorthand, for when it means nothing in this repo.

    Typing `stop cs` in the va checkout otherwise gets the flat "no service
    'cs'", which does not say that cs is a real thing somewhere else.
    """
    owners = [k for k in section(config, "repos")
              if k != key and config.has_option("%s.aliases" % k, name)]
    if not owners:
        return ""
    return ("\n  ('%s' is a shorthand in %s - try -r %s)"
            % (name, ", ".join(owners), owners[0]))


def check_repo(root, why):
    """Refuse anything that is not a deployment repo, however we got here."""
    if "deployment" not in os.path.basename(root).lower():
        fail("%s (%s) is not a deployment repo" % (root, why))
    if not os.path.isfile(os.path.join(root, VALUES)):
        fail("%s (%s) has no %s" % (root, why, VALUES))


# ---------------------------------------------------------------- values.yaml


def indent_of(line):
    return len(line) - len(line.lstrip())


def block(lines):
    """Line range of the service entries: (first one, one past the last).

    Everything indented belongs to the block, so it ends at the next line
    starting in column 0 (or at the end of the file, which is where these
    files normally end). Trailing blank lines are dropped so that a new entry
    is appended tight against the last one rather than after a gap.
    """
    start = next((i for i, l in enumerate(lines) if l.rstrip() == "services:"), None)
    if start is None:
        fail("no 'services:' block in " + VALUES)
    end = start + 1
    while end < len(lines) and (not lines[end].strip() or lines[end].startswith(" ")):
        end += 1
    while end > start + 1 and not lines[end - 1].strip():
        end -= 1
    return start + 1, end


def find(lines, service):
    """Locate one service: (its `  name:` line, one past its last child line).

    A service owns every following line indented deeper than its own name, so
    a `labels:` block and anything nested under it comes along too. Returns
    None when the name is not there, which for deploy means "add it".

    An entry written on one line (`name: {enabled: true}`) does not match the
    pattern, so it is reported as missing rather than mangled.
    """
    start, end = block(lines)
    for i in range(start, end):
        if re.match(r" +%s:\s*$" % re.escape(service), lines[i]):
            j = i + 1
            while j < end and lines[j].startswith(" " * (indent_of(lines[i]) + 1)):
                j += 1
            return i, j
    return None


def select(lines, pattern, aliases):
    """The services a pattern picks out, in the order the file lists them.

    A shorthand from the repo's aliases becomes its pattern first. A plain
    name is passed straight through even if the file has never heard of it,
    so that deploy can add it; a glob only ever selects entries that are
    already there.

    Keys nested under an entry (`labels:`) look just like an entry, so only
    lines at the indent of the first one count as a service.
    """
    glob = aliases.get(pattern, pattern)
    if not any(char in glob for char in "*?["):
        return [pattern]
    start, end = block(lines)
    names, depth = [], None
    for i in range(start, end):
        entry = re.match(r"( +)([^#\s]\S*):\s*$", lines[i])
        if entry:
            depth = len(entry.group(1)) if depth is None else depth
            if len(entry.group(1)) == depth:
                names.append(entry.group(2))
    hits = fnmatch.filter(names, glob)
    if not hits:
        # Name the pattern the alias stands for, or "nothing matches cs" is a
        # puzzle rather than a message.
        shown = "'%s'" % pattern if glob == pattern else "'%s' (%s)" % (pattern, glob)
        fail("nothing in %s matches %s" % (VALUES, shown))
    return hits


def summarise(names, pattern):
    """Name the services for a message, without printing an essay for 'fe*'."""
    joined = ", ".join(names)
    if len(joined) > 60:
        return "%d services (%s)" % (len(names), pattern)
    return joined


def set_key(lines, service, key, value):
    """Set `key: value` for the service, rewriting its line or adding one.

    A key that is not there yet goes at the end of the entry, indented one
    level (two spaces) deeper than the service name. Rewriting an existing key
    keeps any trailing `#` comment, so a note beside `enabled:` is not lost.
    """
    i, j = find(lines, service)
    for k in range(i + 1, j):
        if re.match(r" +%s:" % re.escape(key), lines[k]):
            _, hash_, note = lines[k].partition("#")
            lines[k] = "%s%s: %s%s\n" % (" " * indent_of(lines[k]), key, value,
                                         "  " + hash_ + note.rstrip() if hash_ else "")
            return
    lines.insert(j, "%s%s: %s\n" % (" " * (indent_of(lines[i]) + 2), key, value))


# ------------------------------------------------------------- on the cluster


def helper(config, action):
    """Where the shell script for a cluster action is.

    It ships beside this one, so normally there is nothing to configure.
    realpath, not abspath: the README suggests putting the tool on your PATH,
    which people do with a symlink, and the sibling has to be found through it.
    The config key is for running a copy from somewhere else.
    """
    name = HELPERS[action]
    key = "%s_script" % action
    configured = config.get("general", key, fallback=None)
    if configured:
        script = expand(configured)
        if not os.path.isfile(script):
            fail("[general] %s is %s, which is not there" % (key, script))
        return script

    script = os.path.join(os.path.dirname(os.path.realpath(__file__)), name)
    if not os.path.isfile(script):
        fail("%s is missing from the checkout - restore it, or point"
             " [general] %s at a copy" % (name, key))
    return script


def on_cluster(config, action, service, dry_run):
    """Carry out a cluster action by handing the service name to its script.

    Nothing here touches the deployment repo: restarting a pod or opening a
    shell in one changes no file, so there is nothing to commit and no repo to
    resolve. The scripts own everything about reaching the cluster -- finding
    kubectl, sourcing the klogin setup, checking the service is really there --
    and this only locates the right one.
    """
    # One at a time on purpose. Restarting a glob's worth of IOCs is not
    # something to make easy to do by accident, and a shell in several pods at
    # once is not a thing.
    if any(char in service for char in "*?["):
        fail("%s takes one service, not a glob" % action)

    script = helper(config, action)
    if dry_run:
        return print("would run: %s %s" % (script, service))

    # Its exit status becomes ours, so a failure cannot look like a success.
    # The terminal is left alone: rollout progress has to be readable, and for
    # exec the remote shell needs the tty.
    sys.stdout.flush()
    sys.exit(subprocess.call([script, service]))


# ----------------------------------------------------------------------- main


def shorthand_help(config):
    """The [<repo>.aliases] sections, for the bottom of --help.

    Built from the config so the help cannot drift from what is configured.
    """
    out = []
    for name in config.sections():
        if name.endswith(".aliases") and config[name]:
            out.append("  %s:" % name[:-len(".aliases")])
            out += ["    %-4s %s" % pair for pair in config[name].items()]
    return "shorthand for SERVICE, by repo:\n" + "\n".join(out) if out else None


def show_repos(config, config_path):
    """--list: what is configured, and whether it is actually there."""
    print("config  %s" % (config_path or "none found"))
    repos = section(config, "repos")
    if not repos:
        return print("no [repos] section")
    for key, path in repos.items():
        root = expand(path)
        state = "ok" if os.path.isfile(os.path.join(root, VALUES)) else "NOT THERE"
        print("%-10s %-10s %s" % (key, state, root))


def main():
    config, config_path = read_config(None)
    # --help gets the top of the docstring only. Everything under "How it
    # works" is reference for whoever opens the file, and printing it at a
    # prompt turned --help into three screens.
    parser = argparse.ArgumentParser(
        description=__doc__.partition("\nHow it works\n")[0].rstrip(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=shorthand_help(config))
    parser.add_argument("action", nargs="?",
                        choices=("start", "stop", "deploy", "restart", "exec"),
                        help="restart and exec act on the running pod and"
                             " change nothing in the repo; the rest edit it")
    parser.add_argument("service", metavar="SERVICE", nargs="?",
                        help="service name as it appears in %s, a quoted glob"
                             " like 'fe15*' to do several at once, or one of the"
                             " shorthand names below" % VALUES)
    parser.add_argument("revision", nargs="?", help="branch or tag; deploy only,"
                        " and refused by restart and exec")
    parser.add_argument("-r", "--repo", help="which deployment repo: a key from"
                        " the config, or a path to a checkout")
    parser.add_argument("--config", metavar="PATH", help="config file to use")
    parser.add_argument("--list", action="store_true",
                        help="show the configured repos and exit")
    parser.add_argument("--no-git", action="store_true",
                        help="edit the file only: no pull, commit or push")
    parser.add_argument("--dry-run", action="store_true",
                        help="show the change, write nothing")
    args = parser.parse_args()
    # --config has to be read again now that argparse has seen it; the first
    # read was only to build the shorthand list in --help.
    if args.config:
        config, config_path = read_config(args.config)
    if args.list:
        return show_repos(config, config_path)
    if not args.action or not args.service:
        parser.error("give an action and a service, e.g. stop sr22c-va-ioc-01")
    if args.action == "deploy" and not args.revision:
        parser.error("deploy needs a revision, e.g. deploy sr22c-va-ioc-01 2026_sd3")

    # These act on the cluster, not the repo, so they take none of what
    # follows: no repo to pick, nothing to pull, edit, commit or push. The
    # third positional is deploy's revision and means nothing here; catching
    # it beats silently ignoring a mistyped command.
    if args.action in HELPERS:
        if args.revision:
            parser.error("%s takes only a service name" % args.action)
        return on_cluster(config, args.action, args.service, args.dry_run)

    root, key, why = resolve_repo(config, args.repo, args.service)
    check_repo(root, why)
    print("repo    %s%s  (%s)" % (key + "  " if key else "", root, why))
    path = os.path.join(root, VALUES)

    # Pull before reading, so the edit is made against what is really deployed.
    # --ff-only: if the branch has diverged, stop and let a human sort it out
    # rather than quietly building a merge on top of someone else's work.
    if not (args.no_git or args.dry_run):
        git(root, "pull", "--ff-only")

    with open(path) as handle:
        lines = handle.readlines()
    # If the file does not end in a newline, appending an entry would join it
    # onto the last line.
    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    before = list(lines)

    aliases = section(config, "%s.aliases" % key) if key else {}
    targets = select(lines, args.service, aliases)
    if len(targets) > 1:
        print("%s: %s" % (args.action, ", ".join(targets)))

    for service in targets:
        # deploy is the only action allowed to introduce a service; start and
        # stop on an unknown name are far more likely to be a typo. A bare
        # `  name:` line is a complete entry, and set_key fills in the rest.
        if find(lines, service) is None:
            if args.action != "deploy":
                fail("no service '%s' in %s%s"
                     % (service, VALUES, shorthand_hint(config, service, key)))
            lines.insert(block(lines)[1], "  %s:\n" % service)
            print("adding a new entry for %s" % service)

        # A service with no `enabled` key is running, so start could just
        # delete the key. Writing it out explicitly matches how the file is
        # already kept, and leaves no doubt about the state it was put in.
        set_key(lines, service, "enabled", "false" if args.action == "stop" else "true")
        if args.action == "deploy":
            set_key(lines, service, "targetRevision", args.revision)

    # Already in the wanted state: that is a success, not an error. It keeps
    # the tool safe to re-run and avoids an empty commit.
    if lines == before:
        print("nothing to do - %s already as asked for"
              % summarise(targets, args.service))
        return
    sys.stdout.writelines(difflib.unified_diff(before, lines, VALUES, VALUES))

    if args.dry_run:
        return
    with open(path, "w") as handle:
        handle.writelines(lines)
    if args.no_git:
        return

    # Both commands name the file, so anything else staged in the repo is left
    # out of this commit.
    message = "%s %s" % (VERBS[args.action], summarise(targets, args.service))
    if args.action == "deploy":
        message += " on " + args.revision
    git(root, "add", "--", VALUES)
    git(root, "commit", "-m", message, "--", VALUES)
    git(root, "push")


if __name__ == "__main__":
    main()
