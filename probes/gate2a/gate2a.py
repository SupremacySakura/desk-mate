#!/usr/bin/env python3

import argparse
import datetime
import hashlib
import ipaddress
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


NATIVE_NCM_BUNDLES = {
    "com.apple.driver.usb.cdc.ncm",
}
NATIVE_ECM_BUNDLES = {
    "com.apple.driver.usb.cdc.ecm",
    "com.apple.DriverKit.AppleUserECM",
}


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def rndis_detected(capture):
    descriptors = capture.get("usb_device", {}).get("interfaces", [])
    for descriptor in descriptors:
        if (
            descriptor.get("interface_class") == 224
            and descriptor.get("interface_subclass") == 1
            and descriptor.get("interface_protocol") == 3
        ):
            return True
        if "rndis" in descriptor.get("name", "").lower():
            return True

    for interface in capture.get("network_interfaces", []):
        text = " ".join(
            str(interface.get(field, ""))
            for field in ("name", "driver_class", "driver_bundle_id", "descriptor_family")
        ).lower()
        if "rndis" in text:
            return True
    return False


def descriptor_family(descriptor):
    interface_class = descriptor.get("interface_class")
    interface_subclass = descriptor.get("interface_subclass")
    interface_protocol = descriptor.get("interface_protocol")
    if (
        interface_class == 224
        and interface_subclass == 1
        and interface_protocol == 3
    ):
        return "rndis"
    if interface_class == 2 and interface_subclass == 13:
        return "ncm"
    if interface_class == 2 and interface_subclass == 6:
        return "ecm"
    return None


def interface_family(interface, descriptors):
    text = " ".join(
        str(interface.get(field, ""))
        for field in (
            "name",
            "driver_class",
            "driver_bundle_id",
            "descriptor_family",
        )
    ).lower()
    if "rndis" in text:
        return "rndis"

    declared = interface.get("descriptor_family")
    if declared in {"ncm", "ecm"}:
        return declared

    bundle = interface.get("driver_bundle_id", "")
    if bundle in NATIVE_NCM_BUNDLES or ".ncm" in bundle.lower():
        return "ncm"
    if bundle in NATIVE_ECM_BUNDLES or ".ecm" in bundle.lower():
        return "ecm"

    families = {descriptor_family(descriptor) for descriptor in descriptors}
    families.discard(None)
    if len(families) == 1:
        return families.pop()
    return None


def network_interface_for_device(capture):
    device = capture.get("usb_device", {})
    serial = device.get("serial")
    candidates = []
    for interface in capture.get("network_interfaces", []):
        serial_matches = bool(serial) and interface.get("usb_serial") == serial
        id_matches = (
            interface.get("usb_id_vendor") == device.get("id_vendor")
            and interface.get("usb_id_product") == device.get("id_product")
            and interface.get("usb_serial") == serial
        )
        if serial_matches or id_matches:
            candidates.append(interface)
    if len(candidates) == 1:
        return candidates[0]
    return None


def build_topology(device):
    hubs = device.get("hubs", [])
    reference_link = not hubs
    topology = {
        "ancestors": device.get("ancestors", []),
        "hub_count": len(hubs),
        "hubs": hubs,
        "negotiated_speed_mbps": device.get("link_speed_mbps"),
        "direct_to_host": reference_link,
        "reference_link": reference_link,
    }
    if hubs:
        topology["reason"] = "device path contains USB hub(s), so it is not a Reference Link"
    else:
        topology["reason"] = "device is directly attached to the Mac USB controller"
    return topology


def classify(capture):
    rndis = rndis_detected(capture)
    if rndis:
        return {
            "driver_family": "rndis",
            "interface": None,
            "rndis_detected": True,
            "native_bundled_driver": False,
            "eligible": False,
            "topology": build_topology(capture.get("usb_device", {})),
            "decision": {
                "status": "failed",
                "reasons": [
                    "RNDIS is disqualified; Gate 2A accepts only native bundled NCM or ECM"
                ],
            },
        }

    device = capture.get("usb_device", {})
    interface = network_interface_for_device(capture)
    if interface is None:
        return {
            "driver_family": "none",
            "interface": None,
            "rndis_detected": False,
            "native_bundled_driver": False,
            "eligible": False,
            "topology": build_topology(device),
            "decision": {
                "status": "pending",
                "reasons": ["no NCM/ECM network interface is associated with the target device"],
            },
        }

    family = interface_family(interface, device.get("interfaces", []))
    bundle = interface.get("driver_bundle_id", "")
    native = (
        family == "ncm"
        and bundle in NATIVE_NCM_BUNDLES
        or family == "ecm"
        and bundle in NATIVE_ECM_BUNDLES
    )
    topology = build_topology(device)
    driver_eligible = family in {"ncm", "ecm"} and native
    eligible = driver_eligible and topology["reference_link"]
    reasons = []
    if family not in {"ncm", "ecm"}:
        reasons.append("associated interface is not NCM or ECM")
    if not native:
        reasons.append("network driver is not a native bundled Apple NCM/ECM driver")
    if not topology["reference_link"]:
        reasons.append("device path is not a direct Reference Link")
    if eligible:
        reasons.append("reference-link throughput, RTT, and jitter measurements are pending")
    if driver_eligible and not topology["reference_link"]:
        status = "pending"
    elif driver_eligible:
        status = "pending"
    else:
        status = "failed"
    return {
        "driver_family": family or "unknown",
        "interface": interface.get("name"),
        "network_driver_class": interface.get("driver_class"),
        "network_driver_bundle_id": bundle,
        "rndis_detected": False,
        "native_bundled_driver": native,
        "eligible": eligible,
        "topology": topology,
        "decision": {
            "status": status,
            "reasons": reasons,
        },
    }


def value_at(document, dotted_path):
    value = document
    for part in dotted_path.split("."):
        if not isinstance(value, dict) or part not in value:
            raise KeyError(dotted_path)
        value = value[part]
    return value


def require_path(report, dotted_path, errors, allow_none=False):
    try:
        value = value_at(report, dotted_path)
    except KeyError:
        errors.append(f"{dotted_path}: missing")
        return None
    if value is None and not allow_none:
        errors.append(f"{dotted_path}: must not be null")
    elif isinstance(value, str) and not value.strip() and not allow_none:
        errors.append(f"{dotted_path}: must not be empty")
    return value


def nonempty_string(value):
    return isinstance(value, str) and bool(value.strip())


def valid_sha256(value):
    return isinstance(value, str) and bool(re.fullmatch(r"[0-9a-f]{64}", value))


