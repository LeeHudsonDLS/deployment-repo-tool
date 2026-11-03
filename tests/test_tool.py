#!/usr/bin/env python3
# /// script
# requires-python = ">=3.8"
# dependencies = ["pyyaml"]
# ///
"""Tests for deployment-repo-tool.

    make test                  # builds the binary first, then runs this
    uv run tests/test_tool.py  # if it is already built

The tool itself is C++; this drives the built binary as a black box, which is
the level worth testing it at -- what it does to a values.yaml and to a git
repo. The pieces underneath (globbing, the ini parser, the diff) have their own
tests in tests/unit.cpp, run by `make check`. Python here because the check that
matters needs a real YAML parser, and because a test suite that cannot share the
tool's own mistakes is worth more than one that can.

Every edit is checked by loading the result with a real YAML parser and
comparing it against the original: the tool writes YAML by hand, so the thing
worth proving is that the document means what it should afterwards, and that
nothing else in it moved.

Nothing here touches a real deployment repo. Each fixture is committed to a
throwaway bare repo standing in for GitLab, the tool is pointed at it with a
generated --config, and its cache is kept in the same temporary directory with
XDG_CACHE_HOME. What a command did is read back from that remote.
"""

import configparser
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(ROOT, "ioc")
VA = os.path.join(HERE, "va-values.yaml")
FE = os.path.join(HERE, "fe-values.yaml")

if not os.access(TOOL, os.X_OK):
    sys.exit("no binary at %s - run make first" % TOOL)

SHIPPED = configparser.ConfigParser(interpolation=None)
SHIPPED.read(os.path.join(ROOT, "builtin.ini"))

fails = []


def check(name, ok, detail=""):
    # str(): a check that reports its detail as a list must still report, not
    # blow up and take the rest of the run with it.
    print(("PASS  " if ok else "FAIL  ") + name + (("  -- " + str(detail)) if not ok else ""))
    if not ok:
        fails.append(name)


def load(path):
    with open(path) as handle:
        return yaml.safe_load(handle)


# A fixed identity, so the commits the tool makes do not depend on whoever runs
# the tests having one configured.
ENV = dict(os.environ, GIT_AUTHOR_NAME="Tester", GIT_AUTHOR_EMAIL="test@example.com",
           GIT_COMMITTER_NAME="Tester", GIT_COMMITTER_EMAIL="test@example.com")


def git_in(remote, *args, **kwargs):
    return subprocess.run(["git", "--git-dir", remote] + list(args), env=ENV,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, **kwargs).stdout.strip()


def publish(remote, text, message="edit"):
    """Commit text as apps/values.yaml on main, as someone else pushing would."""
    blob = git_in(remote, "hash-object", "-w", "--stdin", input=text)
    apps = git_in(remote, "mktree", input="100644 blob %s\tvalues.yaml\n" % blob)
    tree = git_in(remote, "mktree", input="040000 tree %s\tapps\n" % apps)
    parent = git_in(remote, "rev-parse", "-q", "--verify", "refs/heads/main")
    commit = git_in(remote, "commit-tree", tree, *(["-p", parent] if parent else []),
                    "-m", message)
    git_in(remote, "update-ref", "refs/heads/main", commit)


def make_remote(root, text, name="va-deployment"):
    """A bare repo standing in for GitLab, holding text as its values file."""
    remote = os.path.join(root, name + ".git")
    subprocess.run(["git", "init", "-q", "--bare", remote], check=True)
    git_in(remote, "symbolic-ref", "HEAD", "refs/heads/main")
    publish(remote, text, "initial")
    return remote


def read(remote):
    return subprocess.run(["git", "--git-dir", remote, "show", "main:apps/values.yaml"],
                          stdout=subprocess.PIPE, universal_newlines=True).stdout


def head(remote):
    return git_in(remote, "rev-parse", "main")


def write_config(root, key, remote):
    """Point `key` at this remote, carrying over the shipped shorthand."""
    config = configparser.ConfigParser(interpolation=None)
    config["repos"] = {key: remote}
    config["match"] = {key: "*"}
    if SHIPPED.has_section("%s.aliases" % key):
        config["%s.aliases" % key] = dict(SHIPPED["%s.aliases" % key])
    path = os.path.join(root, "config.ini")
    with open(path, "w") as handle:
        config.write(handle)
    return path


def run(root, *args, **kwargs):
    config = kwargs.get("config") or os.path.join(root, "config.ini")
    return subprocess.run([TOOL, "--config", config] + list(args), cwd=root,
                          env=dict(ENV, XDG_CACHE_HOME=os.path.join(root, "cache")),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True)


def changes(before, after):
    """Which (service, key) pairs differ between two parsed documents."""
    changed = set()
    if {k: v for k, v in before.items() if k != "services"} != \
       {k: v for k, v in after.items() if k != "services"}:
        changed.add(("<document>", "<outside services>"))
    old, new = before["services"] or {}, after["services"] or {}
    for name in set(old) | set(new):
        if name not in old or name not in new:
            changed.add((name, "<entry>"))
            continue
        entry_old, entry_new = old[name] or {}, new[name] or {}
        for key in set(entry_old) | set(entry_new):
            if entry_old.get(key) != entry_new.get(key):
                changed.add((name, key))
    return changed


