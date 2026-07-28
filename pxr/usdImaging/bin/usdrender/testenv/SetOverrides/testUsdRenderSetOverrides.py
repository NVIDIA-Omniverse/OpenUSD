#!/usr/bin/env python3

import pathlib
import struct
import subprocess
import sys


def render(usdrender, name, *set_specs, session_layer=None, print_overrides=False):
    output = pathlib.Path(name)
    command = [
        usdrender,
        "test.usda",
        "--disableGpu",
        "--renderer",
        "Embree",
        "--outputRoot",
        str(output),
    ]
    if session_layer:
        command.extend(("--sessionLayer", session_layer))
    for index, spec in enumerate(set_specs):
        command.extend(("--set" if index % 2 == 0 else "-s", spec))
    if print_overrides:
        command.append("--printOverrides")
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(
            f"render {name} failed ({result.returncode})\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    image = output / "set.png"
    if print_overrides and "over \"Product\"" not in result.stdout:
        raise AssertionError("--printOverrides did not print the override layer")
    return image.read_bytes()


def png_size(image):
    if image[:8] != b"\x89PNG\r\n\x1a\n" or image[12:16] != b"IHDR":
        raise AssertionError("render did not produce a PNG")
    return struct.unpack(">II", image[16:24])


def main():
    if len(sys.argv) != 2:
        raise RuntimeError("expected the usdrender executable path")
    usdrender = sys.argv[1]

    resized = render(
        usdrender, "resolution", "/Render/Product.resolution = (24, 12)"
    )
    if png_size(resized) != (24, 12):
        raise AssertionError("structural resolution override did not compose")

    seed_one = render(
        usdrender,
        "seed-one",
        "{settings}.ty:randomNumberSeed = 1",
        "{settings}.ty:maxBounces = 8",
    )
    seed_one_repeat = render(
        usdrender,
        "seed-one-repeat",
        "{SETTINGS}.ty:randomNumberSeed = 1",
        "{settings}.ty:maxBounces = 8",
    )
    seed_two = render(
        usdrender,
        "seed-two",
        "{settings}.ty:randomNumberSeed = 2",
        "{settings}.ty:maxBounces = 8",
    )
    no_indirect = render(
        usdrender,
        "no-indirect",
        "{settings}.ty:randomNumberSeed = 1",
        "{settings}.ty:maxBounces = 0",
    )
    wide_camera = render(
        usdrender,
        "wide-camera",
        "{settings}.ty:randomNumberSeed = 1",
        "/Camera.focalLength = 25",
    )
    session_path = pathlib.Path("session.usda")
    session_before = session_path.read_bytes()
    with_session = render(
        usdrender,
        "with-session",
        "/Render/Product.resolution = (24, 12)",
        session_layer=str(session_path),
        print_overrides=True,
    )
    if session_path.read_bytes() != session_before:
        raise AssertionError("--sessionLayer file was modified")
    if png_size(with_session) != (24, 12):
        raise AssertionError("--set did not win over --sessionLayer")
    if with_session == resized:
        raise AssertionError("an unrelated --sessionLayer camera opinion was dropped")

    failed = subprocess.run(
        [
            usdrender,
            "test.usda",
            "--sessionLayer",
            str(session_path),
            "--set",
            "/Missing.value = 1",
        ],
        text=True,
        capture_output=True,
    )
    if failed.returncode != 1 or "/Missing.value = 1" not in failed.stderr:
        raise AssertionError("invalid --set did not fail with its source argument")
    if session_path.read_bytes() != session_before:
        raise AssertionError("failed --set modified --sessionLayer")

    if seed_one != seed_one_repeat:
        raise AssertionError("a fixed seed did not produce identical pixels")
    if seed_one == seed_two:
        raise AssertionError("different seed overrides produced identical pixels")
    if seed_one == no_indirect:
        raise AssertionError("maxBounces override did not affect interreflection")
    if seed_one == wide_camera:
        raise AssertionError("non-settings camera override did not affect framing")


if __name__ == "__main__":
    main()
