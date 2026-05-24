#!/usr/bin/env python3
"""Check that docs/RESULTS.md still mirrors key native result rows."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
CURRENT = ROOT / "results" / "native" / "current.md"
SUMMARY = ROOT / "docs" / "RESULTS.md"


CHECKS = {
    "floor_ns": (
        r"\| empty same-hook BPF row \| [^|]+ \| ([0-9.]+) \|",
        r"\| floor \| empty same-hook raw tracepoint BPF row \| [^|]+ \| ([0-9.]+) \|",
    ),
    "100_compact_ns": (
        r"\| 100 typical compact hit \| [^|]+ \| ([0-9.]+) \|",
        r"\| 100 \| compact matched decision \| [^|]+ \| ([0-9.]+) \|",
    ),
    "1000_compact_ns": (
        r"\| 1000 typical compact hit \| [^|]+ \| ([0-9.]+) \|",
        r"\| 1000 \| compact matched decision \| [^|]+ \| ([0-9.]+) \|",
    ),
    "1000_drop_ns": (
        r"\| 1000 typical compact prefilter DROP \| [^|]+ \| ([0-9.]+) \|",
        r"\| 1000 \| compact matched DROP \| [^|]+ \| ([0-9.]+) \|",
    ),
    "lpm_bounded_1000_hit_ns": (
        r"\| 1000 typical hit \| [0-9.]+ \| [0-9.]+ \| ([0-9.]+) \|",
        r"\| 1000 typical hit \| [0-9.]+ \| [0-9.]+ \| ([0-9.]+) \|",
    ),
    "discard_ns": (
        r"\| discard-after-reserve \| [^|]+ \| ([0-9.]+) \|",
        r"\| discard-after-reserve \| ([0-9.]+) \| 8 B \| 0 B \|",
    ),
    "post_800_ns": (
        r"\| 800 \| 840 \| [^|]+ \| ([0-9.]+) \|",
        r"\| 800 \| ([0-9.]+) \| 610\.87 \|",
    ),
    "entry64_saved_ns": (
        r"\| 64 \| [0-9.]+ \| [0-9.]+ \| ([0-9.]+) \|",
        r"\| 64 \| [0-9.]+ \| [0-9.]+ \| ([0-9.]+) \|",
    ),
}


def extract(text: str, pattern: str, label: str, path: Path) -> str:
    match = re.search(pattern, text)
    if not match:
        raise ValueError(f"{label}: pattern not found in {path}")
    return match.group(1)


def main() -> int:
    current = CURRENT.read_text(encoding="utf-8")
    summary = SUMMARY.read_text(encoding="utf-8")
    mismatches = []

    for label, (current_re, summary_re) in CHECKS.items():
        current_value = extract(current, current_re, label, CURRENT)
        summary_value = extract(summary, summary_re, label, SUMMARY)
        if current_value != summary_value:
            mismatches.append((label, current_value, summary_value))

    if mismatches:
        for label, current_value, summary_value in mismatches:
            print(
                f"{label}: results/native/current.md={current_value} "
                f"docs/RESULTS.md={summary_value}",
                file=sys.stderr,
            )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
