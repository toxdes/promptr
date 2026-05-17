#!/usr/bin/env python3
"""Build .deb and .rpm packages for promptr."""

import os
import subprocess
import shutil
from pathlib import Path

VERSION = Path("/build/VERSION").read_text().strip()
ARCH = os.environ.get("TARGETARCH", "amd64")
OUT = Path("/output")

# ── arch mapping ──────────────────────────────────────────────
ARCH_MAP = {
    "amd64": {"deb": "amd64", "rpm": "x86_64"},
    "arm64": {"deb": "arm64", "rpm": "aarch64"},
}
if ARCH not in ARCH_MAP:
    print(f"Unknown arch: {ARCH}")
    exit(1)

deb_arch = ARCH_MAP[ARCH]["deb"]
rpm_arch = ARCH_MAP[ARCH]["rpm"]


# ── .deb ──────────────────────────────────────────────────────
def build_deb():
    name = f"promptr_{VERSION}_{deb_arch}"
    pkg = Path("/pkg-deb")

    dirs = [
        pkg / "DEBIAN",
        pkg / "usr/bin",
        pkg / "usr/share/icons/hicolor/scalable/apps",
        pkg / "usr/share/applications",
    ]
    for d in dirs:
        d.mkdir(parents=True, exist_ok=True)

    shutil.copy("/build/promptr", pkg / "usr/bin/promptr")
    (pkg / "usr/bin/promptr").chmod(0o755)
    shutil.copy(
        "/build/data/promptr.svg",
        pkg / "usr/share/icons/hicolor/scalable/apps/promptr.svg",
    )
    shutil.copy(
        "/build/com.toxdes.promptr.desktop",
        pkg / "usr/share/applications/com.toxdes.promptr.desktop",
    )

    control = f"""\
Package: promptr
Version: {VERSION}
Architecture: {deb_arch}
Maintainer: toxdes <toxdes@proton.me>
Section: utils
Priority: optional
Depends: libgtk-4-1, libgtksourceview-5-0, libgtk4-layer-shell0
Description: GTK4 overlay prompt for opencode
 promptr is a GTK4 overlay application for running opencode
 commands through a resizable, always-on-top window.
"""
    (pkg / "DEBIAN/control").write_text(control)

    subprocess.run(
        ["dpkg-deb", "--build", str(pkg), str(OUT / f"{name}.deb")], check=True
    )
    print(f"  -> {OUT}/{name}.deb")
    shutil.rmtree(pkg)


# ── .rpm ──────────────────────────────────────────────────────
def build_rpm():
    name = f"promptr-{VERSION}-1.{rpm_arch}"
    topdir = Path("/tmp/rpm")

    for d in ["BUILD", "RPMS", "SOURCES", "SPECS", "SRPMS"]:
        (topdir / d).mkdir(parents=True, exist_ok=True)

    spec = f"""\
Name:           promptr
Version:        {VERSION}
Release:        1%{{?dist}}
Summary:        GTK4 overlay prompt for opencode
License:        MIT
BuildArch:      {rpm_arch}
Requires:       gtk4
Requires:       gtksourceview5
Requires:       gtk4-layer-shell

%description
promptr is a GTK4 overlay application for running opencode
commands through a resizable, always-on-top window.

%install
mkdir -p %{{buildroot}}/usr/bin \\
         %{{buildroot}}/usr/share/icons/hicolor/scalable/apps \\
         %{{buildroot}}/usr/share/applications
install -m755 /build/promptr %{{buildroot}}/usr/bin/promptr
install -m644 /build/data/promptr.svg \\
    %{{buildroot}}/usr/share/icons/hicolor/scalable/apps/promptr.svg
install -m644 /build/com.toxdes.promptr.desktop \\
    %{{buildroot}}/usr/share/applications/com.toxdes.promptr.desktop

%files
/usr/bin/promptr
/usr/share/icons/hicolor/scalable/apps/promptr.svg
/usr/share/applications/com.toxdes.promptr.desktop
"""
    (topdir / "SPECS/promptr.spec").write_text(spec)

    subprocess.run(
        [
            "rpmbuild",
            "-bb",
            "--define",
            f"_topdir {topdir}",
            str(topdir / "SPECS/promptr.spec"),
        ],
        check=True,
    )

    src = topdir / "RPMS" / rpm_arch / f"{name}.rpm"
    shutil.copy(str(src), str(OUT / f"{name}.rpm"))
    print(f"  -> {OUT}/{name}.rpm")
    shutil.rmtree(topdir)


# ── main ──────────────────────────────────────────────────────
def main():
    OUT.mkdir(parents=True, exist_ok=True)
    build_deb()
    build_rpm()
    print("Done.")


if __name__ == "__main__":
    main()
