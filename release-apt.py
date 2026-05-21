#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["boto3"]
# ///

"""Build and publish an apt repository to Cloudflare R2.

Uploads under "apt/" prefix (including pubkey.gpg).

Usage:
    ./release-apt.py              # build repo, sign, upload to R2
    ./release-apt.py --dry-run    # build repo locally, skip upload
    ./release-apt.py --serve      # build repo and serve via HTTP for testing

Environment:
    AWS_ACCESS_KEY_ID         Cloudflare R2 access key
    AWS_SECRET_ACCESS_KEY     Cloudflare R2 secret key
    AWS_ENDPOINT_URL          R2 endpoint
    AWS_BUCKET                R2 bucket name
    GPG_KEY_ID                GPG key ID for signing
    GPG_PASSPHRASE            passphrase (optional, only if key has one)
"""

import argparse
import atexit
import email.parser
import gzip
import hashlib
import http.server
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DIST = ROOT / "dist"
VERSION = (ROOT / "VERSION").read_text().strip()

SUITE = "stable"
COMPONENT = "main"
ARCHS = ("amd64", "arm64")
ORIGIN = "toxdes"
LABEL = "promptr"
PREFIX = "apt"


def parse_control(deb_path):
    result = subprocess.run(
        ["dpkg-deb", "-f", str(deb_path)],
        capture_output=True, text=True, check=True,
    )
    parser = email.parser.HeaderParser()
    msg = parser.parsestr(result.stdout)
    return dict(msg)


def compute_hashes(path):
    md5 = hashlib.md5()
    sha1 = hashlib.sha1()
    sha256 = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            size += len(chunk)
            md5.update(chunk)
            sha1.update(chunk)
            sha256.update(chunk)
    return md5.hexdigest(), sha1.hexdigest(), sha256.hexdigest(), size


def find_debs():
    debs = sorted(DIST.glob("promptr_*.deb"))
    if not debs:
        print("No .deb files found in dist/", file=sys.stderr)
        sys.exit(1)
    return debs


def build_repo(root_dir, debs):
    prefix_dir = root_dir / PREFIX
    pool_dir = prefix_dir / "pool" / COMPONENT / "p" / "promptr"
    pool_dir.mkdir(parents=True, exist_ok=True)

    for deb in debs:
        shutil.copy2(deb, pool_dir / deb.name)

    for arch in ARCHS:
        bins_dir = prefix_dir / "dists" / SUITE / COMPONENT / f"binary-{arch}"
        bins_dir.mkdir(parents=True, exist_ok=True)

        arch_debs = [pool_dir / d.name for d in debs
                     if d.name.endswith(f"_{arch}.deb")]
        if not arch_debs:
            continue

        entries = []
        for deb_path in sorted(arch_debs):
            control = parse_control(deb_path)
            rel_name = f"pool/{COMPONENT}/p/promptr/{deb_path.name}"
            md5, sha1, sha256, size = compute_hashes(deb_path)

            control["Filename"] = rel_name
            control["Size"] = str(size)
            control["MD5sum"] = md5
            control["SHA1"] = sha1
            control["SHA256"] = sha256

            entries.append("\n".join(
                f"{k}: {v}" for k, v in control.items()
            ))

        content = "\n\n".join(entries) + "\n"
        (bins_dir / "Packages").write_text(content)
        with gzip.open(bins_dir / "Packages.gz", "wt") as fp:
            fp.write(content)

    dist_dir = prefix_dir / "dists" / SUITE
    build_release(dist_dir)


def build_release(dist_dir):
    now = datetime.now(timezone.utc).strftime("%a, %d %b %Y %H:%M:%S UTC")
    lines = [
        f"Origin: {ORIGIN}",
        f"Label: {LABEL}",
        f"Suite: {SUITE}",
        f"Codename: {SUITE}",
        f"Architectures: {' '.join(ARCHS)}",
        f"Components: {COMPONENT}",
        "Description: Promptr apt repository",
        f"Date: {now}",
    ]

    meta = [f for f in sorted(dist_dir.rglob("*")) if f.is_file()]

    md5_lines = []
    sha1_lines = []
    sha256_lines = []

    for fpath in meta:
        rel = str(fpath.relative_to(dist_dir))
        md5, sha1, sha256, size = compute_hashes(fpath)
        md5_lines.append(f" {md5} {size} {rel}")
        sha1_lines.append(f" {sha1} {size} {rel}")
        sha256_lines.append(f" {sha256} {size} {rel}")

    if md5_lines:
        lines.append("MD5Sum:")
        lines.extend(md5_lines)
    if sha1_lines:
        lines.append("SHA1:")
        lines.extend(sha1_lines)
    if sha256_lines:
        lines.append("SHA256:")
        lines.extend(sha256_lines)

    (dist_dir / "Release").write_text("\n".join(lines) + "\n")