def validate_measurement(measurement, interface, reference_link):
    errors = []
    if not isinstance(measurement, dict):
        return ["measurements: must be an object"]
    if measurement.get("status") != "executed":
        errors.append("measurements.status: must be executed")
    if measurement.get("reference_link") is not True:
        errors.append("measurements.reference_link: must be true")
    if not reference_link:
        errors.append("measurements.reference_link: current topology is not a Reference Link")
    if not nonempty_string(interface) or measurement.get("interface") != interface:
        errors.append(
            "measurements.interface: must match the associated Device interface"
        )
    try:
        ipaddress.ip_address(str(measurement.get("target")))
    except ValueError:
        errors.append("measurements.target: must be an IP address")

    ping = measurement.get("ping")
    if not isinstance(ping, dict):
        errors.append("measurements.ping: must record executed ping evidence")
    else:
        if (
            not isinstance(ping.get("packets_received"), int)
            or ping["packets_received"] <= 0
        ):
            errors.append("measurements.ping.packets_received: must be positive")
        if not isinstance(ping.get("jitter_ms"), (int, float)):
            errors.append("measurements.ping.jitter_ms: must be numeric")
        rtt = ping.get("rtt_ms")
        if not isinstance(rtt, dict) or not all(
            isinstance(rtt.get(field), (int, float))
            for field in ("min", "avg", "max", "stddev")
        ):
            errors.append("measurements.ping.rtt_ms: must contain numeric RTT samples")
        if not nonempty_string(ping.get("raw_path")):
            errors.append("measurements.ping.raw_path: must not be empty")
        if not valid_sha256(ping.get("sha256")):
            errors.append("measurements.ping.sha256: must be a lowercase SHA-256")

    throughput = measurement.get("throughput")
    runs = throughput.get("runs") if isinstance(throughput, dict) else None
    if not isinstance(runs, list) or not runs:
        errors.append("measurements.throughput.runs: must contain executed runs")
    else:
        directions = set()
        for index, run in enumerate(runs):
            if not isinstance(run, dict):
                errors.append(
                    f"measurements.throughput.runs[{index}]: must be an object"
                )
                continue
            direction = run.get("direction")
            if direction not in {"host_to_device", "device_to_host"}:
                errors.append(
                    f"measurements.throughput.runs[{index}].direction: invalid"
                )
            else:
                directions.add(direction)
            if (
                not isinstance(run.get("bits_per_second"), (int, float))
                or run["bits_per_second"] <= 0
            ):
                errors.append(
                    f"measurements.throughput.runs[{index}].bits_per_second: "
                    "must be positive"
                )
            if not nonempty_string(run.get("raw_path")):
                errors.append(
                    f"measurements.throughput.runs[{index}].raw_path: "
                    "must not be empty"
                )
            if not valid_sha256(run.get("sha256")):
                errors.append(
                    f"measurements.throughput.runs[{index}].sha256: "
                    "must be a lowercase SHA-256"
                )
        missing = {"host_to_device", "device_to_host"} - directions
        if missing:
            errors.append(
                "measurements.throughput.runs: missing directions "
                + ", ".join(sorted(missing))
            )

    commands = measurement.get("commands")
    purposes = {
        command.get("purpose")
        for command in commands
        if isinstance(command, dict) and command.get("returncode") == 0
    } if isinstance(commands, list) else set()
    if not {"ping", "iperf-host-to-device", "iperf-device-to-host"} <= purposes:
        errors.append(
            "measurements.commands: must record successful ping and both iperf3 directions"
        )
    return errors


def validate_report(report):
    errors = []
    if require_path(report, "schema_version", errors) != 1:
        errors.append("schema_version: expected 1")
    if require_path(report, "probe", errors) != "gate2a-usb-network":
        errors.append("probe: expected gate2a-usb-network")
    require_path(report, "generated_at_utc", errors)

    for field in (
        "scope.product_channel",
        "scope.reverse_input_supported",
        "scope.depends_on_aoa_result",
        "scope.reference_link_measurements_only",
    ):
        require_path(report, field, errors)
    if report.get("scope") != {
        "product_channel": False,
        "reverse_input_supported": False,
        "depends_on_aoa_result": False,
        "reference_link_measurements_only": True,
    }:
        errors.append(
            "scope: must disable product channel/reverse input/AOA dependency and enable only reference-link measurements"
        )

    for field in (
        "reference_environment.host.model",
        "reference_environment.host.chip",
        "reference_environment.host.memory_bytes",
        "reference_environment.host.os_version",
        "reference_environment.host.os_build",
        "reference_environment.host.architecture",
        "reference_environment.device.model",
        "reference_environment.device.product_name",
        "reference_environment.device.serial",
        "reference_environment.device.software_version",
        "reference_environment.device.os_version",
        "reference_environment.device.api_version",
    ):
        require_path(report, field, errors)

    for field in (
        "usb_topology.target_device.name",
        "usb_topology.target_device.vendor",
        "usb_topology.target_device.product",
        "usb_topology.target_device.serial",
        "usb_topology.target_device.id_vendor",
        "usb_topology.target_device.id_product",
        "usb_topology.target_device.interfaces",
        "usb_topology.ancestors",
        "usb_topology.hubs",
        "usb_topology.observed_hubs",
        "usb_topology.link_speed_mbps",
        "usb_topology.direct_to_host",
        "usb_topology.reference_link",
        "usb_topology.reason",
    ):
        require_path(report, field, errors)

    for field in (
        "device_network.interface",
        "device_network.driver_family",
        "device_network.native_bundled_driver",
        "device_network.rndis_detected",
        "device_network.eligible",
        "device_network.reason",
    ):
        require_path(report, field, errors, allow_none=field == "device_network.interface")

    for field in (
        "host_network.hardware_ports",
        "host_network.services",
        "host_network.interfaces",
        "host_network.candidate_interfaces",
        "host_network.associated_device_interface",
    ):
        require_path(
            report,
            field,
            errors,
            allow_none=field == "host_network.associated_device_interface",
        )

    for field in (
        "device_entry.status",
        "device_entry.user_operable",
        "device_entry.developer_mode_required",
        "device_entry.path",
        "device_entry.evidence",
    ):
        require_path(report, field, errors, allow_none=True)

    for field in (
        "measurements.status",
        "measurements.reference_link",
        "measurements.reason",
        "measurements.ping",
        "measurements.throughput",
    ):
        require_path(report, field, errors, allow_none=field in {"measurements.ping", "measurements.throughput"})

    for field in ("decision.status", "decision.reasons"):
        require_path(report, field, errors)

    require_path(report, "artifacts", errors)

    if not errors:
        decision = report["decision"]["status"]
        if decision not in {"pending", "failed", "passed"}:
            errors.append("decision.status: expected pending, failed, or passed")
        if report["device_network"]["rndis_detected"] or report["device_network"]["driver_family"] == "rndis":
            if decision != "failed":
                errors.append("decision.status: RNDIS must produce failed, never pending/passed")
        if decision == "passed":
            if not report["usb_topology"]["reference_link"]:
                errors.append("decision.status: passed requires reference_link=true")
            if not report["usb_topology"]["direct_to_host"]:
                errors.append("decision.status: passed requires direct_to_host=true")
            if report["usb_topology"].get("hub_count") != 0:
                errors.append("decision.status: passed requires hub_count=0")
            if report["usb_topology"].get("hubs"):
                errors.append("decision.status: passed requires no USB hubs")
            if not report["device_network"]["eligible"]:
                errors.append("decision.status: passed requires an eligible native NCM/ECM interface")
            device_interface = report["device_network"].get("interface")
            if not nonempty_string(device_interface):
                errors.append(
                    "device_network.interface: passed requires an associated interface"
                )
            family = report["device_network"].get("driver_family")
            if family not in {"ncm", "ecm"}:
                errors.append(
                    "device_network.driver_family: passed requires ncm or ecm"
                )
            if report["device_network"].get("native_bundled_driver") is not True:
                errors.append(
                    "device_network.native_bundled_driver: passed requires true"
                )
            if report["device_network"].get("rndis_detected") is not False:
                errors.append(
                    "device_network.rndis_detected: passed requires false"
                )
            bundle = report["device_network"].get("network_driver_bundle_id")
            allowed_bundles = (
                NATIVE_NCM_BUNDLES if family == "ncm" else NATIVE_ECM_BUNDLES
            )
            if bundle not in allowed_bundles:
                errors.append(
                    "device_network.network_driver_bundle_id: "
                    "passed requires the native bundled driver for its family"
                )
            if (
                report["host_network"].get("associated_device_interface")
                != device_interface
            ):
                errors.append(
                    "host_network.associated_device_interface: "
                    "passed must match device_network.interface"
                )

            device_entry = report["device_entry"]
            if device_entry.get("status") != "verified":
                errors.append("decision.status: passed requires verified device_entry")
            if device_entry.get("user_operable") is not True:
                errors.append("device_entry.user_operable: passed requires true")
            if device_entry.get("developer_mode_required") is not False:
                errors.append("decision.status: passed requires developer_mode_required=false")
            if not nonempty_string(device_entry.get("path")):
                errors.append("device_entry.path: passed requires the observed UI path")
            evidence = device_entry.get("evidence")
            if (
                not isinstance(evidence, list)
                or not evidence
                or not all(nonempty_string(item) for item in evidence)
            ):
                errors.append(
                    "device_entry.evidence: passed requires non-empty evidence"
                )
            steps = device_entry.get("steps")
            if (
                not isinstance(steps, list)
                or not steps
                or not all(nonempty_string(item) for item in steps)
            ):
                errors.append(
                    "device_entry.steps: passed requires recorded manual steps"
                )
            environment_serial = report["reference_environment"]["device"]["serial"]
            topology_serial = report["usb_topology"]["target_device"].get("serial")
            if device_entry.get("device_serial") != environment_serial:
                errors.append(
                    "device_entry.device_serial: passed must match the reference device"
                )
            if topology_serial != environment_serial:
                errors.append(
                    "usb_topology.target_device.serial: "
                    "passed must match the reference device"
                )

            if report["measurements"].get("status") != "executed":
                errors.append("decision.status: passed requires executed measurements")
            errors.extend(
                validate_measurement(
                    report["measurements"],
                    device_interface,
                    report["usb_topology"]["reference_link"],
                )
            )

            artifacts = report.get("artifacts")
            if not isinstance(artifacts, list) or not artifacts:
                errors.append("artifacts: passed requires archived raw evidence")
            elif not all(
                isinstance(artifact, dict)
                and nonempty_string(artifact.get("path"))
                and valid_sha256(artifact.get("sha256"))
                for artifact in artifacts
            ):
                errors.append(
                    "artifacts: each passed artifact requires path and SHA-256"
                )
            artifact_hashes = {
                artifact.get("sha256")
                for artifact in artifacts
                if isinstance(artifact, dict)
            } if isinstance(artifacts, list) else set()
            measurement_hashes = []
            ping = report["measurements"].get("ping")
            if isinstance(ping, dict) and valid_sha256(ping.get("sha256")):
                measurement_hashes.append(ping["sha256"])
            throughput = report["measurements"].get("throughput")
            runs = throughput.get("runs") if isinstance(throughput, dict) else None
            if isinstance(runs, list):
                measurement_hashes.extend(
                    run["sha256"]
                    for run in runs
                    if isinstance(run, dict) and valid_sha256(run.get("sha256"))
                )
            if any(
                sha256 not in artifact_hashes for sha256 in measurement_hashes
            ):
                errors.append(
                    "measurement evidence is not archived in artifacts"
                )
    return errors


