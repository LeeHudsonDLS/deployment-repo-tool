# deployment-repo-tool

Start, stop and deploy services in a Diamond ArgoCD `*-deployment` repo, without
hand-editing `apps/values.yaml` and without getting the git dance wrong. It also
restarts a running IOC and opens a shell in one, which are the things it does
that leave the repo alone.

```console
$ deployment-repo-tool stop sr22c-va-ioc-01
repo    va  /path/to/va-deployment  ('sr22c-va-ioc-01' matches sr*-va-ioc-*)
Already up to date.
--- apps/values.yaml
+++ apps/values.yaml
@@ -69,7 +69,7 @@
     labels:
       description:
   sr22c-va-ioc-01:
-    enabled: true
+    enabled: false
     targetRevision: 2026_sd3
     labels:
       description:
[main 2cf974f] Stopping sr22c-va-ioc-01
 1 file changed, 1 insertion(+), 1 deletion(-)
To https://gitlab.diamond.ac.uk/.../va-deployment.git
   572870b..2cf974f  main -> main
```

It pulls first, edits the file, shows the diff, commits and pushes — run from
anywhere, it works out which repo you meant.

## Requirements

C++17 and make to build it; nothing at all to run it. It is one binary with no
interpreter, no packages and no environment to go wrong underneath it — which is
why it is not a python script any more. The gcc that ships with RHEL8 (8.5) is
enough.

If `libstdc++.a` is there the C++ runtime is linked in rather than loaded, so
there is nothing but libc underneath it. The build checks and falls back to the
ordinary shared link if it is not, because needing a package installed to build
would defeat the point; `dnf install libstdc++-static` if you want it.

`restart` and `exec` additionally need the `ioc-restart` and `ioc-exec` scripts
kept beside it, and a working kubectl setup.

## Installing

Clone it, build it, and put the binary on your PATH or alias it:

```bash
make
alias dep=/path/to/deployment-repo-tool/deployment-repo-tool
```

The binary is built at the top of the checkout, beside `ioc-restart` and
`ioc-exec`, which is where it looks for them — through a symlink if you put one
on your PATH. `make clean` removes it and the `build/` directory; neither is
committed.

Then copy `config.ini.example` to `~/.config/deployment-repo-tool/config.ini` and
set the paths to your own checkouts. Nothing in the checkout is read as config —
the example file is named so the tool ignores it, so a fresh clone cannot touch
anyone else's repos — and your settings under `~/.config` survive a `git pull`.

## Usage

```
deployment-repo-tool ACTION SERVICE [REVISION] [options]
```

| Action | What it writes |
|---|---|
| `start` | `enabled: true` |
| `stop` | `enabled: false` |
| `deploy REVISION` | `enabled: true` and `targetRevision: REVISION`, adding the entry if the service is not listed yet |
| `restart` | nothing — see below |
| `exec` | nothing — see below |

```bash
dep start sr22c-va-ioc-01
dep stop sr22c-va-ioc-01
dep deploy sr22c-va-ioc-01 2026_sd3
```

### Restarting, and getting a shell

`restart` and `exec` are the odd ones out: they change nothing in the repo and
make no commit. They do what the ArgoCD web UI's pod delete and terminal do.

```bash
dep restart fe22i-mo-ioc-01   # delete the pod; the StatefulSet recreates it
dep exec fe22i-mo-ioc-01      # a bash prompt inside the running pod
```

Because nothing is edited, no repo has to be worked out, so `-r`, `--no-git`
and the current directory are all irrelevant here. `--dry-run` prints the
command instead of running it. The script's exit status becomes the tool's, so
a failure fails the command.

One service at a time — a glob is refused rather than restarting a beamline's
worth of IOCs by accident. Both check the name against the cluster first, so a
typo is an error rather than a silent no-op, and both say so rather than
hanging if the IOC is stopped.

`ioc-restart` and `ioc-exec` ship in this repo beside the built binary
and are used from there, so there is nothing to configure and nothing extra to
install. Symlinking the tool onto your PATH is fine — the siblings are found
through the link. Set `restart_script` or `exec_script` in `[general]` only to
run a copy from somewhere else.

They are normal shell scripts and work on their own:

```bash
./ioc-restart fe22i-mo-ioc-01
./ioc-exec fe22i-mo-ioc-01
./ioc-exec -n some-other-namespace -s sh fe22i-mo-ioc-01
```

Both put kubectl on PATH by sourcing the DLS cluster setup if your shell has not
already done it (`KLOGIN` overrides where that lives). The namespace comes from
`$EC_TARGET`, defaulting to `accelerator`. `ioc-exec` runs `bash`; `-s` or
`$IOC_EXEC_SHELL` picks another, and it asks kubectl for a TTY only when it has
one to give, so piping into it behaves.

