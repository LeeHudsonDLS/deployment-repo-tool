#!/usr/bin/env python3
"""Manual sync integration tests: real local Git, stubbed Argo CD only."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "ioc"


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.repo = self.base / "fe-deployment"
        self.remote = self.base / "remote.git"
        self.repo.mkdir()
        self.git("init", "--bare", str(self.remote))
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Test")
        self.git("config", "user.email", "test@example.com")
        (self.repo / "apps").mkdir()
        self.values = self.repo / "apps/values.yaml"
        self.values.write_text("services:\n  fe-one:\n    enabled: true\n  fe-two:\n    enabled: true\n")
        self.git("add", "apps/values.yaml")
        self.git("commit", "-m", "initial")
        self.git("remote", "add", "origin", str(self.remote))
        self.git("push", "-u", "origin", "main")
        self.config = self.base / "config.ini"
        self.config.write_text(f"[repos]\nfe = {self.repo}\n[fe.argocd]\napp = accelerator/fe\n")
        self.log = self.base / "calls"
        stub = self.base / "argocd"
        stub.write_text(f"#!{sys.executable}\n" + '''import json, os, subprocess, sys
args = sys.argv[1:]
if '--server' in args:
    pos = args.index('--server')
    assert args[pos + 1] == 'hylas-argocd.diamond.ac.uk'
    del args[pos:pos + 2]
    args.remove('--grpc-web')
if args[:1] == ['login']:
    assert '--sso' in args and '--grpc-web' in args
    assert '--sso-launch-browser=false' not in args
    open(os.environ['SYNC_LOG'] + '.login', 'w').close()
    sys.exit(1 if os.environ.get('LOGIN_FAIL') else 0)
if args == ['account', 'get-user-info'] or '--output' in args:
    failure = os.environ.get('AUTH_FAIL')
    if failure and (os.environ.get('AUTH_ALWAYS') or not os.path.exists(os.environ['SYNC_LOG'] + '.login')):
        print(failure, file=sys.stderr)
        sys.exit(1)
    if args == ['account', 'get-user-info']:
        anonymous = os.environ.get('ANONYMOUS') and not os.path.exists(os.environ['SYNC_LOG'] + '.login')
        print('Logged In: false' if anonymous else 'Logged In: true')
    elif os.environ.get('PROJECT_DENIED'):
        print('rpc error: code = PermissionDenied desc = permission denied: projects, get, accelerator', file=sys.stderr)
        sys.exit(1)
    sys.exit(0)
if args == ['version', '--client']:
    sys.exit(0)
# Every Argo call must happen after the requested setting reaches the remote.
assert subprocess.check_output(['git', 'rev-parse', 'HEAD']) == subprocess.check_output(
    ['git', 'ls-remote', 'origin', 'refs/heads/main']).split()[0] + b'\\n'
with open(os.environ['SYNC_LOG'], 'a') as out:
    out.write(json.dumps(args) + '\\n')
sys.exit(1 if os.environ.get('SYNC_FAIL') == ' '.join(args[:3]) else 0)
''')
        stub.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.base) + os.pathsep + os.environ['PATH'],
                        SYNC_LOG=str(self.log))

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.repo, stderr=subprocess.DEVNULL)

    def run_tool(self, *args):
        return subprocess.run([str(TOOL), "--config", str(self.config), "-r", "fe", *args],
                              cwd=self.repo, env=self.env, text=True, capture_output=True)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_order_glob_and_retry(self):
        result = self.run_tool("stop", "fe-*", "--force-sync")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls(), [
            ["app", "get", "accelerator/fe", "--hard-refresh"],
            ["app", "sync", "accelerator/fe", "--timeout", "300", "--resource",
             "argoproj.io:Application:accelerator/fe-one", "--resource",
             "argoproj.io:Application:accelerator/fe-two"],
            ["app", "get", "accelerator/fe-one", "--hard-refresh"],
            ["app", "sync", "accelerator/fe-one", "--prune", "--timeout", "300"],
            ["app", "get", "accelerator/fe-two", "--hard-refresh"],
            ["app", "sync", "accelerator/fe-two", "--prune", "--timeout", "300"],
        ])
        head = self.git("rev-parse", "HEAD")
        result = self.run_tool("stop", "fe-*", "--force-sync")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD"), head)
        self.assertEqual(len(self.calls()), 12)

    def test_preview_and_invalid_combinations(self):
        before = self.values.read_text()
        result = self.run_tool("stop", "fe-one", "--force-sync", "--dry-run",
                               "--argocd-app", "fe")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("would run: argocd app sync fe ", result.stdout)
        for args in [("--no-git", "stop", "fe-one"), ("restart", "fe-one"),
                     ("exec", "fe-one"), ("--list",)]:
            self.assertEqual(self.run_tool(*args, "--force-sync").returncode, 2)
        self.config.write_text(f"[repos]\nfe = {self.repo}\n")
        self.assertEqual(self.run_tool("stop", "fe-one", "--force-sync", "--dry-run").returncode, 0)
        self.assertEqual(self.values.read_text(), before)
        self.assertEqual(self.calls(), [])

    def test_infers_from_resolved_directory_not_config_key(self):
        self.config.write_text(f"[repos]\ncustom = {self.repo}\n")
        result = self.run_tool('stop', 'fe-one', '--force-sync', '--dry-run', '-r', 'custom')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('argocd app sync accelerator/fe ', result.stdout)
        self.assertNotIn('accelerator/custom', result.stdout)
        result = self.run_tool('stop', 'fe-one', '--force-sync', '--dry-run', '-r', str(self.repo))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('argocd app sync accelerator/fe ', result.stdout)

    def test_config_and_cli_override_inference(self):
        self.config.write_text(f"[repos]\nfe = {self.repo}\n[fe.argocd]\napp = accelerator/configured\n")
        result = self.run_tool('stop', 'fe-one', '--force-sync', '--dry-run')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('argocd app sync accelerator/configured ', result.stdout)
        result = self.run_tool('stop', 'fe-one', '--force-sync', '--dry-run',
                               '--argocd-app', 'accelerator/explicit')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('argocd app sync accelerator/explicit ', result.stdout)

    def test_parent_failure_stops_children_and_can_retry(self):
        self.env['SYNC_FAIL'] = 'app sync accelerator/fe'
        result = self.run_tool("stop", "fe-one", "--force-sync")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Git changes remain pushed", result.stderr)
        self.assertEqual(len(self.calls()), 2)
        del self.env['SYNC_FAIL']
        self.assertEqual(self.run_tool("stop", "fe-one", "--force-sync").returncode, 0)
        self.assertEqual(len(self.calls()), 6)

    def test_child_failure_stops_later_children(self):
        self.env['SYNC_FAIL'] = 'app sync accelerator/fe-one'
        self.assertNotEqual(self.run_tool("stop", "fe-*", "--force-sync").returncode, 0)
        self.assertEqual(len(self.calls()), 4)

    def test_failed_push_never_syncs(self):
        hook = self.remote / "hooks/pre-receive"
        hook.write_text("#!/bin/sh\nexit 1\n")
        hook.chmod(0o755)
        self.assertNotEqual(self.run_tool("stop", "fe-one", "--force-sync").returncode, 0)
        self.assertEqual(self.calls(), [])
        hook.unlink()
        self.assertEqual(self.run_tool("stop", "fe-one", "--force-sync").returncode, 0)
        self.assertEqual(len(self.calls()), 4)

    def test_unusable_cli_fails_before_git(self):
        (self.base / 'argocd').write_text('#!/bin/sh\nexit 127\n')
        # A broken remote would make pull fail if the preflight ran too late.
        self.git('remote', 'set-url', 'origin', str(self.base / 'missing.git'))
        before = self.values.read_text()
        head = self.git('rev-parse', 'HEAD')
        result = self.run_tool('stop', 'fe-one', '--force-sync')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('requires a working argocd CLI on PATH', result.stderr)
        self.assertIn('No Git operations have been performed', result.stderr)
        self.assertEqual(self.values.read_text(), before)
        self.assertEqual(self.git('rev-parse', 'HEAD'), head)
        self.assertEqual(self.calls(), [])
        result = self.run_tool('stop', 'fe-one', '--force-sync', '--dry-run')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_dirty_values_rejected_and_default_never_syncs(self):
        self.values.write_text(self.values.read_text() + "# local edit\n")
        self.assertNotEqual(self.run_tool("stop", "fe-one", "--force-sync").returncode, 0)
        self.assertEqual(self.calls(), [])
        self.assertEqual(self.run_tool("stop", "fe-one").returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_logged_out_session_starts_login(self):
        self.env['ANONYMOUS'] = '1'
        result = self.run_tool('stop', 'fe-one', '--force-sync')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(Path(str(self.log) + '.login').exists())
        self.assertEqual(len(self.calls()), 4)

    def test_project_denied_after_login_stops_before_git(self):
        self.env['ANONYMOUS'] = '1'
        self.env['PROJECT_DENIED'] = '1'
        before = self.values.read_text()
        result = self.run_tool('stop', 'fe-one', '--force-sync')
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(Path(str(self.log) + '.login').exists())
        self.assertIn('permission denied: projects, get, accelerator', result.stderr)
        self.assertEqual(self.values.read_text(), before)
        self.assertEqual(self.calls(), [])

    def test_expired_login_uses_sso_once(self):
        self.env['AUTH_FAIL'] = 'oauth2: Refresh token is invalid'
        self.env['SSH_CONNECTION'] = 'workstation 123 host 22'
        result = self.run_tool('stop', 'fe-one', '--force-sync')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(Path(str(self.log) + '.login').exists())
        self.assertNotIn('forward port 8085', result.stderr)
        self.assertEqual(len(self.calls()), 4)

    def test_failed_or_ineffective_login_stops_before_git(self):
        before = self.values.read_text()
        head = self.git('rev-parse', 'HEAD')
        for key in ('LOGIN_FAIL', 'AUTH_ALWAYS'):
            with self.subTest(key=key):
                self.env['AUTH_FAIL'] = 'rpc error: code = Unauthenticated'
                self.env[key] = '1'
                Path(str(self.log) + '.login').unlink(missing_ok=True)
                result = self.run_tool('stop', 'fe-one', '--force-sync')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('No Git operations', result.stderr)
                self.assertEqual(self.values.read_text(), before)
                self.assertEqual(self.git('rev-parse', 'HEAD'), head)
                self.assertEqual(self.calls(), [])
                del self.env[key]

    def test_non_auth_failure_does_not_login(self):
        self.env['PROJECT_DENIED'] = '1'
        result = self.run_tool('stop', 'fe-one', '--force-sync')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(Path(str(self.log) + '.login').exists())
        self.assertEqual(self.calls(), [])

    def test_missing_cli_loads_only_argocd_module(self):
        # Exercise the real helper with a simulated Environment Modules function.
        self.env['PATH'] = '/usr/bin:/bin'
        self.env['TEST_CLI_DIR'] = str(self.base)
        self.env['BASH_FUNC_module%%'] = '''() { [[ "$1" == load && "$2" == argocd/v2.14.10 ]] || return 1;
export PATH="$TEST_CLI_DIR:$PATH";
}'''
        result = self.run_tool('stop', 'fe-one', '--force-sync')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Loading argocd/v2.14.10', result.stderr)
        self.assertEqual(len(self.calls()), 4)

    def test_existing_cli_skips_module_loading(self):
        self.env['BASH_FUNC_module%%'] = '() { echo unexpected-module >&2; return 1; }'
        result = self.run_tool('stop', 'fe-one', '--force-sync')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('unexpected-module', result.stderr)

    def test_module_failure_stops_before_git(self):
        self.env['PATH'] = '/usr/bin:/bin'
        self.env['BASH_FUNC_module%%'] = '() { return 1; }'
        before = self.values.read_text()
        result = self.run_tool('stop', 'fe-one', '--force-sync')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('No Git operations', result.stderr)
        self.assertEqual(self.values.read_text(), before)


if __name__ == '__main__':
    unittest.main()