def command_classify(arguments):
    classification = classify(load_json(arguments.capture))
    write_json(arguments.output, classification)
    return 0


def command_validate_report(arguments):
    errors = validate_report(load_json(arguments.report))
    if errors:
        for error in errors:
            print(f"report validation failed: {error}", file=sys.stderr)
        return 1
    print("report validation passed")
    return 0


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_ping(path):
    text = Path(path).read_text(encoding="utf-8")
    samples = [
        float(match.group(1))
        for match in re.finditer(r"time=([0-9]+(?:\.[0-9]+)?)\s*ms", text)
    ]
    summary = re.search(
        r"(\d+)\s+packets transmitted,\s+(\d+)\s+packets received.*?"
        r"([0-9]+(?:\.[0-9]+)?)%\s+packet loss",
        text,
        re.DOTALL,
    )
    if summary is None:
        raise ValueError(f"{path}: ping summary not found")
    if not samples:
        raise ValueError(f"{path}: no successful ping RTT samples")

    rtt = re.search(
        r"round-trip min/avg/max/(?:stddev|mdev)\s*=\s*"
        r"([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)\s*ms",
        text,
    )
    if rtt:
        rtt_ms = {
            "min": float(rtt.group(1)),
            "avg": float(rtt.group(2)),
            "max": float(rtt.group(3)),
            "stddev": float(rtt.group(4)),
        }
    else:
        mean = sum(samples) / len(samples)
        variance = sum((sample - mean) ** 2 for sample in samples) / len(samples)
        rtt_ms = {
            "min": min(samples),
            "avg": mean,
            "max": max(samples),
            "stddev": variance**0.5,
        }

    deltas = [
        abs(current - previous)
        for previous, current in zip(samples, samples[1:])
    ]
    jitter = sum(deltas) / len(deltas) if deltas else 0.0
    resolved = Path(path).resolve()
    return {
        "raw_path": str(resolved),
        "sha256": sha256_file(resolved),
        "packets_transmitted": int(summary.group(1)),
        "packets_received": int(summary.group(2)),
        "packet_loss_percent": float(summary.group(3)),
        "rtt_ms": rtt_ms,
        "jitter_ms": jitter,
        "jitter_definition": "mean absolute difference of consecutive ICMP RTT samples",
    }


def iperf_rate(document):
    end = document.get("end", {})
    for section in ("sum_received", "sum_sent", "sum"):
        values = end.get(section)
        if isinstance(values, dict) and "bits_per_second" in values:
            return float(values["bits_per_second"]), f"end.{section}.bits_per_second"
    raise ValueError("iperf JSON has no end.sum_received/sum_sent.bits_per_second")


def command_link_summary(arguments):
    ping = parse_ping(arguments.ping)
    iperf_path = Path(arguments.iperf_json).resolve()
    iperf_document = load_json(iperf_path)
    bits_per_second, source_field = iperf_rate(iperf_document)
    summary = {
        "status": "executed",
        "interface": arguments.interface,
        "target": arguments.target,
        "ping": ping,
        "throughput": {
            "runs": [
                {
                    "direction": arguments.direction,
                    "raw_path": str(iperf_path),
                    "sha256": sha256_file(iperf_path),
                    "source_field": source_field,
                    "bits_per_second": bits_per_second,
                    "megabytes_per_second": bits_per_second / 8_000_000,
                }
            ]
        },
    }
    write_json(arguments.output, summary)
    return 0


def parse_scalar(value):
    value = value.strip()
    quoted = False
    while value.startswith('"') and value.endswith('"') and len(value) >= 2:
        quoted = True
        value = value[1:-1]
    if quoted:
        return value
    if value in {"Yes", "No"}:
        return value == "Yes"
    if re.fullmatch(r"-?\d+", value):
        return int(value)
    return value


def parse_ioreg_tree(text):
    nodes = []
    stack = []
    current = None
    node_pattern = re.compile(
        r"^(?P<prefix>[ |]*)\+-o (?P<label>.+?)  <class (?P<kind>[^,>]+)"
    )
    property_pattern = re.compile(r'^\s*(?:\|\s*)*"([^"]+)"\s*=\s*(.+?)\s*$')

    for line in text.splitlines():
        node_match = node_pattern.match(line)
        if node_match:
            label = node_match.group("label").strip()
            location = None
            location_match = re.search(r"@([0-9A-Fa-f]+)$", label)
            if location_match:
                location = location_match.group(1)
                label = label[: location_match.start()]
            if label.startswith('"') and label.endswith('"'):
                label = label[1:-1]
            depth = len(node_match.group("prefix")) // 2
            while stack and stack[-1]["depth"] >= depth:
                stack.pop()
            node = {
                "name": label,
                "class": node_match.group("kind"),
                "location": location,
                "depth": depth,
                "parent": stack[-1] if stack else None,
                "properties": {},
            }
            nodes.append(node)
            stack.append(node)
            current = node
            continue

        property_match = property_pattern.match(line)
        if property_match and current is not None:
            current["properties"][property_match.group(1)] = parse_scalar(
                property_match.group(2)
            )
    return nodes


