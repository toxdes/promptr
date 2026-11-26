#!/usr/bin/env python3
"""Small OpenCode CLI stand-in; exercises the adapter without credentials."""

from pathlib import Path
import json
import os
import sys


def main():
    if sys.argv[1] == "--version":
        print(os.environ.get("PROMPTR_TEST_OPENCODE_VERSION", "1.19.3"))
        return
    if sys.argv[1] == "models":
        print("fixture/first\nfixture/second")
        return
    attachment = Path(sys.argv[sys.argv.index("--file") + 1]) if "--file" in sys.argv else None
    config = json.loads(os.environ["OPENCODE_CONFIG_CONTENT"])
    context = config["agent"]["promptr-context"]["prompt"]
    if context.startswith("{file:") and context.endswith("}"):
        context = Path(context[6:-1]).read_text()
    result = {
        "cwd": os.getcwd(),
        "conversation": attachment.read_text() if attachment else sys.argv[-1],
        "attached": attachment is not None,
        "context": context,
        "reused": "--session" in sys.argv,
        "tools_denied": config.get("permission", {}).get("*") == "deny",
        "standalone": "--standalone" in sys.argv,
        "model": sys.argv[sys.argv.index("--model") + 1] if "--model" in sys.argv else None,
    }
    print(json.dumps({"type": "text", "sessionID": "fixture-session",
                      "part": {"text": json.dumps(result, sort_keys=True)}}), flush=True)


if __name__ == "__main__":
    main()
