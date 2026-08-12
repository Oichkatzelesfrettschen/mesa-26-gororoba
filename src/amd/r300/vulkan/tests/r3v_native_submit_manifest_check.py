# SPDX-License-Identifier: MIT
#
# Checks that the submit-object manifest binds evidence to the executable
# that issued the ioctl-shaped submission and remains parser-valid JSON.

import json
import os
import re
import struct
import subprocess
import sys
import tempfile


def fail(message, *outputs):
    print("FAIL: " + message, file=sys.stderr)
    for output in outputs:
        print(output, file=sys.stderr)
    return 1


def read_bytes(path):
    with open(path, "rb") as artifact:
        return artifact.read()


def relocation_struct(byte_order=None):
    if byte_order is None:
        byte_order = sys.byteorder
    if byte_order not in ("little", "big"):
        raise ValueError("unsupported byte order")
    prefix = "<" if byte_order == "little" else ">"
    return struct.Struct(prefix + "4I")


def decode_relocations(reloc_bytes, byte_order=None):
    decoder = relocation_struct(byte_order)
    if len(reloc_bytes) % decoder.size != 0:
        raise ValueError("submit relocation bytes are not whole entries")
    return list(decoder.iter_unpack(reloc_bytes))


def submit_identity_error(manifest, reloc_bytes, byte_order=None):
    try:
        relocations = decode_relocations(reloc_bytes, byte_order)
    except ValueError as error:
        return str(error)
    rows = manifest.get("bo_table")
    if not isinstance(rows, list) or len(rows) != len(relocations):
        return "bo_table length differs from submit relocation count"
    if manifest.get("reloc_count") != len(relocations):
        return "manifest relocation count differs from submit bytes"

    completion_rows = 0
    for index, (relocation, row) in enumerate(zip(relocations, rows)):
        if not isinstance(row, dict):
            return "bo_table row is not an object"
        if row.get("reloc_index") != index:
            return "bo_table relocation index is not final-list order"
        for field, word in zip(("handle", "read_domains", "write_domain",
                                "flags"), relocation):
            if field != "flags" and row.get(field) != word:
                return "bo_table field differs from submit relocation bytes"
        role = row.get("role")
        if role == "completion":
            completion_rows += 1
        elif role != "command":
            return "bo_table row has no recognized role"
    if completion_rows != 1:
        return "bo_table does not identify one completion relocation"
    return None


def native_relocation_calibration_error():
    words = (0x01020304, 0x10203040, 0x55667788, 0x99AABBCC)
    for byte_order in ("little", "big"):
        decoder = relocation_struct(byte_order)
        fixture = decoder.pack(*words)
        if decode_relocations(fixture, byte_order) != [words]:
            return "native relocation fixture did not decode"
        opposite_order = "big" if byte_order == "little" else "little"
        if decode_relocations(fixture, opposite_order) == [words]:
            return "byte-swapped relocation fixture was accepted"
    return None


def native_identity_path_calibration_error():
    malformed = json.loads(
        '{"driver_elf_path":"path\\udcff\\udcc3x"}')
    malformed_path = malformed.get("driver_elf_path")
    try:
        malformed_bytes = os.fsencode(malformed_path)
    except UnicodeEncodeError as error:
        return "surrogate path encoding failed: " + str(error)
    if malformed_bytes != b"path\xff\xc3x":
        return "surrogate path bytes changed during os.fsencode"

    utf8 = json.loads('{"driver_elf_path":"path\\u00ffx"}')
    if os.fsencode(utf8["driver_elf_path"]) != b"path\xc3\xbfx":
        return "UTF-8 path bytes changed during os.fsencode"
    return None


def duplicate_relocation_model_error():
    references = [
        (31, 0x1, 0),
        (47, 0, 0x2),
        (31, 0, 0x4),
    ]
    index_by_handle = {}
    relocations = []
    indices = []
    for handle, read_domains, write_domain in references:
        index = index_by_handle.get(handle)
        if index is None:
            index = len(relocations)
            index_by_handle[handle] = index
            relocations.append([handle, read_domains, write_domain, 0])
        else:
            relocations[index][1] |= read_domains
            relocations[index][2] |= write_domain
        indices.append(index)

    if indices != [0, 1, 0] or len(relocations) != 2:
        return "duplicate relocation model lost the returned index"

    completion_index = len(relocations)
    relocations.append([53, 0, 0x2, 0])
    rows = [
        {
            "reloc_index": index,
            "handle": relocation[0],
            "read_domains": relocation[1],
            "write_domain": relocation[2],
            "role": "completion" if index == completion_index else "command",
        }
        for index, relocation in enumerate(relocations)
    ]
    manifest = {"reloc_count": len(relocations), "bo_table": rows}
    native_decoder = relocation_struct()
    reloc_bytes = b"".join(native_decoder.pack(*relocation)
                             for relocation in relocations)
    error = submit_identity_error(manifest, reloc_bytes)
    if error is not None:
        return "duplicate relocation model failed identity check: " + error

    mutant = {"reloc_count": len(relocations),
              "bo_table": [dict(row) for row in rows]}
    mutant["bo_table"][0]["reloc_index"] = 1
    if submit_identity_error(mutant, reloc_bytes) is None:
        return "duplicate relocation identity mutant was accepted"
    return None