def property_value(node, *names):
    for name in names:
        if name in node["properties"]:
            return node["properties"][name]
    return None


def node_ancestors(node):
    ancestors = []
    parent = node.get("parent")
    while parent is not None:
        ancestors.append(parent)
        parent = parent.get("parent")
    ancestors.reverse()
    return ancestors


def node_summary(node):
    return {
        "name": node["name"],
        "class": node["class"],
        "vendor": property_value(node, "USB Vendor Name", "kUSBVendorString"),
        "product": property_value(node, "USB Product Name", "kUSBProductString"),
        "location": node["location"],
    }


def interface_driver(interface_node, nodes):
    child_depth = interface_node["depth"] + 1
    for node in nodes:
        if node is interface_node or node["depth"] <= interface_node["depth"]:
            continue
        if node["depth"] < child_depth:
            continue
        if interface_node not in node_ancestors(node):
            continue
        bundle = property_value(node, "CFBundleIdentifier")
        if bundle:
            return node["class"], bundle
    return None, None


def discover_topology(text, serial, interfaces_text=None):
    nodes = parse_ioreg_tree(text)
    target = None
    for node in nodes:
        if node["class"] != "IOUSBHostDevice":
            continue
        candidate_serial = property_value(
            node,
            "kUSBSerialNumberString",
            "USB Serial Number",
            "IOPlatformSerialNumber",
        )
        if candidate_serial == serial:
            target = node
            break
    if target is None:
        raise ValueError(f"USB device with serial {serial!r} not found in ioreg output")

    ancestors = node_ancestors(target)
    hub_nodes = [
        node
        for node in ancestors
        if node["class"] == "IOUSBHostDevice"
        and (
            "hub" in str(property_value(node, "USB Product Name", "")).lower()
            or "hub" in str(property_value(node, "UsbExclusiveOwner", "")).lower()
        )
    ]
    interfaces = []
    for node in nodes:
        if node["class"] != "IOUSBHostInterface" or node["depth"] <= target["depth"]:
            continue
        if target not in node_ancestors(node):
            continue
        driver_class, bundle = interface_driver(node, nodes)
        interfaces.append(
            {
                "name": property_value(node, "kUSBString") or node["name"],
                "interface_class": property_value(node, "bInterfaceClass"),
                "interface_subclass": property_value(node, "bInterfaceSubClass"),
                "interface_protocol": property_value(node, "bInterfaceProtocol"),
                "driver_class": driver_class,
                "driver_bundle_id": bundle,
            }
        )
    if interfaces_text:
        interface_nodes = parse_ioreg_tree(interfaces_text)
        for node in interface_nodes:
            if node["class"] != "IOUSBHostInterface":
                continue
            node_serial = property_value(
                node,
                "USB Serial Number",
                "kUSBSerialNumberString",
            )
            node_vid = property_value(node, "idVendor")
            node_pid = property_value(node, "idProduct")
            if node_serial != serial and not (
                node_vid == property_value(target, "idVendor")
                and node_pid == property_value(target, "idProduct")
                and node_serial is None
            ):
                continue
            driver_class, bundle = interface_driver(node, interface_nodes)
            candidate = {
                "name": property_value(node, "kUSBString") or node["name"],
                "interface_class": property_value(node, "bInterfaceClass"),
                "interface_subclass": property_value(node, "bInterfaceSubClass"),
                "interface_protocol": property_value(node, "bInterfaceProtocol"),
                "driver_class": driver_class,
                "driver_bundle_id": bundle,
            }
            if candidate not in interfaces:
                interfaces.append(candidate)

    link_speed_bps = property_value(target, "UsbLinkSpeed")
    link_speed_mbps = (
        int(link_speed_bps) // 1_000_000 if isinstance(link_speed_bps, int) else None
    )
    target_summary = node_summary(target)
    target_summary.update(
        {
            "serial": serial,
            "id_vendor": property_value(target, "idVendor"),
            "id_product": property_value(target, "idProduct"),
            "link_speed_mbps": link_speed_mbps,
            "owner": property_value(target, "UsbExclusiveOwner"),
            "interfaces": interfaces,
        }
    )
    hubs = [node_summary(node) for node in hub_nodes]
    observed_hubs = []
    for node in nodes:
        if node["class"] != "IOUSBHostDevice":
            continue
        product = str(property_value(node, "USB Product Name", ""))
        owner = str(property_value(node, "UsbExclusiveOwner", ""))
        if "hub" not in product.lower() and "hub" not in owner.lower():
            continue
        summary = node_summary(node)
        if summary not in observed_hubs:
            observed_hubs.append(summary)
    direct = not hubs
    return {
        "target_device": target_summary,
        "ancestors": [node_summary(node) for node in ancestors if node["name"] != "Root"],
        "hubs": hubs,
        "observed_hubs": observed_hubs,
        "hub_count": len(hubs),
        "negotiated_speed_mbps": link_speed_mbps,
        "direct_to_host": direct,
        "reference_link": direct,
        "reason": (
            "device is directly attached to the Mac USB controller"
            if direct
            else "device path contains USB hub(s), so it is not a Reference Link"
        ),
    }


def command_topology(arguments):
    text = Path(arguments.ioreg).read_text(encoding="utf-8")
    interfaces_text = (
        Path(arguments.interfaces_ioreg).read_text(encoding="utf-8")
        if arguments.interfaces_ioreg
        else None
    )
    write_json(
        arguments.output,
        discover_topology(text, arguments.serial, interfaces_text),
    )
    return 0


def control_descendants(nodes, control):
    descendants = []
    control_depth = control["depth"]
    for node in nodes:
        if node is control or node["depth"] <= control_depth:
            continue
        if control in node_ancestors(node):
            descendants.append(node)
    return descendants


def network_interfaces_from_ioreg(text, family):
    nodes = parse_ioreg_tree(text)
    interfaces = []
    control_classes = {
        "ncm": {"AppleUSBNCMControl"},
        "ecm": {"AppleUSBECMControl"},
    }[family]
    data_class_fragment = "NCMData" if family == "ncm" else "ECMData"
    native_bundles = NATIVE_NCM_BUNDLES if family == "ncm" else NATIVE_ECM_BUNDLES

    for control in nodes:
        if control["class"] not in control_classes:
            continue
        descendants = control_descendants(nodes, control)
        data_nodes = [
            node for node in descendants if data_class_fragment in node["class"]
        ]
        ethernet_nodes = [
            node for node in descendants if node["class"] == "IOEthernetInterface"
        ]
        if not data_nodes or not ethernet_nodes:
            continue
        data_node = data_nodes[0]
        ethernet_node = ethernet_nodes[0]
        bundle = (
            property_value(control, "CFBundleIdentifier")
            or property_value(data_node, "CFBundleIdentifier")
            or property_value(ethernet_node, "CFBundleIdentifier")
        )
        text_hint = " ".join(
            str(value)
            for value in (
                control["class"],
                data_node["class"],
                bundle,
                property_value(control, "IOVendor"),
                property_value(data_node, "IOVendor"),
            )
        ).lower()
        detected_family = "rndis" if "rndis" in text_hint else family
        interfaces.append(
            {
                "name": property_value(ethernet_node, "BSD Name")
                or property_value(ethernet_node, "IOInterfaceUnit"),
                "driver_family": detected_family,
                "driver_class": control["class"],
                "driver_bundle_id": bundle,
                "native_bundled_driver": bundle in native_bundles
                and detected_family == family,
                "usb_id_vendor": property_value(
                    data_node, "idVendor"
                )
                or property_value(control, "idVendor"),
                "usb_id_product": property_value(
                    data_node, "idProduct"
                )
                or property_value(control, "idProduct"),
                "usb_serial": None,
                "usb_vendor": None,
                "usb_product": None,
                "usb_topology": None,
                "associated_to_target_device": False,
                "association": "not_matched",
            }
        )
    return interfaces


