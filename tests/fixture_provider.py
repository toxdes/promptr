#!/usr/bin/env python3
"""Offline protocol peer with deterministic failure modes for lifecycle tests."""

import json
import os
import signal
import subprocess
import sys
import threading
import time

lock = threading.Lock()
cancelled = threading.Event()
mode = sys.argv[1] if len(sys.argv) > 1 else "normal"
if mode == "ignore_cancel":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)


def send(message):
    with lock:
        print(json.dumps({"jsonrpc": "2.0", **message}), flush=True)


def reply(request, result):
    send({"id": request["id"], "result": result})


def run_request(request):
    params = request["params"]
    ids = {key: params[key] for key in ("thread_id", "request_id")}

    def emit(text, **overrides):
        send({"method": "provider/event", "params": {
            **ids, "type": "delta", "output": text, **overrides,
        }})

    if mode == "ignore_cancel":
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        emit(f"child:{child.pid}")
        while True:
            time.sleep(1)
    if mode == "exit":
        os._exit(7)
    if mode == "stall_reply":
        if cancelled.wait(60):
            reply(request, {**ids, "status": "cancelled"})
        return
    if mode == "malformed":
        with lock:
            print("[]", flush=True)
        return
    if mode == "long_error":
        details = "Usage exceeded\n" + "\n".join(
            f"Backend diagnostic line {number}: try another model." for number in range(1, 51)
        )
        send({"id": request["id"], "error": {"code": -32010, "message": details}})
        return
    if mode == "reject":
        send({"id": request["id"], "error": {"code": -32010, "message": "backend unavailable"}})
        return
    if mode == "secret_error":
        send({"id": request["id"], "error": {"code": -32010, "message": settings["api_key"]}})
        return
    if mode == "late":
        emit("WRONG THREAD", thread_id="another-thread")
        emit("WRONG REQUEST", request_id="previous-request")
    if mode == "oversize":
        emit("x" * (65 * 1024))
    elif mode == "echo":
        emit(json.dumps(params, sort_keys=True))
    elif mode == "stream":
        for _ in range(50):
            if cancelled.wait(0.02):
                break
            emit("partial ")
    elif mode != "empty":
        emit("pong")
    reply(request, {**ids, "status": "cancelled" if cancelled.is_set() else "completed",
                    "resume_state": "opaque-state"})


settings = {}
for line in sys.stdin:
    request = json.loads(line)
    method = request["method"]
    if method == "provider/initialize":
        if mode == "stall":
            time.sleep(60)
        settings = request["params"]["config"]
        reply(request, {"protocol_version": "1.0" if mode == "incompatible" else "2.0",
                        "capabilities": {"streaming": True, "models": True}})
    elif method == "provider/submit":
        threading.Thread(target=run_request, args=(request,), daemon=True).start()
    elif method == "provider/models":
        reply(request, {"models": [] if mode == "no_models" else ["fixture/first", "fixture/second"]})
    elif method == "provider/cancel":
        if mode != "ignore_cancel":
            cancelled.set()
        reply(request, {"ok": True})
    elif method == "provider/shutdown":
        break

if mode == "ignore_cancel":
    while True:
        time.sleep(1)