def case(label, args, expect, state=None, source=VA, expect_fail=False, key=None):
    """Run one command against a throwaway remote and check what it did."""
    root = tempfile.mkdtemp(prefix="case-")
    try:
        raw = open(source).read()
        remote = make_remote(root, raw)
        write_config(root, key or ("fe" if source == FE else "va"), remote)
        before, start = yaml.safe_load(raw), head(remote)
        proc = run(root, *args)
        if expect_fail:
            check(label + " (rejected)", proc.returncode != 0, proc.stdout.strip())
            check(label + " (untouched)", head(remote) == start)
            return
        if proc.returncode != 0:
            check(label, False, proc.stdout.strip())
            return
        text = read(remote)
        try:
            after = yaml.safe_load(text)
        except yaml.YAMLError as exc:
            check(label + " (still parses)", False, str(exc))
            return
        got = changes(before, after)
        check(label + " (only intended keys changed)", got == set(expect),
              "changed=%s expected=%s" % (sorted(got), sorted(expect)))
        if state:
            value = (after["services"].get(state[0]) or {}).get(state[1])
            check(label + " (%s.%s == %r)" % state, value == state[2], "got %r" % value)
        names_before = list(before["services"])
        check(label + " (order kept)",
              list(after["services"])[:len(names_before)] == names_before)
        check(label + " (comments kept)", text.count("#") == raw.count("#"))
        was = head(remote)
        run(root, *args)
        check(label + " (idempotent, no second commit)", head(remote) == was)
    finally:
        shutil.rmtree(root, ignore_errors=True)


def matching(source, pattern):
    """Services a pattern should select, straight out of a real YAML parse."""
    return fnmatch.filter(list(load(source)["services"]), pattern)


def expected(source, names, action, revision=None):
    """Keys that should differ afterwards, from what the action means.

    stop means enabled false, start and deploy mean enabled true, and deploy
    also pins the revision. A service already in that state must not change.
    """
    services, out = load(source)["services"], set()
    for name in names:
        entry = services.get(name) or {}
        if entry.get("enabled") != (action != "stop"):
            out.add((name, "enabled"))
        if action == "deploy" and entry.get("targetRevision") != revision:
            out.add((name, "targetRevision"))
    return out


# Regression fixtures deliberately differ from the site's usual formatting.
with tempfile.TemporaryDirectory(prefix="review-deployment-") as root:
    remote = make_remote(root, "services:\n")
    config = write_config(root, "va", remote)
    fixtures = [
        ("nested enabled", "    settings:\n      enabled: true\n    enabled: true\n"),
        ("blank line", "    targetRevision: main\n\n    enabled: true\n"),
        ("unindented comment", "    targetRevision: main\n# keep this\n    enabled: true\n"),
        ("unusual indent", "      settings:\n        enabled: true\n      enabled: true\n"),
    ]
    for label, body in fixtures:
        raw = "services:\n  demo:\n" + body + "  other:\n    enabled: true\n"
        publish(remote, raw)
        before = yaml.safe_load(read(remote))
        proc = run(root, "stop", "demo", "-r", "va")
        after = yaml.safe_load(read(remote))
        check(label + ": succeeds", proc.returncode == 0, proc.stdout)
        check(label + ": only service enabled changes",
              changes(before, after) == {("demo", "enabled")} and
              after["services"]["demo"]["enabled"] is False)
        check(label + ": does not duplicate enabled",
              read(remote).count("enabled:") == raw.count("enabled:"))
        edited = read(remote)
        run(root, "stop", "demo", "-r", "va")
        check(label + ": idempotent", read(remote) == edited)

    for action, body in [("stop", "    labels:\n      enabled: true\n"),
                         ("deploy", "    enabled: true\n")]:
        raw = "services:\n  demo: {enabled: false}\n  other:\n" + body
        publish(remote, raw)
        args = [action, "demo"] + (["main"] if action == "deploy" else [])
        proc = run(root, *args, "-r", "va")
        check(action + ": inline entry rejected unchanged",
              proc.returncode != 0 and read(remote) == raw, proc.stdout)

    for label, raw, args in [
        ("nested exact name", "services:\n  demo:\n    labels:\n      enabled: true\n",
         ["stop", "labels"]),
        ("duplicate service", "services:\n  demo:\n  demo:\n", ["stop", "demo"]),
        ("duplicate key", "services:\n  demo:\n    enabled: true\n    enabled: false\n",
         ["stop", "demo"]),
        ("revision newline", "services:\n  demo:\n", ["deploy", "demo", "bad\nrevision"]),
    ]:
        publish(remote, raw)
        proc = run(root, *args, "-r", "va")
        check(label + ": rejected unchanged",
              proc.returncode != 0 and read(remote) == raw, proc.stdout)

    for revision in ["123", "true", "null", "2026-09-15", "1e3", "yes", "a'b", "a # b", "a: b", "main"]:
        publish(remote, "services:\n  demo:\n    targetRevision: 'old#tag'  # keep note\n")
        proc = run(root, "deploy", "demo", revision, "-r", "va")
        check("revision remains a string: " + revision,
              proc.returncode == 0 and yaml.safe_load(read(remote))["services"]["demo"]["targetRevision"] == revision,
              proc.stdout)
        edited = read(remote)
        check("revision comment preserved: " + revision, "  # keep note\n" in edited)
        run(root, "deploy", "demo", revision, "-r", "va")
        check("revision idempotent: " + revision, read(remote) == edited)

    raw = read(remote)
    proc = subprocess.run([TOOL, "--config", os.path.join(root, "missing.ini"),
                           "-r", "va", "stop", "demo"], cwd=root,
                          env=dict(ENV, XDG_CACHE_HOME=os.path.join(root, "cache")),
                          capture_output=True, text=True)
    check("missing explicit config cannot fall back",
          proc.returncode != 0 and read(remote) == raw, proc.stdout + proc.stderr)

    # An unavailable or failing diff must stop before anything is committed.
    # git stays reachable: the tool needs it to fetch before it gets that far.
    fake_bin = os.path.join(root, "bin")
    os.mkdir(fake_bin)
    os.symlink(shutil.which("git"), os.path.join(fake_bin, "git"))
    start = head(remote)
    for status in [None, 2]:
        if status is not None:
            fake_diff = os.path.join(fake_bin, "diff")
            open(fake_diff, "w").write("#!/bin/sh\nexit 2\n")
            os.chmod(fake_diff, 0o755)
        proc = subprocess.run([TOOL, "--config", config, "-r", "va", "stop", "demo"],
                              cwd=root, env=dict(ENV, PATH=fake_bin,
                                                 XDG_CACHE_HOME=os.path.join(root, "cache")),
                              capture_output=True, text=True)
        check("diff failure commits nothing: " + str(status),
              proc.returncode != 0 and head(remote) == start, proc.stdout + proc.stderr)

