#!/usr/bin/env python3
"""Push PKGBUILD + .SRCINFO to AUR for promptr-git and promptr-bin.

Usage:
    python3 release-aur.py --type git     # initial submit, then only on dep changes
    python3 release-aur.py --type bin     # every release
    python3 release-aur.py --type both
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
AUR_HOST = "aur.archlinux.org"
AUR_SSH = f"aur@{AUR_HOST}"
MAINTAINER = "toxdes <hi@toxdes.com>"
GITHUB = "https://github.com/toxdes/promptr"
VERSION = (ROOT / "VERSION").read_text().strip()

PKGBUILD_GIT = ROOT / "PKGBUILD"

BIN_PKGBUILD_TEMPLATE = """\
# Maintainer: {maintainer}
pkgname=promptr-bin
pkgver={version}
pkgrel=1
pkgdesc="GTK4 overlay prompt for opencode"
arch=('x86_64' 'aarch64')
url="{url}"
license=('MIT')
depends=('gtk4' 'gtksourceview5' 'gtk4-layer-shell')

source_x86_64=("{deb_name}::https://github.com/toxdes/promptr/releases/download/v${{pkgver}}/promptr_${{pkgver}}_amd64.deb")
sha256sums_x86_64=('{sha256_amd64}')

source_aarch64=("{deb_name}::https://github.com/toxdes/promptr/releases/download/v${{pkgver}}/promptr_${{pkgver}}_arm64.deb")
sha256sums_aarch64=('{sha256_arm64}')

