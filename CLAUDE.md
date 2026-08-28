# Working on deployment-repo-tool

`deployment-repo-tool` edits the `services:` block of `apps/values.yaml` in a
Diamond ArgoCD `*-deployment` repo, then pulls, commits and pushes. `config.ini`
says where the repos are and what the shorthand names mean. See README.md for
what it does; this file is about changing it.

It is C++ in `src/`, built by `make` into a binary at the top of the checkout.
It was a single python script until the author asked for it in C++: python
environments on the DLS machines change underneath it, and a compiled binary
cannot be broken by one. Everything below that is
not about the language is unchanged from that script, on purpose — the
behaviour was ported line for line, and `tests/test_tool.py` passed unmodified
except for running the binary instead of the interpreter.

## Hard constraints

- **Standard library only.** No third-party libraries, no package manager, no
  vendored code. `make` and a compiler is the whole toolchain, and that is the
  point: nothing to install, nothing to keep up to date, nothing to go missing.
  That includes build-time extras: `-static-libstdc++` is worth having and is
  used when it works, but the Makefile probes for it rather than requiring it,
  because RHEL8 keeps `libstdc++.a` in a `libstdc++-static` package that is
  usually not installed. A build that needs a package installed first is the
  thing this tool is not supposed to be. Do not turn the probe into a
  requirement.
- **It must build with the gcc that ships with RHEL8** — 8.5, C++17. Verified
  there and with gcc 13. Notably that means no `<filesystem>`: it is a
  separately linked library on gcc 8. The path handling in `paths.h` is POSIX
  and lexical instead, which it has to be anyway (see the header).
- **Never load `values.yaml` with a YAML library, even if one becomes
  available.** Re-dumping the document destroys the comments, key order and
  spacing that make these files readable and reviewable, and the diff on every
  commit becomes noise. It is edited as lines of text on purpose.
- **The helper scripts stay shell scripts.** `ioc-restart` and `ioc-exec` are
  not modules and nothing links them; the cluster actions shell out. A new one
  is warranted only by a new cluster action, and the author decides that.

## How it is laid out

One concern per module, each with the reasoning in its header:

    src/main.cpp     the flow: what happens in what order
    src/cli.*        the command line, and the table of actions
    src/config.*     the ini file
    src/repo.*       which deployment repo a command means
    src/values.*     editing the services block
    src/glob.*       shell-style name matching  (python: fnmatch)
    src/diff.*       the unified diff           (python: difflib)
    src/paths.*      path handling              (python: os.path)
    src/process.*    running git and the helper scripts
    src/support.*    string helpers, and fail()

The python was one file because the author objected to it being spread out; the
split here is the same instinct applied to a language with headers. The
alternative was a 1300-line `main.cpp`, which is not smaller, only harder to
find anything in. Do not merge them back together, and do not add a module for
something that belongs in one of these.

The last four are the ones python got for free. They are the risky part of the
conversion, and are the reason `tests/unit.cpp` exists.

## Keep it small

The python this came from started at 897 lines, was cut to 145 after the author
objected to the size, and grew back only where a feature was asked for. That
still applies. Before adding anything, ask whether it was requested. Prefer the
smallest change that works, and do not restore features that were deliberately
cut (status/list of services, colour output, `git ls-remote` revision checking,
close-match suggestions for typos, atomic writes, push-race rebase retry,
per-edit verification passes).

Comments should explain *why*, not restate the code — the author asked for more
of them, so do not strip them out either. The bulk of the reasoning lives in the
headers, next to the declaration it is about.

## Decisions that were made deliberately

Do not silently reverse these:

- **Current directory beats inference.** If you are inside a `*-deployment`
  checkout, that is the repo, even when `[match]` would point elsewhere.
  Inference-first was tried and rejected: standing in your own clone and having
  the tool edit and push from a different one is a foot-gun.
- **But only a checkout with an `apps/values.yaml` in it counts.** Having the
  file is what makes a directory a deployment repo; the name test alone is not
  enough, because this tool's own source tree is called `deployment-repo-tool`
  and passes it. Standing in the checkout you just cloned and built — the
  documented way to install it — used to claim the command and then refuse it
  for having nothing to edit. The trade is that a genuine `*-deployment`
  checkout missing its `apps/values.yaml` now falls through to inference
  instead of erroring; it is not a usable checkout either way, and `check_repo`
  still catches one named explicitly with `-r`.