def attach_usb_identity(interfaces, usb_text, target_serial):
    usb_nodes = parse_ioreg_tree(usb_text)
    usb_devices = [node for node in usb_nodes if node["class"] == "IOUSBHostDevice"]
    for interface in interfaces:
        matches = [
            node
            for node in usb_devices
            if property_value(node, "idVendor") == interface["usb_id_vendor"]
            and property_value(node, "idProduct") == interface["usb_id_product"]
        ]
        if len(matches) != 1:
            interface["association"] = (
                "ambiguous_vendor_product" if matches else "usb_device_not_found"
            )
            continue
        device = matches[0]
        serial = property_value(
            device, "kUSBSerialNumberString", "USB Serial Number"
        )
        interface["usb_serial"] = serial
        interface["usb_vendor"] = property_value(
            device, "USB Vendor Name", "kUSBVendorString"
        )
        interface["usb_product"] = property_value(
            device, "USB Product Name", "kUSBProductString"
        )
        if serial:
            interface["usb_topology"] = discover_topology(usb_text, serial)
        interface["associated_to_target_device"] = serial == target_serial
        interface["association"] = (
            "serial_match" if serial == target_serial else "different_serial"
        )
    return interfaces


def build_network_inventory(usb_text, controller_texts, target_serial):
    interfaces = []
    for family, text in controller_texts:
        if text:
            interfaces.extend(network_interfaces_from_ioreg(text, family))
    attach_usb_identity(interfaces, usb_text, target_serial)
    interfaces.sort(key=lambda item: str(item.get("name")))
    associated = [
        interface
        for interface in interfaces
        if interface["associated_to_target_device"]
    ]
    eligible = [
        interface
        for interface in associated
        if interface["native_bundled_driver"]
        and interface["driver_family"] in {"ncm", "ecm"}
    ]
    associated_interface = eligible[0]["name"] if len(eligible) == 1 else None
    if associated_interface:
        reason = "one native bundled NCM/ECM interface is associated with the target device"
    elif interfaces:
        reason = (
            "native NCM/ECM interfaces were found, but they are not associated with "
            f"target device serial {target_serial}"
        )
    else:
        reason = "no native NCM/ECM interfaces were found"
    return {
        "interfaces": interfaces,
        "associated_interface": associated_interface,
        "device_network_eligible": bool(associated_interface),
        "reason": reason,
    }


def command_network_inventory(arguments):
    controller_texts = []
    if arguments.ncm_ioreg:
        controller_texts.append(
            ("ncm", Path(arguments.ncm_ioreg).read_text(encoding="utf-8"))
        )
    if arguments.ecm_ioreg:
        controller_texts.append(
            ("ecm", Path(arguments.ecm_ioreg).read_text(encoding="utf-8"))
        )
    usb_text = Path(arguments.usb_ioreg).read_text(encoding="utf-8")
    inventory = build_network_inventory(
        usb_text, controller_texts, arguments.device_serial
    )
    write_json(arguments.output, inventory)
    return 0


def parse_hardware_ports(text):
    ports = []
    current = {}
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("Hardware Port:"):
            if current.get("hardware_port"):
                ports.append(current)
            current = {"hardware_port": stripped.split(":", 1)[1].strip()}
        elif stripped.startswith("Device:") and current.get("hardware_port"):
            current["device"] = stripped.split(":", 1)[1].strip()
        elif not stripped and current.get("hardware_port"):
            ports.append(current)
            current = {}
    if current.get("hardware_port"):
        ports.append(current)
    return [port for port in ports if port.get("device")]


def parse_network_services(text):
    services = []
    for line in text.splitlines():
        name = line.strip()
        if not name or name.startswith("An asterisk"):
            continue
        disabled = name.startswith("*")
        if disabled:
            name = name[1:].strip()
        services.append({"name": name, "enabled": not disabled})
    return services


def parse_ifconfig(text):
    interfaces = []
    current = None
    for line in text.splitlines():
        header = re.match(r"^([^:\s]+): flags=([^ ]+) mtu (\d+)", line)
        if header:
            current = {
                "name": header.group(1),
                "flags": header.group(2),
                "mtu": int(header.group(3)),
                "status": None,
                "ipv4_configured": False,
            }
            interfaces.append(current)
            continue
        if current is None:
            continue
        stripped = line.strip()
        if stripped.startswith("status:"):
            current["status"] = stripped.split(":", 1)[1].strip()
        elif stripped.startswith("inet "):
            current["ipv4_configured"] = True
    return interfaces


def parse_device_interface_names(text):
    return re.findall(r"(?m)^(\S+)\s+Link encap:", text)


def build_host_network(raw_dir, network_inventory):
    hardware_ports = []
    services = []
    interfaces = []
    hardware_ports_path = Path(raw_dir) / "networksetup-hardware-ports.txt"
    services_path = Path(raw_dir) / "networksetup-services.txt"
    ifconfig_path = Path(raw_dir) / "ifconfig.txt"
    if hardware_ports_path.exists():
        hardware_ports = parse_hardware_ports(
            hardware_ports_path.read_text(encoding="utf-8")
        )
    if services_path.exists():
        services = parse_network_services(services_path.read_text(encoding="utf-8"))
    if ifconfig_path.exists():
        interfaces = parse_ifconfig(ifconfig_path.read_text(encoding="utf-8"))

    service_names = {service["name"] for service in services}
    for interface in network_inventory["interfaces"]:
        port = next(
            (
                candidate
                for candidate in hardware_ports
                if candidate.get("device") == interface.get("name")
            ),
            None,
        )
        interface["macos_hardware_port"] = port["hardware_port"] if port else None
        interface["macos_network_service"] = (
            port["hardware_port"]
            if port and port["hardware_port"] in service_names
            else None
        )
    return {
        "hardware_ports": hardware_ports,
        "services": services,
        "interfaces": interfaces,
        "candidate_interfaces": network_inventory["interfaces"],
        "associated_device_interface": network_inventory["associated_interface"],
    }


def default_device_entry():
    return {
        "status": "not_executed",
        "user_operable": None,
        "developer_mode_required": None,
        "path": None,
        "evidence": [],
    }


def default_measurements(reference_link, reason):
    return {
        "status": "not_executed",
        "reference_link": reference_link,
        "reason": reason,
        "ping": None,
        "throughput": None,
    }


def artifact_records(paths, base):
    records = []
    for path in sorted({Path(path).resolve() for path in paths}):
        if not path.is_file():
            continue
        try:
            display_path = str(path.relative_to(Path(base).resolve()))
        except ValueError:
            display_path = str(path)
        records.append({"path": display_path, "sha256": sha256_file(path)})
    return records


