#!/usr/bin/env python3
"""Build and run the lvk-metal screenshot tests, optionally under a sanitizer
or with LLVM source-based code coverage.

Usage:
  python3 tests/run_screenshot_tests.py [--asan | --tsan | --ubsan | --coverage]
                                        [--skip-build] [--update] [--only NAME]

Flags mirror neogen's tools/run_desktop_tests.py:
  --asan / --tsan / --ubsan   configure with -DLVK_METAL_SANITIZER=<s>, build, run
  --coverage                  configure with -DLVK_METAL_COVERAGE=ON, build, run,
                              then merge profiles and emit build-coverage/coverage.lcov

Sanitizer / coverage builds go into build-<asan|tsan|ubsan> / build-coverage so
they never clobber the normal build/.

Vulkan (KosmicKrisp) is forwarded from the environment when present:
  Vulkan_INCLUDE_DIR / Vulkan_LIBRARY, or KK_INCLUDE / KK_LIB.
"""

import argparse
import glob
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def build_dir_for(sanitizer, coverage):
    if coverage:
        return ROOT / "build-coverage"
    if sanitizer:
        return ROOT / f"build-{sanitizer}"
    return ROOT / "build"


def configure_and_build(build_dir, sanitizer, coverage):
    args = [
        "cmake", "-S", str(ROOT), "-B", str(build_dir), "-G", "Ninja",
        "-DLVK_METAL_WITH_SAMPLES_SLANG=ON",
    ]
    if sanitizer:
        args += [f"-DLVK_METAL_SANITIZER={sanitizer}", "-DCMAKE_BUILD_TYPE=Debug"]
    if coverage:
        args += ["-DLVK_METAL_COVERAGE=ON", "-DCMAKE_BUILD_TYPE=Debug"]

    inc = os.environ.get("Vulkan_INCLUDE_DIR") or os.environ.get("KK_INCLUDE")
    lib = os.environ.get("Vulkan_LIBRARY") or os.environ.get("KK_LIB")
    if inc:
        args.append(f"-DVulkan_INCLUDE_DIR={inc}")
    if lib:
        args.append(f"-DVulkan_LIBRARY={lib}")

    subprocess.run(args, check=True)
    subprocess.run(["cmake", "--build", str(build_dir)], check=True)


def run_env(build_dir, sanitizer, coverage):
    env = dict(os.environ)
    if sanitizer == "asan":
        # Apple clang has no LSan; keep leak detection off.
        env.setdefault("ASAN_OPTIONS", "abort_on_error=1:halt_on_error=1:detect_leaks=0")
    elif sanitizer == "tsan":
        env.setdefault("TSAN_OPTIONS", "halt_on_error=1")
    elif sanitizer == "ubsan":
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    if coverage:
        # %p -> one profraw per sample process (each sample is its own process).
        env["LLVM_PROFILE_FILE"] = str(build_dir / "cov-%p.profraw")
    return env


def export_coverage(build_dir):
    print()
    print("=== Coverage (LLVM source-based) ===")
    profraws = glob.glob(str(build_dir / "cov-*.profraw"))
    if not profraws:
        print("ERROR: no .profraw files produced - was the build configured with -DLVK_METAL_COVERAGE=ON?", file=sys.stderr)
        return 1

    profdata = build_dir / "coverage.profdata"
    subprocess.run(
        ["xcrun", "llvm-profdata", "merge", "-sparse", *profraws, "-o", str(profdata)],
        check=True,
    )

    objects = []
    for path in sorted(glob.glob(str(build_dir / "samples" / "*" / "*"))):
        p = Path(path)
        if p.is_file() and not p.suffix and os.access(p, os.X_OK):
            objects += ["-object", str(p)]
    if not objects:
        print("ERROR: no sample executables found for coverage", file=sys.stderr)
        return 1

    # Only the lvk/ library is of interest. llvm-cov's -ignore-filename-regex
    # has no negative lookahead, so export everything (minus 3party) and then
    # keep only records whose source path is under lvk/.
    result = subprocess.run(
        ["xcrun", "llvm-cov", "export", *objects, f"-instr-profile={profdata}", "-format=lcov",
         "-ignore-filename-regex=3party/.*"],
        check=True, capture_output=True,
    )
    lvk_marker = os.sep + "lvk" + os.sep
    kept = []
    for record in result.stdout.decode("utf-8", "replace").split("end_of_record"):
        sf = next((ln for ln in record.splitlines() if ln.startswith("SF:")), "")
        if lvk_marker in sf:
            kept.append(record.lstrip("\n") + "end_of_record\n")

    lcov = build_dir / "coverage.lcov"
    lcov.write_text("".join(kept))

    print()
    print("--- Coverage summary (lvk/) ---")
    subprocess.run(
        ["xcrun", "llvm-cov", "report", *objects, f"-instr-profile={profdata}",
         "-show-region-summary=false", "-show-branch-summary=false",
         "-ignore-filename-regex=3party/.*", "-ignore-filename-regex=.*/samples/.*"],
        check=False,
    )
    print()
    print(f"Wrote {lcov}")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Run lvk-metal screenshot tests with sanitizers / coverage")
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--asan", action="store_const", const="asan", dest="sanitizer", help="AddressSanitizer")
    group.add_argument("--tsan", action="store_const", const="tsan", dest="sanitizer", help="ThreadSanitizer")
    group.add_argument("--ubsan", action="store_const", const="ubsan", dest="sanitizer", help="UndefinedBehaviorSanitizer")
    ap.add_argument("--coverage", action="store_true", help="LLVM source-based coverage + coverage.lcov")
    ap.add_argument("--skip-build", action="store_true", help="reuse the existing build directory")
    ap.add_argument("--update", action="store_true", help="overwrite reference images")
    ap.add_argument("--only", default="", help="run only the named sample")
    args = ap.parse_args()

    if args.coverage and args.sanitizer:
        print(f"ERROR: --coverage and --{args.sanitizer} are mutually exclusive", file=sys.stderr)
        return 2

    build_dir = build_dir_for(args.sanitizer, args.coverage)

    if not args.skip_build:
        configure_and_build(build_dir, args.sanitizer, args.coverage)

    cmd = [sys.executable, str(ROOT / "tests" / "screenshot_test.py"), "--build-dir", str(build_dir)]
    if args.update:
        cmd.append("--update")
    if args.only:
        cmd += ["--only", args.only]

    print()
    print("=== Screenshot tests ===")
    test_rc = subprocess.run(cmd, env=run_env(build_dir, args.sanitizer, args.coverage)).returncode

    cov_rc = export_coverage(build_dir) if args.coverage else 0

    return test_rc or cov_rc


if __name__ == "__main__":
    sys.exit(main())