package() {{
  bsdtar -xOf "${{srcdir}}/promptr-${{pkgver}}-${{CARCH}}.deb" data.tar.xz \\
    | tar -x -C "${{pkgdir}}"
}}
"""


def run(cmd, **kwargs):
    subprocess.run(cmd, shell=True, check=True, **kwargs)


def parse_bash_array(val):
    """Parse a bash array value like ('a' 'b') and return a list of strings.
    Returns None if not an array."""
    val = val.strip()
    if not val.startswith("(") or not val.endswith(")"):
        return None
    inner = val[1:-1]
    parts = []
    current = ""
    in_quote = False
    for ch in inner:
        if ch == "'" or ch == '"':
            in_quote = not in_quote
        elif ch.isspace() and not in_quote:
            if current:
                parts.append(current)
                current = ""
        else:
            current += ch
    if current:
        parts.append(current)
    return parts or None


def strip_quotes(val):
    """Remove surrounding quotes from a value."""
    val = val.strip()
    if (val.startswith('"') and val.endswith('"')) or \
       (val.startswith("'") and val.endswith("'")):
        return val[1:-1]
    return val


def generate_srcinfo(pkgbuild_text):
    """Generate .SRCINFO content from PKGBUILD text."""
    pkgbase = None
    pkgname = None
    lines_out = []
    in_func = 0  # brace depth

    for line in pkgbuild_text.splitlines():
        stripped = line.strip()

        if stripped.startswith("#"):
            continue
        if not stripped:
            continue

        # Track function body depth
        if "()" in stripped and "{" in stripped:
            in_func = 1
            continue
        if in_func > 0:
            in_func += stripped.count("{") - stripped.count("}")
            continue

        # Skip .SRCINFO-incompatible fields
        if stripped.startswith("source_") or \
           stripped.startswith("sha256sums_"):
            continue

        if "=" in stripped:
            key, val = stripped.split("=", 1)
            key = key.strip()
            val = val.strip()

            # Track pkgname/pkgbase
            if key == "pkgname":
                pkgname = val
                if pkgbase is None:
                    pkgbase = val
                continue

            # Parse arrays: arch, depends, makedepends, license
            arr = parse_bash_array(val)
            if arr:
                for item in arr:
                    lines_out.append(f"\t{key} = {strip_quotes(item)}")
            else:
                lines_out.append(f"\t{key} = {strip_quotes(val)}")

    header = f"pkgbase = {pkgbase or 'promptr-git'}\n"
    lines_out.append(f"\npkgname = {pkgname or 'promptr-git'}")
    return header + "\n".join(lines_out) + "\n"


def clone_or_pull(repo_name, workdir):
    """Clone AUR repo or pull if exists."""
    repo_path = workdir / repo_name
    aur_url = f"{AUR_SSH}:{repo_name}.git"

    if (repo_path / ".git").exists():
        run(f"git -C {repo_path} pull --rebase", cwd=workdir)
    else:
        run(f"git clone {aur_url} {repo_path}", cwd=workdir)
    return repo_path


def push_aur(repo_name, pkgbuild_text):
    """Copy files into AUR clone and push."""
    with tempfile.TemporaryDirectory(prefix="aur-") as tmp:
        workdir = Path(tmp)
        repo = clone_or_pull(repo_name, workdir)

        pkgbuild_text = pkgbuild_text.rstrip("\n") + "\n"
        srcinfo = generate_srcinfo(pkgbuild_text)

        old_pkg = (repo / "PKGBUILD").read_text() if (repo / "PKGBUILD").exists() else ""
        old_src = (repo / ".SRCINFO").read_text() if (repo / ".SRCINFO").exists() else ""

        if old_pkg.rstrip() == pkgbuild_text.rstrip() and old_src.rstrip() == srcinfo.rstrip():
            print(f"  -> AUR {repo_name}: no changes, skipping")
            return

        (repo / "PKGBUILD").write_text(pkgbuild_text)
        (repo / ".SRCINFO").write_text(srcinfo)

        run(f'git -C {repo} add PKGBUILD .SRCINFO')
        subprocess.run(
            f'git -C {repo} commit --author "{MAINTAINER}" '
            f'-m "Release {VERSION}"',
            shell=True,
        )
        run(f"git -C {repo} push -u origin HEAD:master")


def fetch_checksum(url):
    """Download a URL and return its SHA256 hex digest."""
    import urllib.request

    sha = hashlib.sha256()
    with urllib.request.urlopen(url) as resp:
        while True:
            chunk = resp.read(65536)
            if not chunk:
                break
            sha.update(chunk)
    return sha.hexdigest()


def release_git():
    """Push promptr-git to AUR using the root PKGBUILD."""
    pkgbuild = PKGBUILD_GIT.read_text()
    pkgbuild = re.sub(r'^pkgver=.*$', f'pkgver={VERSION}', pkgbuild, flags=re.MULTILINE)
    push_aur("promptr-git", pkgbuild)
    print("  -> AUR: promptr-git updated")


def release_bin():
    """Compute checksums for .deb files and push promptr-bin to AUR."""
    deb_name = f"promptr-{VERSION}-${{CARCH}}.deb"
    base_url = (
        f"https://github.com/toxdes/promptr/releases"
        f"/download/v{VERSION}"
    )

    debs = {
        "amd64": f"{base_url}/promptr_{VERSION}_amd64.deb",
        "arm64": f"{base_url}/promptr_{VERSION}_arm64.deb",
    }

    checksums = {}
    for arch, url in debs.items():
        print(f"  Fetching checksum for {arch}...")
        try:
            checksums[arch] = fetch_checksum(url)
        except Exception as e:
            print(f"Error fetching {arch}: {e}")
            sys.exit(1)

    pkgbuild = BIN_PKGBUILD_TEMPLATE.format(
        maintainer=MAINTAINER,
        version=VERSION,
        url=GITHUB,
        deb_name=deb_name,
        sha256_amd64=checksums["amd64"],
        sha256_arm64=checksums["arm64"],
    )
    push_aur("promptr-bin", pkgbuild)
    print("  -> AUR: promptr-bin updated")


def main():
    parser = argparse.ArgumentParser(description="Push to AUR")
    parser.add_argument(
        "--type",
        choices=["git", "bin", "both"],
        default="both",
        help="Package type to release (default: both)",
    )
    args = parser.parse_args()

    run("ssh -o StrictHostKeyChecking=accept-new -T "
        f"{AUR_SSH} 2>&1 | grep -q username || true",
        capture_output=True)

    if args.type in ("git", "both"):
        release_git()
    if args.type in ("bin", "both"):
        release_bin()


if __name__ == "__main__":
    main()
