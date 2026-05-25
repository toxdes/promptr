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
    ./cf-purge-cache.py --check-ttl  # check current cache TTL settings
    ./cf-purge-cache.py --set-ttl    # set cache TTL from env vars

Environment (via --env PATH):
    CF_API_TOKEN             Cloudflare API token with Zone.Cache Purge
    CF_ZONE_ID               Zone ID for packages.toxdes.com
    CF_BROWSER_TTL           Browser cache TTL in seconds (for --set-ttl)
    CF_EDGE_TTL              Edge cache TTL in seconds (for --set-ttl)
"""

import argparse
import json
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
    headers = _headers(token)

    if dry_run:
        print(f"[dry-run] Would POST {url}")
        print(f"[dry-run] Body: {body}")
        return True

    resp = requests.post(url, json=body, headers=headers, timeout=30)
    return _handle_response(resp, "Cache purge queued")


def check_ttl(token, zone_id):
    settings = (
        ("browser_cache_ttl", "Browser"),
        ("edge_cache_ttl", "Edge"),
    )
    headers = _headers(token)

    for key, label in settings:
        url = f"{API_BASE}/zones/{zone_id}/settings/{key}"
        resp = requests.get(url, headers=headers, timeout=30)
        data = resp.json()
        if data.get("success"):
            value = data["result"]["value"]
            print(f"{label} cache TTL: {value}")
        else:
            for e in data.get("errors", []):
                print(f"Error ({key}): {e.get('message', str(e))}",
                      file=sys.stderr)


def set_ttl(token, zone_id, browser_ttl, edge_ttl):
    settings = (
        ("browser_cache_ttl", "Browser", browser_ttl),
        ("edge_cache_ttl", "Edge", edge_ttl),
    )
    headers = _headers(token)
    ok = True

    for key, label, ttl in settings:
        url = f"{API_BASE}/zones/{zone_id}/settings/{key}"
        body = {"value": int(ttl)}
        resp = requests.patch(url, json=body, headers=headers, timeout=30)
        data = resp.json()
        if data.get("success"):
            print(f"{label} cache TTL set to {ttl}")
        else:
            for e in data.get("errors", []):
                print(f"Error ({key}): {e.get('message', str(e))}",
                      file=sys.stderr)
            ok = False

    return ok


def _headers(token):
    return {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
    }


def _handle_response(resp, ok_msg):
    data = resp.json()
    if not data.get("success"):
        for e in data.get("errors", []):
            print(f"Error: {e.get('message', str(e))}", file=sys.stderr)
        return False
    print(ok_msg)
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Purge Cloudflare cache for packages.toxdes.com")
    parser.add_argument("--env", metavar="PATH",
                        help="Load env vars from file (KEY=VALUE per line)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show what would happen without purging")
    parser.add_argument("--check-ttl", action="store_true",
                        help="Check current cache TTL settings")
    parser.add_argument("--set-ttl", action="store_true",
                        help="Set cache TTL from CF_BROWSER_TTL and CF_EDGE_TTL env vars")
    args = parser.parse_args()

    if args.env:
        load_env_file(args.env)

    check_required("CF_API_TOKEN", "CF_ZONE_ID")

    token = os.environ["CF_API_TOKEN"]
    zone = os.environ["CF_ZONE_ID"]

    if args.check_ttl:
        check_ttl(token, zone)
    elif args.set_ttl:
        check_required("CF_BROWSER_TTL", "CF_EDGE_TTL")
        ok = set_ttl(token, zone,
                     os.environ["CF_BROWSER_TTL"],
                     os.environ["CF_EDGE_TTL"])
        sys.exit(0 if ok else 1)
    else:
        ok = purge_by_hostname(token, zone, args.dry_run)
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
