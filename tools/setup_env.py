#!/usr/bin/env python3
"""Set up a Python virtual environment.

Usage: python3 tools/setup_env.py
"""

import argparse
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent


def run(cmd: list[str], check: bool = True) -> int:
    print("+", " ".join(cmd))
    return subprocess.run(cmd, check=check).returncode


def run_quiet(cmd: list[str]) -> tuple[int, str]:
    # OSError covers a missing executable and a console script with a stale shebang
    # ("bad interpreter") — both surface as FileNotFoundError from exec.
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except OSError:
        return 1, ""
    return proc.returncode, (proc.stdout or "").strip()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Set up a Python virtual environment."
    )
    parser.parse_args()

    venv_dir = SCRIPT_DIR / ".venv"

    # Platform-specific paths inside the venv
    if sys.platform == "win32":
        scripts_dir = venv_dir / "Scripts"
        python_path = scripts_dir / "python.exe"
    else:
        scripts_dir = venv_dir / "bin"
        python_path = scripts_dir / "python"

    pip_wrapper = scripts_dir / ("pip.exe" if sys.platform == "win32" else "pip")
    venv_ok = (
        python_path.is_file()
        and run_quiet([str(python_path), "-m", "pip", "--version"])[0] == 0
        and pip_wrapper.is_file()
        and run_quiet([str(pip_wrapper), "--version"])[0] == 0
    )
    if not venv_ok:
        print(f"Creating virtual environment in {venv_dir}...")
        run([sys.executable, "-m", "venv", "--clear", str(venv_dir)])

    # Install tools (use python -m pip to avoid Windows self-update issues)
    print("Installing tools...")
    run([str(python_path), "-m", "pip", "install", "--quiet", "--upgrade", "pip"])
    run([str(python_path), "-m", "pip", "install", "--quiet", "-r",
         str(SCRIPT_DIR / "requirements.txt")])

    # Show installed tool versions
    print()
    print("Done. Tools available:")
    for tool in ("clang-format", "cmake-format"):
        rc, stdout = run_quiet([str(scripts_dir / tool), "--version"])
        if rc == 0 and stdout:
            print(f"  {tool}: {stdout.splitlines()[0]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
