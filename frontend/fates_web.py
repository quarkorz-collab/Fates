from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import mimetypes
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse
import uuid
import webbrowser


SERVER_VERSION = "1.0"
MAX_REQUEST_BYTES = 1_048_576
MAX_ARGUMENTS = 256
MAX_ARGUMENT_LENGTH = 16_384
MAX_CAPTURE_CHARS = 8_388_608
MAX_ACTIVE_JOBS = 4
LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}


def configure_utf8_console() -> None:
    if os.name == "nt":
        try:
            import ctypes

            kernel32 = ctypes.windll.kernel32
            kernel32.SetConsoleCP(65001)
            kernel32.SetConsoleOutputCP(65001)
        except (AttributeError, OSError):
            pass
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="replace")


class CaptureBuffer:
    def __init__(self, maximum: int) -> None:
        self.maximum = maximum
        self.chunks: list[str] = []
        self.ends: list[int] = []
        self.length = 0
        self.truncated = False

    def append(self, text: str) -> None:
        if self.length >= self.maximum:
            self.truncated = True
            return
        remaining = self.maximum - self.length
        accepted = text[:remaining]
        if accepted:
            self.chunks.append(accepted)
            self.length += len(accepted)
            self.ends.append(self.length)
        if len(text) > remaining:
            self.truncated = True

    def read_from(self, offset: int) -> tuple[str, int]:
        offset = min(max(offset, 0), self.length)
        chunk_index = bisect.bisect_right(self.ends, offset)
        if chunk_index >= len(self.chunks):
            return "", self.length
        chunk_start = 0 if chunk_index == 0 else self.ends[chunk_index - 1]
        pieces = [self.chunks[chunk_index][offset - chunk_start :]]
        pieces.extend(self.chunks[chunk_index + 1 :])
        return "".join(pieces), self.length


def process_creation_flags() -> int:
    return subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def static_root() -> Path:
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS")) / "frontend_static"
    return Path(__file__).resolve().parent / "static"


def find_fates(explicit: str | None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit).expanduser())
    if getattr(sys, "frozen", False):
        candidates.append(Path(sys.executable).resolve().parent / "fates.exe")
        candidates.append(Path(sys.executable).resolve().parent / "fates")
    else:
        project_root = Path(__file__).resolve().parent.parent
        candidates.extend((project_root / "fates.exe", project_root / "fates"))
    candidates.extend((Path.cwd() / "fates.exe", Path.cwd() / "fates"))
    for name in ("fates.exe", "fates"):
        located = shutil.which(name)
        if located:
            candidates.append(Path(located))

    checked: set[Path] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved in checked:
            continue
        checked.add(resolved)
        if resolved.is_file():
            return resolved
    raise FileNotFoundError(
        "找不到 fates.exe；请把 fates-web.exe 与 fates.exe 放在同一目录，"
        "或使用 --fates PATH 指定。"
    )


def inspect_fates(executable: Path) -> str:
    completed = subprocess.run(
        [str(executable), "--version"],
        cwd=str(executable.parent),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=10,
        creationflags=process_creation_flags(),
        check=False,
    )
    output = completed.stdout.strip()
    if completed.returncode != 0 or not output.startswith("Fates "):
        raise RuntimeError(f"无法验证 Fates 可执行文件：{output or completed.returncode}")
    return output


def inspect_symbol_catalog(executable: Path) -> dict[str, object]:
    completed = subprocess.run(
        [str(executable), "--list-symbols", "--json"],
        cwd=str(executable.parent),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=10,
        creationflags=process_creation_flags(),
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"无法读取 Fates 符号目录：{completed.stderr.strip() or completed.returncode}"
        )
    try:
        catalog = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("Fates 符号目录不是有效 JSON") from error
    if not isinstance(catalog, dict) or not isinstance(catalog.get("constants"), list):
        raise RuntimeError("Fates 符号目录缺少 constants")
    return catalog


class Job:
    def __init__(self, arguments: list[str], command: list[str]) -> None:
        self.id = uuid.uuid4().hex
        self.arguments = arguments
        self.command = command
        self.status = "queued"
        self.return_code: int | None = None
        self.created_at = time.time()
        self.started_at: float | None = None
        self.ended_at: float | None = None
        self.stdout = CaptureBuffer(MAX_CAPTURE_CHARS)
        self.stderr = CaptureBuffer(MAX_CAPTURE_CHARS)
        self.cancel_requested = False
        self.process: subprocess.Popen[str] | None = None
        self.lock = threading.RLock()

    def append(self, channel: str, text: str) -> None:
        with self.lock:
            getattr(self, channel).append(text)

    def snapshot(self, stdout_offset: int, stderr_offset: int) -> dict[str, object]:
        with self.lock:
            stdout, next_stdout_offset = self.stdout.read_from(stdout_offset)
            stderr, next_stderr_offset = self.stderr.read_from(stderr_offset)
            return {
                "id": self.id,
                "status": self.status,
                "return_code": self.return_code,
                "created_at": self.created_at,
                "started_at": self.started_at,
                "ended_at": self.ended_at,
                "stdout": stdout,
                "stderr": stderr,
                "stdout_offset": next_stdout_offset,
                "stderr_offset": next_stderr_offset,
                "stdout_truncated": self.stdout.truncated,
                "stderr_truncated": self.stderr.truncated,
                "command": self.command,
            }


