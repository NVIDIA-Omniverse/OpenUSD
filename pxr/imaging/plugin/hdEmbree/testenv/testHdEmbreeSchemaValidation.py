#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#

import argparse
import difflib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def _ParseArguments():
    parser = argparse.ArgumentParser(
        description="Validate hdEmbree's generated codeless schema.")
    parser.add_argument("--schema", required=True, type=Path)
    parser.add_argument("--generated-schema", required=True, type=Path)
    parser.add_argument("--plug-info", required=True, type=Path)
    parser.add_argument("--usd-gen-schema", required=True, type=Path)
    return parser.parse_args()


def _ReadCanonicalText(path):
    # Universal-newline mode canonicalizes CRLF and CR to LF while preserving
    # all other content, including the exact number of trailing newlines.
    with path.open("r", encoding="utf-8", newline=None) as stream:
        return stream.read()


def _PrintGeneratorFailure(result):
    print("usdGenSchema failed with return code %d" % result.returncode)
    if result.stdout:
        print("stdout:\n%s" % result.stdout)
    if result.stderr:
        print("stderr:\n%s" % result.stderr)


def main():
    args = _ParseArguments()
    repositoryPaths = (
        args.schema,
        args.generated_schema,
        args.plug_info,
    )
    repositoryBytes = {path: path.read_bytes() for path in repositoryPaths}

    with tempfile.TemporaryDirectory(
            prefix="testHdEmbreeSchemaValidation-") as temporaryDirectory:
        stagingRoot = Path(temporaryDirectory)
        stagingSchemaDirectory = stagingRoot / "schema"
        stagingSchemaDirectory.mkdir()
        stagingSchema = stagingSchemaDirectory / "schema.usda"
        shutil.copy2(args.schema, stagingSchema)
        shutil.copy2(args.plug_info, stagingRoot / "plugInfo.json")

        result = subprocess.run(
            [
                sys.executable,
                str(args.usd_gen_schema),
                str(stagingSchema),
                str(stagingRoot),
                "--quiet",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            _PrintGeneratorFailure(result)
            return 1

        stagedGeneratedSchema = stagingRoot / "generatedSchema.usda"
        expected = _ReadCanonicalText(args.generated_schema)
        actual = _ReadCanonicalText(stagedGeneratedSchema)
        if actual != expected:
            print("generatedSchema.usda does not match usdGenSchema output")
            sys.stdout.writelines(difflib.unified_diff(
                expected.splitlines(keepends=True),
                actual.splitlines(keepends=True),
                fromfile=str(args.generated_schema),
                tofile=str(stagedGeneratedSchema),
            ))
            return 1

    modifiedPaths = [
        path for path, originalBytes in repositoryBytes.items()
        if path.read_bytes() != originalBytes
    ]
    if modifiedPaths:
        print("schema validation modified repository files:")
        for path in modifiedPaths:
            print("  %s" % path)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