# ------------------------------------------------------------- single service

case("stop", ["stop", "sr21c-va-ioc-01"],
     [("sr21c-va-ioc-01", "enabled")], ("sr21c-va-ioc-01", "enabled", False))
case("start", ["start", "sr22c-va-ioc-01"],
     [("sr22c-va-ioc-01", "enabled")], ("sr22c-va-ioc-01", "enabled", True))
case("start entry with no keys", ["start", "sr04c-va-ioc-01"],
     [("sr04c-va-ioc-01", "enabled")], ("sr04c-va-ioc-01", "enabled", True))
case("stop entry with no keys", ["stop", "sr04c-va-ioc-01"],
     [("sr04c-va-ioc-01", "enabled")], ("sr04c-va-ioc-01", "enabled", False))
case("stop first entry", ["stop", "va-epics-pvcs"],
     [("va-epics-pvcs", "enabled")], ("va-epics-pvcs", "enabled", False))
case("stop last entry", ["stop", "sr23c-va-ioc-01"],
     [("sr23c-va-ioc-01", "enabled")], ("sr23c-va-ioc-01", "enabled", False))
case("deploy adds a revision", ["deploy", "sr06c-va-ioc-01", "2026_sd3"],
     [("sr06c-va-ioc-01", "targetRevision")],
     ("sr06c-va-ioc-01", "targetRevision", "2026_sd3"))
case("deploy changes a revision", ["deploy", "sr21c-va-ioc-01", "main"],
     [("sr21c-va-ioc-01", "targetRevision")],
     ("sr21c-va-ioc-01", "targetRevision", "main"))
case("deploy re-enables", ["deploy", "sr22c-va-ioc-01", "beam-run"],
     [("sr22c-va-ioc-01", "targetRevision"), ("sr22c-va-ioc-01", "enabled")],
     ("sr22c-va-ioc-01", "enabled", True))
case("deploy an entry with no keys", ["deploy", "sr05c-va-ioc-01", "2026_sd3"],
     [("sr05c-va-ioc-01", "targetRevision"), ("sr05c-va-ioc-01", "enabled")],
     ("sr05c-va-ioc-01", "targetRevision", "2026_sd3"))
case("deploy a new service", ["deploy", "sr30c-va-ioc-01", "2026_sd3"],
     [("sr30c-va-ioc-01", "<entry>")],
     ("sr30c-va-ioc-01", "targetRevision", "2026_sd3"))

# ------------------------------------------------------------ globs and names

for pattern in ("fe15*", "fe*", "*", "fe1[59]*", "*-mo-ioc-01"):
    hits = matching(FE, pattern)
    check("'%s' selects something to test" % pattern, len(hits) > 1, str(hits))
    for act in ("stop", "start"):
        case("glob %r %s (%d services)" % (pattern, act, len(hits)), [act, pattern],
             expected(FE, hits, act), source=FE)

case("glob deploy", ["deploy", "fe15*", "2026_sd3"],
     expected(FE, matching(FE, "fe15*"), "deploy", "2026_sd3"),
     ("fe15i-mo-ioc-01", "targetRevision", "2026_sd3"), source=FE)
case("glob leaves the rest alone", ["stop", "fe15*"],
     expected(FE, matching(FE, "fe15*"), "stop"),
     ("fe18i-cs-ioc-01", "enabled", True), source=FE)
case("glob matching nothing", ["stop", "zz*"], [], source=FE, expect_fail=True)

# Shorthand must select the same services as the obvious pattern for each: if
# the precise version in builtin.ini drifts, these two diverge.
for name, obvious in (("all", "fe[0-9]*"), ("cs", "fe*-cs-ioc-*"),
                      ("mo", "fe*-mo-ioc-*"), ("py", "fe*-py-ioc-*")):
    hits = matching(FE, obvious)
    check("shorthand %r has something to select" % name, len(hits) > 1, str(hits))
    for act in ("stop", "start"):
        case("shorthand %r %s (%d services)" % (name, act, len(hits)), [act, name],
             expected(FE, hits, act), source=FE)

check("shorthand 'all' leaves out the fe- services",
      not any(n.startswith("fe-") for n in matching(FE, "fe[0-9]*")),
      str(matching(FE, "fe[0-9]*")))
