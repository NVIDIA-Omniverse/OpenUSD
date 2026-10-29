#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Check Tf's core boundary using compiler traces and an installed consumer."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


def run(command, log_path=None, **kwargs):
    result = subprocess.run(
        [str(arg) for arg in command], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, errors="replace", **kwargs)
    if log_path:
        log_path.write_text(result.stdout)
    if result.returncode:
        lines = result.stdout.splitlines()
        errors = [line for line in lines if "error" in line.lower()]
        detail = "\n".join(errors[:40] if errors else lines[-40:])
        raise RuntimeError(f"Command failed: {command}\n{detail}\nLog: {log_path}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-dir", "build-dir", "prefix", "headers", "module-deps", "link-libraries",
                 "cmake", "generator", "config"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    build = Path(args.build_dir)
    prefix = Path(args.prefix)

    module_deps = Path(args.module_deps).read_text()
    dependencies = re.findall(r'TfToken\("([^\"]+)"\)', module_deps)
    if not dependencies:
        raise RuntimeError("No generated Tf module dependencies found")
    if any(dep in ("python", "boost") or dep.startswith("Python3::")
           for dep in dependencies):
        raise RuntimeError(f"Binding/runtime targets in module metadata: {dependencies}")

    links = Path(args.link_libraries).read_text()
    checked_links = links
    if sys.platform != "win32":
        # Non-Windows runtime linkage is intentionally unchanged by this slice.
        checked_links = checked_links.replace("Python3::Python", "")
    if re.search(r"(^|[;:\n<>])(boost|python)($|[;:\n<>])", checked_links, re.I):
        raise RuntimeError(f"Binding or versioned Python targets in Tf build graph: {links}")
    if sys.platform == "win32" and re.search(r"python[0-9]{2,}(_d)?\.(lib|dll)", links, re.I):
        raise RuntimeError(f"Versioned Python library in Tf build graph: {links}")

    run([args.cmake, "-S", args.source_dir, "-B", build,
         "-G", args.generator, f"-DUSD_PREFIX={prefix}",
         f"-DCORE_HEADERS={args.headers}"])
    # Recompile even on repeated CTest runs so dependency traces remain valid.
    output = run([args.cmake, "--build", build, "--config", args.config,
                  "--clean-first", "--parallel", "2"], log_path=build / "build.log")
    boost_includes = [line for line in output.splitlines()
                      if re.search(r"[/\\]boost[/\\].*\.(h|hpp)", line, re.I)]
    if boost_includes:
        raise RuntimeError("Core headers include Boost:\n" + "\n".join(boost_includes))

    consumer = Path((build / f"consumer-{args.config}.txt").read_text().strip())
    env = os.environ.copy()
    dll_dirs = [prefix / "lib", prefix / "bin", Path(sys.base_prefix)]
    env["PATH"] = os.pathsep.join([str(p) for p in dll_dirs] + [env.get("PATH", "")])
    run([consumer], env=env, timeout=60)

    if sys.platform == "win32":
        linker = Path((build / "linker.txt").read_text().strip())
        dumpbin = linker.with_name("dumpbin.exe")
        for binary in (prefix / "lib/usd_tf.dll", consumer):
            imports = run([dumpbin, "/DEPENDENTS", binary])
            (build / f"{binary.stem}-imports.log").write_text(imports)
            if "python3.dll" not in imports.lower():
                raise RuntimeError(f"{binary} does not import the stable Python ABI DLL")
            if re.search(r"usd_(python|boost)\.dll|python[0-9]{2,}(_d)?\.dll", imports, re.I):
                raise RuntimeError(f"Unexpected {binary} imports:\n{imports}")

    count = len(Path(args.headers).read_text().splitlines())
    print(f"PASS: {count} core public headers compiled with full and Limited API;")
    print("no Boost headers; clean Tf dependency metadata; installed consumer ran.")


if __name__ == "__main__":
    main()
