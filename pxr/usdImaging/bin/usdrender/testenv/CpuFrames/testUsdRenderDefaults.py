#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise RuntimeError("expected the usdrender executable path")

    result = subprocess.run(
        [
            sys.argv[1],
            "test.usda",
            "--disableGpu",
            "--enableCameraLight",
            "--frames",
            "1",
            "--outputRoot",
            "defaults",
        ],
        text=True,
        capture_output=True,
    )
    if result.returncode:
        raise RuntimeError(
            f"usdrender failed ({result.returncode})\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if "Renderer plugin: HdEmbreeRendererPlugin" not in result.stdout:
        raise AssertionError(
            "omitting --renderer did not select Embree\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if not pathlib.Path("defaults/frame.0001.png").is_file():
        raise AssertionError("default renderer did not write the RenderProduct")


if __name__ == "__main__":
    main()
