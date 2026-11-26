#!/usr/bin/env python3
"""Stage the provider files and their libraries before Yesb builds the AppImage."""

from pathlib import Path
import shutil
import subprocess
import sys
import tomllib


ROOT = Path(__file__).resolve().parent.parent
PLUGIN_DIR = Path("/usr/lib/promptr/plugins")
# Match the runtime exclusions used by Yesb for the main executable.
SYSTEM_LIBRARIES = (
    "ld-linux", "libc.so", "libm.so", "libpthread", "libdl.so", "libstdc++.so",
    "libgcc_s.so", "libresolv.so", "librt.so", "libutil.so", "libnss_", "libnsl",
)


def stage_providers(appdir: Path) -> None:
    manifest = tomllib.loads((ROOT / "release.toml").read_text())
    libraries = set()
    for asset in manifest["package"]["archive"]["assets"]:
        target = Path(asset["dest"])
        if not target.is_relative_to(PLUGIN_DIR):
            continue
        source = ROOT / asset["source"]
        destination = appdir / target.relative_to("/")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        destination.chmod(asset["mode"])
        if asset["mode"] & 0o111:
            dependencies = subprocess.check_output(["ldd", source], text=True)
            for line in dependencies.splitlines():
                name, divider, location = line.strip().partition(" => ")
                if not divider:
                    continue
                if location.startswith("not found"):
                    raise SystemExit(f"{source.name}: {name} was not found")
                library = Path(location.split()[0])
                if library.is_absolute() and not library.name.startswith(SYSTEM_LIBRARIES):
                    libraries.add(library)

    libdir = appdir / "usr/lib"
    for library in sorted(libraries):
        shutil.copy2(library, libdir / library.name)


if __name__ == "__main__":
    stage_providers(Path(sys.argv[1]))
