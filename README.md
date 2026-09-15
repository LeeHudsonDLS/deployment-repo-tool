# deployment-repo-tool

`ioc` starts, stops and deploys IOCs and other services by updating a Diamond
Argo CD deployment repository. It pulls, edits `apps/values.yaml`, shows the
diff, commits and pushes. Add `--force-sync` to apply the change during a run.

It can also restart a running IOC or open a shell in its pod.

## Install

Build with a C++17 compiler and make (GCC 8.5 on RHEL8 is sufficient):

```bash
make
mkdir -p ~/bin
install -m 755 ioc ~/bin/ioc
```

Ensure `~/bin` is on your PATH. **Copy only `ioc`**: all three helper scripts
are embedded in the binary. A symlink or alias to the built binary also works.

Runtime requirements depend on the action:

| Action | External tools |
|---|---|
| `start`, `stop`, `deploy` | Git |
| `--force-sync` | Bash and Argo CD |
| `restart`, `exec` | Bash and kubectl |

The binary needs a compatible Linux architecture and C runtime. Build on the
oldest system you need to support. The build links the C++ runtime statically
when available, and otherwise uses the shared runtime.

Copy `config.ini.example` to `~/.config/deployment-repo-tool/config.ini` and
edit the checkout paths. The example file itself is never loaded. See
[Configuration](#configuration) for a minimal example.

## Common commands

```bash
ioc start fe03i-mo-ioc-01
ioc stop fe03i-mo-ioc-01
ioc deploy fe03i-mo-ioc-01 2026_sd3
ioc stop fe03i-mo-ioc-01 --force-sync
ioc restart fe03i-mo-ioc-01
ioc exec fe03i-mo-ioc-01
```

```text
ioc ACTION SERVICE [REVISION] [options]
```

| Action | Effect |
|---|---|
| `start` | Set `enabled: true` |
| `stop` | Set `enabled: false` |
| `deploy` | Enable the service and set `targetRevision`; takes a revision and can add a new service |
| `restart` | Delete the running pod and wait for its StatefulSet to recreate it |
| `exec` | Open a shell in the running pod |

`start` and `stop` require an existing service. If the requested settings are
already in Git, no new commit is made. With `--force-sync`, the tool still
pushes and syncs, so the same command can retry a previous failure.

### Options

| Option | Purpose |
|---|---|
| `-r REPO`, `--repo REPO` | Select a configured repo key or a checkout path |
| `--config PATH` | Use a specific configuration file |
| `--list` | Show configured repos and whether their paths exist |
| `--dry-run` | Preview without writing files, running Git, loading modules or contacting the cluster |
| `--no-git` | Edit the values file without pulling, committing or pushing |
| `--force-sync` | Manually sync after pushing; for `start`, `stop` and `deploy` only |
| `--argocd-app APP` | Override the inferred parent app; requires `--force-sync` |
| `-h`, `--help` | Show usage and configured aliases |

`--force-sync` cannot be combined with `--no-git`.

### Multiple services and aliases

For `start`, `stop` and `deploy`, quote a glob to select several existing
services in one commit and push:

```bash
ioc stop 'fe15*'
ioc start 'fe[0-9]*'
ioc stop 'fe1[59]*' --force-sync --dry-run
```

Quoting prevents your shell from expanding the pattern against local files.
Globs cannot add new services.

Aliases define reusable patterns in the configuration:

```bash
ioc stop cs -r fe
ioc start all -r fe
```

## Applying changes during a run

Argo CD normally applies repository changes according to its sync windows.
To request a manual sync after pushing:

```bash
ioc stop fe03i-mo-ioc-01 --force-sync
```

The tool resolves the deployment checkout first, then infers `accelerator/fe`
from the directory name `fe-deployment`. The namespace defaults to
`accelerator`. Parent selection uses this order:

1. `--argocd-app APP`
2. `[<repo>.argocd] app` in the configuration
3. `accelerator/<directory name without -deployment>`

Use an override for a differently named checkout. A bare app name, such as
`fe`, uses Argo CD's default Application namespace.

### Setup and login

Only with `--force-sync`, the tool:

1. Uses `argocd` on PATH, or loads Diamond's `argocd/v2.14.10` module if missing.
2. Checks login status with `argocd account get-user-info`.
3. Starts SSO if the login is missing or expired, then checks the session again.
4. Checks access to the parent app before any Git operations.

The default server is `hylas-argocd.diamond.ac.uk`; all server commands use
`--grpc-web`. SSO launches a browser on the host running the tool, including
over SSH with X11 forwarding, just like:

```bash
argocd login hylas-argocd.diamond.ac.uk --grpc-web --sso
```

Login, network or permission failures stop before Git changes. Module loading
stays in the helper's child shell and does not load `ec/va` or change the
Kubernetes context. `--dry-run` skips setup and login entirely.

| Environment variable | Default |
|---|---|
| `IOC_ARGOCD_SERVER` | `hylas-argocd.diamond.ac.uk` |
| `IOC_ARGOCD_MODULE` | `argocd/v2.14.10` |

The server override applies to both login and sync. Argo CD setup does not use
`EC_LOGIN` or `EC_TARGET`.

### Sync sequence and retries

After pushing, the tool refreshes the parent from Git and syncs only the
selected child Application definitions. It then refreshes and syncs each
selected service app, with pruning enabled. Each sync waits for completion
before the next begins and has a 300-second timeout.

The child apps must match the service names and share the parent's Application
namespace. The parent must track the repository and branch you push to.
Child syncs apply **all pending changes in those selected apps**, including
resource deletions; unrelated child definitions are excluded from the parent sync.

Your account needs permission to read and sync the apps, and the project's
[sync windows must permit manual syncs](https://argo-cd.readthedocs.io/en/stable/user-guide/sync_windows/).
`--force-sync` requests a normal manual sync; it does not change project policy
or use Argo CD's force-apply option.

Uncommitted changes to `apps/values.yaml` are rejected with `--force-sync`.
If a sync fails, the pushed Git changes remain. Fix the reported problem and
repeat the command. A timeout may leave the server-side operation running;
check it before retrying.

## Restarting and opening a shell

`restart` and `exec` act directly on the running pod without editing Git or
resolving a deployment repo. They accept one service, reject globs, and report
an error if the service is missing or stopped. `--dry-run` previews the action.

Both use kubectl from PATH or source the Diamond cluster setup at
`/dls_sw/kubernetes/klogin/1.0/acastus` if needed.

| Environment variable | Purpose |
|---|---|
| `KLOGIN` | Override the cluster setup script |
| `EC_TARGET` | Kubernetes namespace; defaults to `accelerator` |
| `IOC_EXEC_SHELL` | Shell for `exec`; defaults to `bash` |
| `IOC_RESTART_TIMEOUT` | Rollout timeout for `restart`; defaults to `120s` |

`exec` requests a TTY only when its input is a terminal. Helper exit statuses
are passed through. To use custom external executables, set `restart_script`
or `exec_script` under `[general]` in the configuration.

## Configuration

```ini
[repos]
fe = ~/work/fe-deployment
va = ~/work/va-deployment

[match]
fe = fe*
va = sr*-va-ioc-*, va-*

[fe.aliases]
all = fe[0-9]*
cs = fe[0-9][0-9][ijkb]-cs-ioc-0[1-9]

[fe.argocd]
# Optional: inferred from fe-deployment if omitted.
app = accelerator/fe

[general]
# default = fe
# restart_script = /path/to/custom-restart
# exec_script = /path/to/custom-exec
```

The first available configuration file is used, in this order:

1. `--config PATH`
2. `$DEPLOYMENT_REPO_CONFIG`
3. `$XDG_CONFIG_HOME/deployment-repo-tool/config.ini` (`~/.config` if unset)
4. `config.ini` beside the binary

Without a config, the tool can use the deployment checkout containing the
current directory. `ioc --list` shows the loaded configuration and repo paths.

### Repository selection

The first matching rule selects the checkout:

1. `-r`: a configured key or a checkout path
2. The deployment checkout containing the current directory
3. The service's alias, otherwise the `[match]` patterns
4. `[general] default`

The tool prints the selected repo and the reason before acting. Parent-app
inference happens only after that repo is resolved and validated.

`[match]` tests the service argument exactly as typed. A partial glob such as
`sr2*` may not match `sr*-va-ioc-*`; use `'sr2*-va-ioc-*'` or `-r va`.
If an alias is defined in more than one repo, use `-r` to disambiguate it.

## Git behavior

- Pulls with `--ff-only` before editing; a diverged branch stops the command.
- Stages and commits only `apps/values.yaml`, leaving other staged files out.
- Preserves comments, key order and blank lines by editing the file in place.
- Uses messages such as `Stopping fe03i-mo-ioc-01`.
- Makes no empty commit when settings already match.

Services must use the block format shown in the example values files. Inline
entries such as `name: {enabled: true}` are not supported by the editor.

Exit status is 0 on success, 1 for tool failures, and 2 for invalid arguments.
`restart` and `exec` return the helper's exit status.

## Development and tests

Edit `ioc-restart`, `ioc-exec` or `ioc-argocd` and run `make` to regenerate the
embedded scripts and relink the binary. Reinstall `ioc` to distribute helper
changes. The source scripts also remain runnable for development. Embedded
helpers execute through Bash without temporary files.

```bash
make check   # C++ unit tests
make test    # Unit tests and Python integration tests
make clean   # Remove the binary and generated build files
```

`make test` requires uv, which supplies Python and PyYAML for the integration
suite. With Python and PyYAML already installed, you can instead run
`python3 tests/test_tool.py` and `python3 tests/test_sync.py` after building.

Tests use temporary deployment repositories, local Git remotes and stubbed
kubectl/Argo CD commands. They exercise copied binaries without sibling scripts,
configured helper overrides, sync ordering, login recovery and failure handling.
No real deployment repos or clusters are changed.