def main():
    if len(sys.argv) != 2:
        print("usage: r3v_native_submit_manifest_check.py <harness>",
              file=sys.stderr)
        return 2
    harness = sys.argv[1]
    triangle_harness = os.path.join(
        os.path.dirname(harness), "r3v_native_triangle_cell_harness")
    environment = dict(os.environ)

    endian_error = native_relocation_calibration_error()
    if endian_error is not None:
        return fail("native relocation calibration failed", endian_error)

    identity_path_error = native_identity_path_calibration_error()
    if identity_path_error is not None:
        return fail("native identity path calibration failed",
                    identity_path_error)

    model_error = duplicate_relocation_model_error()
    if model_error is not None:
        return fail("duplicate relocation calibration failed", model_error)

    with tempfile.TemporaryDirectory() as manifest_dir:
        environment["R3V_NATIVE_MANIFEST_DIR"] = manifest_dir
        run = subprocess.run([harness, "open"], env=environment,
                             capture_output=True, text=True)
        if run.returncode != 0:
            return fail("drm-shim open leg failed", run.stdout, run.stderr)

        submit_manifest_path = os.path.join(manifest_dir,
                                            "submit_manifest.json")
        try:
            with open(submit_manifest_path, encoding="utf-8") as manifest_file:
                manifest_text = manifest_file.read()
            manifest = json.loads(manifest_text)
        except (OSError, json.JSONDecodeError) as error:
            return fail("submit manifest is not parser-valid JSON", error)

        if manifest.get("object") != "submit-object":
            return fail("manifest names no submit object", manifest)
        elf_path = manifest.get("driver_elf_path")
        if not isinstance(elf_path, str) or not os.path.isabs(elf_path):
            return fail("manifest carries no absolute issuer path", elf_path)
        try:
            elf_path_bytes = os.fsencode(elf_path)
            harness_bytes = os.fsencode(harness)
        except UnicodeEncodeError as error:
            return fail("manifest issuer path cannot be filesystem-encoded",
                        error)
        if not os.path.isabs(elf_path_bytes):
            return fail("manifest issuer path is not absolute", elf_path)
        if elf_path == "unresolved":
            return fail("manifest carries unresolved issuer identity")
        try:
            same_issuer = os.path.samefile(elf_path_bytes, harness_bytes)
        except OSError as error:
            return fail("manifest issuer path is not present", error)
        if not same_issuer:
            return fail("manifest issuer differs from the submitting "
                        "executable", elf_path, harness)

        digest = manifest.get("driver_elf_blake3")
        if not isinstance(digest, str) or \
                re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            return fail("manifest issuer digest is not lowercase BLAKE3",
                        digest)

        # A literal control byte is the calibrated bad JSON mutant.  The
        # parser must reject it, so a check that only searches for field
        # names cannot stand in for JSON escaping.
        mutant = manifest_text.replace(
            '"driver_elf_path": "', '"driver_elf_path": "bad\n', 1)
        if mutant == manifest_text:
            return fail("manifest mutant did not alter the issuer field")
        try:
            json.loads(mutant)
        except json.JSONDecodeError:
            pass
        else:
            return fail("malformed JSON mutant was accepted")

        artifact_names = (
            "ib.bin",
            "relocs.bin",
            "manifest.json",
            "submit_relocs.bin",
            "submit_manifest.json",
        )
        baseline = {}
        for name in artifact_names:
            path = os.path.join(manifest_dir, name)
            try:
                baseline[name] = read_bytes(path)
            except OSError as error:
                return fail("initial retained artifact is unreadable", name,
                            error)

        identity_error = submit_identity_error(
            manifest, baseline["submit_relocs.bin"])
        if identity_error is not None:
            return fail("submit manifest does not bind submit_relocs.bin",
                        identity_error)
        mutant_manifest = json.loads(manifest_text)
        mutant_manifest["bo_table"][0]["reloc_index"] = 1
        if submit_identity_error(mutant_manifest,
                                 baseline["submit_relocs.bin"]) is None:
            return fail("relocation-index manifest mutant was accepted")

        # A reused evidence directory is a known-bad submit destination.  A
        # stale semantic manifest must remain untouched, and no other byte
        # may be replaced while the second submit is refused.
        stale_semantic_manifest = b"stale semantic manifest\n"
        semantic_manifest_path = os.path.join(manifest_dir, "manifest.json")
        with open(semantic_manifest_path, "wb") as artifact:
            artifact.write(stale_semantic_manifest)
        reused = subprocess.run([harness, "open"], env=environment,
                                capture_output=True, text=True)
        if reused.returncode == 0:
            return fail("reused evidence directory was accepted", reused.stdout,
                        reused.stderr)
        try:
            if read_bytes(semantic_manifest_path) != stale_semantic_manifest:
                return fail("stale semantic manifest was replaced", reused.stderr)
            for name, expected in baseline.items():
                if name == "manifest.json":
                    continue
                if read_bytes(os.path.join(manifest_dir, name)) != expected:
                    return fail("reused evidence changed " + name,
                                reused.stderr)
        except OSError as error:
            return fail("reused evidence artifact disappeared", error)

    # A stale semantic manifest must block the semantic artifact group before
    # any replacement bytes are published.
    with tempfile.TemporaryDirectory() as stale_dir:
        stale_path = os.path.join(stale_dir, "manifest.json")
        stale_bytes = b"stale semantic manifest\n"
        with open(stale_path, "wb") as artifact:
            artifact.write(stale_bytes)
        stale_environment = dict(os.environ)
        stale_environment["R3V_NATIVE_MANIFEST_DIR"] = stale_dir
        run = subprocess.run([harness, "open"], env=stale_environment,
                             capture_output=True, text=True)
        if run.returncode == 0:
            return fail("stale semantic artifact group was accepted", run.stdout,
                        run.stderr)
        if read_bytes(stale_path) != stale_bytes:
            return fail("stale semantic manifest changed", run.stderr)
        for name in ("ib.bin", "relocs.bin"):
            if os.path.exists(os.path.join(stale_dir, name)):
                return fail("semantic artifact published beside stale manifest",
                            name, run.stderr)

    # A stale submit manifest must block the exact-submit artifact group before
    # submit_relocs.bin can create a mixed bundle.
    with tempfile.TemporaryDirectory() as stale_dir:
        stale_path = os.path.join(stale_dir, "submit_manifest.json")
        stale_bytes = b"stale submit manifest\n"
        with open(stale_path, "wb") as artifact:
            artifact.write(stale_bytes)
        stale_environment = dict(os.environ)
        stale_environment["R3V_NATIVE_MANIFEST_DIR"] = stale_dir
        run = subprocess.run([harness, "open"], env=stale_environment,
                             capture_output=True, text=True)
        if run.returncode == 0:
            return fail("stale submit artifact group was accepted", run.stdout,
                        run.stderr)
        if read_bytes(stale_path) != stale_bytes:
            return fail("stale submit manifest changed", run.stderr)
        if os.path.exists(os.path.join(stale_dir, "submit_relocs.bin")):
            return fail("submit relocation bytes published beside stale "
                        "manifest", run.stderr)

    # A prior attempt token is a known-bad destination.  The open gate must
    # reject it before either semantic or submit-object retention runs.
    with tempfile.TemporaryDirectory() as attempted_dir:
        token_path = os.path.join(attempted_dir, "attempt.token")
        token_bytes = b"prior attempt\n"
        with open(token_path, "wb") as artifact:
            artifact.write(token_bytes)
        attempted_environment = dict(os.environ)
        attempted_environment["R3V_NATIVE_MANIFEST_DIR"] = attempted_dir
        run = subprocess.run([triangle_harness, "open"],
                             env=attempted_environment,
                             capture_output=True, text=True)
        if run.returncode == 0:
            return fail("attempt token was accepted", run.stdout, run.stderr)
        if read_bytes(token_path) != token_bytes:
            return fail("attempt token changed", run.stderr)
        for name in artifact_names:
            if os.path.exists(os.path.join(attempted_dir, name)):
                return fail("attempt token did not preclude " + name,
                            run.stderr)

    # A multi-command submit is a known-bad retained shape.  The queue
    # refuses it before the first command executes or any artifact is born.
    with tempfile.TemporaryDirectory() as multi_dir:
        multi_environment = dict(os.environ)
        multi_environment["R3V_NATIVE_MANIFEST_DIR"] = multi_dir
        run = subprocess.run([triangle_harness, "multi"],
                             env=multi_environment,
                             capture_output=True, text=True)
        if run.returncode != 0:
            return fail("multi-command retention refusal check failed",
                        run.stdout, run.stderr)
        for name in artifact_names:
            if os.path.exists(os.path.join(multi_dir, name)):
                return fail("multi-command retention published " + name,
                            run.stderr)

    # An empty value is a known-bad destination, not a path rooted at /.  The
    # harness checks the captured native device state before queue submission.
    empty_environment = dict(os.environ)
    empty_environment["R3V_NATIVE_MANIFEST_DIR"] = ""
    run = subprocess.run([triangle_harness, "empty"], env=empty_environment,
                         capture_output=True, text=True)
    if run.returncode != 0:
        return fail("empty manifest path was not rejected", run.stdout,
                    run.stderr)

    print("r3v_native_submit_manifest_check: issuer identity and JSON "
          "escaping and retention freshness hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
