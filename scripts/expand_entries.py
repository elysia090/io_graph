#!/usr/bin/env python3
"""Expand an io_graph blob's entry table for entry_id vs entry_idx benches."""

import argparse
import struct
import sys


HDR = struct.Struct("<IHH13I")
ENTRY = struct.Struct("<II")
IOG_MAGIC = 0x494F4752
IOG_VERSION = 1
MAX_ENTRIES = 256


def parse_args():
    parser = argparse.ArgumentParser(
        description="Copy a verified single-entry blob and replace its entry "
        "section with N entries that all point at the original entry state.")
    parser.add_argument("input", help="input .iog blob")
    parser.add_argument("output", help="output .iog blob")
    parser.add_argument("--entries", type=int, required=True,
                        help=f"entry count, 1..{MAX_ENTRIES}")
    parser.add_argument("--first-id", type=int, default=0,
                        help="first generated entry id")
    return parser.parse_args()


def die(message):
    print(f"expand_entries.py: {message}", file=sys.stderr)
    sys.exit(1)


def main():
    args = parse_args()
    if args.entries < 1 or args.entries > MAX_ENTRIES:
        die(f"--entries must be in 1..{MAX_ENTRIES}")
    if args.first_id < 0 or args.first_id + args.entries - 1 > 0xFFFFFFFF:
        die("--first-id range overflows u32")

    data = bytearray(open(args.input, "rb").read())
    if len(data) < HDR.size:
        die("input is shorter than io_graph header")

    fields = list(HDR.unpack_from(data, 0))
    magic, version = fields[0], fields[1]
    if magic != IOG_MAGIC or version != IOG_VERSION:
        die("input is not an io_graph v1 blob")

    (
        _magic, _version, _flags, node_cnt, edge_cnt, entry_cnt, accept_cnt,
        alphabet_size, initial_state, nodes_off, edges_off, entries_off,
        accepts_off, total_size, max_input_len, reserved,
    ) = fields

    if total_size != len(data):
        die("input has trailing bytes or a stale total_size")
    if entry_cnt < 1:
        die("input has no entries")
    if entries_off + entry_cnt * ENTRY.size > total_size:
        die("input entry section is out of bounds")
    if accepts_off > total_size:
        die("input accepts offset is out of bounds")

    _, state = ENTRY.unpack_from(data, entries_off)
    if state >= node_cnt:
        die("input first entry state is out of range")

    new_entries = bytearray()
    for i in range(args.entries):
        new_entries += ENTRY.pack(args.first_id + i, state)

    prefix = data[:entries_off]
    accepts = data[accepts_off:total_size]
    new_accepts_off = entries_off + len(new_entries)
    new_total_size = new_accepts_off + len(accepts)

    fields[5] = args.entries
    fields[12] = new_accepts_off
    fields[13] = new_total_size

    out = bytearray(prefix)
    HDR.pack_into(out, 0, *fields)
    out += new_entries
    out += accepts

    with open(args.output, "wb") as fp:
        fp.write(out)

    print(
        f"entries={args.entries} first_id={args.first_id} "
        f"entry_state={state} total_size={new_total_size}"
    )


if __name__ == "__main__":
    main()
