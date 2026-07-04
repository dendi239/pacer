#!/usr/bin/env python3
"""Generate all Python bindings for pacer and imgui.
This script sequentially runs the existing generate-bindings.py scripts
for each submodule.
"""
import subprocess
import sys
from pathlib import Path

def run_script(script_path: Path) -> None:
    result = subprocess.run([sys.executable, str(script_path)], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Error running {script_path}:", result.stderr, file=sys.stderr)
        sys.exit(result.returncode)
    else:
        print(result.stdout)

def main() -> None:
    base = Path(__file__).parent
    for sub in ["pacer", "imgui"]:
        script = base / sub / "generate-bindings.py"
        if script.is_file():
            print(f"Generating bindings for {sub}...")
            run_script(script)
        else:
            print(f"Script not found: {script}", file=sys.stderr)
            sys.exit(1)

if __name__ == "__main__":
    main()
