#!/usr/bin/env python3
"""Format all .c and .h files in the current directory using clang-format."""

import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent

files = sorted(root.glob("*.c")) + sorted(root.glob("*.h"))
files += sorted((root / "src").glob("*.c")) + sorted((root / "src").glob("*.h"))

for f in files:
    subprocess.run(["clang-format", "-style=file", "-i", str(f)], check=True, cwd=root)

print(f"Formatted {len(files)} file(s).")
