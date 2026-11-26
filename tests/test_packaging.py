#!/usr/bin/env python3
"""Validate provider payloads and discovery in an isolated, relocated installation."""

from pathlib import Path
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import tomllib


def main():
    root = Path(__file__).resolve().parent.parent
    configuration = tomllib.loads((root / "release.toml").read_text())
    required = {
        f"/usr/lib/promptr/plugins/{name}/{filename}"
        for name in ("opencode", "openrouter")
        for filename in (f"promptr-{name}", "plugin.json")
    }
    for kind in ("deb", "rpm", "archive"):
        assets = configuration["package"][kind]["assets"]
        assert required <= {item["dest"] for item in assets}, kind
        for item in assets:
            assert (root / item["source"]).is_file(), item
            if item["dest"] in required:
                expected = 0o644 if item["dest"].endswith(".json") else 0o755
                assert item["mode"] == expected, item
    flags = subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "gio-2.0", "json-glib-1.0"], text=True,
    )
    with tempfile.TemporaryDirectory(prefix="promptr-package-") as temporary:
        stage = Path(temporary) / "relocated/usr"
        (stage / "bin").mkdir(parents=True)
        for asset in configuration["package"]["archive"]["assets"]:
            destination = stage.parent / asset["dest"].lstrip("/")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / asset["source"], destination)
            destination.chmod(asset["mode"])
        # Compile a display-independent discovery probe into the relocated bin directory.
        invalid = stage / "lib/promptr/plugins/bad-secret-schema"
        invalid.mkdir()
        (invalid / "plugin.json").write_text(json.dumps({
            "name": "bad-secret-schema", "command": "/bin/false", "manifest_version": 1,
            "protocol_versions": ["2.0"], "config_schema": {
                "api_key": {"type": "string", "secret": "yes"},
            },
        }))
        probe = stage / "bin/probe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Og", "-I.",
            "tests/test_discovery.c", "src/plugin-manager.c", "lib/promptr-protocol/jsonrpc.c",
            *shlex.split(flags), "-o", str(probe),
        ], cwd=root, check=True)
        environment = {**os.environ, "XDG_CONFIG_HOME": str(Path(temporary) / "config")}
        environment.pop("PROMPTR_PLUGIN_DIR", None)
        subprocess.run([str(probe), str(stage)], cwd="/tmp", env=environment, check=True)
        for name in ("opencode", "openrouter"):
            manifest = json.loads((stage / f"lib/promptr/plugins/{name}/plugin.json").read_text())
            assert manifest["manifest_version"] == 1
            assert manifest["protocol_versions"] == ["2.0"]
        appdir = Path(temporary) / "appdir"
        subprocess.run([
            sys.executable, str(root / "scripts/stage-appimage-providers.py"), str(appdir),
        ], check=True)
        appimage_probe = appdir / "usr/bin/probe"
        appimage_probe.parent.mkdir()
        shutil.copy2(probe, appimage_probe)
        subprocess.run([str(appimage_probe), str(appdir / "usr")], cwd="/tmp",
                       env=environment, check=True)
        assert (appdir / "usr/lib/libcurl.so.4").is_file()
        print("Package assets, AppImage staging and relocated provider discovery passed.")


if __name__ == "__main__":
    main()