def compose_report(raw_dir, serial, output_path, device_entry_path=None, measurement_path=None):
    raw_dir = Path(raw_dir)
    host = load_json(raw_dir / "host.json")
    device = load_json(raw_dir / "device.json")
    usb_text = (raw_dir / "ioreg-usb.txt").read_text(encoding="utf-8")
    interfaces_path = raw_dir / "ioreg-interfaces.txt"
    interfaces_text = (
        interfaces_path.read_text(encoding="utf-8")
        if interfaces_path.exists()
        else None
    )
    topology = discover_topology(usb_text, serial, interfaces_text)
    controller_texts = []
    for family, filename in (("ncm", "ioreg-ncm.txt"), ("ecm", "ioreg-ecm.txt")):
        path = raw_dir / filename
        if path.exists():
            controller_texts.append((family, path.read_text(encoding="utf-8")))
    network_inventory = build_network_inventory(
        usb_text, controller_texts, serial
    )

    device_entry = (
        load_json(device_entry_path)
        if device_entry_path
        else default_device_entry()
    )
    target = topology["target_device"]
    classification_input = {
        "usb_device": {
            "name": target["name"],
            "serial": target["serial"],
            "vendor": target["vendor"],
            "product": target["product"],
            "id_vendor": target["id_vendor"],
            "id_product": target["id_product"],
            "link_speed_mbps": target["link_speed_mbps"],
            "interfaces": target["interfaces"],
            "ancestors": topology["ancestors"],
            "hubs": topology["hubs"],
        },
        "network_interfaces": network_inventory["interfaces"],
        "device_entry": device_entry,
    }
    classification = classify(classification_input)

    if measurement_path:
        measurements = load_json(measurement_path)
        measurement_errors = validate_measurement(
            measurements,
            classification.get("interface"),
            topology["reference_link"],
        )
        if measurement_errors:
            for error in measurement_errors:
                print(f"composed report is invalid: {error}", file=sys.stderr)
            return 1
    else:
        measurements = default_measurements(
            topology["reference_link"],
            "throughput, RTT, and jitter were not executed",
        )

    reasons = list(classification["decision"]["reasons"])
    if device_entry.get("status") != "verified":
        reasons.append("manual Device USB-sharing entry verification is pending")
    if measurements.get("status") != "executed":
        reasons.append("Reference Link measurements are pending")

    decision = classification["decision"]["status"]
    if decision != "failed":
        if device_entry.get("status") != "verified":
            decision = "pending"
        elif not classification.get("eligible"):
            decision = "pending"
        elif measurements.get("status") == "failed":
            decision = "failed"
        elif measurements.get("status") != "executed":
            decision = "pending"
        else:
            decision = "passed"

    host_network = build_host_network(raw_dir, network_inventory)
    device_network = {
        "interface": classification.get("interface"),
        "driver_family": classification["driver_family"],
        "network_driver_class": classification.get("network_driver_class"),
        "network_driver_bundle_id": classification.get("network_driver_bundle_id"),
        "native_bundled_driver": classification["native_bundled_driver"],
        "rndis_detected": classification["rndis_detected"],
        "eligible": classification["eligible"],
        "reason": network_inventory["reason"],
    }
    associated = next(
        (
            interface
            for interface in network_inventory["interfaces"]
            if interface.get("name") == classification.get("interface")
        ),
        None,
    )
    if associated:
        device_network["macos_hardware_port"] = associated.get("macos_hardware_port")
        device_network["macos_network_service"] = associated.get(
            "macos_network_service"
        )

    artifact_paths = list(raw_dir.rglob("*"))
    if device_entry_path:
        artifact_paths.append(Path(device_entry_path))
    if measurement_path:
        artifact_paths.append(Path(measurement_path))
    hdc_ifconfig_path = raw_dir / "hdc-ifconfig.txt"
    if hdc_ifconfig_path.exists():
        device["interface_names"] = parse_device_interface_names(
            hdc_ifconfig_path.read_text(encoding="utf-8")
        )

    report = {
        "schema_version": 1,
        "probe": "gate2a-usb-network",
        "generated_at_utc": datetime.datetime.now(
            datetime.timezone.utc
        ).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "scope": {
            "product_channel": False,
            "reverse_input_supported": False,
            "depends_on_aoa_result": False,
            "reference_link_measurements_only": True,
        },
        "reference_environment": {
            "host": {
                "model": host["model"],
                "chip": host["chip"],
                "memory_bytes": host["memory_bytes"],
                "os_version": host["os_version"],
                "os_build": host["os_build"],
                "architecture": host["architecture"],
            },
            "device": {
                "model": device["model"],
                "product_name": device["product_name"],
                "serial": device["serial"],
                "software_version": device["software_version"],
                "os_version": device["os_version"],
                "api_version": device["api_version"],
                "interface_names": device.get("interface_names", []),
            },
        },
        "usb_topology": {
            "target_device": target,
            "ancestors": topology["ancestors"],
            "hubs": topology["hubs"],
            "observed_hubs": topology["observed_hubs"],
            "hub_count": topology["hub_count"],
            "link_speed_mbps": topology["negotiated_speed_mbps"],
            "direct_to_host": topology["direct_to_host"],
            "reference_link": topology["reference_link"],
            "reason": topology["reason"],
        },
        "host_network": host_network,
        "device_network": device_network,
        "device_entry": device_entry,
        "measurements": measurements,
        "decision": {"status": decision, "reasons": reasons},
        "artifacts": artifact_records(
            artifact_paths, Path(output_path).resolve().parent
        ),
    }
    write_json(output_path, report)
    errors = validate_report(report)
    if errors:
        for error in errors:
            print(f"composed report is invalid: {error}", file=sys.stderr)
        return 1
    return 0


def command_compose(arguments):
    return compose_report(
        arguments.raw_dir,
        arguments.device_serial,
        arguments.output,
        arguments.device_entry,
        arguments.measurement,
    )


def measurement_preflight(report, interface, target):
    errors = []
    try:
        ipaddress.ip_address(target)
    except ValueError:
        errors.append(f"target {target!r} is not an IP address")
    if not report.get("usb_topology", {}).get("reference_link"):
        errors.append("report is not a direct Reference Link")
    if report.get("device_network", {}).get("interface") != interface:
        errors.append(
            f"report device interface {report.get('device_network', {}).get('interface')!r} "
            f"does not match --interface {interface!r}"
        )
    if not report.get("device_network", {}).get("eligible"):
        errors.append("device network driver is not an eligible native NCM/ECM interface")
    if report.get("device_entry", {}).get("status") != "verified":
        errors.append("manual Device USB-sharing entry is not verified")
    if report.get("device_entry", {}).get("developer_mode_required") is not False:
        errors.append("Device entry does not prove developer_mode_required=false")
    return errors


def measurement_plan(interface, target, duration):
    return {
        "status": "planned_not_executed",
        "reference_link": True,
        "interface": interface,
        "target": target,
        "created_at_utc": datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        ),
        "commands": [
            {
                "purpose": "RTT and jitter",
                "argv": [
                    "ping",
                    "-b",
                    interface,
                    "-c",
                    "120",
                    "-i",
                    "0.2",
                    "-W",
                    "1000",
                    target,
                ],
            },
            {
                "purpose": "host-to-device throughput",
                "argv": [
                    "iperf3",
                    "-c",
                    target,
                    "-t",
                    str(duration),
                    "-O",
                    "3",
                    "-J",
                ],
            },
            {
                "purpose": "device-to-host throughput",
                "argv": [
                    "iperf3",
                    "-c",
                    target,
                    "-t",
                    str(duration),
                    "-O",
                    "3",
                    "-R",
                    "-J",
                ],
            },
        ],
        "device_requirement": "Run an operator-supplied iperf3 server on the Device: iperf3 -s -p 5201",
    }