case("shorthand deploy", ["deploy", "cs", "2026_sd3"],
     expected(FE, matching(FE, "fe*-cs-ioc-*"), "deploy", "2026_sd3"),
     ("fe15i-cs-ioc-01", "targetRevision", "2026_sd3"), source=FE)
case("shorthand that matches nothing here", ["stop", "cs"], [], expect_fail=True)

# ----------------------------------------------------------- must be refused

case("unknown service", ["stop", "sr99c-va-ioc-01"], [], expect_fail=True)
# A name that is not an action at all. It used to say "restart", from before
# restart was one -- which by the time it was, meant this case was reaching the
# real ioc-restart beside the tool and only passing because the machine running
# the tests had no kubectl. The cluster actions are covered further down,
# against stubs.
case("bad action", ["frobnicate", "sr21c-va-ioc-01"], [], expect_fail=True)
case("deploy with no revision", ["deploy", "sr21c-va-ioc-01"], [], expect_fail=True)
case("no arguments", [], [], expect_fail=True)

# ------------------------------------------------------------ awkward layouts

tmp = tempfile.mkdtemp(prefix="odd-")
raw = open(VA).read()

no_eol = os.path.join(tmp, "no-eol.yaml")
open(no_eol, "w").write(raw.rstrip("\n"))
case("no trailing newline", ["stop", "sr23c-va-ioc-01"],
     [("sr23c-va-ioc-01", "enabled")], ("sr23c-va-ioc-01", "enabled", False), source=no_eol)
case("append with no trailing newline", ["deploy", "sr30c-va-ioc-01", "main"],
     [("sr30c-va-ioc-01", "<entry>")], ("sr30c-va-ioc-01", "enabled", True), source=no_eol)

comment = os.path.join(tmp, "comment.yaml")
open(comment, "w").write(raw.replace("  sr22c-va-ioc-01:\n    enabled: false\n",
                                     "  # cell 22 is on the sd3 branch\n"
                                     "  sr22c-va-ioc-01:\n    enabled: false  # note\n"))
case("comments around the entry", ["start", "sr22c-va-ioc-01"],
     [("sr22c-va-ioc-01", "enabled")], ("sr22c-va-ioc-01", "enabled", True), source=comment)

after_block = os.path.join(tmp, "after.yaml")
open(after_block, "w").write(raw + "\nextraKey:\n  foo: bar\n")
case("key after the services block", ["deploy", "sr30c-va-ioc-01", "main"],
     [("sr30c-va-ioc-01", "<entry>")], ("sr30c-va-ioc-01", "enabled", True),
     source=after_block)

trailing = os.path.join(tmp, "trailing.yaml")
open(trailing, "w").write(raw + "\n\n")
case("trailing blank lines", ["deploy", "sr30c-va-ioc-01", "main"],
     [("sr30c-va-ioc-01", "<entry>")], ("sr30c-va-ioc-01", "enabled", True),
     source=trailing)

# YAML does not care, but a new entry parked after the blank lines at the end
# of the file looks like a mistake in review.
gap = tempfile.mkdtemp(prefix="gap-")
gap_remote = make_remote(gap, open(trailing).read())
write_config(gap, "va", gap_remote)
run(gap, "deploy", "sr30c-va-ioc-01", "main")
grown = read(gap_remote)
check("new entry is appended without a gap",
      "\n\n  sr30c-va-ioc-01:" not in grown and "  sr30c-va-ioc-01:\n" in grown,
      repr(grown[-120:]))
shutil.rmtree(gap, ignore_errors=True)
shutil.rmtree(tmp, ignore_errors=True)

# ----------------------------------------------------------- repo resolution

SANDBOX = tempfile.mkdtemp(prefix="resolve-")
FE_REMOTE = make_remote(SANDBOX, open(FE).read(), "fe-deployment")
VA_REMOTE = make_remote(SANDBOX, open(VA).read(), "va-deployment")

RESOLVE_CFG = os.path.join(SANDBOX, "config.ini")
resolve_cfg = configparser.ConfigParser(interpolation=None)
resolve_cfg["repos"] = {"fe": FE_REMOTE, "va": VA_REMOTE}
resolve_cfg["match"] = dict(SHIPPED["match"])
resolve_cfg["fe.aliases"] = dict(SHIPPED["fe.aliases"])
resolve_cfg["va.aliases"] = dict(SHIPPED["va.aliases"])
with open(RESOLVE_CFG, "w") as handle:
    resolve_cfg.write(handle)


def spoke(argv, config=RESOLVE_CFG):
    proc = subprocess.run([TOOL, "--dry-run"]
                          + (["--config", config] if config else []) + argv,
                          cwd=SANDBOX, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True,
                          env=dict(ENV, XDG_CACHE_HOME=os.path.join(SANDBOX, "cache")))
    return proc.stdout


def resolves(label, argv, want, **kwargs):
    """The first line says which repo was picked and why."""
    out = spoke(argv, **kwargs)
    first = out.splitlines()[0] if out.strip() else ""
    check("resolve: " + label, want in first, "got %r" % first)


def says(label, argv, want, **kwargs):
    out = spoke(argv, **kwargs)
    check("resolve: " + label, want in out, "got %r" % out.strip()[:200])


