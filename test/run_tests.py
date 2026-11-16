"""Build and run the host tests (no hardware needed).

    python test/run_tests.py

Needs a C compiler on the PATH (gcc, clang or zig; `pip install ziglang`
also works) and Node.js for the DBC tests. Set CC to force a compiler.
"""

import glob
import os
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "test", "host", "build")


def find_compiler():
    if os.environ.get("CC"):
        return shlex.split(os.environ["CC"])
    for name in ("gcc", "clang", "cc"):
        if shutil.which(name):
            return [name]
    if shutil.which("zig"):
        return ["zig", "cc"]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "cc"]
    except ImportError:
        return None


def find_cjson():
    """cJSON ships with ESP-IDF; reuse that copy instead of vendoring one."""
    roots = []
    if os.environ.get("IDF_PATH"):
        roots.append(os.environ["IDF_PATH"])
    roots += sorted(glob.glob(os.path.join(os.path.expanduser("~"), ".platformio", "packages", "framework-espidf*")))
    for root in roots:
        path = os.path.join(root, "components", "json", "cJSON")
        if os.path.exists(os.path.join(path, "cJSON.c")):
            return path
    return None


def run_c_tests():
    cc = find_compiler()
    if not cc:
        print("C tests: SKIPPED (no C compiler found; install gcc/clang or `pip install ziglang`)")
        return None
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, "test_gateway.exe" if os.name == "nt" else "test_gateway")
    src = os.path.join(ROOT, "src")
    cmd = cc + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I", src,
                os.path.join(ROOT, "test", "host", "test_gateway.c"),
                os.path.join(src, "filter.c"), os.path.join(src, "xl2515.c")]
    cjson = find_cjson()
    if cjson:
        cmd += ["-I", cjson, os.path.join(src, "config_json.c"), os.path.join(cjson, "cJSON.c")]
    else:
        cmd += ["-DNO_CJSON"]
    if os.name != "nt":
        cmd += ["-pthread"]
    cmd += ["-o", exe]
    if subprocess.call(cmd) != 0:
        print("C tests: BUILD FAILED")
        return False
    return subprocess.call([exe]) == 0


def run_js_tests():
    node = shutil.which("node")
    if not node:
        print("DBC tests: SKIPPED (Node.js not found)")
        return None
    return subprocess.call([node, os.path.join(ROOT, "test", "dbc.test.js")]) == 0


def main():
    results = [run_c_tests(), run_js_tests()]
    sys.exit(1 if False in results else 0)


if __name__ == "__main__":
    main()
