#!/usr/bin/env python3
"""Validate DeskMate Gate 3 evidence records without inventing device data."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ALLOWED_STATES = {"passed", "failed", "pending", "not executed"}
SCHEMA = "deskmate.gate3.records.v1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def require_state(value: object, where: str) -> str:
    require(isinstance(value, str) and value in ALLOWED_STATES, f"{where}: invalid state {value!r}")
    return value


def validate_common(record: dict, path: Path) -> None:
    require(record.get("schema") == SCHEMA, f"{path}: schema must be {SCHEMA}")
    require(isinstance(record.get("record_kind"), str), f"{path}: record_kind is required")
    require(isinstance(record.get("created_at"), str), f"{path}: created_at is required")
    interop = record.get("annexb_host_sample_interop")
    require(isinstance(interop, dict), f"{path}: annexb_host_sample_interop is required")
    state = require_state(interop.get("state"), f"{path}: annexb_host_sample_interop")
    if state == "passed":
        require(bool(interop.get("sample")), f"{path}: passed interop needs the real Host sample")
    else:
        require(bool(interop.get("reason")), f"{path}: non-passed interop needs a reason")


def validate_local(record: dict, path: Path) -> None:
    checks = record.get("checks")
    require(isinstance(checks, list) and checks, f"{path}: checks must be a non-empty list")
    ids = set()
    for index, check in enumerate(checks):
        where = f"{path}: checks[{index}]"
        require(isinstance(check, dict), f"{where}: must be an object")
        check_id = check.get("id")
        require(isinstance(check_id, str) and check_id, f"{where}: id is required")
        require(check_id not in ids, f"{where}: duplicate id {check_id}")
        ids.add(check_id)
        state = require_state(check.get("state"), where)
        require(bool(check.get("contract")), f"{where}: contract is required")
        if state == "passed":
            require(bool(check.get("observed")), f"{where}: passed check needs observed output")
    require("surface-callback-invalidation" in ids, f"{path}: surface callback regression check missing")
    require("no-cpu-pixel-readback-source-audit" in ids, f"{path}: no-readback audit missing")


def validate_device(record: dict, path: Path) -> None:
    device = record.get("device")
    require(isinstance(device, dict), f"{path}: device identity is required")
    for field in ("productModel", "serial", "osFullName", "sdkApiVersion", "resolution"):
        require(device.get(field) not in (None, ""), f"{path}: device.{field} is required")
    require(device.get("identity_source") in {"task-brief", "hdc", "hdc+task-brief"},
            f"{path}: device identity must state task-brief, hdc, or hdc+task-brief")

    build = record.get("build")
    require(isinstance(build, dict), f"{path}: build section is required")
    build_state = require_state(build.get("state"), f"{path}: build")
    if build_state == "passed":
        require(bool(build.get("hap_sha256")), f"{path}: built HAP needs sha256")
        require(build.get("signature") == "unsigned", f"{path}: this build must be recorded unsigned")
    else:
        require(build.get("measurements") is None, f"{path}: unbuilt build cannot include measurements")

    install = record.get("install")
    run = record.get("run")
    require(isinstance(install, dict) and isinstance(run, dict),
            f"{path}: install and run sections are required")
    install_state = require_state(install.get("state"), f"{path}: install")
    run_state = require_state(run.get("state"), f"{path}: run")
    if install_state != "passed":
        require(bool(install.get("reason")), f"{path}: non-passed install needs a reason")
        require(run_state == "not executed", f"{path}: run cannot execute after failed install")
        require(record.get("measurements") is None,
                f"{path}: uninstalled probe cannot claim device measurements")
    if run_state != "passed":
        require(bool(run.get("reason")), f"{path}: non-passed run needs a reason")
        require(record.get("measurements") is None,
                f"{path}: unrun probe cannot claim device measurements")
    require(record.get("no_cpu_pixel_readback") is True,
            f"{path}: no_cpu_pixel_readback must be true by design")


def validate_file(path: Path) -> None:
    record = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(record, dict), f"{path}: record must be a JSON object")
    validate_common(record, path)
    if record["record_kind"] == "local-host":
        validate_local(record, path)
    elif record["record_kind"] == "device":
        validate_device(record, path)
    else:
        raise ValueError(f"{path}: unknown record_kind {record['record_kind']!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("records", nargs="?", default="records")
    args = parser.parse_args()
    root = Path(args.records)
    files = sorted(root.glob("*.json")) if root.is_dir() else [root]
    require(bool(files), f"{root}: no JSON records found")
    for path in files:
        validate_file(path)
        print(f"{path}: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"record validation failed: {error}", file=sys.stderr)
        sys.exit(1)