says("--list before any run: nothing cloned yet", ["--list"], "fe         not cloned")
resolves("service name picks va", ["stop", "sr22c-va-ioc-01"], "matches sr*-va-ioc-*")
resolves("service name picks fe", ["stop", "fe15i-cs-ioc-01"], "matches fe*")
resolves("glob picks fe", ["stop", "fe15*"], "matches fe*")
resolves("shorthand picks its own repo", ["stop", "cs"], "'cs' is a fe shorthand")
resolves("-r key beats the service name", ["stop", "fe15*", "-r", "va"], "-r va")
says("ambiguous shorthand asks", ["stop", "all"], "'all' is a shorthand in fe and va")
says("unknown -r key lists the real ones", ["stop", "x", "-r", "nope"],
     "no repo 'nope' in the config")
says("a path is not a repo key", ["stop", "x", "-r", SANDBOX], "no repo")
says("unmatched name is an error", ["stop", "zzz-nothing"], "cannot tell which repo")
says("foreign shorthand explains itself", ["stop", "cs", "-r", "va"],
     "'cs' is a shorthand in fe")
says("--dry-run still shows the edit", ["stop", "sr21c-va-ioc-01"], "-    enabled: true")
check("resolve: --dry-run committed nothing",
      read(VA_REMOTE) == open(VA).read() and read(FE_REMOTE) == open(FE).read())
says("--list after a run shows the clone", ["--list"], "va         cached")
check("resolve: the clone is in the cache, named after the repo",
      os.path.isfile(os.path.join(SANDBOX, "cache", "deployment-repo-tool",
                                  "va-deployment.git", "HEAD")))

# Version is embedded and needs neither a repo nor valid config.
versioned = subprocess.run([TOOL, "--version", "--config", "/missing/config.ini"],
                           cwd=ROOT, text=True, capture_output=True)
check("--version works without config", versioned.returncode == 0 and
      versioned.stdout.startswith("ioc ") and len(versioned.stdout.split()) == 2,
      versioned.stdout + versioned.stderr)

# With no --config the built-in one is used, and it names the GitLab remotes.
# --list contacts nothing, so this is safe to run anywhere.
builtin = spoke(["--list"], config=None)
check("built-in config is used without --config",
      "config  built in" in builtin and "git@gitlab.diamond.ac.uk:" in builtin, builtin)
shutil.rmtree(SANDBOX, ignore_errors=True)

# ----------------------------------------------------------- on the cluster

# restart and exec shell out to scripts that talk to a real cluster, so each is
# replaced with one that records how it was called. What is worth proving is
# that the tool picks the right script, passes exactly the service name,
# reports its exit status, and leaves the repo completely alone.
#
# The mapping is written out again here rather than imported: the tool's
# filename has a dash in it and will not import, and an independent statement
# of what must exist is worth more in a test anyway.

ACTIONS = {"restart": "ioc-restart", "exec": "ioc-exec"}

CLUSTER = tempfile.mkdtemp(prefix="cluster-")
RECORD = os.path.join(CLUSTER, "argv")

CLUSTER_REPO = make_remote(CLUSTER, open(VA).read())

# Each stub records which action it belongs to as well as its argv, so a run
# that reached the wrong script cannot pass for the right one.
STUBS = {}
for action, name in ACTIONS.items():
    STUBS[action] = os.path.join(CLUSTER, name)
    with open(STUBS[action], "w") as handle:
        handle.write('#!/bin/bash\nprintf "%s\\n" ' + action
                     + ' "$@" > ' + RECORD + '\nexit ${STUB_EXIT:-0}\n')
    os.chmod(STUBS[action], 0o755)

cluster_cfg = configparser.ConfigParser(interpolation=None)
cluster_cfg["repos"] = {"va": CLUSTER_REPO}
cluster_cfg["general"] = dict(("%s_script" % a, STUBS[a]) for a in ACTIONS)
CLUSTER_CFG = os.path.join(CLUSTER, "config.ini")
with open(CLUSTER_CFG, "w") as handle:
    cluster_cfg.write(handle)


def ran(*args, **kwargs):
    if os.path.exists(RECORD):
        os.remove(RECORD)
    return subprocess.run([kwargs.get("tool", TOOL), "--config",
                           kwargs.get("config", CLUSTER_CFG)] + list(args),
                          cwd=CLUSTER,
                          env=dict(ENV, XDG_CACHE_HOME=os.path.join(CLUSTER, "cache"),
                                   **kwargs.get("env", {})),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True)


def recorded():
    if not os.path.exists(RECORD):
        return None
    with open(RECORD) as handle:
        return handle.read().split()


helped = subprocess.run([TOOL, "--help"], stdout=subprocess.PIPE,
                        universal_newlines=True).stdout
for action in sorted(ACTIONS):
    check("%s: the tool offers it as an action" % action, action in helped)

