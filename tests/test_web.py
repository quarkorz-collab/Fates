"""Standard-library tests for the cross-platform WebUI server."""
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch


SOURCE = Path(__file__).resolve().parents[1] / "frontend" / "fates_web.py"
SPEC = importlib.util.spec_from_file_location("fates_web", SOURCE)
web = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(web)


class ExecutableDiscoveryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="fates web test ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.project = self.root / "source"
        self.cwd = self.root / "working directory"
        self.bundle = self.root / "package directory"
        for directory in (self.project / "frontend", self.cwd, self.bundle):
            directory.mkdir(parents=True)
        self.addCleanup(patch.stopall)
        patch.object(web, "__file__", str(self.project / "frontend" / "fates_web.py")).start()
        patch.object(web.Path, "cwd", return_value=self.cwd).start()
        patch.object(web.shutil, "which", return_value=None).start()
        patch.object(web.sys, "frozen", False, create=True).start()

    def executable(self, path):
        path.touch()
        path.chmod(0o755)
        return path

    def test_native_engine_wins_when_both_platform_builds_exist(self):
        self.executable(self.project / "fates.exe")
        self.executable(self.project / "fates")
        self.assertEqual(web.find_fates(None), self.project / web.fates_names()[0])

    @unittest.skipIf(os.name == "nt", "POSIX does not auto-launch Windows engines")
    def test_windows_only_build_is_not_used_on_posix(self):
        self.executable(self.project / "fates.exe")
        with self.assertRaises(FileNotFoundError):
            web.find_fates(None)

    def test_explicit_path_with_spaces_is_authoritative(self):
        self.executable(self.project / web.fates_names()[0])
        explicit = self.executable(self.bundle / "custom engine")
        self.assertEqual(web.find_fates(str(explicit)), explicit)
        with self.assertRaises(FileNotFoundError):
            web.find_fates(str(self.bundle / "missing"))

    def test_frozen_build_finds_engine_next_to_web_executable(self):
        sibling = self.executable(self.bundle / web.fates_names()[0])
        self.executable(self.cwd / web.fates_names()[0])
        with patch.object(web.sys, "frozen", True), patch.object(
            web.sys, "executable", str(self.bundle / "fates-web")
        ):
            self.assertEqual(web.find_fates(None), sibling)

    def test_current_directory_and_path_fallback(self):
        engine = self.executable(self.cwd / web.fates_names()[0])
        self.assertEqual(web.find_fates(None), engine)
        engine.unlink()
        engine = self.executable(self.bundle / web.fates_names()[0])
        with patch.object(web.shutil, "which", return_value=str(engine)):
            self.assertEqual(web.find_fates(None), engine)

    @unittest.skipIf(os.name == "nt", "POSIX executable permissions")
    def test_permission_error_is_actionable(self):
        engine = self.project / "fates"
        engine.touch()
        engine.chmod(0o644)
        with self.assertRaisesRegex(PermissionError, "chmod"):
            web.find_fates(str(engine))
        with self.assertRaisesRegex(PermissionError, "chmod"):
            web.find_fates(None)
        fallback = self.executable(self.cwd / "fates")
        self.assertEqual(web.find_fates(None), fallback)

    def test_static_assets_use_bundle_root_when_frozen(self):
        with patch.object(web.sys, "frozen", True), patch.object(
            web.sys, "_MEIPASS", str(self.bundle), create=True
        ):
            self.assertEqual(web.static_root(), self.bundle / "frontend_static")
        self.assertEqual(web.static_root(), self.project / "frontend" / "static")


class RuntimeEnvironmentTests(unittest.TestCase):
    def test_loopback_listener_does_not_reverse_resolve_its_address(self):
        with patch.object(web.socket, "getfqdn", side_effect=AssertionError("DNS lookup")):
            server = web.create_http_server("127.0.0.1", 0, web.BaseHTTPRequestHandler)
        try:
            self.assertEqual(server.server_name, "127.0.0.1")
            self.assertEqual(server.server_port, server.server_address[1])
        finally:
            server.server_close()

    def test_linux_frozen_restores_original_library_path(self):
        for original in (None, "", "/opt/user-libraries"):
            environment = {"LD_LIBRARY_PATH": "/tmp/_MEI/private", "UNCHANGED": "yes"}
            if original is not None:
                environment["LD_LIBRARY_PATH_ORIG"] = original
            with self.subTest(original=original), patch.dict(os.environ, environment, clear=True), \
                    patch.object(web.sys, "platform", "linux"), \
                    patch.object(web.sys, "frozen", True, create=True):
                web.configure_external_environment()
                self.assertEqual(os.environ.get("LD_LIBRARY_PATH"), original)
                self.assertEqual(os.environ["UNCHANGED"], "yes")

    def test_source_environment_is_not_modified(self):
        environment = {"LD_LIBRARY_PATH": "/opt/user-libraries"}
        with patch.dict(os.environ, environment, clear=True), \
                patch.object(web.sys, "frozen", False, create=True):
            web.configure_external_environment()
            self.assertEqual(dict(os.environ), environment)

    def test_headless_browser_failure_keeps_manual_url(self):
        for outcome in (False, web.webbrowser.Error("no browser")):
            with self.subTest(outcome=outcome), patch.object(web.webbrowser, "open") as opener:
                if isinstance(outcome, Exception):
                    opener.side_effect = outcome
                else:
                    opener.return_value = outcome
                stderr = io.StringIO()
                with redirect_stderr(stderr):
                    web.open_browser("http://127.0.0.1:9000/")
                self.assertIn("http://127.0.0.1:9000/", stderr.getvalue())

    def test_sigterm_runs_server_cleanup(self):
        arguments = SimpleNamespace(fates=None, host="127.0.0.1", port=0, verbose=False, no_browser=True)
        with patch.object(web, "configure_utf8_console"), \
                patch.object(web, "configure_external_environment"), \
                patch.object(web, "parse_arguments", return_value=arguments), \
                patch.object(web, "find_fates", return_value=Path(sys.executable)), \
                patch.object(web, "inspect_fates", return_value="Fates test"), \
                patch.object(web, "inspect_symbol_catalog", return_value={"constants": []}), \
                patch.object(web, "JobManager") as manager_type, \
                patch.object(web, "create_http_server") as server_factory, \
                redirect_stdout(io.StringIO()):
            server = server_factory.return_value
            server.server_address = ("127.0.0.1", 9000)
            server.serve_forever.side_effect = lambda **_: signal.raise_signal(signal.SIGTERM)
            old_handler = signal.getsignal(signal.SIGTERM)
            self.assertEqual(web.main(), 0)
            manager_type.return_value.shutdown.assert_called_once()
            server.server_close.assert_called_once()
            self.assertEqual(signal.getsignal(signal.SIGTERM), old_handler)


