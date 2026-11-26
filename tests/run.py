#!/usr/bin/env python3
"""Build and run offline architecture checks with isolated data and credentials."""

from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="Enable address and undefined-behavior sanitizers")
    options = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    sources = [
        "tests/test_core.c", "tests/fake_keyring.c", "src/app-diagnostics.c", "src/app-menu.c", "src/shortcuts.c", "src/configfile.c", "src/thread-store.c", "src/plugin-runner.c",
        "src/provider-profile.c", "src/plugin-manager.c", "src/agent-context.c",
        "lib/promptr-protocol/jsonrpc.c",
    ]
    flags = subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "gio-2.0", "json-glib-1.0", "libsecret-1"], text=True,
    )
    with tempfile.TemporaryDirectory(prefix="promptr-tests-") as temporary:
        base = Path(temporary)
        binary = base / "test-core"
        sanitizer_flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if options.sanitize else []
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Og", "-ggdb3",
            "-I.", "-Isrc", "-DDEBUG_BUILD", "-DPROMPTR_STARTUP_SECONDS=2", "-DPROMPTR_TEST_SECONDS=2", "-DPROMPTR_CANCEL_SECONDS=1", "-DPROMPTR_SHUTDOWN_SECONDS=1", *sanitizer_flags, *sources, *shlex.split(flags),
            "-Wl,--wrap=secret_password_store_sync", "-Wl,--wrap=secret_password_lookup_sync", "-Wl,--wrap=secret_password_clear_sync", "-o", str(binary),
        ], cwd=root, check=True)
        environment = {
            **os.environ,
            "XDG_CONFIG_HOME": str(base / "config"),
            "XDG_DATA_HOME": str(base / "data"),
            "XDG_CACHE_HOME": str(base / "cache"),
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent/promptr-test-bus",
            "PROMPTR_PLUGIN_DIR": str(root / "providers"),
            "PROMPTR_TEST_FIXTURE": str(root / "tests/fixture_provider.py"),
        }
        environment.pop("OPENROUTER_API_KEY", None)
        subprocess.run([str(binary)], cwd=root, env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
