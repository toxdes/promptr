#!/usr/bin/env python3
"""Recursively format all .c and .h files using clang-format."""

import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent
exclude_dirs = {"build", ".git", "dist"}

files = []
for p in root.rglob("*"):
    if p.is_dir() and p.name in exclude_dirs:
        continue
    if p.suffix in {".c", ".h"}:
        files.append(p)
files.sort()

count = 0
for f in files:
    subprocess.run(["clang-format", "-style=file", "-i", str(f)], check=True, cwd=root)
    count += 1

print(f"Formatted {count} file(s).")
