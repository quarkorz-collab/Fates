#!/usr/bin/env python3
"""Exercise the source or packaged WebUI against a real engine, without a browser.

Only loopback HTTP is used. All files and processes are created in an isolated
temporary directory; the supplied engine/WebUI binaries are never modified.
"""
from __future__ import annotations

import argparse
import http.client
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]
TERMINAL = {"completed", "failed", "cancelled"}
LIVE_PREFIX = "[fates-live] "
LONG_SEARCH = ["777777", "--max-cost", "24", "--beam", "100000", "--pairs", "50000000",
               "--threads", "1", "--no-stop", "--json"]


class RetryingTemporaryDirectory(tempfile.TemporaryDirectory):
    def cleanup(self):
        # Windows can release a PyInstaller child's executable handle just after
        # taskkill and wait return. Still fail if the file remains locked.
        for attempt in range(10):
            try:
                super().cleanup()
                return
            except PermissionError:
                if attempt == 9:
                    raise
                time.sleep(0.2)


def request(url: str, path: str, body: object = None, headers: dict | None = None):
    parsed = urlparse(url)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=10)
    request_headers = dict(headers or {})
    encoded = None
    if body is not None:
        encoded = json.dumps(body, ensure_ascii=False).encode("utf-8")
        request_headers["Content-Type"] = "application/json"
    try:
        connection.request("POST" if body is not None else "GET", path, encoded, request_headers)
        response = connection.getresponse()
        data = response.read()
        if response.getheader("Content-Type", "").startswith("application/json"):
            data = json.loads(data)
        return response.status, dict(response.getheaders()), data
    finally:
        connection.close()


def wait_job(url: str, job_id: str, timeout: float = 20):
    deadline = time.monotonic() + timeout
    stdout, stderr = [], []
    stdout_offset = stderr_offset = 0
    while time.monotonic() < deadline:
        status, _, data = request(
            url, f"/api/jobs/{job_id}?stdout_offset={stdout_offset}&stderr_offset={stderr_offset}"
        )
        assert status == 200, (status, data)
        stdout.append(data["stdout"])
        stderr.append(data["stderr"])
        stdout_offset, stderr_offset = data["stdout_offset"], data["stderr_offset"]
        if data["status"] in TERMINAL:
            status, _, tail = request(
                url, f"/api/jobs/{job_id}?stdout_offset={stdout_offset}&stderr_offset={stderr_offset}"
            )
            assert status == 200 and tail["stdout"] == tail["stderr"] == "", tail
            data["stdout"], data["stderr"] = "".join(stdout), "".join(stderr)
            return data
        time.sleep(0.02)
    raise AssertionError(f"Job did not finish: {job_id}")


def create_job(url: str, arguments: list[str]):
    status, _, job = request(url, "/api/jobs", {"args": arguments})
    assert status == 202, (status, job)
    return job


def linux_engine_children(parent: int, engine: Path) -> list[int]:
    """Inspect only the test server's descendant processes, never the global list."""
    if not sys.platform.startswith("linux"):
        return []
    found = []
    pending = [parent]
    visited = set()
    while pending:
        pid = pending.pop()
        if pid in visited:
            continue
        visited.add(pid)
        try:
            tasks = list(Path(f"/proc/{pid}/task").iterdir())
        except FileNotFoundError:
            continue
        # Linux records children under the thread that called Popen, which is
        # a job worker here, not necessarily the process's main thread.
        for task in tasks:
            try:
                children = (task / "children").read_text().split()
            except FileNotFoundError:
                continue
            for child in map(int, children):
                pending.append(child)
                try:
                    if Path(f"/proc/{child}/exe").resolve() == engine:
                        found.append(child)
                except FileNotFoundError:
                    pass
    return found