class JobManager:
    def __init__(self, executable: Path) -> None:
        self.executable = executable
        self.jobs: dict[str, Job] = {}
        self.lock = threading.RLock()

    @staticmethod
    def validate_arguments(value: object) -> list[str]:
        if not isinstance(value, list) or not value:
            raise ValueError("args 必须是非空字符串数组")
        if len(value) > MAX_ARGUMENTS:
            raise ValueError(f"参数数量不能超过 {MAX_ARGUMENTS}")
        arguments: list[str] = []
        total_length = 0
        for item in value:
            if not isinstance(item, str):
                raise ValueError("每个参数都必须是字符串")
            if "\0" in item:
                raise ValueError("参数不能包含 NUL 字节")
            if len(item) > MAX_ARGUMENT_LENGTH:
                raise ValueError(f"单个参数不能超过 {MAX_ARGUMENT_LENGTH} 个字符")
            total_length += len(item)
            arguments.append(item)
        if total_length > MAX_REQUEST_BYTES:
            raise ValueError("参数总长度过大")
        return arguments

    def create(self, raw_arguments: object) -> Job:
        arguments = self.validate_arguments(raw_arguments)
        with self.lock:
            active = sum(job.status in {"queued", "running"} for job in self.jobs.values())
            if active >= MAX_ACTIVE_JOBS:
                raise RuntimeError(f"同时最多运行 {MAX_ACTIVE_JOBS} 个任务")
            command = [str(self.executable), *arguments]
            job = Job(arguments, command)
            self.jobs[job.id] = job
            self._discard_old_jobs_locked()
        threading.Thread(target=self._run, args=(job,), daemon=True, name=f"fates-job-{job.id[:8]}").start()
        return job

    def get(self, job_id: str) -> Job | None:
        with self.lock:
            return self.jobs.get(job_id)

    def cancel(self, job_id: str) -> bool:
        job = self.get(job_id)
        if job is None:
            return False
        with job.lock:
            if job.status not in {"queued", "running"}:
                return True
            job.cancel_requested = True
            process = job.process
        if process is not None and process.poll() is None:
            process.terminate()
            threading.Thread(target=self._kill_if_needed, args=(process,), daemon=True).start()
        return True

    def shutdown(self) -> None:
        with self.lock:
            active_ids = [job.id for job in self.jobs.values() if job.status in {"queued", "running"}]
        for job_id in active_ids:
            self.cancel(job_id)

    def _discard_old_jobs_locked(self) -> None:
        if len(self.jobs) <= 64:
            return
        completed = sorted(
            (job for job in self.jobs.values() if job.status not in {"queued", "running"}),
            key=lambda job: job.ended_at or job.created_at,
        )
        for job in completed[: len(self.jobs) - 64]:
            self.jobs.pop(job.id, None)

    @staticmethod
    def _read_stream(job: Job, channel: str, stream: object) -> None:
        try:
            for line in stream:  # type: ignore[union-attr]
                job.append(channel, line)
        except (OSError, ValueError) as error:
            job.append("stderr", f"\n[fates-web] 读取 {channel} 失败：{error}\n")
        finally:
            try:
                stream.close()  # type: ignore[union-attr]
            except (OSError, ValueError):
                pass

    @staticmethod
    def _kill_if_needed(process: subprocess.Popen[str]) -> None:
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()

    def _run(self, job: Job) -> None:
        try:
            with job.lock:
                job.status = "running"
                job.started_at = time.time()
            process = subprocess.Popen(
                job.command,
                cwd=str(self.executable.parent),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=process_creation_flags(),
            )
            with job.lock:
                job.process = process
                cancel_immediately = job.cancel_requested
            if cancel_immediately:
                process.terminate()

            stdout_reader = threading.Thread(
                target=self._read_stream, args=(job, "stdout", process.stdout), daemon=True
            )
            stderr_reader = threading.Thread(
                target=self._read_stream, args=(job, "stderr", process.stderr), daemon=True
            )
            stdout_reader.start()
            stderr_reader.start()
            return_code = process.wait()
            stdout_reader.join(timeout=2)
            stderr_reader.join(timeout=2)
            with job.lock:
                job.return_code = return_code
                if job.cancel_requested:
                    job.status = "cancelled"
                elif return_code == 0:
                    job.status = "completed"
                else:
                    job.status = "failed"
                job.ended_at = time.time()
        except Exception as error:
            job.append("stderr", f"[fates-web] 无法启动搜索：{error}\n")
            with job.lock:
                job.status = "cancelled" if job.cancel_requested else "failed"
                job.return_code = -1
                job.ended_at = time.time()