def run_capture_command(argv, timeout=120):
    return subprocess.run(
        argv,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def command_measure(arguments):
    report = load_json(arguments.report)
    errors = measurement_preflight(report, arguments.interface, arguments.target)
    if errors:
        for error in errors:
            print(f"refusing measurement: {error}", file=sys.stderr)
        return 2

    plan = measurement_plan(
        arguments.interface, arguments.target, arguments.duration
    )
    output_dir = Path(arguments.output)
    if not arguments.execute:
        output_dir.mkdir(parents=True, exist_ok=True)
        write_json(output_dir / "measurement-plan.json", plan)
        print(f"measurement plan written to {output_dir / 'measurement-plan.json'}")
        print("not executed; rerun with --execute on the Reference Link")
        return 0

    missing = [
        executable
        for executable in ("ping", "iperf3", "ipconfig", "route")
        if shutil.which(executable) is None
    ]
    if missing:
        print(
            "refusing measurement: missing executable(s): " + ", ".join(missing),
            file=sys.stderr,
        )
        return 2

    route = run_capture_command(
        ["route", "-n", "get", arguments.target], timeout=15
    )
    interface_match = re.search(r"^\s*interface:\s*(\S+)", route.stdout, re.MULTILINE)
    route_interface = interface_match.group(1) if interface_match else None
    if route.returncode != 0 or route_interface != arguments.interface:
        print(
            "refusing measurement: route to target does not use "
            f"{arguments.interface} (observed {route_interface!r})",
            file=sys.stderr,
        )
        return 2
    address = run_capture_command(
        ["ipconfig", "getifaddr", arguments.interface], timeout=15
    )
    if address.returncode != 0 or not address.stdout.strip():
        print(
            f"refusing measurement: {arguments.interface} has no IPv4 address",
            file=sys.stderr,
        )
        return 2

    output_dir.mkdir(parents=True, exist_ok=True)
    commands = []
    raw_results = {}
    for purpose, filename, argv in (
        ("ping", "ping.txt", plan["commands"][0]["argv"]),
        ("iperf-host-to-device", "iperf-host-to-device.json", plan["commands"][1]["argv"]),
        ("iperf-device-to-host", "iperf-device-to-host.json", plan["commands"][2]["argv"]),
    ):
        result = run_capture_command(argv, timeout=arguments.duration + 90)
        (output_dir / filename).write_text(result.stdout, encoding="utf-8")
        if result.stderr:
            (output_dir / f"{filename}.stderr").write_text(
                result.stderr, encoding="utf-8"
            )
        commands.append(
            {
                "purpose": purpose,
                "argv": argv,
                "returncode": result.returncode,
                "stdout_path": filename,
                "stderr_path": f"{filename}.stderr" if result.stderr else None,
            }
        )
        raw_results[purpose] = result
        if result.returncode != 0:
            break

    write_json(output_dir / "commands.json", commands)
    if any(result.returncode != 0 for result in raw_results.values()):
        print("measurement failed; raw command output was retained", file=sys.stderr)
        return 1

    ping = parse_ping(output_dir / "ping.txt")
    throughput_runs = []
    for direction, filename in (
        ("host_to_device", "iperf-host-to-device.json"),
        ("device_to_host", "iperf-device-to-host.json"),
    ):
        document = load_json(output_dir / filename)
        bits_per_second, source_field = iperf_rate(document)
        throughput_runs.append(
            {
                "direction": direction,
                "raw_path": filename,
                "sha256": sha256_file(output_dir / filename),
                "source_field": source_field,
                "bits_per_second": bits_per_second,
                "megabytes_per_second": bits_per_second / 8_000_000,
            }
        )
    measurement = {
        "status": "executed",
        "reference_link": True,
        "reason": "ping and both iperf3 directions completed on the Reference Link",
        "interface": arguments.interface,
        "target": arguments.target,
        "ping": ping,
        "throughput": {"runs": throughput_runs},
        "commands": commands,
    }
    write_json(output_dir / "link-measurement.json", measurement)
    print(f"measurement written to {output_dir / 'link-measurement.json'}")
    return 0


def redact_mac_values(text):
    text = re.sub(
        r"\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b",
        "<redacted-mac>",
        text,
    )
    text = re.sub(
        r'(?i)("IOMACAddress"\s*=\s*)<[0-9A-Fa-f]{12}>',
        r"\1<redacted-mac>",
        text,
    )
    text = re.sub(
        r"(?im)^(\s*Ethernet Address:\s*).*$",
        r"\1<redacted>",
        text,
    )
    return text


def sanitize_ioreg_output(text):
    return redact_mac_values(text)


def sanitize_network_output(text):
    text = redact_mac_values(text)
    text = re.sub(
        r"\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b",
        "<redacted-ip>",
        text,
    )
    text = re.sub(
        r"(?im)^(\s*ether\s+)[0-9a-f:]+",
        r"\1<redacted>",
        text,
    )
    text = re.sub(
        r"(?im)^(\s*inet6?\s+)\S+",
        r"\1<redacted>",
        text,
    )
    return text


def parse_sw_vers(text):
    values = {}
    for line in text.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            values[key.strip()] = value.strip()
    return values


def find_hdc(explicit):
    if explicit:
        return Path(explicit)
    if Path("/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc").is_file():
        return Path(
            "/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
        )
    discovered = shutil.which("hdc")
    return Path(discovered) if discovered else None


def device_entry_template(serial):
    return {
        "status": "not_executed",
        "device_serial": serial,
        "observed_at_utc": None,
        "operator": None,
        "user_operable": None,
        "developer_mode_required": None,
        "path": None,
        "steps": [],
        "evidence": [],
        "notes": "Do not mark verified until the exact Device UI path and absence of Developer Mode are observed.",
    }


def capture_host_and_device(output_dir, serial, hdc_path):
    raw_dir = Path(output_dir) / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    commands = []

    def record(
        identifier,
        argv,
        filename=None,
        sanitizer=None,
        timeout=120,
        required=False,
    ):
        try:
            result = run_capture_command(argv, timeout=timeout)
            stdout = sanitizer(result.stdout) if sanitizer else result.stdout
            stderr = sanitizer(result.stderr) if sanitizer else result.stderr
        except (OSError, subprocess.TimeoutExpired) as error:
            result = None
            stdout = ""
            stderr = str(error)
        command = {
            "id": identifier,
            "argv": argv,
            "returncode": result.returncode if result else None,
            "stdout_path": None,
            "stderr_path": None,
        }
        if filename:
            (raw_dir / filename).write_text(stdout, encoding="utf-8")
            command["stdout_path"] = filename
        if stderr:
            stderr_name = f"{identifier}.stderr.txt"
            (raw_dir / stderr_name).write_text(stderr, encoding="utf-8")
            command["stderr_path"] = stderr_name
        commands.append(command)
        if required and (result is None or result.returncode != 0):
            raise RuntimeError(f"required command failed: {' '.join(argv)}: {stderr}")
        return stdout

    sw_versions = parse_sw_vers(record("sw-vers", ["sw_vers"], "sw-vers.txt", required=True))
    model = record(
        "hw-model", ["sysctl", "-n", "hw.model"], "sysctl-hw-model.txt", required=True
    ).strip()
    memory = int(
        record(
            "hw-memsize",
            ["sysctl", "-n", "hw.memsize"],
            "sysctl-hw-memsize.txt",
            required=True,
        ).strip()
    )
    chip = record(
        "cpu-brand",
        ["sysctl", "-n", "machdep.cpu.brand_string"],
        "sysctl-cpu-brand.txt",
    ).strip()
    architecture = record("uname", ["uname", "-m"], "uname.txt", required=True).strip()
    host = {
        "model": model,
        "chip": chip,
        "memory_bytes": memory,
        "os_version": sw_versions.get("ProductVersion"),
        "os_build": sw_versions.get("BuildVersion"),
        "architecture": architecture,
    }

    record(
        "system-profiler-usb",
        ["system_profiler", "SPUSBDataType", "-json"],
        "system-profiler-usb.json",
        timeout=60,
    )
    record(
        "ioreg-usb",
        ["ioreg", "-p", "IOUSB", "-w0", "-l"],
        "ioreg-usb.txt",
        sanitizer=sanitize_ioreg_output,
        required=True,
    )
    record(
        "ioreg-interfaces",
        ["ioreg", "-r", "-c", "IOUSBHostInterface", "-l"],
        "ioreg-interfaces.txt",
        sanitizer=sanitize_ioreg_output,
    )
    record(
        "ioreg-ncm",
        ["ioreg", "-r", "-c", "AppleUSBNCMControl", "-l"],
        "ioreg-ncm.txt",
        sanitizer=sanitize_ioreg_output,
    )
    record(
        "ioreg-ecm",
        ["ioreg", "-r", "-c", "AppleUSBECMControl", "-l"],
        "ioreg-ecm.txt",
        sanitizer=sanitize_ioreg_output,
    )
    record(
        "networksetup-hardware-ports",
        ["networksetup", "-listallhardwareports"],
        "networksetup-hardware-ports.txt",
        sanitizer=sanitize_network_output,
    )
    record(
        "networksetup-services",
        ["networksetup", "-listallnetworkservices"],
        "networksetup-services.txt",
    )
    record(
        "ifconfig",
        ["ifconfig", "-a"],
        "ifconfig.txt",
        sanitizer=sanitize_network_output,
    )

    if hdc_path is None:
        raise RuntimeError(
            "hdc not found; set --hdc or install DevEco Studio toolchains"
        )
    target_listing = record(
        "hdc-targets",
        [str(hdc_path), "list", "targets", "-v"],
        "hdc-targets.txt",
        required=True,
    )
    targets = {
        line.split()[0]
        for line in target_listing.splitlines()
        if line.strip() and not line.startswith("[Empty]")
    }
    if serial not in targets:
        raise RuntimeError(
            f"device serial {serial} is not an hdc target; observed {sorted(targets)}"
        )

    parameter_names = {
        "model": "const.product.model",
        "product_name": "const.product.name",
        "software_version": "const.product.software.version",
        "os_version": "const.product.os.dist.version",
        "api_version": "const.ohos.apiversion",
    }
    parameters = {}
    for key, parameter in parameter_names.items():
        parameters[key] = record(
            f"hdc-{key}",
            [str(hdc_path), "-t", serial, "shell", "param", "get", parameter],
            f"hdc-{key}.txt",
            required=True,
        ).strip()
    hdc_ifconfig = record(
        "hdc-ifconfig",
        [str(hdc_path), "-t", serial, "shell", "ifconfig", "-a"],
        "hdc-ifconfig.txt",
        sanitizer=sanitize_network_output,
        required=True,
    )
    interface_names = parse_device_interface_names(hdc_ifconfig)
    device = {
        "model": parameters["model"],
        "product_name": parameters["product_name"],
        "serial": serial,
        "software_version": parameters["software_version"],
        "os_version": parameters["os_version"],
        "api_version": parameters["api_version"],
        "interface_names": interface_names,
    }

    write_json(raw_dir / "host.json", host)
    write_json(raw_dir / "device.json", device)
    write_json(raw_dir / "commands.json", commands)
    return raw_dir


def command_capture(arguments):
    output_dir = Path(arguments.output)
    output_dir.mkdir(parents=True, exist_ok=True)
    hdc_path = find_hdc(arguments.hdc)
    raw_dir = capture_host_and_device(
        output_dir, arguments.device_serial, hdc_path
    )

    template_path = output_dir / "device-entry.template.json"
    if not template_path.exists():
        write_json(template_path, device_entry_template(arguments.device_serial))
    device_entry_path = arguments.device_entry
    measurement_path = arguments.measurement
    report_path = output_dir / "report.json"
    status = compose_report(
        raw_dir,
        arguments.device_serial,
        report_path,
        device_entry_path,
        measurement_path,
    )
    print(f"report written to {report_path}")
    print(f"raw evidence written to {raw_dir}")
    return status


def build_parser():
    parser = argparse.ArgumentParser(
        description="DeskMate Gate 2A USB-network hardware probe"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    classify_parser = subparsers.add_parser(
        "classify", help="classify a normalized capture"
    )
    classify_parser.add_argument("--capture", required=True)
    classify_parser.add_argument("--output", required=True)
    classify_parser.set_defaults(handler=command_classify)

    validate_parser = subparsers.add_parser(
        "validate-report", help="validate a complete Gate 2A report"
    )
    validate_parser.add_argument("--report", required=True)
    validate_parser.set_defaults(handler=command_validate_report)

    link_parser = subparsers.add_parser(
        "link-summary", help="derive RTT/jitter/throughput from raw evidence"
    )
    link_parser.add_argument("--ping", required=True)
    link_parser.add_argument("--iperf-json", required=True)
    link_parser.add_argument(
        "--direction",
        required=True,
        choices=("host_to_device", "device_to_host"),
    )
    link_parser.add_argument("--interface", required=True)
    link_parser.add_argument("--target", required=True)
    link_parser.add_argument("--output", required=True)
    link_parser.set_defaults(handler=command_link_summary)

    topology_parser = subparsers.add_parser(
        "topology", help="derive USB device topology from ioreg output"
    )
    topology_parser.add_argument("--ioreg", required=True)
    topology_parser.add_argument("--interfaces-ioreg")
    topology_parser.add_argument("--serial", required=True)
    topology_parser.add_argument("--output", required=True)
    topology_parser.set_defaults(handler=command_topology)

    network_parser = subparsers.add_parser(
        "network-inventory",
        help="associate native NCM/ECM interfaces with the target USB device",
    )
    network_parser.add_argument("--usb-ioreg", required=True)
    network_parser.add_argument("--ncm-ioreg")
    network_parser.add_argument("--ecm-ioreg")
    network_parser.add_argument("--device-serial", required=True)
    network_parser.add_argument("--output", required=True)
    network_parser.set_defaults(handler=command_network_inventory)

    compose_parser = subparsers.add_parser(
        "compose", help="compose a report from archived raw host evidence"
    )
    compose_parser.add_argument("--raw-dir", required=True)
    compose_parser.add_argument("--device-serial", required=True)
    compose_parser.add_argument("--device-entry")
    compose_parser.add_argument("--measurement")
    compose_parser.add_argument("--output", required=True)
    compose_parser.set_defaults(handler=command_compose)

    measure_parser = subparsers.add_parser(
        "measure", help="plan or execute ping/iperf3 on a verified Reference Link"
    )
    measure_parser.add_argument("--report", required=True)
    measure_parser.add_argument("--target", required=True)
    measure_parser.add_argument("--interface", required=True)
    measure_parser.add_argument("--output", required=True)
    measure_parser.add_argument("--duration", type=int, default=30)
    measure_parser.add_argument("--execute", action="store_true")
    measure_parser.set_defaults(handler=command_measure)

    capture_parser = subparsers.add_parser(
        "capture", help="capture safe read-only Host/Device network evidence"
    )
    capture_parser.add_argument("--output", required=True)
    capture_parser.add_argument("--device-serial", required=True)
    capture_parser.add_argument("--hdc")
    capture_parser.add_argument("--device-entry")
    capture_parser.add_argument("--measurement")
    capture_parser.set_defaults(handler=command_capture)

    return parser


def main(argv=None):
    arguments = build_parser().parse_args(argv)
    return arguments.handler(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
