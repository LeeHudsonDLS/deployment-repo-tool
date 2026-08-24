#!/usr/bin/env python3
# /// script
# requires-python = ">=3.8"
# dependencies = ["pyyaml"]
# ///
"""Tests for deployment-repo-tool.

    uv run tests/test_tool.py                 # or --python 3.8 to check that too

Every edit is checked by loading the result with a real YAML parser and
comparing it against the original: the tool writes YAML by hand, so the thing
worth proving is that the document means what it should afterwards, and that
nothing else in it moved.

Nothing here touches a real deployment repo. Fixtures are copied into a
temporary directory and the tool is pointed at that copy with a generated
--config; setting the working directory alone is not enough, because the tool
can infer a repo from the service name.
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
TOOL = os.path.join(ROOT, "deployment-repo-tool.py")
VA = os.path.join(HERE, "va-values.yaml")
FE = os.path.join(HERE, "fe-values.yaml")

SHIPPED = configparser.ConfigParser(interpolation=None)
SHIPPED.read(os.path.join(ROOT, "config.ini"))

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


def write_config(root, key):
    """Point `key` at this temp copy, carrying over the shipped shorthand."""
    config = configparser.ConfigParser(interpolation=None)
    config["repos"] = {key: root}
    config["match"] = {key: "*"}
    if SHIPPED.has_section("%s.aliases" % key):
        config["%s.aliases" % key] = dict(SHIPPED["%s.aliases" % key])
    path = os.path.join(root, "config.ini")
    with open(path, "w") as handle:
        config.write(handle)
    return path


def run(root, *args, **kwargs):
    argv = [sys.executable, TOOL]
    if not kwargs.get("with_git"):
        argv.append("--no-git")
    config = kwargs.get("config") or os.path.join(root, "config.ini")
    return subprocess.run(argv + ["--config", config] + list(args),
                          cwd=kwargs.get("cwd") or root,
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


def case(label, args, expect, state=None, source=VA, cwd=None, expect_fail=False,
         key=None):
    """Run one command against a throwaway copy and check what it did."""
    root = tempfile.mkdtemp(prefix="va-deployment-")
    try:
        os.makedirs(os.path.join(root, "apps"))
        os.makedirs(os.path.join(root, ".git"))
        path = os.path.join(root, "apps", "values.yaml")
        shutil.copy(source, path)
        write_config(root, key or ("fe" if source == FE else "va"))
        before, raw = load(path), open(path).read()
        proc = run(root, *args, cwd=os.path.join(root, cwd) if cwd else root)
        if expect_fail:
            check(label + " (rejected)", proc.returncode != 0, proc.stdout.strip())
            check(label + " (untouched)", open(path).read() == raw)
            return
        if proc.returncode != 0:
            check(label, False, proc.stdout.strip())
            return
        try:
            after = load(path)
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
        check(label + " (comments kept)", open(path).read().count("#") == raw.count("#"))
        was = open(path).read()
        run(root, *args)
        check(label + " (idempotent)", open(path).read() == was)
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
case("from a subdirectory", ["stop", "sr21c-va-ioc-01"],
     [("sr21c-va-ioc-01", "enabled")], ("sr21c-va-ioc-01", "enabled", False), cwd="apps")

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
# the precise version in config.ini drifts, these two diverge.
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
case("bad action", ["restart", "sr21c-va-ioc-01"], [], expect_fail=True)
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
gap = tempfile.mkdtemp(prefix="va-deployment-")
os.makedirs(os.path.join(gap, "apps"))
os.makedirs(os.path.join(gap, ".git"))
shutil.copy(trailing, os.path.join(gap, "apps", "values.yaml"))
write_config(gap, "va")
run(gap, "deploy", "sr30c-va-ioc-01", "main")
grown = open(os.path.join(gap, "apps", "values.yaml")).read()
check("new entry is appended without a gap",
      "\n\n  sr30c-va-ioc-01:" not in grown and "  sr30c-va-ioc-01:\n" in grown,
      repr(grown[-120:]))
shutil.rmtree(gap, ignore_errors=True)
shutil.rmtree(tmp, ignore_errors=True)

# ----------------------------------------------------------- repo resolution

SANDBOX = tempfile.mkdtemp(prefix="resolve-")          # not a deployment repo
FE_REPO = os.path.join(SANDBOX, "fe-deployment")
VA_REPO = os.path.join(SANDBOX, "va-deployment")
for repo, fixture in ((FE_REPO, FE), (VA_REPO, VA)):
    os.makedirs(os.path.join(repo, "apps"))
    os.makedirs(os.path.join(repo, ".git"))
    shutil.copy(fixture, os.path.join(repo, "apps", "values.yaml"))

RESOLVE_CFG = os.path.join(SANDBOX, "config.ini")
resolve_cfg = configparser.ConfigParser(interpolation=None)
resolve_cfg["repos"] = {"fe": FE_REPO, "va": VA_REPO}
resolve_cfg["match"] = dict(SHIPPED["match"])
resolve_cfg["fe.aliases"] = dict(SHIPPED["fe.aliases"])
resolve_cfg["va.aliases"] = dict(SHIPPED["va.aliases"])
with open(RESOLVE_CFG, "w") as handle:
    resolve_cfg.write(handle)


def spoke(argv, cwd=SANDBOX, config=RESOLVE_CFG, env=None):
    proc = subprocess.run([sys.executable, TOOL, "--dry-run"]
                          + (["--config", config] if config else []) + argv,
                          cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True, env=env)
    return proc.stdout


def resolves(label, argv, want, **kwargs):
    """The first line says which repo was picked and why."""
    out = spoke(argv, **kwargs)
    first = out.splitlines()[0] if out.strip() else ""
    check("resolve: " + label, want in first, "got %r" % first)


def says(label, argv, want, **kwargs):
    out = spoke(argv, **kwargs)
    check("resolve: " + label, want in out, "got %r" % out.strip()[:200])


resolves("service name picks va", ["stop", "sr22c-va-ioc-01"], "matches sr*-va-ioc-*")
resolves("service name picks fe", ["stop", "fe15i-cs-ioc-01"], "matches fe*")
resolves("glob picks fe", ["stop", "fe15*"], "matches fe*")
resolves("shorthand picks its own repo", ["stop", "cs"], "'cs' is a fe shorthand")
resolves("-r key beats the service name", ["stop", "fe15*", "-r", "va"], "-r va")
resolves("-r path", ["stop", "sr22c-va-ioc-01", "-r", VA_REPO], "as a path")
resolves("cwd beats the service name", ["stop", "fe15*"], "current directory", cwd=VA_REPO)
resolves("cwd from a subdirectory", ["stop", "sr22c-va-ioc-01"], "current directory",
         cwd=os.path.join(VA_REPO, "apps"))
resolves("cwd names the repo it matched", ["stop", "sr22c-va-ioc-01"], "va  ", cwd=VA_REPO)
says("ambiguous shorthand asks", ["stop", "all"], "'all' is a shorthand in fe and va")
says("unknown -r key lists the real ones", ["stop", "x", "-r", "nope"],
     "no repo 'nope' in the config")
says("unmatched name is an error", ["stop", "zzz-nothing"], "cannot tell which repo")
says("foreign shorthand explains itself", ["stop", "cs"],
     "'cs' is a shorthand in fe", cwd=VA_REPO)
says("--list shows each repo and its state", ["--list"], "fe         ok")

# With no config at all it must still work the way it did before there was one:
# on the repo the current directory is in.
BARE = tempfile.mkdtemp(prefix="bare-")
shutil.copy(TOOL, os.path.join(BARE, "tool.py"))
bare_env = dict(os.environ, XDG_CONFIG_HOME=BARE)
bare_env.pop("DEPLOYMENT_REPO_CONFIG", None)
bare = subprocess.run([sys.executable, os.path.join(BARE, "tool.py"), "--dry-run",
                       "stop", "sr21c-va-ioc-01"], cwd=VA_REPO, env=bare_env,
                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                      universal_newlines=True)
check("resolve: works with no config file", "current directory" in bare.stdout,
      bare.stdout.strip()[:200])
check("resolve: no config file, edit still correct", "-    enabled: true" in bare.stdout,
      bare.stdout.strip()[:200])
check("resolve: --dry-run left the repos alone",
      open(os.path.join(VA_REPO, "apps", "values.yaml")).read() == open(VA).read())
shutil.rmtree(SANDBOX, ignore_errors=True)
shutil.rmtree(BARE, ignore_errors=True)

# ------------------------------------------------------------------- the git

GIT = tempfile.mkdtemp(prefix="git-")
REMOTE = os.path.join(GIT, "va-deployment.git")
CLONE = os.path.join(GIT, "va-deployment")


def git(*args, **kwargs):
    return subprocess.run(["git"] + list(args), cwd=kwargs.get("cwd", CLONE),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True)


git("init", "-q", "--bare", REMOTE, cwd=GIT)
git("clone", "-q", REMOTE, CLONE, cwd=GIT)
git("config", "user.email", "test@example.com")
git("config", "user.name", "Tester")
os.makedirs(os.path.join(CLONE, "apps"))
shutil.copy(VA, os.path.join(CLONE, "apps", "values.yaml"))
git("add", "-A")
git("commit", "-qm", "initial")
git("push", "-q", "-u", "origin", "HEAD:main")
write_config(CLONE, "va")

# Something else staged: it must not be swept into our commit.
open(os.path.join(CLONE, "unrelated.txt"), "w").write("someone else's work\n")
git("add", "unrelated.txt")

proc = run(CLONE, "stop", "sr21c-va-ioc-01", with_git=True)
check("git: the run succeeded", proc.returncode == 0, proc.stdout.strip()[-300:])
check("git: commit message reads well",
      git("log", "-1", "--format=%s").stdout.strip() == "Stopping sr21c-va-ioc-01",
      git("log", "-1", "--format=%s").stdout.strip())
pushed = git("show", "main:apps/values.yaml", cwd=REMOTE).stdout
check("git: the change reached the remote",
      "  sr21c-va-ioc-01:\n    enabled: false\n" in pushed)
committed = git("show", "--name-only", "--format=", "HEAD").stdout.split()
check("git: only values.yaml was committed", committed == ["apps/values.yaml"], committed)
check("git: the other staged file is still staged, uncommitted",
      "A  unrelated.txt" in git("status", "--porcelain").stdout)

count = git("rev-list", "--count", "HEAD").stdout.strip()
again = run(CLONE, "stop", "sr21c-va-ioc-01", with_git=True)
check("git: nothing to do is a success", again.returncode == 0, again.stdout.strip())
check("git: nothing to do makes no commit",
      git("rev-list", "--count", "HEAD").stdout.strip() == count)

proc = run(CLONE, "deploy", "sr30c-va-ioc-01", "2026_sd3", with_git=True)
check("git: deploy names the revision in the message",
      git("log", "-1", "--format=%s").stdout.strip()
      == "Deploying sr30c-va-ioc-01 on 2026_sd3",
      git("log", "-1", "--format=%s").stdout.strip())

# A branch that has diverged from its remote must stop the tool, never turn
# into a merge or a rebase of someone else's work.
OTHER = os.path.join(GIT, "other")
git("clone", "-q", REMOTE, OTHER, cwd=GIT)
git("config", "user.email", "other@example.com", cwd=OTHER)
git("config", "user.name", "Other", cwd=OTHER)
open(os.path.join(OTHER, "theirs.txt"), "w").write("theirs\n")
git("add", "-A", cwd=OTHER)
git("commit", "-qm", "someone else's commit", cwd=OTHER)
git("push", "-q", "origin", "HEAD:main", cwd=OTHER)
open(os.path.join(CLONE, "mine.txt"), "w").write("mine\n")
git("add", "mine.txt")
git("commit", "-qm", "my unpushed commit")

# Plenty of people set this globally. --ff-only has to win over it: rebasing
# someone's unpushed commit underneath them is not this tool's decision to make.
git("config", "pull.rebase", "true")

head = git("rev-parse", "HEAD").stdout.strip()
values = open(os.path.join(CLONE, "apps", "values.yaml")).read()
diverged = run(CLONE, "stop", "sr22c-va-ioc-01", with_git=True)
check("git: a diverged branch stops the tool", diverged.returncode != 0,
      diverged.stdout.strip()[-200:])
check("git: a diverged branch produces no commit",
      git("rev-parse", "HEAD").stdout.strip() == head)
check("git: a diverged branch leaves the file alone",
      open(os.path.join(CLONE, "apps", "values.yaml")).read() == values)
shutil.rmtree(GIT, ignore_errors=True)

print("")
print("%d FAILURE(S): %s" % (len(fails), ", ".join(fails)) if fails else "all checks passed")
sys.exit(1 if fails else 0)
