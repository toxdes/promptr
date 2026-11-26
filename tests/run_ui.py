#!/usr/bin/env python3
"""Compile native UI checks; --run launches isolated GTK windows after user approval."""

from pathlib import Path
import argparse
import os
import shutil
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="Launch the UI checks; requires explicit user permission")
    parser.add_argument("--artifacts", type=Path, default=Path("/tmp/promptr-ui-review"))
    parser.add_argument("--scenario", choices=("all", "last-tab", "audit", "model-selection"), default="all")
    options = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    flags = subprocess.check_output([
        "pkg-config", "--cflags", "--libs", "gtk4", "gtksourceview-5", "gtk4-layer-shell-0", "json-glib-1.0", "libsecret-1",
    ], text=True)
    sources = [str(path.relative_to(root)) for path in (root / "src").glob("*.c") if path.name != "main.c"]
    with tempfile.TemporaryDirectory(prefix="promptr-ui-tests-") as temporary:
        base = Path(temporary)
        executable = base / "test-ui"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Og", "-ggdb3",
            "-I.", "-Isrc", "-DDEBUG_BUILD", "tests/test_ui.c", *sources,
            "lib/promptr-protocol/jsonrpc.c", *shlex.split(flags), "-o", str(executable),
        ], cwd=root, check=True)
        if not options.run:
            print("Native UI harness compiled. No windows were launched. Use --run only after approval.")
            return
        options.artifacts.mkdir(parents=True, exist_ok=True)
        plugins = base / "plugins"
        for name in ("openrouter", "opencode"):
            plugin = plugins / name
            plugin.mkdir(parents=True)
            shutil.copyfile(root / f"providers/promptr-{name}/plugin.json", plugin / "plugin.json")
            fixture = plugin / f"promptr-{name}"
            shutil.copyfile(root / "tests/fixture_provider.py", fixture)
            fixture.chmod(0o755)
        environment = {
            **os.environ, "XDG_CONFIG_HOME": str(base / "config"),
            "XDG_DATA_HOME": str(base / "data"), "XDG_CACHE_HOME": str(base / "cache"),
            "PROMPTR_PLUGIN_DIR": str(plugins),
            "PROMPTR_TEST_FIXTURE": str(root / "tests/fixture_provider.py"),
            "GSK_RENDERER": "cairo",
            "PROMPTR_UI_SCENARIO": options.scenario,
        }
        environment.pop("OPENROUTER_API_KEY", None)
        result = subprocess.run([str(executable), str(options.artifacts.resolve())], cwd=root,
                                env=environment, text=True, capture_output=True,
                                timeout=20 if options.scenario == "model-selection" else 90)
        (options.artifacts / "ui-checks.log").write_text(result.stdout + result.stderr)
        print(result.stdout, end="")
        if result.returncode:
            print(result.stderr, end="")
            raise SystemExit(result.returncode)
        print(f"UI checks passed; screenshots and measurements: {options.artifacts}")


if __name__ == "__main__":
    main()
