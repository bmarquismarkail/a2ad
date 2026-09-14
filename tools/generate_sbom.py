#!/usr/bin/env python3
"""Generate a CycloneDX 1.5 direct-dependency inventory for an a2ad build.

This records build inputs and a binary digest, not a remote worker attestation
or a complete transitive dependency/license audit.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", type=pathlib.Path, required=True)
parser.add_argument("--output", type=pathlib.Path, required=True)
parser.add_argument("--dependency", action="append", default=[])
args = parser.parse_args()
source = pathlib.Path(__file__).resolve().parents[1]
commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=source, text=True).strip())
components = []
for dependency in sorted(args.dependency):
    name, version = dependency.split("=", 1)
    component = {"type": "library", "name": name, "bom-ref": name}
    if version:
        component["version"] = version
    components.append(component)
with args.binary.open("rb") as binary:
    digest = hashlib.file_digest(binary, "sha256").hexdigest()
bom = {
    "bomFormat": "CycloneDX", "specVersion": "1.5", "version": 1,
    "metadata": {"component": {
        "type": "application", "name": "a2ad", "bom-ref": "a2ad", "version": commit,
        "hashes": [{"alg": "SHA-256", "content": digest}],
        "properties": [{"name": "a2ad:source-dirty", "value": str(dirty).lower()},
                       {"name": "a2ad:inventory-scope", "value": "direct build dependencies"}],
    }},
    "components": components,
    "dependencies": [{"ref": "a2ad", "dependsOn": [c["bom-ref"] for c in components]}],
}
args.output.write_text(json.dumps(bom, indent=2) + "\n")
