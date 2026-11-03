# IOC — Deployment Repo Tool

Manage IOCs and other services across Diamond ArgoCD deployment repositories
with a single CLI.

---

## TL;DR

```bash
ioc start fe03i-mo-ioc-01          # Enable service
ioc stop   fe03i-mo-ioc-01         # Disable it
ioc deploy fe03i-mo-ioc-01 2026_sd3 # Enable and set a target revision
ioc restart fe03i-mo-ioc-01        # Delete pod → StatefulSet recreates
ioc exec   fe03i-mo-ioc-01         # Shell into the pod
ioc stop 'fe15*' --force-sync      # Batch stop with immediate sync
```

---

## What it does

`ioc` edits the `services:` block in `apps/values.yaml` of a Diamond ArgoCD
`*-deployment` repo on GitLab, then commits and pushes the change. No checkout
is needed: the tool fetches into its own bare clone of each repo under
`~/.cache/deployment-repo-tool/` and commits from there. The edits
preserve comments, key order, and spacing — the file is edited as text, never
parsed and re-dumped by a YAML library.

`restart` and `exec` skip Git entirely; they act directly on the running pod via
kubectl.

### Actions

| Action | Requires service to exist? | Needs revision? | Batch (glob) allowed? |
|---|---|---|---|
| `start` | Yes | No | Yes |
| `stop` | Yes | No | Yes |
| `deploy` | No (can create) | **Yes** | Yes |
| `restart` | Yes | N/A | No |
| `exec` | Yes | N/A | No |

### Options

| Flag | Purpose |
|---|---|
| `-r REPO`, `--repo REPO` | Select a configured repo by key |
| `--config PATH` | Use this config file instead of the built-in one |
| `--list` | Show configured repos and whether the tool has cloned them yet |
| `--dry-run` | Fetch and preview edits; no commit, push or cluster contact |
| `--force-sync` | Push then manually sync via ArgoCD |
| `--argocd-app APP` | Override the inferred ArgoCD app name (requires `--force-sync`) |

---

## Installation

```bash
./install.sh            # Clean build, then install to ~/bin/ioc
./install.sh DIR        # Or to DIR/ioc
```

The script removes any previous install, runs `make clean` and `make`, and
warns if the directory is not on your PATH. Run it again after pulling or
editing `builtin.ini`.

The binary embeds the four helper scripts (`ioc-restart`, `ioc-exec`,
`ioc-argocd`, `ioc-status`) and `builtin.ini`, so only `ioc` needs to be
installed. A copy, symlink or alias works anywhere: nothing is looked for
beside it.

**Runtime requirements:** git, with an SSH key on your GitLab account that can
push to the deployment repos. Commits use your own git `user.name` and
`user.email`.

**Build requirements:** C++17 compiler and `make`. GCC 8.5 on RHEL8 is sufficient. No third-party dependencies.

---

## Configuration

The configuration is `builtin.ini` in this repo, compiled into the binary by
`make`. No config file is read from disk at run time: to change it, edit
`builtin.ini` and run `./install.sh` again. `--config PATH` uses a file instead
(it must exist; there is no fallback), for trying a change and for the tests.
`ioc --list` shows `config  built in` and each repo.

### Format

```ini
[repos]
fe = git@gitlab.diamond.ac.uk:controls/containers/accelerator/fe-deployment.git
va = git@gitlab.diamond.ac.uk:controls/containers/accelerator/va-deployment.git

[match]
fe = fe*
va = sr*-va-ioc-*, va-*

[fe.aliases]
all = fe[0-9]*
cs  = fe[0-9][0-9][ijkb]-cs-ioc-0[1-9]
```

### Configuration sections

- **`[repos]`** — Maps repo keys to clone URLs.
- **`[match]`** — Maps repo keys to service-name patterns for automatic repo
  resolution. Patterns are compared against the service argument *as typed*.
- **`[<repo>.aliases]`** — Defines reusable glob aliases per repo. Duplicate
  aliases across repos are an error.
- **`[<repo>.argocd]`** — Optional: `app = accelerator/fe` for the parent app
  inference when using `--force-sync`.
- **`[general]`** — Optional: `default` (commented out by default),
  `restart_script`, `exec_script` to override the embedded helpers.

### Repo resolution order

When a repo isn't given with `-r KEY`, the tool selects one by:

1. The service argument as an alias, then the `[match]` patterns.
2. `[general] default` (if uncommented).

The directory you run it from makes no difference.

The tool prints the selected repo and the reason before acting.

**Warning about `[match]`:** patterns compare the service argument exactly as
typed. A partial glob like `sr2*` will not match `sr*-va-ioc-*`. Use the full
pattern or the `-r` flag.

---

## Git behavior

- Keeps a bare clone (no working tree) of each repo at
  `~/.cache/deployment-repo-tool/<repo>.git` (`$XDG_CACHE_HOME` if set). The
  first command clones it; every command after that fetches. Deleting it is
  always safe.
- Fetches before reading, so the edit is made against what is on GitLab now.
- Commits **only** `apps/values.yaml`, on top of exactly the commit it read,
  and pushes to the default branch. The rest of the repo is carried over.
- If someone pushed in between, the push is refused and nothing changes; run
  the command again. It never merges, rebases or force-pushes.
