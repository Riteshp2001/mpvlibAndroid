#!/usr/bin/env python3

import hashlib
import json
import sys
import zipfile
from pathlib import Path


def verify(aar):
    models_dir = Path(__file__).resolve().parent / "prefix" / "share" / "openjoc"
    expected_models = {"sadie-ii-d1-ku100.ojhrtf", "sadie-ii-d2-kemar.ojhrtf"}
    hashes = {}
    for line in (models_dir / "MODELS.sha256").read_text().splitlines():
        digest, name = line.split()
        if name not in expected_models or name in hashes or len(digest) != 64:
            raise ValueError(f"Invalid HRTF provenance entry: {line}")
        hashes[name] = digest
    if set(hashes) != expected_models:
        raise ValueError("Both original HRTF profiles must be packaged")

    with zipfile.ZipFile(aar) as package:
        entries = package.infolist()
        names = [entry.filename for entry in entries]
        models = [entry for entry in entries if entry.filename.endswith(".ojhrtf")]
        if {entry.filename for entry in models} != {
            "assets/openjoc/" + name for name in expected_models
        } or len(models) != 2:
            raise ValueError("Each HRTF profile must occur exactly once as a shared asset")
        for model in models:
            digest = hashlib.sha256(package.read(model)).hexdigest()
            if digest != hashes[Path(model.filename).name]:
                raise ValueError(f"Packaged HRTF data changed: {model.filename}")
        if package.read("assets/openjoc/MODELS.sha256") != (models_dir / "MODELS.sha256").read_bytes():
            raise ValueError("Packaged HRTF provenance does not match the build inputs")

        abis = {
            name.split("/")[1]
            for name in names
            if name.startswith("jni/") and name.endswith("/libmpv.so")
        }
        libraries = [
            entry for entry in entries if entry.filename.endswith("/libopenjoc_capi.so")
        ]
        if not abis or {entry.filename for entry in libraries} != {
            f"jni/{abi}/libopenjoc_capi.so" for abi in abis
        } or len(libraries) != len(abis):
            raise ValueError("Every packaged ABI must have exactly one OpenJOC library")
        for library in libraries:
            if library.file_size > 8 * 1024 * 1024:
                raise ValueError(f"HRTF data or debug bloat remains embedded in {library.filename}")

        overhead = sum(entry.compress_size for entry in models + libraries)
        if overhead > 45 * 1024 * 1024:
            raise ValueError(f"Shared OpenJOC payload exceeds the size budget: {overhead}")
        print(json.dumps({
            "aar": str(aar),
            "aar_bytes": aar.stat().st_size,
            "shared_model_compressed_bytes": sum(entry.compress_size for entry in models),
            "native_decoder_compressed_bytes": sum(entry.compress_size for entry in libraries),
            "openjoc_compressed_bytes": overhead,
            "abis": sorted(abis),
            "model_sha256": hashes,
        }, sort_keys=True))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify-openjoc-package.py PATH_TO_AAR")
    verify(Path(sys.argv[1]))
