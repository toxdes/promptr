#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["boto3"]
# ///

"""Build and publish a DNF/RPM repository to Cloudflare R2.

Uploads under "rpm/" prefix (including pubkey.gpg).

Usage:
    ./release-rpm.py              # build repo, sign, upload to R2
    ./release-rpm.py --dry-run    # build repo locally, skip upload
    ./release-rpm.py --serve      # build repo and serve via HTTP for testing

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
import http.server
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DIST = ROOT / "dist"
VERSION = (ROOT / "VERSION").read_text().strip()

PREFIX = "rpm"


def find_rpms():
    rpms = sorted(DIST.glob("promptr-*.rpm"))
    if not rpms:
        print("No .rpm files found in dist/", file=sys.stderr)
        sys.exit(1)
    return rpms


def build_repo(root_dir, rpms):
    rpm_dir = root_dir / PREFIX
    rpm_dir.mkdir(parents=True, exist_ok=True)

    for rpm in rpms:
        shutil.copy2(rpm, rpm_dir / rpm.name)

    gpg_key = os.environ.get("GPG_KEY_ID")
    if gpg_key:
        sign_rpms(rpm_dir, gpg_key)

    subprocess.run(
        ["createrepo_c", str(rpm_dir)],
        check=True,
    )


def pre_cache_gpg_key(key_id, passphrase=None):
    """Sign a dummy to load the key into gpg-agent so rpmsign never prompts."""
    cmd = ["gpg", "--batch", "--yes", "--pinentry-mode", "loopback"]
    sp_args = {}
    if passphrase:
        cmd += ["--passphrase-fd", "0"]
        sp_args["input"] = passphrase.encode()
    cmd += ["--sign", "--local-user", key_id, "-o", "/dev/null", "/dev/null"]
    subprocess.run(cmd, check=True, **sp_args)


def sign_rpms(rpm_dir, key_id):
    cmd = ["rpmsign", "--addsign", "--define", f"_gpg_name {key_id}"]
    for rpm in sorted(rpm_dir.glob("*.rpm")):
        subprocess.run(cmd + [str(rpm)], check=True)


def gpg_sign_repomd(repomd_path, key_id, passphrase=None):
    cmd = ["gpg", "--batch", "--yes"]
    sp_args = {}
    if key_id:
        cmd += ["--local-user", key_id]
    if passphrase:
        cmd += ["--pinentry-mode", "loopback", "--passphrase-fd", "0"]
        sp_args["input"] = passphrase.encode()

    asc_path = repomd_path.parent / "repomd.xml.asc"
    subprocess.run(
        cmd + ["--detach-sign", "--armor", "-o",
               str(asc_path), str(repomd_path)],
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
    print("  docker run --network=host --rm -it fedora:latest bash")
    print("  # Inside container:")
    print("  dnf install -y dnf-plugins-core")
    print(f"  tee /etc/yum.repos.d/promptr.repo <<'EOF'")
    print("[promptr]")
    print(f"baseurl=http://localhost:{port}/{PREFIX}")
    print("gpgcheck=0")
    print("enabled=1")
    print("EOF")
    print("  dnf install promptr")
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
    for tool in ("createrepo_c", "gpg", "rpmsign"):
        if subprocess.run(["which", tool], capture_output=True).returncode != 0:
            print(f"Error: {tool} not found", file=sys.stderr)
            sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="Publish RPM repo to R2")
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

    gpg_key = os.environ.get("GPG_KEY_ID")
    if gpg_key and os.environ.get("GPG_PASSPHRASE"):
        pre_cache_gpg_key(gpg_key, os.environ["GPG_PASSPHRASE"])

    rpms = find_rpms()
    print(f"Found {len(rpms)} package(s):")
    for r in rpms:
        print(f"  {r.name}")

    repo = tempfile.mkdtemp(prefix="promptr-rpm-")
    repo_path = Path(repo)
    keep = args.dry_run
    if not keep:
        atexit.register(shutil.rmtree, repo, ignore_errors=True)
    print(f"\nBuilding RPM repository in {repo} ...")
    build_repo(repo_path, rpms)

    if args.serve:
        serve_repo(repo_path)
        return

    gpg_key = os.environ.get("GPG_KEY_ID")
    if gpg_key:
        repomd = repo_path / PREFIX / "repodata" / "repomd.xml"
        gpg_sign_repomd(repomd, gpg_key,
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

    print(f"\nRepository built for version {VERSION}.")


if __name__ == "__main__":
    main()
