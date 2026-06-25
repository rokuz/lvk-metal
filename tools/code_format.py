#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# This script lives in tools/; the formatter configs live in the repo root.
SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
VENV_BIN = SCRIPT_DIR / ".venv" / "bin"

# Directories to skip (exact match)
SKIP_DIRS = {"3party", ".venv", "__pycache__", "node_modules", ".git"}

# Directory prefixes to skip
SKIP_DIR_PREFIXES = ("build",)

# Extensions for clang-format
CLANG_FORMAT_EXTENSIONS = {".h", ".c", ".m", ".hpp", ".cpp", ".mm"}

# Extensions for cmake-format
CMAKE_FORMAT_NAMES = {"CMakeLists.txt"}
CMAKE_FORMAT_EXTENSIONS = {".cmake"}


def find_clang_format() -> str:
    """Find clang-format: prefer venv, then PATH."""
    venv_cf = VENV_BIN / "clang-format"
    if venv_cf.is_file():
        return str(venv_cf)
    system_cf = shutil.which("clang-format")
    if system_cf:
        return system_cf
    sys.stderr.write(
        "Error: clang-format not found.\n"
        "  Set up the tools venv via: python3 tools/setup_env.py\n"
        "  Or install directly:       pip install clang-format\n"
    )
    sys.exit(2)


def find_cmake_format() -> str | None:
    """Find cmake-format if available: prefer venv, then PATH."""
    venv_cmf = VENV_BIN / "cmake-format"
    if venv_cmf.is_file():
        return str(venv_cmf)
    return shutil.which("cmake-format")


def should_skip_dir(name: str) -> bool:
    """Check if a directory name should be skipped."""
    if name in SKIP_DIRS:
        return True
    for prefix in SKIP_DIR_PREFIXES:
        if name.startswith(prefix):
            return True
    return False


def path_is_skipped(path: Path) -> bool:
    """True if any component of `path` is a skipped directory.

    Checked relative to the repo root so that pointing the formatter directly at a
    skipped tree (e.g. `code_format.py 3party`) is still skipped — pruning during
    os.walk only affects subdirectories, not the walk root itself.
    """
    try:
        parts = path.resolve().relative_to(ROOT_DIR).parts
    except ValueError:
        parts = (path.name,)
    return any(should_skip_dir(part) for part in parts)


def collect_files(directories: list[Path]) -> tuple[list[Path], list[Path]]:
    """Collect source files for clang-format and cmake-format."""
    clang_files: list[Path] = []
    cmake_files: list[Path] = []

    for directory in directories:
        for root, dirs, files in os.walk(directory):
            root_path = Path(root)

            # Never format inside a skipped tree (e.g. 3party, build*, .venv), even
            # when it is the walk root itself.
            if path_is_skipped(root_path):
                dirs[:] = []
                continue

            # Prune skipped subdirectories in-place
            dirs[:] = [d for d in dirs if not should_skip_dir(d)]

            for name in files:
                filepath = root_path / name
                if filepath.suffix in CLANG_FORMAT_EXTENSIONS:
                    clang_files.append(filepath)
                elif name in CMAKE_FORMAT_NAMES or filepath.suffix in CMAKE_FORMAT_EXTENSIONS:
                    cmake_files.append(filepath)

    return clang_files, cmake_files


def format_file(tool: str, args: list[str], filepath: Path) -> tuple[Path, int, str]:
    """Run a formatter on a single file."""
    try:
        proc = subprocess.run(
            [tool] + args + [str(filepath)],
            capture_output=True,
            text=True,
        )
        output = proc.stderr if proc.stderr else ""
        return filepath, proc.returncode, output
    except FileNotFoundError as e:
        return filepath, 127, str(e)


def main() -> int:
    parser = argparse.ArgumentParser(description="Format project source code")
    parser.add_argument(
        "directories",
        nargs="+",
        type=Path,
        help="Directories to format recursively",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Check formatting without modifying files (exit 1 if changes needed)",
    )
    args = parser.parse_args()

    # Resolve directories
    directories = [d.resolve() for d in args.directories]
    for d in directories:
        if not d.is_dir():
            sys.stderr.write(f"Error: {d} is not a directory\n")
            return 1

    clang_format = find_clang_format()
    cmake_format = find_cmake_format()

    clang_format_config = ROOT_DIR / ".clang-format"
    cmake_format_config = ROOT_DIR / ".cmake-format"

    clang_files, cmake_files = collect_files(directories)

    if not clang_files and not cmake_files:
        print("No source files found to format.")
        return 0

    # Build format tasks
    tasks: list[tuple[str, list[str], Path]] = []

    if clang_files:
        clang_args = ["-i", f"-style=file:{clang_format_config}"]
        if args.check:
            clang_args = ["--dry-run", "--Werror", f"-style=file:{clang_format_config}"]
        for f in clang_files:
            tasks.append((clang_format, clang_args, f))

    if cmake_files and cmake_format:
        if cmake_format_config.exists():
            cmake_args = ["-i", "-c", str(cmake_format_config), "--"]
            if args.check:
                cmake_args = ["--check", "-c", str(cmake_format_config), "--"]
            for f in cmake_files:
                tasks.append((cmake_format, cmake_args, f))
        else:
            print("Skipping cmake-format: no .cmake-format config found",
                  file=sys.stderr)

    if not tasks:
        print("No files to format.")
        return 0

    print(f"Formatting {len(tasks)} file(s)...", file=sys.stderr)

    failed = 0
    workers = max(1, (os.cpu_count() or 4) - 1)

    with ThreadPoolExecutor(max_workers=workers) as ex:
        futures = {
            ex.submit(format_file, tool, tool_args, filepath): filepath
            for tool, tool_args, filepath in tasks
        }
        for fut in as_completed(futures):
            filepath, rc, output = fut.result()
            if rc != 0:
                failed += 1
                sys.stderr.write(f"FAIL: {filepath}\n")
                if output:
                    sys.stderr.write(output)

    if args.check and failed > 0:
        print(f"\n{failed} file(s) need formatting.", file=sys.stderr)
        return 1

    if failed > 0:
        print(f"\n{failed} file(s) had formatting errors.", file=sys.stderr)
        return 1

    print("Done.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
