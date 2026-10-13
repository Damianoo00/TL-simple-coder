#!/usr/bin/env python3
"""Build and serve a workspace copy of Trybuna overlays, leaving the source untouched."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
source = Path(sys.argv[1]).expanduser() if len(sys.argv) > 1 else Path.home() / "Documents/Trybuna-Live-Overlays"
target = root / "build/trybuna-overlay"
if not (source / "package.json").is_file():
    sys.exit(f"Overlay project not found: {source}")
if not shutil.which("node"):
    candidates = sorted((root / "build/tooling").glob("node-*-linux-x64/bin"))
    if candidates:
        os.environ["PATH"] = str(candidates[-1]) + os.pathsep + os.environ.get("PATH", "")
if not shutil.which("npm") or not shutil.which("node"):
    sys.exit("Install Node.js >=22.12 and npm, then run this command again.")
shutil.copytree(source, target, dirs_exist_ok=True, ignore=shutil.ignore_patterns(".git", "node_modules", "dist"))
subprocess.run(["npm", "ci", "--cache", str(root / "build/tooling/npm-cache")], cwd=target, check=True)
subprocess.run(["npm", "run", "build"], cwd=target, check=True)
os.execvp("node", ["node", str(target / "src/server.js")])