- **Shorthand lives in the config, not the code.** It used to be a table in the
  source. `[<repo>.aliases]` keeps site conventions out of the tool, and the
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
  The `ACTIONS` table in `cli.h` is what makes an action work this way: a script
  name instead of null, and a script beside the tool, is all a new one needs.
- **They refuse a glob.** Batch restarting was explicitly not asked for, and a
  whole beamline's IOCs going down at once is not a good accident.
- **They refuse a third positional.** It is `deploy`'s revision and means
  nothing to them; ignoring it would swallow a mistyped command.
- **The script's exit status becomes the tool's**, so a failure cannot report
  success. It is `execvp`, not fork-and-wait: the script replaces the tool
  entirely, which makes the status its own without anything having to pass it
  on and leaves no wrapper process between the terminal and the remote shell to
  swallow a Ctrl-C. (The python forked and exited with the child's status; this
  is the same guarantee with one fewer moving part.)
- **All cluster knowledge stays in the shell scripts.** Finding kubectl,
  sourcing the klogin setup, checking the service exists, waiting for the
  rollout: none of it belongs in the C++, which locates a script and passes one
  name. They stay runnable on their own — do not make them depend on being
  called by the tool.
- **The kubectl/klogin preamble is duplicated between the two scripts, on
  purpose.** Factoring it into a third, sourced-only file was offered and
  turned down: each script stays independently runnable and obvious. If a bug
  is found in that block, fix it in both.
- **The shipped scripts are found through `/proc/self/exe`, not `argv[0]`.**
  Installing the tool by symlinking it onto PATH is documented, and that gives
  the real file rather than the link, so the sibling is looked for next to the
  binary. `tests/test_tool.py` covers this; the check is worthless unless the
  symlink lives in a directory with no helper scripts in it, which is how it was
  first written and why it initially passed under the broken version too.
- **Errors exit 1, a bad command line exits 2.** `fail()` for the first,
  `usage_error()` for the second. That split came from argparse and is worth
  keeping: a script can tell "you typed it wrong" from "it would not do it".

## Known limitations

- An entry written inline (`name: {enabled: true}`) does not match the pattern
  `find_service()` looks for, so it is reported as missing rather than mangled.
  Deliberate: safe direction, though the message is a little misleading.
- `[match]` compares against the SERVICE argument as typed, so a half-written
  glob like `sr2*` identifies no repo. Documented in README.md and config.ini.
- `find_service()` matches a name at any indent, so typing the exact name of a
  key nested under an entry (`stop labels`) reaches that key. A glob cannot —
  `select()` only counts lines at the depth of the first entry. Inherited from
  the python and pinned by a check in `tests/unit.cpp` rather than fixed,
  because changing it would change what the tool does.
- The unified diff is Myers rather than difflib's SequenceMatcher. Same output
  on everything this tool produces, and it gives up past 2000 differing lines
  and reports the region as wholly replaced rather than carrying the memory for
  it. Nothing this tool does gets near that.

## Verifying a change

```bash
make check   # tests/unit.cpp, instant
make test    # builds, then check, then tests/test_tool.py: 257 checks
```

`make check` covers the pieces written out by hand because C++ has no standard
library equivalent: globbing, path handling, the ini parser, the diff. Those are
where a subtle mistake changes which services a command picks *without* causing
an obvious failure, so test them directly rather than only through the tool.

`tests/test_tool.py` drives the built binary as a black box and is the
definition of what must keep working. It carries its own PEP 723 header, so uv
fetches PyYAML into a throwaway environment; there is nothing to install and no
venv is created in the repo. Without uv: `pip install pyyaml && python3
tests/test_tool.py`.

It checks that every edit still parses as YAML and that **only the intended
`(service, key)` pairs differ** from the original — compared with a real parser,
not by eye — plus service order, comment count, idempotency, repo resolution,
and the git flow against a local bare remote.

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
script and delete a real pod. One case in the suite got this wrong for a while:
`case("bad action", ["restart", ...])` was written before `restart` was an
action, and once it was, that case ran the real `ioc-restart` and passed only
because the machine had no kubectl. It now names an action that does not exist.

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