for action in sorted(ACTIONS):
    untouched = head(CLUSTER_REPO)

    proc = ran(action, "fe22i-mo-ioc-01")
    check("%s: the run succeeded" % action, proc.returncode == 0,
          proc.stdout.strip()[-300:])
    check("%s: its own script got exactly the service name" % action,
          recorded() == [action, "fe22i-mo-ioc-01"], recorded())
    check("%s: the repo was not touched" % action, head(CLUSTER_REPO) == untouched)

    # A failure must not report success: a dead IOC would look restarted.
    proc = ran(action, "fe22i-mo-ioc-01", env={"STUB_EXIT": "3"})
    check("%s: the script's exit status is passed on" % action,
          proc.returncode == 3, "got %d" % proc.returncode)

    proc = ran("--dry-run", action, "fe22i-mo-ioc-01")
    check("%s: --dry-run says what it would run" % action,
          "would run" in proc.stdout and "fe22i-mo-ioc-01" in proc.stdout,
          proc.stdout.strip()[:200])
    check("%s: --dry-run ran nothing" % action, recorded() is None, recorded())

    proc = ran(action, "fe15*")
    check("%s: a glob is refused" % action,
          proc.returncode != 0 and "not a glob" in proc.stdout,
          proc.stdout.strip()[:200])
    check("%s: a refused glob ran nothing" % action, recorded() is None, recorded())

    # The third positional is deploy's revision. Silently ignoring it here
    # would swallow a mistyped command.
    proc = ran(action, "fe22i-mo-ioc-01", "2026_sd3")
    check("%s: a stray extra argument is refused" % action,
          proc.returncode != 0 and "only a service name" in proc.stdout,
          proc.stdout.strip()[-200:])
    check("%s: a refused extra argument ran nothing" % action,
          recorded() is None, recorded())

    # No repo is resolved, so nothing is cloned or fetched.
    check("%s: needs no deployment repo" % action,
          not os.path.exists(os.path.join(CLUSTER, "cache")))

    absent = configparser.ConfigParser(interpolation=None)
    absent["general"] = {"%s_script" % action: os.path.join(CLUSTER, "not-there")}
    absent_cfg = os.path.join(CLUSTER, "absent-%s.ini" % action)
    with open(absent_cfg, "w") as handle:
        absent.write(handle)
    proc = ran(action, "fe22i-mo-ioc-01", config=absent_cfg)
    check("%s: a configured script that is not there is an error" % action,
          proc.returncode != 0 and "which is not there" in proc.stdout,
          proc.stdout.strip()[:200])

# A copied binary must run the embedded scripts without any siblings. Stub
# kubectl, not the helper, so this exercises the actual embedded script bodies.
ALONE = os.path.join(CLUSTER, "alone")
os.makedirs(ALONE)
ALONE_TOOL = os.path.join(ALONE, "ioc")
shutil.copy(TOOL, ALONE_TOOL)
LINKED = os.path.join(CLUSTER, "ioc-link")
os.symlink(ALONE_TOOL, LINKED)
BARE_CLUSTER_CFG = os.path.join(CLUSTER, "bare.ini")
with open(BARE_CLUSTER_CFG, "w") as handle:
    # No script overrides, so the embedded helpers run; a repo for status.
    handle.write("[repos]\nva = %s\n[general]\n" % CLUSTER_REPO)
KUBE_BIN = os.path.join(CLUSTER, "stub-bin")
os.makedirs(KUBE_BIN)
with open(os.path.join(KUBE_BIN, "kubectl"), "w") as handle:
    handle.write('#!/bin/bash\nprintf "%s\\n" "$*" >> "$KUBE_RECORD"\n'
                 'if [[ $1 == get ]]; then echo 1; else exit "${STUB_EXIT:-0}"; fi\n')
os.chmod(os.path.join(KUBE_BIN, "kubectl"), 0o755)
KUBE_RECORD = os.path.join(CLUSTER, "kubectl-calls")
kube_env = dict(PATH=KUBE_BIN + os.pathsep + os.environ["PATH"],
                KUBE_RECORD=KUBE_RECORD, EC_TARGET="accelerator",
                # Any address counts as the primary network, so status queries.
                IOC_PRIMARY_NETWORK="0.0.0.0/0")
for action in sorted(ACTIONS):
    for binary in (ALONE_TOOL, LINKED):
        open(KUBE_RECORD, "w").close()
        proc = ran(action, "fe22i-mo-ioc-01", tool=binary,
                   config=BARE_CLUSTER_CFG, env=kube_env)
        calls = open(KUBE_RECORD).read()
        check("%s: standalone binary/symlink runs embedded helper" % action,
              proc.returncode == 0 and "get statefulset fe22i-mo-ioc-01" in calls,
              proc.stdout + calls)
        expected_call = ("delete pod -n accelerator -l app=fe22i-mo-ioc-01"
                         if action == "restart" else "statefulset/fe22i-mo-ioc-01 -- bash")
        check("%s: embedded helper reaches kubectl" % action, expected_call in calls, calls)
    proc = ran(action, "fe22i-mo-ioc-01", tool=ALONE_TOOL,
               config=BARE_CLUSTER_CFG, env=dict(kube_env, STUB_EXIT="7"))
    check("%s: embedded helper preserves failure status" % action, proc.returncode == 7)
    open(KUBE_RECORD, "w").close()
    proc = ran(action, "fe22i-mo-ioc-01", "--dry-run", tool=ALONE_TOOL,
               config=BARE_CLUSTER_CFG, env=kube_env)
    check("%s: embedded preview does not run kubectl" % action,
          proc.returncode == 0 and "(embedded)" in proc.stdout
          and not open(KUBE_RECORD).read())

# status is a cluster action that does read the repo: a glob or shorthand is
# expanded against values.yaml, and the script's rows are lined up against it.
STATUS_STUB = os.path.join(CLUSTER, "ioc-status")
with open(STATUS_STUB, "w") as handle:
    handle.write('#!/bin/bash\nprintf "%s\\n" status "$@" > ' + RECORD + '\n'
                 'echo "SERVICE NODE STATUS RESTARTS AGE POD"\n'
                 'echo "sr21c-va-ioc-01 node-a Running 2 3d sr21c-va-ioc-01-0"\n'
                 'exit ${STUB_EXIT:-0}\n')