- Preserves comments, key order, and blank lines in the file.
- Uses descriptive messages like `Stopping fe03i-mo-ioc-01`.
- **Makes no empty commit** when the requested settings already match what's in
  Git.

---

## Force sync (ArgoCD)

Without `--force-sync`, ArgoCD picks up changes on its own schedule (sync
windows). With `--force-sync`, the tool:

1. Pushes the commit
2. Refreshes the parent app from Git
3. Syncs the child application definitions (one per service) with pruning
4. Each sync waits for completion with a 300-second timeout

### Prerequisites for `--force-sync`

- The ArgoCD CLI (`argocd`) must be on PATH, or the Diamond module
  `argocd/v2.14.10` must be available.
- The tool checks login status. If missing or expired, it starts an SSO flow
  that opens a browser on the host (works with X11 forwarding over SSH).
- Your account needs read and sync permissions on the apps.
- The project's sync windows must allow manual syncs.
- The child apps must match service names and share the parent's namespace.

### Parent app inference

The tool infers the parent app in this order:

1. `--argocd-app APP`
2. `[<repo>.argocd] app` in the configuration
3. `accelerator/<directory-name-without--deployment>` (e.g. `fe-deployment` →
   `accelerator/fe`)

### Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `IOC_ARGOCD_SERVER` | `hylas-argocd.diamond.ac.uk` | ArgoCD server for login/sync |
| `IOC_ARGOCD_MODULE` | `argocd/v2.14.10` | Module to load if `argocd` is missing |
| `EC_TARGET` | `accelerator` | Kubernetes namespace |
| `IOC_EXEC_SHELL` | `bash` | Shell for `exec` |
| `IOC_RESTART_TIMEOUT` | `120s` | Rollout timeout for `restart` |
| `KLOGIN` | *(system path)* | Override the cluster setup script |

---

## Restart and exec

These actions act **directly on the cluster**, not on Git. They:

- Accept a **single service only** — globs are rejected (batch pod deletion
  was explicitly not asked for).
- Use kubectl from PATH or source Diamond's cluster setup at
  `/dls_sw/kubernetes/klogin/1.0/acastus`.
- Report an error if the service is missing or stopped.
- `exec` requests a TTY only when stdin is a terminal.
- `restart` waits for the StatefulSet rollout to complete.

Helper exit statuses are passed through as the tool's own exit status, so a
failure cannot silently report success. The scripts replace the tool entirely
(`execvp`), so there is no wrapper process to swallow a Ctrl-C.

---

## YAML editing rules

- Services must use **block format**:

  ```yaml
  fe03i-mo-ioc-01:
    enabled: false
    targetRevision: "2026_sd3"
  ```

- **Inline mappings are rejected** (`name: {enabled: true}`) — they must be
  expanded first.
- Blank lines and comments *within* a service block do **not** terminate the
  service.
- Revisions are quoted when necessary to preserve their YAML string type;
  control characters are rejected.

---

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success (or nothing to do) |
| 1 | Tool failure |
| 2 | Bad arguments / usage error |

`restart` and `exec` return the helper script's exit status.

---

## Design decisions

- **No checkouts.** The tool edits through its own cached bare clone, so
  there are no per-user paths, no stale or dirty working trees and no diverged
  branches. The config is the same for everyone and is compiled in.
- **Nothing-to-do is exit 0.** The tool is safe to re-run and avoids empty
  commits.
- **Only `deploy` creates entries.** `start`/`stop` on an unknown service is
  almost certainly a typo.
- **`start` writes `enabled: true` explicitly** rather than deleting the key,
  even though a missing key means running. It matches how the files are
  already kept.
- **`[general] default` is commented out in `builtin.ini`.** A name that
  matches nothing is usually a typo.
- **All cluster knowledge stays in the shell scripts.** The C++ tool locates a
  script and passes one name. The scripts remain runnable on their own.

---

## Development

```bash
make check   # C++ unit tests — instant
make test    # Unit + Python integration tests
make clean   # Remove binary and build artefacts
```

The Python integration tests use a PEP 723 header so `uv` fetches PyYAML into a
throwaway environment — nothing to install globally. Without uv:

```bash
uvx python3 tests/test_tool.py
```

Tests use temporary bare repos in place of GitLab, a temporary cache, and
stubbed kubectl/ArgoCD commands.
**They never touch real deployment repos or clusters.**

Helper scripts live in the repo as source files (`ioc-restart`, `ioc-exec`,
`ioc-argocd`, `ioc-status`). Edit them and run `make` to regenerate the embedded
versions. The source scripts also remain runnable for development.

### Architecture

One concern per module, with reasoning in the header comments:

| Module | Responsibility |
|---|---|
| `src/main.cpp` | Program flow |
| `src/cli.*` | CLI parsing and action table |
| `src/config.*` | INI parsing, and loading the built-in config |
| `src/repo.*` | Deployment repo resolution |
| `src/remote.*` | The cached bare clone: fetch, commit, push |
| `src/values.*` | Editing the services block |
| `src/glob.*` | Shell-style name matching |
| `src/diff.*` | Unified diff generation |
| `src/paths.*` | Path handling (POSIX, lexical) |
| `src/process.*` | Running git and helper scripts |
| `src/support.*` | String helpers and `fail()` |

---

*Built for RHEL8/GCC 8.5. Standard library only. Never loads a YAML parser.
Keeps the diff clean.*