def check(engine_source: Path, web_source: Path | None) -> None:
    with RetryingTemporaryDirectory(prefix="fates web smoke ") as temporary:
        directory = Path(temporary).resolve()
        package = directory / "package with spaces"
        working = directory / "unrelated working directory"
        package.mkdir()
        working.mkdir()
        engine = package / ("fates.exe" if os.name == "nt" else "fates")
        shutil.copy2(engine_source, engine)
        engine.chmod(0o755)
        if os.name != "nt":
            # A Windows build must never shadow the native engine, including WSL.
            (package / "fates.exe").write_bytes(b"not the native engine\n")
        if web_source:
            web = package / ("fates-web.exe" if os.name == "nt" else "fates-web")
            shutil.copy2(web_source, web)
            web.chmod(0o755)
            command = [str(web)]  # Deliberately exercise sibling auto-discovery.
        else:
            command = [sys.executable, str(ROOT / "frontend" / "fates_web.py"), "--fates", str(engine)]

        rejected = subprocess.run(
            [*command, "--host", "0.0.0.0", "--no-browser"], cwd=working,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8", timeout=20,
        )
        assert rejected.returncode == 2, rejected.stdout

        process = subprocess.Popen(
            [*command, "--port", "0", "--no-browser"], cwd=working,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            encoding="utf-8", errors="replace", bufsize=1,
        )
        output: queue.Queue[str] = queue.Queue()
        logs: list[str] = []
        engine_pids: list[int] = []

        def read_output():
            for line in process.stdout:
                logs.append(line)
                output.put(line)

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        try:
            url = None
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline and url is None:
                try:
                    line = output.get(timeout=0.1)
                except queue.Empty:
                    if process.poll() is not None:
                        raise AssertionError("Server exited before startup: " + "".join(logs))
                    continue
                match = re.search(r"Open\s+:\s+(http://\S+)", line)
                if match:
                    url = match[1]
            assert url, "Server did not print its URL: " + "".join(logs)
            status, _, health = request(url, "/api/health")
            assert status == 200 and health == {"ok": True}, health
            status, _, metadata = request(url, "/api/meta")
            assert status == 200, metadata
            assert Path(metadata["fates_path"]) == engine, metadata
            assert metadata["command_shell"] == ("powershell" if os.name == "nt" else "posix"), metadata
            assert metadata["symbols"]["constants"], metadata
            operator_names = {item["name"] for item in metadata["symbols"]["unary_operators"]}
            assert {"zeta", "besselj0", "besselj1", "ellintk", "ellinte"} <= operator_names
            constant_names = {item["name"] for item in metadata["symbols"]["constants"]}
            assert {"apery", "glaisher", "khinchin", "zeta2"} <= constant_names

            for asset in ("index.html", "app.js", "styles.css", "favicon.svg", "vendor/katex/katex.min.js",
                          "vendor/katex/katex.min.css", "vendor/katex/fonts/KaTeX_Main-Regular.woff2"):
                status, headers, data = request(url, "/" if asset == "index.html" else "/" + asset)
                assert status == 200, (asset, status)
                assert data == (ROOT / "frontend" / "static" / asset).read_bytes(), f"Stale/missing asset: {asset}"
                assert headers["X-Content-Type-Options"] == "nosniff", headers
                cached_status, _, _ = request(url, "/" + asset, headers={"If-None-Match": headers["ETag"]})
                assert cached_status == 304, (asset, cached_status)
            assert request(url, "/%2e%2e/fates_web.py")[0] == 404
            assert request(url, "/api/jobs", {"args": [None]})[0] == 400
            assert request(url, "/api/jobs", {"args": ["--version"]},
                           {"Origin": "https://example.invalid"})[0] == 403
            print("PASS startup, native discovery, loopback restrictions, offline assets and cache", flush=True)

            base = ["1.4142135623730951", "--digits", "2", "--constants", "none", "--threads", "1",
                    "--no-stop", "--json", "--live", "--live-json"]
            for label, mode, arguments in (
                ("constants", "constants", [*base, "--ops", "sqrt", "--max-cost", "2"]),
                ("equations", "equations", [*base, "--equations", "--ops", "^", "--max-cost", "5"]),
                ("grouped constants", "constants", [
                    "5.859874482048838", "--digits=", "--constants", "pi,e", "--ops", "+",
                    "--max-cost", "3", "--constant-count", "pi,e=2:2", "--threads", "1",
                    "--no-stop", "--json", "--live", "--live-json",
                ]),
                ("special functions", "constants", [
                    "1.6449340668482264", "--digits", "2", "--constants", "none",
                    "--ops", "zeta", "--max-cost", "4", "--results", "3", "--threads", "1",
                    "--no-stop", "--json", "--live", "--live-json",
                ]),
            ):
                reference = subprocess.run(
                    [str(engine), *arguments], cwd=package, capture_output=True,
                    encoding="utf-8", timeout=20, check=True,
                )
                expected = json.loads(reference.stdout)
                job = create_job(url, arguments)
                result = wait_job(url, job["id"])
                assert result["status"] == "completed" and result["return_code"] == 0, result
                actual = json.loads(result["stdout"])
                # Constant searches omit search_mode; equation JSON sets it.
                assert actual.get("search_mode", "constants") == mode and actual["results"], actual
                assert actual["results"] == expected["results"], (expected, actual)
                if label == "grouped constants":
                    assert any(row["expression"] == "pi+e" for row in actual["results"]), actual
                    assert all(len(re.findall(r"pi|e", row["expression"])) == 2
                               for row in actual["results"]), actual
                if label == "special functions":
                    assert any(row["expression"] == "zeta(2)" and
                               row["latex"] == r"\zeta\left(2\right)" for row in actual["results"]), actual
                events = [json.loads(line[len(LIVE_PREFIX):]) for line in result["stderr"].splitlines()
                          if line.startswith(LIVE_PREFIX)]
                assert events, result["stderr"]
                print(f"PASS {label}: CLI/Web results identical; {len(events)} live JSON events", flush=True)

            failed = wait_job(url, create_job(url, ["--not-a-fates-option"])["id"])
            assert failed["status"] == "failed" and failed["return_code"] != 0, failed
            job = create_job(url, LONG_SEARCH)
            assert request(url, f"/api/jobs/{job['id']}/cancel", {})[0] == 200
            assert wait_job(url, job["id"])["status"] == "cancelled"
            print("PASS process errors and cancellation", flush=True)

            if os.name != "nt":
                # SIGTERM is the normal stop mechanism for headless Linux services.
                shutdown_job = create_job(url, LONG_SEARCH)
                if sys.platform.startswith("linux"):
                    deadline = time.monotonic() + 5
                    while not engine_pids and time.monotonic() < deadline:
                        engine_pids = linux_engine_children(process.pid, engine)
                        time.sleep(0.01)
                    assert engine_pids, (
                        "Expected an active engine child before SIGTERM",
                        request(url, f"/api/jobs/{shutdown_job['id']}")[2],
                    )
                process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=8) == 0, "".join(logs)
                for pid in engine_pids:
                    assert Path(f"/proc/{pid}/exe").resolve() != engine, f"Engine {pid} survived server shutdown"
                print("PASS SIGTERM shutdown with an active search" +
                      (" (no engine left running)" if engine_pids else ""), flush=True)
            print("Web smoke passed (" + ("packaged" if web_source else "source") + ").", flush=True)
        finally:
            if process.poll() is None:
                if os.name == "nt":
                    # A Windows one-file bundle has a bootloader and a Python
                    # child. Terminating only the bootloader leaves the server
                    # (and its stdout handle) alive. Target this test-owned tree.
                    subprocess.run(
                        ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10, check=True,
                    )
                else:
                    process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            # On a failed shutdown assertion, clean up only the exact test-owned
            # engine processes, checking their executable again against PID reuse.
            for pid in engine_pids:
                if Path(f"/proc/{pid}/exe").resolve() == engine:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            reader.join(timeout=2)
            process.stdout.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True, help="Native Fates engine")
    parser.add_argument("--web", type=Path, help="Packaged fates-web (omit to test the Python source)")
    arguments = parser.parse_args()
    engine = arguments.bin.resolve(strict=True)
    web = arguments.web.resolve(strict=True) if arguments.web else None
    check(engine, web)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
