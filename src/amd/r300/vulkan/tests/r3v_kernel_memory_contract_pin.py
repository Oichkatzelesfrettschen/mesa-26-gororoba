#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pin the kernel memory-contract rows the R3V memory model consumes.

The R3V memory model rests on kernel-side facts recorded as rows of the
linux-radeon-gororoba policy ledgers (cache attribute normalization, TTM
cached selection, snoop encoding and the global snoop disable, the
payload publication/invalidation ownership, placement order, the
single-BO GTT ceiling, pinned-capacity accounting, and the capacity
counters).  This check binds each consumed row to a SHA-256 of its exact
ledger line, checked into Mesa beside this script.  A changed row means
the kernel contract moved under the driver's assumptions, so a digest
mismatch fails the test; it does not skip.  An absent kernel checkout
(no R3V_KERNEL_MEMORY_CONTRACT_ROOT) exits 77, the meson SKIP verdict,
because Mesa builds stand alone.

The selftest calibrates on synthetic fixtures: a matching pin passes,
a mutated row, a deleted row, a missing contract file, a duplicate row,
and a changed header each fail for their declared reason.  The checks use
explicit failures instead of ``assert`` so calibration remains active under
optimized Python.
"""

import hashlib
import os
import sys
import tempfile

PIN_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "r3v_kernel_memory_contract_pins.tsv"
)
SCHEMA_PIN_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "r3v_kernel_memory_contract_schemas.tsv"
)
PIN_HEADER = "contract_file\trow_id\tsha256"
SCHEMA_PIN_HEADER = "contract_file\theader_sha256"


def load_pins(path):
    pins = []
    with open(path, encoding="utf-8") as handle:
        header = handle.readline().rstrip("\n")
        if header != PIN_HEADER:
            raise SystemExit(f"pin header mismatch: {header!r}")
        seen = set()
        for line in handle:
            fields = line.rstrip("\n").split("\t")
            if len(fields) != 3:
                raise SystemExit(f"pin row has {len(fields)} fields: {line!r}")
            contract_file, row_id, digest = fields
            key = (contract_file, row_id)
            if key in seen:
                raise SystemExit(
                    f"duplicate pin manifest row: {contract_file}:{row_id}"
                )
            seen.add(key)
            pins.append((contract_file, row_id, digest))
    if not pins:
        raise SystemExit("pin file carries no rows")
    return pins


def load_schema_pins(path):
    schemas = []
    with open(path, encoding="utf-8") as handle:
        header = handle.readline().rstrip("\n")
        if header != SCHEMA_PIN_HEADER:
            raise SystemExit(f"schema pin header mismatch: {header!r}")
        seen = set()
        for line in handle:
            fields = line.rstrip("\n").split("\t")
            if len(fields) != 2:
                raise SystemExit(f"schema pin row has {len(fields)} fields: {line!r}")
            contract_file, digest = fields
            if contract_file in seen:
                raise SystemExit(f"duplicate schema pin: {contract_file}")
            seen.add(contract_file)
            schemas.append((contract_file, digest))
    if not schemas:
        raise SystemExit("schema pin file carries no contracts")
    return schemas


def load_contract(root, contract_file):
    path = os.path.join(root, contract_file)
    if not os.path.isfile(path):
        return None, {}, [f"missing contract file: {contract_file}"]
    with open(path, encoding="utf-8") as handle:
        lines = handle.readlines()
    header = lines[0].rstrip("\n") if lines else ""
    if header.startswith("row_id\t"):
        data_lines, start_line = lines[1:], 2
    else:
        data_lines, start_line = lines, 1
    rows = {}
    failures = []
    for line_number, line in enumerate(data_lines, start_line):
        row = line.rstrip("\n")
        current_id = row.split("\t", 1)[0]
        if current_id in rows:
            failures.append(
                f"{contract_file}: duplicate row {current_id} " f"at line {line_number}"
            )
            continue
        rows[current_id] = row
    return header, rows, failures


def check(root, pins, schema_pins=None):
    """Returns a list of failure strings; empty means every pin holds."""
    failures = []
    if schema_pins is None:
        configured_schemas = load_schema_pins(SCHEMA_PIN_FILE)
        pinned_files = {contract_file for contract_file, _, _ in pins}
        schema_pins = [
            (contract_file, digest)
            for contract_file, digest in configured_schemas
            if contract_file in pinned_files
        ]
    contracts = {}

    def contract(path):
        if path not in contracts:
            contracts[path] = load_contract(root, path)
        return contracts[path]

    for contract_file, expected in schema_pins:
        header, _, contract_failures = contract(contract_file)
        failures.extend(contract_failures)
        if header is None:
            continue
        actual = hashlib.sha256(header.encode("utf-8")).hexdigest()
        if actual != expected:
            failures.append(
                f"{contract_file}: schema digest {actual} != pinned {expected}"
            )
    for contract_file, row_id, expected in pins:
        header, rows, contract_failures = contract(contract_file)
        failures.extend(contract_failures)
        if header is None:
            continue
        if row_id not in rows:
            failures.append(f"{contract_file}: row {row_id} absent")
            continue
        actual = hashlib.sha256(rows[row_id].encode("utf-8")).hexdigest()
        if actual != expected:
            failures.append(
                f"{contract_file}: row {row_id} digest {actual} != pinned "
                f"{expected}"
            )
    return list(dict.fromkeys(failures))


def selftest():
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "policy"))
        contract = os.path.join(root, "policy", "fixture.tsv")
        header = "row_id\tfield_a\tfield_b"
        row = "FIXTURE_ROW\tvalue-a\tvalue-b"
        with open(contract, "w", encoding="utf-8") as handle:
            handle.write(header + "\n" + row + "\n")
        digest = hashlib.sha256(row.encode("utf-8")).hexdigest()
        schema_digest = hashlib.sha256(header.encode("utf-8")).hexdigest()
        pins = [("policy/fixture.tsv", "FIXTURE_ROW", digest)]
        schemas = [("policy/fixture.tsv", schema_digest)]

        def expect(condition, message):
            if not condition:
                raise RuntimeError(message)

        good = check(root, pins, schemas)
        expect(good == [], f"known-good fixture failed: {good}")

        with open(contract, "w", encoding="utf-8") as handle:
            handle.write(header + "\n" + row.replace("value-b", "value-mutated") + "\n")
        mutated = check(root, pins, schemas)
        expect(len(mutated) == 1 and "digest" in mutated[0], mutated)

        with open(contract, "w", encoding="utf-8") as handle:
            handle.write(header + "\n")
        deleted = check(root, pins, schemas)
        expect(len(deleted) == 1 and "absent" in deleted[0], deleted)

        with open(contract, "w", encoding="utf-8") as handle:
            handle.write(header + "\n" + row + "\n" + row + "\n")
        duplicate = check(root, pins, schemas)
        expect(
            len(duplicate) == 1 and "duplicate row FIXTURE_ROW" in duplicate[0],
            duplicate,
        )

        changed_header = "row_id\tfield_b\tfield_a"
        with open(contract, "w", encoding="utf-8") as handle:
            handle.write(changed_header + "\n" + row + "\n")
        header_mutation = check(root, pins, schemas)
        expect(
            len(header_mutation) == 1 and "schema digest" in header_mutation[0],
            header_mutation,
        )

        os.unlink(contract)
        missing = check(root, pins, schemas)
        expect(
            len(missing) == 1
            and all("missing contract file" in item for item in missing),
            missing,
        )
    print("selftest: 6 calibrations OK")


def main():
    if "--selftest" in sys.argv:
        selftest()
        return 0
    pins = load_pins(PIN_FILE)
    schemas = load_schema_pins(SCHEMA_PIN_FILE)
    root = os.environ.get("R3V_KERNEL_MEMORY_CONTRACT_ROOT")
    if not root:
        print(
            "R3V_KERNEL_MEMORY_CONTRACT_ROOT unset: kernel checkout "
            "unavailable, contract not checked"
        )
        return 77
    failures = check(root, pins, schemas)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(
            f"{len(failures)} pinned kernel-contract rows moved; the "
            "memory model's kernel assumptions need re-derivation",
            file=sys.stderr,
        )
        return 1
    print(f"{len(pins)} pinned kernel-contract rows hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
