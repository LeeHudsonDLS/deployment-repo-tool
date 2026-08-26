# Working on deployment-repo-tool

`deployment-repo-tool.py` edits the `services:` block of `apps/values.yaml` in a
Diamond ArgoCD `*-deployment` repo, then pulls, commits and pushes. `config.ini`
says where the repos are and what the shorthand names mean. See README.md for
what it does; this file is about changing it.

## Hard constraints

- **Standard library only.** It runs on the system python on DLS RHEL8
  machines, where nothing can be pip installed. There is no PyYAML. Do not add a
  dependency, do not split it into a package.
- **One python file.** `deployment-repo-tool.py` stays a single script. The
  others that ship are `ioc-restart` and `ioc-exec`, bash scripts the cluster
  actions shell out to; they are not modules and nothing imports them. A new
  one is warranted only by a new cluster action, and the author decides that.
- **Never load `values.yaml` with a YAML library, even if one becomes
  available.** Re-dumping the document destroys the comments, key order and
  spacing that make these files readable and reviewable, and the diff on every
  commit becomes noise. It is edited as lines of text on purpose.
- **Python 3.8 compatible.** Verified with 3.8 and 3.12; keep it that way unless
  told otherwise.

## Keep it small

This started at 897 lines and was cut to 145 after the author objected to the
size; it has since grown back only where a feature was asked for. Before adding
anything, ask whether it was requested. Prefer the smallest change that works,
and do not restore features that were deliberately cut (status/list of services,
colour output, `git ls-remote` revision checking, close-match suggestions for
typos, atomic writes, push-race rebase retry, per-edit verification passes).

Comments should explain *why*, not restate the code — the author asked for more
of them after the slimming pass, so do not strip them out either.

## Decisions that were made deliberately

Do not silently reverse these:

- **Current directory beats inference.** If you are inside a `*-deployment`
  checkout, that is the repo, even when `[match]` would point elsewhere.
  Inference-first was tried and rejected: standing in your own clone and having
  the tool edit and push from a different one is a foot-gun.
- **Shorthand lives in the config, not the code.** It used to be a dict in the
  script. `[<repo>.aliases]` keeps site conventions out of the tool, and the
  `--help` epilog is built from whatever config is loaded so the two cannot
  drift.
- **`[general] default` is commented out in the shipped config.** A name that
  matches nothing is usually a typo or a half-written glob, and an error naming
  the configured repos is more use than quietly acting on one of them.
- **The same shorthand defined in two repos is an error**, not first-wins.
- **Nothing to do is exit 0**, not an error. It keeps the tool safe to re-run and
  avoids empty commits.
- **Only `deploy` may create an entry.** `start`/`stop` on an unknown name is far
  more likely to be a typo than an intention.
- **`start` writes `enabled: true` explicitly** rather than deleting the key,
  even though an absent key means running. It matches how the files are already
  kept and leaves no doubt what state a service was put in.
- **git: `pull --ff-only`, and a pathspec on both `add` and `commit`.** Never
  merge or rebase on the user's behalf; never sweep up their other staged work.
- **`restart` and `exec` short-circuit before repo resolution.** They act on the
  cluster, not the repo: no pull, no edit, no commit, and nothing for `-r`,
  `--no-git` or the current directory to influence. Resist the pull to route
  them through `resolve_repo` for consistency's sake — there is no file to find.
  `HELPERS` is the list of actions that work this way; adding a key to it and a
  script beside the tool is all a new one needs.
- **They refuse a glob.** Batch restarting was explicitly not asked for, and a
  whole beamline's IOCs going down at once is not a good accident.
- **They refuse a third positional.** It is `deploy`'s revision and means
  nothing to them; ignoring it would swallow a mistyped command.
- **The script's exit status becomes the tool's**, so a failure cannot report
  success. `subprocess.call` with the terminal left attached, because `exec`
  needs the tty for the remote shell.
- **All cluster knowledge stays in the shell scripts.** Finding kubectl,
  sourcing the klogin setup, checking the service exists, waiting for the
  rollout: none of it belongs in the python, which locates a script and passes
  one name. They stay runnable on their own — do not make them depend on being
  called by the python.
- **The kubectl/klogin preamble is duplicated between the two scripts, on
  purpose.** Factoring it into a third, sourced-only file was offered and
  turned down: each script stays independently runnable and obvious. If a bug
  is found in that block, fix it in both.
- **The shipped scripts are found via `realpath(__file__)`, not `abspath`.**
  Installing the tool by symlinking it onto PATH is documented, and under
  `abspath` the sibling is looked for next to the symlink instead of next to
  the real file. `tests/test_tool.py` covers this; the check is worthless
  unless the symlink lives in a directory with no helper scripts in it, which
  is how it was first written and why it initially passed under both.

## Known limitations

- An entry written inline (`name: {enabled: true}`) does not match the pattern in
  `find()`, so it is reported as missing rather than mangled. Deliberate: safe
  direction, though the message is a little misleading.
- `[match]` compares against the SERVICE argument as typed, so a half-written
  glob like `sr2*` identifies no repo. Documented in README.md and config.ini.

## Verifying a change

```bash
uv run tests/test_tool.py                # ~223 checks, a few seconds
uv run --python 3.8 tests/test_tool.py   # the oldest version that must work
```

The test file carries its own PEP 723 header, so uv fetches PyYAML into a
throwaway environment; there is nothing to install and no venv is created in the
repo. Without uv: `pip install pyyaml && python3 tests/test_tool.py`. It prints
one line per check and exits non-zero if any fail.

The suite is the definition of what must keep working. It checks that every edit
still parses as YAML and that **only the intended `(service, key)` pairs differ**
from the original — compared with a real parser, not by eye — plus service order,
comment count, idempotency, repo resolution, and the git flow against a local
bare remote.

If you add behaviour, add a check for it. If you change behaviour, expect a
failure and decide deliberately whether the test or the code is wrong.

**Tests must never touch the real deployment repos.** Fixtures are copied into a
temp directory and the tool is pointed at the copy with a generated `--config`;
because a repo can be inferred from the service name, setting only the working
directory is not enough to contain it.

**Nor the real cluster.** The cluster checks replace each helper script with a
stub that records its own action name and its argv — the action name is what
catches a run that reached the wrong script. The sibling-lookup checks run
against a *copy* of the tool in a temp directory with the stubs beside it,
because exercising that path against the real checkout would run the real
script and delete a real pod.

Worth doing after a change to the editing logic: break something on purpose and
confirm the suite goes red. Every property above was mutation-tested that way,
which is how two dead tests were found — one crashed on a list argument and
silently aborted the whole run, and the trailing-blank-line handling had no
coverage at all.

## Config in this repo

The repo ships `config.ini.example`, carrying the author's own paths. It is a
starting point to copy to `~/.config/deployment-repo-tool/config.ini`. The
`.example` suffix is deliberate: the lookup chain only ever names `config.ini`,
so a clone of this repo is inert and cannot be made to push to someone else's
checkouts. `config.ini` is gitignored — never commit one. Do not assume the
example's paths exist on any other machine.

The test suite reads `config.ini.example` for `[match]` and the alias sections,
so renaming or dropping it breaks `tests/test_tool.py`.