Options: `-r/--repo`, `--config PATH`, `--list`, `--no-git`, `--dry-run`.
`--dry-run` touches nothing at all — no pull, no write, no commit.

### Several at once

`SERVICE` can be a glob. Every match goes into one commit and one push.

```bash
dep stop 'fe15*'      # fe15i-cs-ioc-01, fe15i-mo-ioc-01, fe15i-py-ioc-01
dep start 'fe[0-9]*'  # every fe IOC, but not fe-epics-* or fe-synoptic
dep stop 'fe1[59]*'   # ? [abc] [0-9] [!x] all work
```

**Quote the pattern.** zsh expands it against your filenames first and fails
with `no matches found` before the script ever runs.

A glob only ever selects entries already in the file, so it can never create
one. Services already in the state you asked for are left alone, and if that is
all of them nothing is committed.

### Shorthand

`[<repo>.aliases]` in the config gives names to the patterns you type most:

```bash
dep stop cs           # every fe cs IOC
dep start all -r fe   # every fe IOC
```

`--help` lists them, grouped by repo, straight from your config.

## Which repo gets edited

First of these that answers:

1. `-r fe` — a config key, or a path to a checkout
2. **the current directory**, if you are inside a `*-deployment` checkout
3. **the service name** — a shorthand belongs to the repo that defines it,
   otherwise the `[match]` globs in the config
4. `[general] default`, if you set one

It prints which repo it picked and why before changing anything, so you can see
when it guessed differently from what you meant.

```console
$ dep --list
config  /home/you/.config/deployment-repo-tool/config.ini
fe         ok         /path/to/fe-deployment
va         ok         /path/to/va-deployment
id         NOT THERE  /path/to/id-deployment
```

## Config

```ini
[repos]
fe = ~/work/containers/accelerator-repos/fe/fe-deployment
va = ~/work/containers/accelerator-repos/va/va-deployment

[match]
fe = fe*
va = sr*-va-ioc-*, va-*
common = sr*-pfwd, common-*, carepeater

[fe.aliases]
cs = fe[0-9][0-9][ijkb]-cs-ioc-0[1-9]

[general]
# default = fe
# restart_script = /path/to/ioc-restart   # only to override the shipped ones
# exec_script = /path/to/ioc-exec
```

Read from the first of: `--config`, `$DEPLOYMENT_REPO_CONFIG`,
`$XDG_CONFIG_HOME/deployment-repo-tool/config.ini` (`~/.config` if unset), then
`config.ini` beside the script (gitignored; the repo ships only
`config.ini.example`). With no config at all it still works on whatever repo you
are standing in.

## Things that will catch you out

- **Half-written globs cannot be traced to a repo.** `[match]` compares against
  the argument as you typed it. `fe15*` matches `fe*` and lands on fe, but
  `sr2*` has none of `-va-ioc-` in it yet, so it matches nothing. Write
  `'sr2*-va-ioc-*'`, or say `-r va`. `sr*` on its own is no help either — it
  cannot tell `sr22c-va-ioc-01` from `sr22c-pfwd` in common-deployment.
- **The same shorthand in two repos is an error**, not a guess:
  `'all' is a shorthand in fe and va - use -r to say which`.
- **A service written inline** (`name: {enabled: true}`) is reported as missing
  rather than edited. Nothing in these repos is written that way today.

## What it does to your repo

- `git pull --ff-only` before reading, so the edit lands on current content. A
  diverged branch stops the tool rather than building a merge.
- `git add`/`git commit` both name `apps/values.yaml`, so anything else you have
  staged stays out of the commit.
- Commit messages read `Stopping sr22c-va-ioc-01`, or
  `Stopping 27 services (fe[0-9]*)` when the list would be too long.
- Comments, key order and blank lines in `values.yaml` are preserved: the file
  is edited line by line, never re-dumped by a YAML library. A trailing comment
  next to `enabled:` survives the value changing.

Exit status is 0 on success (including "nothing to do"), 1 for anything the tool
refuses, 2 for a bad command line.

## Tests

```bash
make check   # tests/unit.cpp, ~100 checks, instant
make test    # those, then tests/test_tool.py: 257 checks, a few seconds
```

`make check` covers the parts written out by hand because C++ has no standard
library equivalent — globbing, path handling, the ini parser, the diff.
`tests/test_tool.py` drives the built binary as a black box and reloads every
edit with a real YAML parser to prove the document still means what it should.

That file declares its own dependency on PyYAML in a PEP 723 header, so uv
handles it — nothing to install, and no virtualenv is left in the repo. Without
uv: `pip install pyyaml && python3 tests/test_tool.py`.

Everything runs against temporary copies of the fixtures in `tests/`, the git
flow against a local bare remote, and the cluster actions against stub scripts.
Your real deployment repos and the real cluster are never touched.
