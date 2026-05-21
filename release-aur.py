#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["boto3"]
# ///

"""Push to AUR and upload release archives to Cloudflare R2.

Usage:
    python3 release-aur.py --type git     # initial submit, then only on dep changes
    python3 release-aur.py --type bin     # every release (uploads .tar.gz + pushes PKGBUILD)
    python3 release-aur.py --type both

Environment:
    AWS_ACCESS_KEY_ID         Cloudflare R2 access key
    AWS_SECRET_ACCESS_KEY     Cloudflare R2 secret key
    AWS_ENDPOINT_URL          R2 endpoint
    AWS_BUCKET                R2 bucket name
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
DIST = ROOT / "dist"
AUR_HOST = "aur.archlinux.org"
AUR_SSH = f"aur@{AUR_HOST}"
MAINTAINER = "toxdes <hi@toxdes.com>"
GITHUB = "https://github.com/toxdes/promptr"
VERSION = (ROOT / "VERSION").read_text().strip()

PKGBUILD_GIT = ROOT / "PKGBUILD"

# Arch CARCH -> our filename arch suffix
_ARCH_MAP = {"x86_64": "amd64", "aarch64": "arm64"}

# R2 releases prefix — matches the GPG key layout used by other tools
RELEASES_PREFIX = "releases"

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

source_x86_64=("promptr-${{pkgver}}-x86_64.tar.gz::https://packages.toxdes.com/releases/promptr_${{pkgver}}_amd64.tar.gz")
sha256sums_x86_64=('{sha256_amd64}')

source_aarch64=("promptr-${{pkgver}}-aarch64.tar.gz::https://packages.toxdes.com/releases/promptr_${{pkgver}}_arm64.tar.gz")
sha256sums_aarch64=('{sha256_arm64}')

package() {{
  bsdtar -xf "${{srcdir}}/promptr-${{pkgver}}-${{CARCH}}.tar.gz" -C "${{pkgdir}}"
}}
"""


def run(cmd, **kwargs):
    subprocess.run(cmd, shell=True, check=True, **kwargs)


def parse_bash_array(val):
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
    val = val.strip()
    if (val.startswith('"') and val.endswith('"')) or \
       (val.startswith("'") and val.endswith("'")):
        return val[1:-1]
    return val


def generate_srcinfo(pkgbuild_text):
    pkgbase = None
    pkgname = None
    lines_out = []
    in_func = 0

    for line in pkgbuild_text.splitlines():
        stripped = line.strip()

        if stripped.startswith("#"):
            continue
        if not stripped:
            continue

        if "()" in stripped and "{" in stripped:
            in_func = 1
            continue
        if in_func > 0:
            in_func += stripped.count("{") - stripped.count("}")
            continue

        if "=" in stripped:
            key, val = stripped.split("=", 1)
            key = key.strip()
            val = val.strip()

            if key == "pkgname":
                pkgname = val
                if pkgbase is None:
                    pkgbase = val
                continue

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
    repo_path = workdir / repo_name
    aur_url = f"{AUR_SSH}:{repo_name}.git"

    if (repo_path / ".git").exists():
        run(f"git -C {repo_path} pull --rebase", cwd=workdir)
    else:
        run(f"git clone {aur_url} {repo_path}", cwd=workdir)
    return repo_path


def push_aur(repo_name, pkgbuild_text):
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


def upload_releases():
    import boto3

    required = ["AWS_ENDPOINT_URL", "AWS_BUCKET",
                "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY"]
    missing = [v for v in required if v not in os.environ]
    if missing:
        print("Missing env vars: " + ", ".join(missing), file=sys.stderr)
        sys.exit(1)

    client = boto3.client(
        "s3",
        endpoint_url=os.environ["AWS_ENDPOINT_URL"],
        aws_access_key_id=os.environ["AWS_ACCESS_KEY_ID"],
        aws_secret_access_key=os.environ["AWS_SECRET_ACCESS_KEY"],
    )
    bucket = os.environ["AWS_BUCKET"]

    for deb_arch in _ARCH_MAP.values():
        tarball = DIST / f"promptr_{VERSION}_{deb_arch}.tar.gz"
        if not tarball.is_file():
            print(f"Error: {tarball} not found", file=sys.stderr)
            sys.exit(1)
        key = f"{RELEASES_PREFIX}/{tarball.name}"
        client.upload_file(str(tarball), bucket, key)
        print(f"  {key}")


def load_env_file(path):
    env_file = Path(path)
    if not env_file.is_file():
        print(f"Error: {path} not found", file=sys.stderr)
        sys.exit(1)

    loaded = 0
    with open(env_file) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                print(f"Warning: {path}:{lineno} not a KEY=VALUE line, skipping",
                      file=sys.stderr)
                continue
            key, _, val = line.partition("=")
            key = key.strip()
            if key not in os.environ:
                os.environ[key] = val.strip()
                loaded += 1
    print(f"Loaded {loaded} variable(s) from {path}")


def release_git():
    pkgbuild = PKGBUILD_GIT.read_text()
    pkgbuild = re.sub(r'^pkgver=.*$', f'pkgver={VERSION}', pkgbuild, flags=re.MULTILINE)
    push_aur("promptr-git", pkgbuild)
    print("  -> AUR: promptr-git updated")


def release_bin():
    checksums = {}
    for car_ch, deb_arch in _ARCH_MAP.items():
        tarball = DIST / f"promptr_{VERSION}_{deb_arch}.tar.gz"
        print(f"  Checksum for {car_ch}...")
        sum_file = DIST / f"{tarball.name}.sha256"
        if sum_file.is_file():
            line = sum_file.read_text().strip()
            checksums[car_ch] = line.split()[0]
        elif tarball.is_file():
            print(f"  (computing from file)", file=sys.stderr)
            checksums[car_ch] = hashlib.sha256(tarball.read_bytes()).hexdigest()
        else:
            print(f"Error: {tarball} not found", file=sys.stderr)
            sys.exit(1)

    pkgbuild = BIN_PKGBUILD_TEMPLATE.format(
        maintainer=MAINTAINER,
        version=VERSION,
        url=GITHUB,
        sha256_amd64=checksums["x86_64"],
        sha256_arm64=checksums["aarch64"],
    )
    push_aur("promptr-bin", pkgbuild)
    print("  -> AUR: promptr-bin updated")


def main():
    parser = argparse.ArgumentParser(description="Push to AUR and upload releases")
    parser.add_argument("--env", metavar="PATH",
                        help="Load env vars from file (KEY=VALUE per line)")
    parser.add_argument(
        "--type",
        choices=["git", "bin", "both"],
        default="both",
        help="Package type to release (default: both)",
    )
    args = parser.parse_args()

    if args.env:
        load_env_file(args.env)

    run("ssh -o StrictHostKeyChecking=accept-new -T "
        f"{AUR_SSH} 2>&1 | grep -q username || true",
        capture_output=True)

    if args.type in ("git", "both"):
        release_git()

    if args.type in ("bin", "both"):
        print("Uploading release archives to R2 ...")
        upload_releases()
        release_bin()


if __name__ == "__main__":
    main()