class JobTests(unittest.TestCase):
    def setUp(self):
        self.manager = web.JobManager(Path(sys.executable).resolve())
        self.addCleanup(self.manager.shutdown)

    def finish(self, job, timeout=8):
        job.worker.join(timeout)
        self.assertFalse(job.worker.is_alive(), job.snapshot(0, 0))
        return job.snapshot(0, 0)

    def ready(self, job):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if "ready" in job.snapshot(0, 0)["stdout"]:
                return
            time.sleep(0.01)
        self.fail(f"Child did not become ready: {job.snapshot(0, 0)}")

    def test_arguments_are_passed_without_a_shell(self):
        arguments = ["", "--digits=", "(0,inf)", "pi,e,phi", "a b", "it's", "$HOME", "$(command)", "π"]
        job = self.manager.create([
            # The real engine always emits UTF-8; Python's redirected stdout on
            # Windows otherwise uses the local code page (e.g. GBK).
            "-X", "utf8", "-c", "import json,sys; print(json.dumps(sys.argv[1:], ensure_ascii=False))", *arguments
        ])
        result = self.finish(job)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(json.loads(result["stdout"]), arguments)
        self.assertEqual(job.snapshot(result["stdout_offset"], result["stderr_offset"])["stdout"], "")

    def test_invalid_arguments_are_rejected(self):
        for arguments in ([], "--version", [None], ["a\0b"], ["x"] * (web.MAX_ARGUMENTS + 1)):
            with self.subTest(arguments=str(arguments)[:80]), self.assertRaises(ValueError):
                self.manager.create(arguments)

    def test_cancel_running_job(self):
        job = self.manager.create(["-c", "import time; print('ready', flush=True); time.sleep(30)"])
        self.ready(job)
        self.assertTrue(self.manager.cancel(job.id))
        self.assertTrue(self.manager.cancel(job.id))
        self.assertEqual(self.finish(job)["status"], "cancelled")
        self.assertIsNotNone(job.process.poll())

    def test_cancel_before_launch_does_not_spawn(self):
        job = web.Job([], ["must not launch"])
        job.cancel_requested = True
        with patch.object(web.subprocess, "Popen") as popen:
            self.manager._run(job)
            popen.assert_not_called()
        self.assertEqual(job.status, "cancelled")

    def test_shutdown_reaps_child_and_rejects_new_jobs(self):
        job = self.manager.create(["-c", "import time; print('ready', flush=True); time.sleep(30)"])
        self.ready(job)
        self.manager.shutdown()
        self.assertIsNotNone(job.process.poll())
        self.assertFalse(job.worker.is_alive())
        with self.assertRaises(RuntimeError):
            self.manager.create(["--version"])

    @unittest.skipIf(os.name == "nt", "POSIX SIGTERM escalation")
    def test_cancel_during_spawn_escalates_if_sigterm_is_ignored(self):
        spawned = threading.Event()
        release = threading.Event()
        original_popen = subprocess.Popen

        def delayed_popen(*arguments, **kwargs):
            process = original_popen(*arguments, **kwargs)
            self.assertEqual(process.stdout.readline().strip(), "ready")
            spawned.set()
            release.wait(5)
            return process

        with patch.object(web.subprocess, "Popen", side_effect=delayed_popen):
            job = self.manager.create([
                "-c", "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "print('ready', flush=True); time.sleep(30)"
            ])
            try:
                self.assertTrue(spawned.wait(5))
                self.assertIsNone(job.process)
                self.manager.cancel(job.id)
            finally:
                release.set()
            self.manager.shutdown()
        self.assertEqual(self.finish(job)["status"], "cancelled")
        self.assertEqual(job.process.returncode, -signal.SIGKILL)


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux /proc child discovery")
class SmokeProcessDiscoveryTests(unittest.TestCase):
    def test_child_launched_by_worker_thread_is_found(self):
        from check_web import linux_engine_children

        ready, release = threading.Event(), threading.Event()
        children = []

        def worker():
            process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
            children.append(process)
            ready.set()
            try:
                release.wait(5)
            finally:
                process.terminate()
                process.wait(timeout=3)

        thread = threading.Thread(target=worker)
        thread.start()
        try:
            self.assertTrue(ready.wait(5))
            self.assertIn(children[0].pid, linux_engine_children(os.getpid(), Path(sys.executable).resolve()))
        finally:
            release.set()
            thread.join(5)


if __name__ == "__main__":
    unittest.main()
