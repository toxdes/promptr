#!/usr/bin/env python3
"""Build .deb, .rpm, and optionally .AppImage packages for promptr."""

import os
import subprocess
import shutil
from pathlib import Path

VERSION = Path("/build/VERSION").read_text().strip()
ARCH = os.environ.get("TARGETARCH", "amd64")
INCLUDE_APPIMAGE = os.environ.get("INCLUDE_APPIMAGE", "")
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
Maintainer: toxdes <hi@toxdes.com>
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


# ── .AppImage ─────────────────────────────────────────────────
def build_appimage():
    runtime = Path("/usr/local/share/appimage-runtime")
    if not runtime.exists():
        print("  (skip AppImage: runtime not found)")
        return

    SKIP_LIBS = {
        "ld-linux", "libc.so", "libm.so", "libpthread", "libdl.so",
        "libstdc++.so", "libgcc_s.so", "libresolv.so", "librt.so",
        "libutil.so", "libnss_", "libnsl",
    }

    name = f"promptr-{VERSION}-{ARCH}.AppImage"
    appdir = Path("/tmp/appdir")

    (appdir / "usr/bin").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/lib").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/share/glib-2.0/schemas").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/share/icons").mkdir(parents=True, exist_ok=True)

    shutil.copy("/build/promptr", appdir / "usr/bin/promptr")
    (appdir / "usr/bin/promptr").chmod(0o755)

    shutil.copy("/build/data/promptr.svg", appdir / "promptr.svg")
    shutil.copy("/build/data/promptr.svg", appdir / ".DirIcon")
    shutil.copy(
        "/build/com.toxdes.promptr.desktop",
        appdir / "com.toxdes.promptr.desktop",
    )

    result = subprocess.run(
        ["ldd", "/build/promptr"], capture_output=True, text=True
    )
    for line in result.stdout.splitlines():
        parts = line.strip().split()
        if "=>" in parts:
            idx = parts.index("=>")
            if idx + 1 < len(parts):
                libpath = parts[idx + 1]
                libname = Path(libpath).name
                skip = False
                for s in SKIP_LIBS:
                    if libname.startswith(s):
                        skip = True
                        break
                if not skip and libpath.startswith("/") and Path(libpath).exists():
                    shutil.copy(libpath, appdir / "usr/lib/")

    schemas_src = Path("/usr/share/glib-2.0/schemas")
    if schemas_src.exists():
        for f in schemas_src.glob("org.gtk*"):
            shutil.copy(f, appdir / "usr/share/glib-2.0/schemas/")
        for f in schemas_src.glob("gschemas.compiled"):
            shutil.copy(f, appdir / "usr/share/glib-2.0/schemas/")
    subprocess.run(
        ["glib-compile-schemas", str(appdir / "usr/share/glib-2.0/schemas")],
        check=False,
    )

    apprun = """\
#!/bin/bash
APPDIR="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="${APPDIR}/usr/lib:${LD_LIBRARY_PATH}"
export GSETTINGS_SCHEMA_DIR="${APPDIR}/usr/share/glib-2.0/schemas"
if [ -n "$WAYLAND_DISPLAY" ]; then
    export GDK_BACKEND=wayland
else
    export GDK_BACKEND=x11
fi
exec "${APPDIR}/usr/bin/promptr" "$@"
"""
    (appdir / "AppRun").write_text(apprun)
    (appdir / "AppRun").chmod(0o755)

    squashed = Path("/tmp/promptr.squashfs")
    subprocess.run(
        ["mksquashfs", str(appdir), str(squashed), "-noappend"], check=True
    )

    dest = OUT / name
    with open(dest, "wb") as out:
        out.write(runtime.read_bytes())
        out.write(squashed.read_bytes())
    dest.chmod(0o755)

    print(f"  -> {dest}")
    shutil.rmtree(appdir)
    squashed.unlink()


# ── main ──────────────────────────────────────────────────────
def main():
    OUT.mkdir(parents=True, exist_ok=True)
    build_deb()
    build_rpm()
    if INCLUDE_APPIMAGE:
        print("Including AppImage...")
        build_appimage()
    print("Done.")


if __name__ == "__main__":
    main()
