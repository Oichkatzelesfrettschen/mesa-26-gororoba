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
a mutated row, a deleted row, and a missing contract file each fail for
their declared reason.
"""

import hashlib
import os
import sys
import tempfile

PIN_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "r3v_kernel_memory_contract_pins.tsv")


def load_pins(path):
    pins = []
    with open(path, encoding="utf-8") as handle:
        header = handle.readline().rstrip("\n")
        if header != "contract_file\trow_id\tsha256":
            raise SystemExit(f"pin header mismatch: {header!r}")
        for line in handle:
            contract_file, row_id, digest = line.rstrip("\n").split("\t")
            pins.append((contract_file, row_id, digest))
    if not pins:
        raise SystemExit("pin file carries no rows")
    return pins


def check(root, pins):
    """Returns a list of failure strings; empty means every pin holds."""
    failures = []
    for contract_file, row_id, expected in pins:
        path = os.path.join(root, contract_file)
        if not os.path.isfile(path):
            failures.append(f"missing contract file: {contract_file}")
            continue
        rows = {}
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                rows[line.split("\t", 1)[0]] = line.rstrip("\n")
        if row_id not in rows:
            failures.append(f"{contract_file}: row {row_id} absent")
            continue
        actual = hashlib.sha256(rows[row_id].encode("utf-8")).hexdigest()
        if actual != expected:
            failures.append(
                f"{contract_file}: row {row_id} digest {actual} != pinned "
                f"{expected}")
    return failures


def selftest():
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "policy"))
        contract = os.path.join(root, "policy", "fixture.tsv")
        row = "FIXTURE_ROW\tvalue-a\tvalue-b"
        with open(contract, "w", encoding="utf-8") as handle:
            handle.write("row_id\tfield_a\tfield_b\n" + row + "\n")
        digest = hashlib.sha256(row.encode("utf-8")).hexdigest()
        pins = [("policy/fixture.tsv", "FIXTURE_ROW", digest)]

        good = check(root, pins)
        assert good == [], f"known-good fixture failed: {good}"

        with open(contract, "w", encoding="utf-8") as handle:
            handle.write("row_id\tfield_a\tfield_b\n" +
                         row.replace("value-b", "value-mutated") + "\n")
        mutated = check(root, pins)
        assert len(mutated) == 1 and "digest" in mutated[0], mutated

        with open(contract, "w", encoding="utf-8") as handle:
            handle.write("row_id\tfield_a\tfield_b\n")
        deleted = check(root, pins)
        assert len(deleted) == 1 and "absent" in deleted[0], deleted

        os.unlink(contract)
        missing = check(root, pins)
        assert len(missing) == 1 and "missing contract file" in missing[0], \
            missing
    print("selftest: 4 calibrations OK")


def main():
    if "--selftest" in sys.argv:
        selftest()
        return 0
    pins = load_pins(PIN_FILE)
    root = os.environ.get("R3V_KERNEL_MEMORY_CONTRACT_ROOT")
    if not root:
        print("R3V_KERNEL_MEMORY_CONTRACT_ROOT unset: kernel checkout "
              "unavailable, contract not checked")
        return 77
    failures = check(root, pins)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"{len(failures)} pinned kernel-contract rows moved; the "
              "memory model's kernel assumptions need re-derivation",
              file=sys.stderr)
        return 1
    print(f"{len(pins)} pinned kernel-contract rows hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
