#!/usr/bin/env python3
"""Build promptr binaries and packages for all platforms using Docker buildx.

Usage:
    python3 build-all.py

Prerequisites:
    - Docker with buildx support (docker buildx create --use)
    - QEMU for cross-platform emulation (docker run --rm --privileged multiarch/qemu-user-static --reset -p yes)

Output:
    dist/  — packages and PKGBUILD for all platforms
"""

import subprocess
import sys
import shutil
from pathlib import Path

VERSION = Path("VERSION").read_text().strip()
DIST = Path("dist")
PLATFORMS = "linux/amd64,linux/arm64"


def run(cmd, **kwargs):
    label = f"  \033[1;36m$\033[m {cmd}"
    print(label)
    subprocess.run(cmd, shell=True, check=True, **kwargs)


def main():
    # Clean output
    if DIST.exists():
        shutil.rmtree(DIST)
    DIST.mkdir()

    # Check Docker
    try:
        run("docker version --format '{{.Server.Version}}'",
            capture_output=True)
    except subprocess.CalledProcessError:
        print("Error: Docker not available")
        sys.exit(1)

    # Ensure QEMU multiarch support
    run("docker run --rm --privileged multiarch/qemu-user-static"
        " --reset -p yes 2>/dev/null; true",
        capture_output=True)

    # Build for all platforms
    print("\nBuilding for %s ...\n" % PLATFORMS.replace(",", ", "))
    run("docker buildx build"
        " --platform " + PLATFORMS +
        " --output type=local,dest=" + str(DIST) +
        " --progress=plain"
        " -f Dockerfile .")

    # Flatten: buildx creates subdirs per platform, move files up
    for pkg_dir in sorted(DIST.iterdir()):
        if pkg_dir.is_dir() and pkg_dir.name != ".git":
            for f in pkg_dir.iterdir():
                if f.is_file():
                    shutil.move(str(f), str(DIST / f.name))
            pkg_dir.rmdir()

    # Copy PKGBUILD
    pkgbuild = Path("PKGBUILD")
    if pkgbuild.exists():
        shutil.copy(str(pkgbuild), str(DIST / "PKGBUILD"))

    # Summary
    print("\nPackages (%s):" % DIST)
    for f in sorted(DIST.iterdir()):
        if f.is_file():
            size = f.stat().st_size
            print(f"  {f.name:<45s}  {size:>8,} bytes")


if __name__ == "__main__":
    sys.exit(main())
