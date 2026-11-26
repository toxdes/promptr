#!/usr/bin/env python3
"""Compare generated preference defaults with the template without launching GTK."""

from pathlib import Path
import difflib
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True)
    with tempfile.TemporaryDirectory(prefix="promptr-config-check-") as temporary:
        executable = Path(temporary) / "config-defaults"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Og", "-I.",
            "tests/config_defaults.c", "src/configfile.c", *shlex.split(flags), "-o", str(executable),
        ], cwd=root, check=True)
        generated = subprocess.check_output([str(executable)], text=True)
    template = (root / "config").read_text()
    if generated != template:
        print("".join(difflib.unified_diff(
            template.splitlines(keepends=True), generated.splitlines(keepends=True),
            fromfile="config", tofile="generated defaults",
        )), end="")
        raise SystemExit(1)
    print("Preference defaults match the config template.")


if __name__ == "__main__":
    main()
