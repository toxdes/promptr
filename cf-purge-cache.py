#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["requests"]
# ///

"""Purge Cloudflare cache for packages.toxdes.com.

Uses Cloudflare API to purge cache by hostname, so only
packages.toxdes.com is affected (other hostnames on the
same zone are left untouched).

Usage:
    ./cf-purge-cache.py              # purge cache
    ./cf-purge-cache.py --dry-run    # show what would happen

Environment (via --env PATH):
    CF_API_TOKEN      Cloudflare API token with Zone.Cache Purge
    CF_ZONE_ID        Zone ID for packages.toxdes.com
"""

import argparse
import os
import sys
from pathlib import Path

import requests

API_BASE = "https://api.cloudflare.com/client/v4"
HOSTNAME = "packages.toxdes.com"


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


def check_required(*vars_):
    missing = [v for v in vars_ if not os.environ.get(v)]
    if missing:
        print(f"Error: missing required env vars: {', '.join(missing)}",
              file=sys.stderr)
        print("Provide them via --env PATH or in the environment",
              file=sys.stderr)
        sys.exit(1)


def purge_by_hostname(token, zone_id, dry_run):
    url = f"{API_BASE}/zones/{zone_id}/purge_cache"
    body = {"hosts": [HOSTNAME]}
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
    }

    if dry_run:
        print(f"[dry-run] Would POST {url}")
        print(f"[dry-run] Body: {body}")
        return True

    resp = requests.post(url, json=body, headers=headers, timeout=30)
    data = resp.json()

    if not data.get("success"):
        errors = data.get("errors", [])
        for e in errors:
            print(f"Error: {e.get('message', str(e))}", file=sys.stderr)
        return False

    print(f"Cache purge queued for {HOSTNAME}")
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Purge Cloudflare cache for packages.toxdes.com")
    parser.add_argument("--env", metavar="PATH",
                        help="Load env vars from file (KEY=VALUE per line)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show what would happen without purging")
    args = parser.parse_args()

    if args.env:
        load_env_file(args.env)

    check_required("CF_API_TOKEN", "CF_ZONE_ID")

    ok = purge_by_hostname(
        os.environ["CF_API_TOKEN"],
        os.environ["CF_ZONE_ID"],
        args.dry_run,
    )
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