os.chmod(STATUS_STUB, 0o755)
status_cfg = configparser.ConfigParser(interpolation=None)
status_cfg["repos"] = {"va": CLUSTER_REPO}
status_cfg["va.aliases"] = {"all": "sr2[0-9]c-va-ioc-*"}
status_cfg["general"] = {"status_script": STATUS_STUB}
STATUS_CFG = os.path.join(CLUSTER, "status.ini")
with open(STATUS_CFG, "w") as handle:
    status_cfg.write(handle)

check("status: the tool offers it as an action", "status" in helped)
untouched = head(CLUSTER_REPO)
proc = ran("status", "all", "-r", "va", config=STATUS_CFG)
check("status: a shorthand passes every matching name to the script",
      (recorded() or [""])[0] == "status" and
      sorted(recorded()[1:]) == ["sr20c-va-ioc-01", "sr21c-va-ioc-01", "sr22c-va-ioc-01",
                                 "sr23c-va-ioc-01", "sr24c-va-ioc-02"], recorded())
rows = dict((line.split()[0], line.split()) for line in proc.stdout.splitlines()
            if line.startswith("sr"))
check("status: a running pod shows its node",
      proc.returncode == 0 and rows.get("sr21c-va-ioc-01", [None] * 4)[3] == "node-a",
      proc.stdout)
check("status: a stopped service without a pod still gets a row",
      rows.get("sr22c-va-ioc-01", [None] * 5)[1:5:3] == ["false", "stopped"], proc.stdout)
check("status: the repo was not touched", head(CLUSTER_REPO) == untouched)
check("status: no override shows the inherited source revision",
      rows.get("sr20c-va-ioc-01", [None] * 3)[2] == "main"
      and rows.get("sr21c-va-ioc-01", [None] * 3)[2] == "2026_sd3", proc.stdout)

# Off the primary network the script answers 3 without trying the cluster, and
# the repo half is shown alone.
proc = ran("status", "all", "-r", "va", config=STATUS_CFG, env={"STUB_EXIT": "3"})
check("status: off the network, the repo state alone and success",
      proc.returncode == 0 and "NODE" not in proc.stdout
      and "sr21c-va-ioc-01  true     2026_sd3" in proc.stdout, proc.stdout)
proc = ran("status", "--node", "node-a", config=STATUS_CFG, env={"STUB_EXIT": "3"})
check("status --node: off the network is a failure", proc.returncode == 3, proc.stdout)

proc = ran("status", "--node", "node-a", config=STATUS_CFG)
check("status --node: passed to the script, no repo needed",
      proc.returncode == 0 and recorded() == ["status", "--node", "node-a"]
      and "node-a" in proc.stdout, proc.stdout + str(recorded()))
proc = ran("status", "--node", "node-a", config=STATUS_CFG, env={"STUB_EXIT": "4"})
check("status: the script's exit status is passed on", proc.returncode == 4)
for label, args in (("--node with another action", ["stop", "--node", "n"]),
                    ("--node with a service", ["status", "sr21c-va-ioc-01", "--node", "n"]),
                    ("a revision", ["status", "sr21c-va-ioc-01", "2026_sd3"]),
                    ("--force-sync", ["status", "sr21c-va-ioc-01", "--force-sync"])):
    proc = ran(*args, config=STATUS_CFG)
    check("status: %s is a usage error" % label,
          proc.returncode == 2 and recorded() is None, proc.stdout.strip()[-200:])
proc = ran("--dry-run", "status", "sr21c-va-ioc-01", "-r", "va", config=STATUS_CFG)
check("status: --dry-run ran nothing",
      "would run" in proc.stdout and recorded() is None, proc.stdout)

# The embedded script, against a stub kubectl: one query for all the names.
open(KUBE_RECORD, "w").close()
proc = ran("status", "sr2[12]c-va-ioc-01", "-r", "va", tool=ALONE_TOOL, config=BARE_CLUSTER_CFG,
           env=kube_env)
calls = open(KUBE_RECORD).read()
check("status: embedded helper makes one kubectl query for every name",
      proc.returncode == 0 and calls.count("get pods") == 1
      and "-l app in (sr21c-va-ioc-01,sr22c-va-ioc-01)" in calls, proc.stdout + calls)
open(KUBE_RECORD, "w").close()
proc = ran("status", "--node", "node-a", tool=ALONE_TOOL, config=BARE_CLUSTER_CFG,
           env=kube_env)
calls = open(KUBE_RECORD).read()
check("status --node: embedded helper selects pods by node",
      proc.returncode == 0 and "--field-selector spec.nodeName=node-a" in calls,
      proc.stdout + calls)

open(KUBE_RECORD, "w").close()
proc = ran("status", "sr2[12]c-va-ioc-01", "-r", "va", tool=ALONE_TOOL, config=BARE_CLUSTER_CFG,
           env=dict(kube_env, IOC_PRIMARY_NETWORK="192.0.2.0/24"))
check("status: embedded helper off the network never runs kubectl",
      proc.returncode == 0 and not open(KUBE_RECORD).read()
      and "not on the primary network" in proc.stdout and "NODE" not in proc.stdout,
      proc.stdout)

shutil.rmtree(CLUSTER, ignore_errors=True)