def gpg_sign(release_path, key_id, passphrase=None):
    dist_dir = release_path.parent
    cmd = ["gpg", "--batch", "--yes"]
    sp_args = {}
    if key_id:
        cmd += ["--local-user", key_id]
    if passphrase:
        cmd += ["--pinentry-mode", "loopback", "--passphrase-fd", "0"]
        sp_args["input"] = passphrase.encode()

    subprocess.run(
        cmd + ["--clearsign", "--armor", "-o",
               str(dist_dir / "InRelease"), str(release_path)],
        check=True, **sp_args,
    )
    subprocess.run(
        cmd + ["--detach-sign", "--armor", "-o",
               str(dist_dir / "Release.gpg"), str(release_path)],
        check=True, **sp_args,
    )


def export_pubkey(key_id, repo_dir):
    result = subprocess.run(
        ["gpg", "--export", "--armor", key_id],
        capture_output=True, text=True, check=True,
    )
    (repo_dir / PREFIX / "pubkey.gpg").write_text(result.stdout)


def upload_r2(repo_dir):
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

    for fpath in sorted(repo_dir.rglob("*")):
        if fpath.is_file():
            key = str(fpath.relative_to(repo_dir))
            client.upload_file(str(fpath), bucket, key)
            print(f"  {key}")


def serve_repo(repo_dir):
    os.chdir(repo_dir)

    host = "0.0.0.0"
    port = 8080
    print(f"Serving repository on http://{host}:{port}")
    print("Test with:\n")
    print("  docker run --network=host --rm -it debian:bookworm bash")
    print("  # Inside container:")
    print(f"  echo 'deb [trusted=yes] http://localhost:{port}/{PREFIX} stable"
          " main' \\")
    print("    > /etc/apt/sources.list.d/promptr.list")
    print("  apt update && apt install promptr")
    print()
    print("Press Ctrl+C to stop.")

    server = http.server.HTTPServer(
        (host, port), http.server.SimpleHTTPRequestHandler,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
        server.shutdown()


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
            key, _, value = line.partition("=")
            key = key.strip()
            if key not in os.environ:
                os.environ[key] = value.strip()
                loaded += 1
    print(f"Loaded {loaded} variable(s) from {path}")


def check_tools():
    for tool in ("dpkg-deb", "dpkg", "gpg"):
        if subprocess.run(["which", tool], capture_output=True).returncode != 0:
            print(f"Error: {tool} not found", file=sys.stderr)
            sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="Publish apt repo to R2")
    parser.add_argument("--env", metavar="PATH",
                        help="Load env vars from file (KEY=VALUE per line)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true",
                      help="Build repo locally, skip upload")
    mode.add_argument("--serve", action="store_true",
                      help="Build repo and serve via HTTP for testing")
    args = parser.parse_args()

    if args.env:
        load_env_file(args.env)

    check_tools()

    debs = find_debs()
    print(f"Found {len(debs)} package(s):")
    for d in debs:
        print(f"  {d.name}")

    repo = tempfile.mkdtemp(prefix="promptr-apt-")
    repo_path = Path(repo)
    keep = args.dry_run  # keep repo for inspection
    if not keep:
        atexit.register(shutil.rmtree, repo, ignore_errors=True)
    print(f"\nBuilding apt repository in {repo} ...")
    build_repo(repo_path, debs)

    if args.serve:
        serve_repo(repo_path)
        return

    gpg_key = os.environ.get("GPG_KEY_ID")
    if gpg_key:
        release_path = repo_path / PREFIX / "dists" / SUITE / "Release"
        gpg_sign(release_path, gpg_key,
                 os.environ.get("GPG_PASSPHRASE"))
        export_pubkey(gpg_key, repo_path)
    else:
        print("Note: GPG_KEY_ID not set, skipping signing.",
              file=sys.stderr)

    if args.dry_run:
        print("Dry run — skipping upload.")
        print(f"Repository at: {repo}")
    else:
        print("Uploading to R2 ...")
        upload_r2(repo_path)
        print("Done.")

    print(f"\nRepository built for version {VERSION} ({SUITE}).")


if __name__ == "__main__":
    main()
