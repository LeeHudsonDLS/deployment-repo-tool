#!/usr/bin/env python3
"""Start, stop or deploy one service in an ArgoCD "*-deployment" repo.

    deployment-repo-tool.py start sr22c-va-ioc-01
    deployment-repo-tool.py stop sr22c-va-ioc-01
    deployment-repo-tool.py deploy sr22c-va-ioc-01 2026_sd3

The service can be a glob, which selects every matching entry in the file and
puts them all in one commit. Quote it, or the shell will try to expand it
first ("no matches found" in zsh):

    deployment-repo-tool.py stop 'fe15*'    # fe15i-cs-ioc-01, -mo-, -py-
    deployment-repo-tool.py start 'fe*'     # every fe service in the file

The shorthand names listed at the bottom of --help stand in for the patterns
used most often, so `stop cs` is `stop 'fe[0-9][0-9][ijkb]-cs-ioc-0[1-9]'`.

Run it from anywhere inside the deployment repo. It edits the services block
of apps/values.yaml, which looks like this:

    services:
      sr06c-va-ioc-01:            # no keys: chart defaults, i.e. running
      sr22c-va-ioc-01:
        enabled: true             # false stops the service
        targetRevision: 2026_sd3  # overrides source.targetRevision above

start and stop set `enabled`. deploy sets `enabled: true` and the revision,
creating the entry if the service is not listed yet. The file is then pulled,
added, committed and pushed.

values.yaml is edited as lines of text rather than loaded with a YAML library:
there is no PyYAML on the machines this runs on, and re-dumping the document
would throw away the comments and ordering that make the file readable. Only
the standard library is used, so it runs on the system python.
"""

import argparse
import difflib
import fnmatch
import os
import re
import subprocess
import sys

VALUES = "apps/values.yaml"
VERBS = {"start": "Starting", "stop": "Stopping", "deploy": "Deploying"}

# Shorthand for the patterns typed most often. These are front end names: the
# domain letter is lower case here because that is how the services are spelt
# in values.yaml, even though the IOCs themselves are FE03I and friends.
ALIASES = {
    "all": "fe[0-9]*",
    "cs": "fe[0-9][0-9][ijkb]-cs-ioc-0[1-9]",
    "mo": "fe[0-9][0-9][ijkb]-mo-ioc-0[1-9]",
    "py": "fe[0-9][0-9][ijkb]-py-ioc-0[1-9]",
}


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


def find_repo():
    """Walk up from the cwd to the checkout we are in, and sanity check it.

    The name test is the guard against doing this in the wrong repo by
    mistake: the deployment repos are all called <area>-deployment. `.git` is
    a file rather than a directory in a worktree, hence os.path.exists.
    """
    root = os.getcwd()
    while not os.path.exists(os.path.join(root, ".git")):
        parent = os.path.dirname(root)
        if parent == root:
            fail("not inside a git repository")
        root = parent
    if "deployment" not in os.path.basename(root).lower():
        fail("%s is not a deployment repo" % root)
    if not os.path.isfile(os.path.join(root, VALUES)):
        fail("%s has no %s" % (root, VALUES))
    return root


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


def select(lines, pattern):
    """The services a pattern picks out, in the order the file lists them.

    A shorthand name from ALIASES becomes its pattern first. A plain name is
    passed straight through even if the file has never heard of it, so that
    deploy can add it; a glob only ever selects entries that are already
    there.

    Keys nested under an entry (`labels:`) look just like an entry, so only
    lines at the indent of the first one count as a service.
    """
    glob = ALIASES.get(pattern, pattern)
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


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        # Built from ALIASES so the help cannot drift from what the code does.
        epilog="shorthand for SERVICE:\n" + "".join(
            "  %-4s %s\n" % (name, glob) for name, glob in ALIASES.items()))
    parser.add_argument("action", choices=("start", "stop", "deploy"))
    parser.add_argument("service", metavar="SERVICE",
                        help="service name as it appears in %s, a quoted glob"
                             " like 'fe15*' to do several at once, or one of the"
                             " shorthand names below" % VALUES)
    parser.add_argument("revision", nargs="?", help="branch or tag, for deploy")
    parser.add_argument("--no-git", action="store_true",
                        help="edit the file only: no pull, commit or push")
    parser.add_argument("--dry-run", action="store_true",
                        help="show the change, write nothing")
    args = parser.parse_args()
    if args.action == "deploy" and not args.revision:
        parser.error("deploy needs a revision, e.g. deploy sr22c-va-ioc-01 2026_sd3")

    root = find_repo()
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

    targets = select(lines, args.service)
    if len(targets) > 1:
        print("%s: %s" % (args.action, ", ".join(targets)))

    for service in targets:
        # deploy is the only action allowed to introduce a service; start and
        # stop on an unknown name are far more likely to be a typo. A bare
        # `  name:` line is a complete entry, and set_key fills in the rest.
        if find(lines, service) is None:
            if args.action != "deploy":
                fail("no service '%s' in %s" % (service, VALUES))
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
