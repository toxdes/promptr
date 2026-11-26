#!/usr/bin/env python3
"""Exercise bundled plugins using a local HTTP fixture and a fake CLI."""

from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import os
import selectors
import shutil
import shlex
import signal
import socket
import subprocess
import tempfile
import threading
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parent.parent


class Peer:
    def __init__(self, executable, environment):
        self.process = subprocess.Popen(
            [str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=environment, start_new_session=True,
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer = b""

    def send(self, method, params, request_id):
        envelope = {"jsonrpc": "2.0", "method": method, "id": request_id, "params": params}
        self.process.stdin.write(json.dumps(envelope).encode() + b"\n")
        self.process.stdin.flush()

    def read(self, timeout=15):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self.selector.select(remaining):
                raise TimeoutError("Plugin did not answer")
            data = os.read(self.process.stdout.fileno(), 16384)
            if not data:
                raise EOFError("Plugin exited before answering")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def initialize(self, settings, purpose="submit"):
        self.send("provider/initialize", {"protocol_version": "2.0", "config": settings, "purpose": purpose}, 1)
        response = self.read()
        assert response["result"]["protocol_version"] == "2.0", response

    def request(self, params):
        self.send("provider/submit", params, 2)
        output = ""
        deadline = time.monotonic() + 30
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Plugin did not finish within 30 seconds")
            response = self.read(min(15, remaining))
            if "method" in response:
                assert response["params"]["thread_id"] == params["thread_id"]
                assert response["params"]["request_id"] == params["request_id"]
                output += response["params"]["output"]
            elif response["id"] == 2:
                return output, response

    def close(self):
        self.selector.close()
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait()
            raise
        finally:
            self.process.stdout.close()
            self.process.stderr.close()


class Backend(BaseHTTPRequestHandler):
    received = []
    mode = "normal"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        body = json.dumps({"data": [{"id": "fixture/first"}, {"id": "fixture/second"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        type(self).received.append((body, self.headers["Authorization"]))
        if self.mode == "denied":
            self.send_response(403)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        try:
            for text in ("pon", "g"):
                self.wfile.write(f'data: {json.dumps({"id": "chatcmpl-fixture", "object": "chat.completion.chunk", "created": 0, "model": "chat", "choices": [{"index": 0, "delta": {"content": text}, "finish_reason": None}]})}\n\n'.encode())
                self.wfile.flush()
            if self.mode != "truncated":
                self.wfile.write(b'data: {"id":"chatcmpl-fixture","object":"chat.completion.chunk","created":0,"model":"chat","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\n')
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="promptr-adapters-")
        self.base = Path(self.temporary.name)
        self.environment = {
            **os.environ, "XDG_CONFIG_HOME": str(self.base / "config"),
            "XDG_DATA_HOME": str(self.base / "data"), "XDG_CACHE_HOME": str(self.base / "cache"),
        }
        for name in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
            self.environment.pop(name, None)
        self.params = {
            "thread_id": str(uuid.uuid4()), "request_id": str(uuid.uuid4()),
            "messages": [{"role": "user", "content": "one question"}],
            "agent": {"name": "example", "content": "The complete Markdown context."},
            "model": "fixture/model",
        }

    def tearDown(self):
        self.temporary.cleanup()

    @contextmanager
    def peer(self, provider):
        peer = Peer(ROOT / f"providers/promptr-{provider}/promptr-{provider}", self.environment)
        try:
            yield peer
        finally:
            peer.close()

    @contextmanager
    def http_backend(self, mode="normal"):
        Backend.mode = mode
        Backend.received = []
        server = ThreadingHTTPServer(("127.0.0.1", 0), Backend)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            yield {"api_key": "fake-token", "base_url": f"http://127.0.0.1:{server.server_port}"}
        finally:
            server.shutdown()
            server.server_close()
            worker.join()

    @contextmanager
    def opencode_server(self, executable):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        with (self.base / "opencode-server.log").open("w") as log:
            server = subprocess.Popen(
                [executable, "serve", "--hostname", "127.0.0.1", "--port", str(port)],
                cwd=self.base, env=self.environment, stdout=log, stderr=log,
            )
            try:
                deadline = time.monotonic() + 10
                while True:
                    if server.poll() is not None or time.monotonic() >= deadline:
                        self.fail("Isolated OpenCode server did not start")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except OSError:
                        time.sleep(0.05)
                yield f"http://127.0.0.1:{port}"
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()

    def test_openrouter_context_stream_and_models(self):
        with self.http_backend() as settings, self.peer("openrouter") as peer:
            peer.initialize(settings)
            output, response = peer.request(self.params)
            self.assertEqual(output, "pong")
            self.assertEqual(response["result"]["status"], "completed")
            body, auth = Backend.received[0]
            self.assertEqual(auth, "Bearer fake-token")
            self.assertEqual(body["messages"], [
                {"role": "system", "content": "The complete Markdown context."},
                {"role": "user", "content": "one question"},
            ])
        with self.http_backend() as settings, self.peer("openrouter") as peer:
            peer.initialize(settings)
            peer.send("provider/models", {}, 2)
            self.assertEqual(peer.read()["result"]["models"], ["fixture/first", "fixture/second"])

    def test_openrouter_missing_key_and_public_models(self):
        self.environment.pop("OPENROUTER_API_KEY", None)
        with self.http_backend() as settings:
            settings.pop("api_key")
            with self.peer("openrouter") as peer:
                peer.send("provider/initialize", {"protocol_version": "2.0", "config": settings}, 1)
                error = peer.read()["error"]
                self.assertEqual(error["code"], -32010)
                self.assertIn("No OpenRouter API key configured", error["message"])
                self.assertIn("Preferences", error["message"])
                self.assertEqual(Backend.received, [])
            with self.peer("openrouter") as peer:
                peer.initialize(settings, purpose="models")
                peer.send("provider/models", {}, 2)
                self.assertEqual(peer.read()["result"]["models"], ["fixture/first", "fixture/second"])

    def test_openrouter_failures_and_compatibility(self):
        for mode in ("denied", "truncated"):
            with self.subTest(mode=mode), self.http_backend(mode) as settings, self.peer("openrouter") as peer:
                peer.initialize(settings)
                _output, response = peer.request(self.params)
                self.assertIn("error", response)
        with self.peer("openrouter") as peer:
            peer.send("provider/initialize", {"protocol_version": "1.0", "config": {}}, 1)
            self.assertEqual(peer.read()["error"]["code"], -32001)

    def test_opencode_context_project_and_resume(self):
        fake = self.base / "fake-opencode"
        shutil.copyfile(ROOT / "tests/fake_opencode.py", fake)
        fake.chmod(0o755)
        project = self.base / "project with spaces"
        project.mkdir()
        sentinel = project / "keep.txt"
        sentinel.write_text("keep this file")
        self.params["project_dir"] = str(project)
        settings = {"opencode_path": str(fake)}
        with self.peer("opencode") as peer:
            peer.initialize(settings)
            output, response = peer.request(self.params)
        observed = json.loads(output)
        self.assertEqual(observed["cwd"], str(project))
        self.assertEqual(observed["context"], "The complete Markdown context.")
        self.assertEqual(observed["conversation"].count("one question"), 1)
        self.assertFalse(observed["reused"])
        state = response["result"]["resume_state"]
        self.params["messages"] += [{"role": "assistant", "content": output}, {"role": "user", "content": "next question"}]
        self.params["resume_state"] = state
        self.params["request_id"] = str(uuid.uuid4())
        with self.peer("opencode") as peer:
            peer.initialize(settings)
            output, _response = peer.request(self.params)
        observed = json.loads(output)
        self.assertTrue(observed["reused"])
        self.assertNotIn("one question", observed["conversation"])
        self.assertEqual(sentinel.read_text(), "keep this file")
        # An intervening provider makes the opaque session stale. The plugin rebuilds history.
        self.params["messages"][-1]["content"] = "another provider answered"
        self.params["messages"].insert(-1, {"role": "assistant", "content": "intervening answer"})
        with self.peer("opencode") as peer:
            peer.initialize(settings)
            output, _response = peer.request(self.params)
        self.assertFalse(json.loads(output)["reused"])

    def test_opencode_connection_test_and_discovery(self):
        fake = self.base / "fake-opencode"
        shutil.copyfile(ROOT / "tests/fake_opencode.py", fake)
        fake.chmod(0o755)
        self.params["connection_test"] = True
        with self.peer("opencode") as peer:
            peer.initialize({"opencode_path": str(fake)})
            output, response = peer.request(self.params)
            self.assertTrue(json.loads(output)["tools_denied"])
            self.assertFalse(json.loads(output)["attached"])
            self.assertEqual(json.loads(output)["conversation"], "one question")
            self.assertNotIn("resume_state", response["result"])
        with self.peer("opencode") as peer:
            peer.initialize({"opencode_path": str(fake)})
            peer.send("provider/models", {}, 2)
            self.assertEqual(peer.read()["result"]["models"], ["fixture/first", "fixture/second"])

    def test_opencode_connection_default_model(self):
        fake = self.base / "fake-opencode"
        shutil.copyfile(ROOT / "tests/fake_opencode.py", fake)
        fake.chmod(0o755)
        for connection_test, selected, expected in (
            (True, None, "opencode/big-pickle"),
            (True, "", "opencode/big-pickle"),
            (True, "None", "opencode/big-pickle"),
            (True, "fixture/explicit", "fixture/explicit"),
            (False, None, None),
        ):
            with self.subTest(connection_test=connection_test, selected=selected):
                self.params["connection_test"] = connection_test
                if selected is None:
                    self.params.pop("model", None)
                else:
                    self.params["model"] = selected
                with self.peer("opencode") as peer:
                    peer.initialize({"opencode_path": str(fake)})
                    output, response = peer.request(self.params)
                self.assertIn("result", response, response)
                self.assertEqual(json.loads(output)["model"], expected)

    def test_opencode_large_markdown_avoids_argument_limits(self):
        fake = self.base / "fake-opencode"
        shutil.copyfile(ROOT / "tests/fake_opencode.py", fake)
        fake.chmod(0o755)
        self.params["agent"]["content"] = "x" * 300_000
        with self.peer("opencode") as peer:
            peer.initialize({"opencode_path": str(fake)})
            output, response = peer.request(self.params)
        self.assertEqual(json.loads(output)["context"], "x" * 300_000)
        self.assertEqual(response["result"]["status"], "completed")

    def test_opencode_cli_versions(self):
        fake = self.base / "fake-opencode"
        shutil.copyfile(ROOT / "tests/fake_opencode.py", fake)
        fake.chmod(0o755)
        for version, standalone in (("1.19.3", False), ("opencode v2.3.0", True)):
            with self.subTest(version=version):
                self.environment["PROMPTR_TEST_OPENCODE_VERSION"] = version
                with self.peer("opencode") as peer:
                    peer.initialize({"opencode_path": str(fake)})
                    output, response = peer.request(self.params)
                self.assertEqual(json.loads(output)["standalone"], standalone)
                self.assertEqual(response["result"]["status"], "completed")

    def test_opencode_invalid_version(self):
        fake = self.base / "fake-opencode"
        shutil.copyfile(ROOT / "tests/fake_opencode.py", fake)
        fake.chmod(0o755)
        self.environment["PROMPTR_TEST_OPENCODE_VERSION"] = "unrecognized CLI"
        with self.peer("opencode") as peer:
            peer.initialize({"opencode_path": str(fake)})
            output, response = peer.request(self.params)
        self.assertEqual(output, "")
        self.assertIn("unrecognized version", response["error"]["message"])

    def test_opencode_version_deadline(self):
        fake = self.base / "hung-opencode"
        fake.write_text('#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n')
        fake.chmod(0o755)
        started = time.monotonic()
        with self.peer("opencode") as peer:
            peer.initialize({"opencode_path": str(fake)})
            output, response = peer.request(self.params)
        self.assertEqual(output, "")
        self.assertIn("version check timed out", response["error"]["message"])
        self.assertLess(time.monotonic() - started, 8)

    def test_opencode_installed_cli(self):
        executable = shutil.which("opencode")
        if executable is None:
            self.skipTest("OpenCode CLI is not installed")
        self.environment.update({
            "OPENCODE_DISABLE_MODELS_FETCH": "1",
            "OPENCODE_DISABLE_AUTOUPDATE": "1",
            "XDG_STATE_HOME": str(self.base / "state"),
            "OPENCODE_PASSWORD": "fixture-password",
        })
        for key in ("OPENCODE_CONFIG", "OPENCODE_CONFIG_CONTENT", "OPENCODE_CONFIG_DIR", "OPENCODE_DB"):
            self.environment.pop(key, None)
        configuration = self.base / "config/opencode/opencode.json"
        configuration.parent.mkdir(parents=True)
        with self.http_backend() as settings:
            configuration.write_text(json.dumps({
                "enabled_providers": ["fixture"],
                "provider": {"fixture": {
                    "npm": "@ai-sdk/openai-compatible", "name": "Offline fixture",
                    "options": {"baseURL": settings["base_url"], "apiKey": "fake-token"},
                    "models": {"chat": {"name": "Fixture chat", "tool_call": False,
                                         "limit": {"context": 8192, "output": 256}}},
                }},
                "model": "fixture/chat", "small_model": "fixture/chat",
                "share": "disabled", "autoupdate": False,
            }))
            self.params["model"] = "fixture/chat"
            self.params["connection_test"] = True
            self.params["project_dir"] = str(self.base)
            version = subprocess.check_output([executable, "--version"], text=True, timeout=5)
            shared_service = version.startswith("opencode v2.")
            if shared_service:
                with self.opencode_server(executable) as server_url:
                    # Route shared-mode runs to a server without Promptr's agent.
                    wrapper = self.base / "opencode-client"
                    wrapper.write_text(
                        '#!/usr/bin/env python3\nimport os, sys\n'
                        f'executable = {executable!r}\nargs = sys.argv[1:]\n'
                        'if args[0] == "run" and "--standalone" not in args:\n'
                        f'    args[1:1] = ["--server", {server_url!r}]\n'
                        'os.execv(executable, [executable, *args])\n'
                    )
                    wrapper.chmod(0o755)
                    with self.peer("opencode") as peer:
                        peer.initialize({"opencode_path": str(wrapper)})
                        output, response = peer.request(self.params)
            else:
                with self.peer("opencode") as peer:
                    peer.initialize({"opencode_path": executable})
                    output, response = peer.request(self.params)
            self.assertIn("result", response, response)
            self.assertEqual(output, "pong")
            self.assertTrue(any(
                self.params["agent"]["content"] in json.dumps(body["messages"])
                for body, _auth in Backend.received
            ), "OpenCode did not send the configured context to the model")

    def test_opencode_connection_deadline(self):
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "gio-2.0", "json-glib-1.0"], text=True)
        executable = self.base / "opencode-short-deadline"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Og",
            "-DPROMPTR_OPENCODE_TEST_SECONDS=1", "providers/promptr-opencode/main.c",
            "lib/promptr-protocol/jsonrpc.c", "lib/promptr-protocol/provider-server.c",
            *shlex.split(flags), "-o", str(executable),
        ], cwd=ROOT, check=True)
        slow = self.base / "slow-opencode"
        slow.write_text("#!/usr/bin/env python3\nimport sys, time\nif '--version' in sys.argv: print('1.19.3'); sys.exit(0)\nsys.stderr.write('Backend is retrying an unavailable model.\\n')\nsys.stderr.flush()\ntime.sleep(60)\n")
        slow.chmod(0o755)
        self.params["connection_test"] = True
        peer = Peer(executable, self.environment)
        started = time.monotonic()
        try:
            peer.initialize({"opencode_path": str(slow)})
            _output, response = peer.request(self.params)
        finally:
            peer.close()
        self.assertLess(time.monotonic() - started, 4)
        self.assertIn("within 1 seconds", response["error"]["message"])
        self.assertIn("unavailable model", response["error"]["message"])
        # Normal replies have no connection-test deadline, and errors from a
        # background title model must not cancel the actual reply.
        ordinary = self.base / "ordinary-opencode"
        ordinary.write_text(
            '#!/usr/bin/env python3\nimport json, sys, time\n'
            "if '--version' in sys.argv: print('1.19.3'); sys.exit(0)\n"
            'sys.stderr.write(\'level=ERROR small=true error.error="AI_APICallError: Free usage limit reached."\\n\')\n'
            'sys.stderr.flush()\ntime.sleep(1.2)\n'
            'print(json.dumps({"type":"text","part":{"text":"pong"}}), flush=True)\n'
        )
        ordinary.chmod(0o755)
        self.params["connection_test"] = False
        peer = Peer(executable, self.environment)
        try:
            peer.initialize({"opencode_path": str(ordinary)})
            output, response = peer.request(self.params)
        finally:
            peer.close()
        self.assertEqual(output, "pong")
        self.assertEqual(response["result"]["status"], "completed")

    def test_opencode_oversized_unterminated_event(self):
        fake = self.base / "oversized-opencode"
        fake.write_text(
            '#!/usr/bin/env python3\nimport sys\n'
            "if '--version' in sys.argv: print('1.19.3'); sys.exit(0)\n"
            'block = b"x" * 65536\n'
            'for _ in range(1025):\n    sys.stdout.buffer.write(block)\n'
            'sys.stdout.buffer.flush()\n'
        )
        fake.chmod(0o755)
        with self.peer("opencode") as peer:
            peer.initialize({"opencode_path": str(fake)})
            _output, response = peer.request(self.params)
        self.assertIn("exceeds limit", response["error"]["message"])

    def test_opencode_stderr_error_details(self):
        failing = self.base / "failing-opencode"
        failing.write_text("#!/usr/bin/env python3\nimport sys\nif '--version' in sys.argv: print('1.19.3'); sys.exit(0)\nsys.stderr.write('startup noise\\n' * 8000)\nsys.stderr.write('Authorization: Bearer fixture-private-key\\napi_key=fixture-other-private-key\\nNot authenticated: log in to the selected backend.\\n')\nsys.exit(2)\n")
        failing.chmod(0o755)
        self.params["connection_test"] = True
        with self.peer("opencode") as peer:
            peer.initialize({"opencode_path": str(failing)})
            _output, response = peer.request(self.params)
        self.assertIn("Not authenticated", response["error"]["message"])
        self.assertNotIn("fixture-private-key", response["error"]["message"])
        self.assertNotIn("fixture-other-private-key", response["error"]["message"])
        self.assertLess(len(response["error"]["message"]), 5000)

    def test_opencode_quota_error_stops_retries(self):
        retrying = self.base / "retrying-opencode"
        retrying.write_text(
            '#!/usr/bin/env python3\nimport sys, time\n'
            "if '--version' in sys.argv: print('1.19.3'); sys.exit(0)\n"
            'sys.stderr.write(\'level=ERROR message="stream error" error.error="AI_APICallError: Free usage limit reached. Try another model."\\n\')\n'
            'sys.stderr.flush()\ntime.sleep(60)\n'
        )
        retrying.chmod(0o755)
        self.params["connection_test"] = True
        started = time.monotonic()
        with self.peer("opencode") as peer:
            peer.initialize({"opencode_path": str(retrying)})
            _output, response = peer.request(self.params)
        self.assertLess(time.monotonic() - started, 3)
        self.assertTrue(response["error"]["message"].startswith("OpenCode: Free usage limit reached"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
