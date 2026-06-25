# Why do we need patch?
# lvk's bundled **SPIRV-Tools** builds a `SPIRV-Tools-shared` target whose `build-version.inc` custom command 
# is attached to BOTH the shared and static libs — Xcode's "new build system" forbids that.

import os
import sys
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
LVK_DIR = os.path.normpath(os.path.join(HERE, "..", "src", "lightweightvk"))

SHARED_BLOCK = (
    "add_library(${SPIRV_TOOLS}-shared SHARED ${SPIRV_SOURCES})\n"
    "if (SPIRV_TOOLS_USE_MIMALLOC)\n"
    "  target_link_libraries(${SPIRV_TOOLS}-shared PRIVATE mimalloc-static)\n"
    "endif()\n"
    "spirv_tools_default_target_options(${SPIRV_TOOLS}-shared)\n"
    "set_target_properties(${SPIRV_TOOLS}-shared PROPERTIES CXX_VISIBILITY_PRESET hidden)\n"
    "target_compile_definitions(${SPIRV_TOOLS}-shared\n"
    "  PRIVATE SPIRV_TOOLS_IMPLEMENTATION\n"
    "  PUBLIC SPIRV_TOOLS_SHAREDLIB\n"
    ")\n"
)

TARGETS_LINE = "set(SPIRV_TOOLS_TARGETS ${SPIRV_TOOLS}-static ${SPIRV_TOOLS}-shared)"


def deploy_lvk_dependencies():
    print("lvk_post_deploy: deploying lightweightvk dependencies")
    subprocess.run([sys.executable, "deploy_deps.py"], cwd=LVK_DIR, check=True)


def patch_spirv_tools_for_xcode():
    path = os.path.join(LVK_DIR, "third-party", "deps", "src", "SPIRV-Tools", "source", "CMakeLists.txt")
    if not os.path.exists(path):
        print("lvk_post_deploy: SPIRV-Tools not found, skipping patch:", path)
        return
    with open(path, "r") as f:
        text = f.read()
    if "if(BUILD_SHARED_LIBS)\n" + SHARED_BLOCK in text:
        print("lvk_post_deploy: SPIRV-Tools already patched")
        return
    if SHARED_BLOCK not in text or TARGETS_LINE not in text:
        print("lvk_post_deploy: SPIRV-Tools layout unexpected, skipping patch")
        return
    text = text.replace(SHARED_BLOCK, "if(BUILD_SHARED_LIBS)\n" + SHARED_BLOCK + "endif()\n", 1)
    text = text.replace(
        TARGETS_LINE,
        "set(SPIRV_TOOLS_TARGETS ${SPIRV_TOOLS}-static)\n"
        "  if(BUILD_SHARED_LIBS)\n"
        "    list(APPEND SPIRV_TOOLS_TARGETS ${SPIRV_TOOLS}-shared)\n"
        "  endif()",
        1,
    )
    with open(path, "w") as f:
        f.write(text)
    print("lvk_post_deploy: patched SPIRV-Tools shared target for the Xcode build system")


def main():
    deploy_lvk_dependencies()
    patch_spirv_tools_for_xcode()
    return 0


if __name__ == "__main__":
    sys.exit(main())
