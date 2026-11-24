#!/usr/bin/env python3
"""Reject cleanup calls whose failure was discarded by the Gate 3 probe engine."""

from pathlib import Path
import re
import sys


def main() -> int:
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    failures: list[str] = []
    ignored_drain = re.compile(r"\(void\)\s*router_\.Drain\s*\(\s*\)")
    ignored_decoder_cleanup = re.compile(r"\(void\)\s*decoder_\.(?:Stop|Destroy)\s*\(")
    ignored_surface_cleanup = re.compile(
        r"\(void\)\s*surfaces_\.(?:Release|CollectRetiredSurface|Shutdown)\s*\("
    )
    ignored_coordinated_shutdown = re.compile(r"\(void\)\s*cleanup_\.Shutdown\s*\(")
    bare_decoder_cleanup = re.compile(r"^\s*decoder_\.(?:Stop|Destroy)\s*\(\s*\)\s*;")
    bare_surface_shutdown = re.compile(r"^\s*surfaces_\.Shutdown\s*\(\s*\)\s*;")
    bare_coordinated_shutdown = re.compile(r"^\s*cleanup_\.Shutdown\s*\(")

    for line_number, line in enumerate(source.splitlines(), start=1):
        if ignored_drain.search(line):
            failures.append(f"{line_number}: router_.Drain() result is discarded")
        if ignored_decoder_cleanup.search(line) or bare_decoder_cleanup.search(line):
            failures.append(f"{line_number}: decoder Stop/Destroy result is not checked")
        if ignored_surface_cleanup.search(line) or bare_surface_shutdown.search(line):
            failures.append(
                f"{line_number}: Surface cleanup result is not checked"
            )
        if ignored_coordinated_shutdown.search(line) or bare_coordinated_shutdown.search(line):
            failures.append(f"{line_number}: coordinated Shutdown result is not checked")

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("probe engine cleanup contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
