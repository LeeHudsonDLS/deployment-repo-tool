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

C++17 and make build a single `ioc` executable, including its helper scripts.
No Python or additional build packages are required. The gcc shipped with
RHEL8 (8.5, C++17) is sufficient. At runtime, Git is required for repository
operations; Bash and kubectl for `restart`/`exec`; Bash and Argo CD for
`--force-sync` (including the existing Diamond module setup when needed).

If `libstdc++.a` is there the C++ runtime is linked in rather than loaded, so
there is nothing but libc underneath it. The build checks and falls back to the
ordinary shared link if it is not, because needing a package installed to build
would defeat the point; `dnf install libstdc++-static` if you want it.

## Installing

Build and copy just the binary onto your PATH:

```bash
make
mkdir -p ~/bin
install -m 755 ioc ~/bin/ioc
```

Ensure `~/bin` is on PATH. The helper scripts are embedded in `ioc` and run
through Bash without extracting temporary files. No sibling scripts or checkout
are needed at runtime. A symlink or alias to the built `ioc` also works.

The binary still needs a compatible Linux architecture and C runtime; embedding
scripts does not make a build for one platform run on every other platform.
Build on the oldest target system you need to support. `make clean` removes
`ioc` and the `build/` directory; neither is committed.

Editing `ioc-restart`, `ioc-exec` or `ioc-argocd` causes `make` to regenerate the
embedded contents and relink `ioc`. Rebuild and copy the binary again to deploy
helper changes. The source scripts remain independently runnable for development.

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

### Applying changes during a run

With the `argocd` CLI installed and logged into the appropriate server, use:

```bash
ioc stop fe22i-mo-ioc-01 --force-sync --argocd-app accelerator/fe
ioc start 'fe15*' --force-sync --argocd-app accelerator/fe --dry-run
```

`--force-sync` uses the embedded `ioc-argocd` helper. If
`argocd` is missing from PATH, the helper initialises Environment Modules and
loads `argocd/v2.14.10`, the CLI dependency supplied by Diamond's `ec/va`
module. It does not load `ec/va` or change the Kubernetes context. An existing
`argocd` on PATH is used directly. All module changes stay in the helper's
child shell.

Before any Git operations, the helper checks the CLI, checks login status with
`argocd account get-user-info`, and reads the parent app
from `hylas-argocd.diamond.ac.uk` using `--grpc-web`. A missing or expired
login starts `argocd login hylas-argocd.diamond.ac.uk --grpc-web --sso`, then
retries the access check once. Other failures, such as denied access or network
errors, stop without attempting SSO. Failed login also stops before Git.

SSO uses Argo CD's default browser launch on the host running the tool,
including over SSH with X11 forwarding. It inherits your display environment,
just like running `argocd login ... --grpc-web --sso` directly.

Set `IOC_ARGOCD_SERVER` to use a different Argo CD server, or
`IOC_ARGOCD_MODULE` to choose a different module version. The server setting
applies to both login and all sync commands. The helper never executes `EC_LOGIN`
or infers a target from `EC_TARGET`, which may refer to a different area.
Without `--force-sync`, none of this setup runs. `--dry-run` also skips module
loading, authentication checks and login.

Normally `--argocd-app` can be omitted:

```bash
ioc stop fe03i-mo-ioc-01 --force-sync
```

After resolving and validating the deployment checkout through the normal repo
selection rules, the tool infers `accelerator/fe` from a directory named
`fe-deployment`. It uses the resolved directory name, not the config key or
service pattern. The namespace defaults to `accelerator`.

An explicit `--argocd-app` takes precedence over `[<repo>.argocd] app, which
in turn overrides inference. For a differently named checkout, configure the
parent or pass it explicitly:

```ini
[fe.argocd]
app = accelerator/fe
```

After a successful push, the tool refreshes the parent from Git, synchronously
syncs its selected child Application resources, then refreshes and syncs each
selected service app with pruning enabled. Syncs have a 300-second timeout.
The parent sync is selective so other services' pending Application changes
are not applied. Child syncs apply all pending changes in those selected apps,
including resource deletions. Child app names must match service names and
share the parent's Application namespace; `accelerator/fe` specifies that
namespace explicitly. A bare `fe` uses Argo CD's default app namespace.
The parent must track the repository and branch you push to.

This uses the CLI's saved login for the selected server. The flag requests a normal manual sync; it does not use Argo CD's `--force`
apply option or alter project policy. The project's
[sync windows must permit manual syncs](https://argo-cd.readthedocs.io/en/stable/user-guide/sync_windows/),
and your account needs permission to get and sync both parent and child apps.

`--dry-run` previews the commands without contacting Git or Argo CD or writing
files. `--force-sync` is accepted for `start`, `stop` and `deploy`, and cannot
be combined with `--no-git`. Uncommitted changes in `apps/values.yaml` are
rejected. If a sync fails, the command fails and the pushed Git changes remain;
fix the reported problem and repeat the command. It pushes and syncs even when
the file already has the requested setting, without making an empty commit.
A timeout may leave the server-side operation running; inspect it before retrying.

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

`ioc-restart` and `ioc-exec` are embedded at build time. Set `restart_script`
or `exec_script` in `[general]` to run a custom external executable instead;
these overrides retain their own shebang and exit status.

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

Options: `-r/--repo`, `--config PATH`, `--list`, `--no-git`, `--dry-run`,
`--force-sync`, `--argocd-app APP`.
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
`config.ini` beside the binary (gitignored; the repo ships only
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

`tests/test_sync.py` checks manual sync ordering and selection, previews,
invalid options, dirty files, failed pushes, failed syncs and retries against
temporary Git repositories and a stub `argocd` executable.