def handler_factory(manager: JobManager, root: Path, metadata: dict[str, object], verbose: bool):
    resolved_root = root.resolve()
    asset_cache: dict[Path, tuple[int, bytes, str, str]] = {}
    asset_lock = threading.Lock()

    def resolve_asset(request_path: str) -> Path | None:
        relative = "index.html" if request_path in {"", "/"} else unquote(request_path).lstrip("/")
        candidate = (resolved_root / relative).resolve()
        if candidate != resolved_root and resolved_root not in candidate.parents:
            return None
        return candidate if candidate.is_file() else None

    def load_asset(path: Path) -> tuple[bytes, str, str]:
        modified = path.stat().st_mtime_ns
        with asset_lock:
            cached = asset_cache.get(path)
            if cached and cached[0] == modified:
                return cached[1], cached[2], cached[3]
            data = path.read_bytes()
            content_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
            if content_type.startswith("text/") or path.suffix in {".js", ".json", ".svg"}:
                content_type += "; charset=utf-8"
            etag = '"' + hashlib.sha256(data).hexdigest()[:24] + '"'
            asset_cache[path] = (modified, data, content_type, etag)
            return data, content_type, etag

    class FatesHandler(BaseHTTPRequestHandler):
        server_version = f"FatesWeb/{SERVER_VERSION}"
        protocol_version = "HTTP/1.1"

        def log_message(self, format_string: str, *arguments: object) -> None:
            if verbose:
                super().log_message(format_string, *arguments)

        def end_headers(self) -> None:
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("X-Frame-Options", "DENY")
            self.send_header(
                "Content-Security-Policy",
                "default-src 'self'; script-src 'self'; style-src 'self'; style-src-attr 'unsafe-inline'; "
                "font-src 'self'; img-src 'self' data:; connect-src 'self'; "
                "frame-ancestors 'none'; base-uri 'none'",
            )
            super().end_headers()

        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            if parsed.path == "/api/meta":
                self._send_json(HTTPStatus.OK, metadata)
                return
            if parsed.path == "/api/health":
                self._send_json(HTTPStatus.OK, {"ok": True})
                return
            parts = parsed.path.strip("/").split("/")
            if len(parts) == 3 and parts[:2] == ["api", "jobs"]:
                job = manager.get(parts[2])
                if job is None:
                    self._send_json(HTTPStatus.NOT_FOUND, {"error": "任务不存在"})
                    return
                query = parse_qs(parsed.query)
                stdout_offset = self._query_offset(query, "stdout_offset")
                stderr_offset = self._query_offset(query, "stderr_offset")
                self._send_json(HTTPStatus.OK, job.snapshot(stdout_offset, stderr_offset))
                return
            asset = resolve_asset(parsed.path)
            if asset is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "页面不存在"})
                return
            self._send_asset(asset)

        def do_POST(self) -> None:
            if not self._origin_allowed():
                self._send_json(HTTPStatus.FORBIDDEN, {"error": "拒绝跨来源请求"})
                return
            parsed = urlparse(self.path)
            if parsed.path == "/api/jobs":
                try:
                    body = self._read_json()
                    job = manager.create(body.get("args") if isinstance(body, dict) else None)
                    self._send_json(HTTPStatus.ACCEPTED, job.snapshot(0, 0))
                except ValueError as error:
                    self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
                except RuntimeError as error:
                    self._send_json(HTTPStatus.TOO_MANY_REQUESTS, {"error": str(error)})
                return
            parts = parsed.path.strip("/").split("/")
            if len(parts) == 4 and parts[:2] == ["api", "jobs"] and parts[3] == "cancel":
                if manager.cancel(parts[2]):
                    self._send_json(HTTPStatus.OK, {"ok": True})
                else:
                    self._send_json(HTTPStatus.NOT_FOUND, {"error": "任务不存在"})
                return
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "接口不存在"})

        def _query_offset(self, query: dict[str, list[str]], name: str) -> int:
            try:
                return max(0, int(query.get(name, ["0"])[0]))
            except ValueError:
                return 0

        def _origin_allowed(self) -> bool:
            origin = self.headers.get("Origin")
            if not origin:
                return True
            hostname = urlparse(origin).hostname
            return hostname in LOOPBACK_HOSTS

        def _read_json(self) -> object:
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError as error:
                raise ValueError("无效 Content-Length") from error
            if length <= 0 or length > MAX_REQUEST_BYTES:
                raise ValueError("请求正文为空或过大")
            try:
                return json.loads(self.rfile.read(length).decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise ValueError("请求必须是 UTF-8 JSON") from error

        def _send_asset(self, path: Path) -> None:
            try:
                data, content_type, etag = load_asset(path)
            except OSError:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "静态资源缺失"})
                return
            if self.headers.get("If-None-Match") == etag:
                self.send_response(HTTPStatus.NOT_MODIFIED)
                self.send_header("ETag", etag)
                self.end_headers()
                return
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("ETag", etag)
            if "vendor" in path.parts:
                self.send_header("Cache-Control", "public, max-age=31536000, immutable")
            else:
                self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

        def _send_json(self, status: HTTPStatus, payload: object) -> None:
            data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

    return FatesHandler


class FatesHttpServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 32


class FatesIPv6HttpServer(FatesHttpServer):
    address_family = socket.AF_INET6


def create_http_server(host: str, port: int, handler: type[BaseHTTPRequestHandler]) -> FatesHttpServer:
    server_type = FatesIPv6HttpServer if host == "::1" else FatesHttpServer
    return server_type((host, port), handler)


def print_server_banner(url: str, fates_version: str, executable: Path) -> None:
    print(
        r"""+-----------------------------------+
| FFFFF  AAAAA  TTTTT  EEEEE  SSSSS |
| F      A   A    T    E      S     |
| FFFF   AAAAA    T    EEEE   SSSSS |
| F      A A      T    E          S |
| F      A  A     T    EEEEE  SSSSS |
+-----------------------------------+
"""
    )
    print(f" Fates Web Server {SERVER_VERSION}")
    print(f" {fates_version}")
    print(f" Executable : {executable}")
    print(f" Open       : {url}")
    print(" Stop       : Ctrl+C\n")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Fates 本地网页服务器")
    parser.add_argument("--host", default="127.0.0.1", help="仅允许回环地址，默认 127.0.0.1")
    parser.add_argument("--port", type=int, default=0, help="监听端口；0 表示自动选择，默认 0")
    parser.add_argument("--fates", help="fates.exe 路径；默认查找服务器同目录和项目目录")
    parser.add_argument("--no-browser", action="store_true", help="启动后不自动打开浏览器")
    parser.add_argument("--verbose", action="store_true", help="显示 HTTP 访问日志")
    arguments = parser.parse_args()
    if arguments.host not in LOOPBACK_HOSTS:
        parser.error("为安全起见，--host 只能是 127.0.0.1、localhost 或 ::1")
    if arguments.port < 0 or arguments.port > 65535:
        parser.error("--port 必须在 0..65535")
    return arguments


def main() -> int:
    configure_utf8_console()
    arguments = parse_arguments()
    try:
        executable = find_fates(arguments.fates)
        fates_version = inspect_fates(executable)
        symbol_catalog = inspect_symbol_catalog(executable)
        root = static_root()
        if not (root / "index.html").is_file():
            raise FileNotFoundError(f"前端静态资源缺失：{root}")
    except (FileNotFoundError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"fates-web 错误：{error}", file=sys.stderr)
        return 2

    manager = JobManager(executable)
    metadata = {
        "server_version": SERVER_VERSION,
        "fates_version": fates_version,
        "fates_path": str(executable),
        "max_active_jobs": MAX_ACTIVE_JOBS,
        "symbols": symbol_catalog,
    }
    handler = handler_factory(manager, root, metadata, arguments.verbose)
    try:
        server = create_http_server(arguments.host, arguments.port, handler)
    except OSError as error:
        print(f"fates-web 错误：无法监听 {arguments.host}:{arguments.port}：{error}", file=sys.stderr)
        return 2

    address, port = server.server_address[:2]
    browser_host = "127.0.0.1" if address in {"0.0.0.0", "::"} else address
    if ":" in browser_host and not browser_host.startswith("["):
        browser_host = f"[{browser_host}]"
    url = f"http://{browser_host}:{port}/"
    metadata["port"] = port
    metadata["url"] = url
    print_server_banner(url, fates_version, executable)
    if not arguments.no_browser:
        threading.Timer(0.35, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        print("\n正在停止 Fates Web Server...")
    finally:
        manager.shutdown()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
