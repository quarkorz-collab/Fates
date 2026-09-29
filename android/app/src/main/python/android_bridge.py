"""Keep the existing loopback WebUI alive inside the Android process."""

from pathlib import Path
import threading

import fates_web


_lock = threading.Lock()
_active = None


def start(executable_path: str, static_path: str) -> str:
    global _active
    with _lock:
        if _active is not None:
            return _active[2]
        executable = fates_web.check_fates_path(Path(executable_path))
        root = Path(static_path).resolve()
        if not (root / "index.html").is_file():
            raise FileNotFoundError(f"前端静态资源缺失：{root}")
        manager = fates_web.JobManager(executable)
        metadata = {
            "server_version": fates_web.SERVER_VERSION,
            "fates_version": fates_web.inspect_fates(executable),
            "fates_path": str(executable),
            "max_active_jobs": fates_web.MAX_ACTIVE_JOBS,
            "symbols": fates_web.inspect_symbol_catalog(executable),
            "platform": "android",
            "command_shell": "posix",
        }
        handler = fates_web.handler_factory(manager, root, metadata, False)
        server = fates_web.create_http_server("127.0.0.1", 0, handler)
        port = server.server_address[1]
        url = f"http://127.0.0.1:{port}/"
        metadata.update(port=port, url=url)
        thread = threading.Thread(target=server.serve_forever,
                                  kwargs={"poll_interval": 0.2}, daemon=True)
        try:
            thread.start()
        except Exception:
            server.server_close()
            raise
        _active = (server, manager, url, thread)
        return url


def stop() -> None:
    global _active
    with _lock:
        active, _active = _active, None
    if active is None:
        return
    server, manager, _, thread = active
    server.shutdown()
    manager.shutdown()
    server.server_close()
    thread.join(timeout=2)