# ------------------------------------------------------------------- the git

# The real flow against a bare remote: fetch into the cache, commit without a
# working tree, push. Someone else's clone of the same remote plays the rest of
# the team.
GIT = tempfile.mkdtemp(prefix="git-")
REMOTE = os.path.join(GIT, "va-deployment.git")
OTHER = os.path.join(GIT, "other")
CACHED = os.path.join(GIT, "cache", "deployment-repo-tool", "va-deployment.git")


def git(*args, **kwargs):
    return subprocess.run(["git"] + list(args), cwd=kwargs.get("cwd", OTHER), env=ENV,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True)


git("init", "-q", "--bare", REMOTE, cwd=GIT)
git("--git-dir", REMOTE, "symbolic-ref", "HEAD", "refs/heads/main", cwd=GIT)
git("clone", "-q", REMOTE, OTHER, cwd=GIT)
os.makedirs(os.path.join(OTHER, "apps"))
shutil.copy(VA, os.path.join(OTHER, "apps", "values.yaml"))
# Something else in the repo: the tool's commit must carry it over untouched.
open(os.path.join(OTHER, "README.md"), "w").write("not the tool's\n")
git("add", "-A")
git("commit", "-qm", "initial")
git("push", "-q", "origin", "HEAD:main")
write_config(GIT, "va", REMOTE)


def log(fmt="%s"):
    return git("--git-dir", REMOTE, "log", "-1", "--format=" + fmt, "main").stdout.strip()


proc = run(GIT, "stop", "sr21c-va-ioc-01")
check("git: the run succeeded", proc.returncode == 0, proc.stdout.strip()[-300:])
check("git: commit message reads well", log() == "Stopping sr21c-va-ioc-01", log())
check("git: the commit is the user's", log("%an <%ae>") == "Tester <test@example.com>",
      log("%an <%ae>"))
check("git: the change reached the remote",
      "  sr21c-va-ioc-01:\n    enabled: false\n" in read(REMOTE))
committed = git("--git-dir", REMOTE, "show", "--name-only", "--format=", "main").stdout.split()
check("git: only values.yaml changed", committed == ["apps/values.yaml"], committed)
check("git: the rest of the repo is carried over",
      git("--git-dir", REMOTE, "show", "main:README.md").stdout == "not the tool's\n")
check("git: one commit on top of the last, no merge",
      git("--git-dir", REMOTE, "rev-list", "--count", "main").stdout.strip() == "2")
check("git: the cache is a bare clone with no working tree",
      git("--git-dir", CACHED, "rev-parse", "--is-bare-repository").stdout.strip() == "true"
      and not os.path.exists(os.path.join(GIT, "cache", "deployment-repo-tool",
                                          "va-deployment")))
check("git: the cache matches the remote after a push",
      git("--git-dir", CACHED, "rev-parse", "main").stdout == git(
          "--git-dir", REMOTE, "rev-parse", "main").stdout)

count = head(REMOTE)
again = run(GIT, "stop", "sr21c-va-ioc-01")
check("git: nothing to do is a success", again.returncode == 0, again.stdout.strip())
check("git: nothing to do makes no commit", head(REMOTE) == count)

proc = run(GIT, "deploy", "sr30c-va-ioc-01", "2026_sd3")
check("git: deploy names the revision in the message",
      log() == "Deploying sr30c-va-ioc-01 on 2026_sd3", log())

# Someone else pushes after the cache was last fetched. The next run must see
# their change and build on it, never push over it.
git("pull", "-q", "origin", "main")
values = os.path.join(OTHER, "apps", "values.yaml")
text = open(values).read().replace(
    "  sr23c-va-ioc-01:\n", "  # someone else's note\n  sr23c-va-ioc-01:\n")
open(values, "w").write(text)
git("commit", "-qam", "someone else's commit")
git("push", "-q", "origin", "HEAD:main")
theirs = head(REMOTE)
proc = run(GIT, "stop", "sr20c-va-ioc-01")
check("git: a stale cache is fetched first", proc.returncode == 0, proc.stdout[-300:])
check("git: someone else's commit is kept underneath ours",
      git("--git-dir", REMOTE, "rev-parse", "main~1").stdout.strip() == theirs
      and "someone else's note" in read(REMOTE))

# A push the server refuses -- someone got in first, or a protected branch --
# must change nothing, here or in the cache, and a re-run must just work.
hook = os.path.join(REMOTE, "hooks", "pre-receive")
open(hook, "w").write("#!/bin/sh\nexit 1\n")
os.chmod(hook, 0o755)
before = head(REMOTE)
refused = run(GIT, "stop", "sr24c-va-ioc-02")
check("git: a refused push is an error", refused.returncode != 0
      and "run the command again" in refused.stdout, refused.stdout[-300:])
check("git: a refused push changes nothing", head(REMOTE) == before)
check("git: a refused push leaves the cache at the remote's tip",
      git("--git-dir", CACHED, "rev-parse", "main").stdout.strip() == before)
os.remove(hook)
proc = run(GIT, "stop", "sr24c-va-ioc-02")
check("git: after a refused push, a re-run succeeds",
      proc.returncode == 0 and log() == "Stopping sr24c-va-ioc-02", proc.stdout[-300:])
shutil.rmtree(GIT, ignore_errors=True)

print("")
print("%d FAILURE(S): %s" % (len(fails), ", ".join(fails)) if fails else "all checks passed")
sys.exit(1 if fails else 0)
