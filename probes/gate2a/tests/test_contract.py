#!/usr/bin/env python3

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROBE_ROOT = Path(__file__).resolve().parents[1]
CLI = PROBE_ROOT / "gate2a.py"
sys.path.insert(0, str(PROBE_ROOT))

import gate2a


def run_cli(*arguments):
    return subprocess.run(
        [sys.executable, str(CLI), *map(str, arguments)],
        cwd=PROBE_ROOT,
        check=False,
        capture_output=True,
        text=True,
    )


def direct_ncm_capture(interfaces=None, network_interfaces=None):
    return {
        "usb_device": {
            "name": "MatePad Composite",
            "serial": "59GYD25716204513",
            "vendor": "HISILICON",
            "product": "MatePad",
            "id_vendor": 4817,
            "id_product": 20480,
            "link_speed_mbps": 5000,
            "interfaces": interfaces
            or [
                {
                    "name": "NCM Control",
                    "interface_class": 2,
                    "interface_subclass": 13,
                    "interface_protocol": 0,
                }
            ],
            "ancestors": [
                {"name": "MatePad Composite", "product": "MatePad"},
            ],
            "hubs": [],
        },
        "network_interfaces": network_interfaces
        or [
            {
                "name": "en7",
                "driver_class": "AppleUSBNCMControl",
                "driver_bundle_id": "com.apple.driver.usb.cdc.ncm",
                "usb_id_vendor": 4817,
                "usb_id_product": 20480,
                "usb_serial": "59GYD25716204513",
                "descriptor_family": "ncm",
            }
        ],
        "device_entry": {
            "status": "verified",
            "network_interface": "en7",
            "user_operable": True,
            "developer_mode_required": False,
        },
    }


def valid_pending_report():
    return {
        "schema_version": 1,
        "probe": "gate2a-usb-network",
        "generated_at_utc": "2026-11-24T00:00:00Z",
        "scope": {
            "product_channel": False,
            "reverse_input_supported": False,
            "depends_on_aoa_result": False,
            "reference_link_measurements_only": True,
        },
        "reference_environment": {
            "host": {
                "model": "Mac16,12",
                "chip": "Apple M4",
                "memory_bytes": 17179869184,
                "os_version": "27.3",
                "os_build": "26C5047e",
                "architecture": "arm64",
            },
            "device": {
                "model": "MRDI-W00",
                "product_name": "HUAWEI MatePad Pro",
                "serial": "59GYD25716204513",
                "software_version": "MRDIL-W00 7.0.0.111(SP8C00E111R6P3)",
                "os_version": "7.0.0",
                "api_version": "26",
            },
        },
        "usb_topology": {
            "target_device": {
                "name": "HDC Device",
                "vendor": "HISILICON",
                "product": "HDC Device",
                "serial": "59GYD25716204513",
                "id_vendor": 4817,
                "id_product": 20480,
                "interfaces": [],
            },
            "ancestors": [
                {"name": "USB2.1 Hub", "vendor": "GenesysLogic"},
                {"name": "HDC Device", "vendor": "HISILICON"},
            ],
            "hubs": [
                {"name": "USB2.1 Hub", "vendor": "GenesysLogic"},
            ],
            "observed_hubs": [
                {"name": "USB2.1 Hub", "vendor": "GenesysLogic"},
            ],
            "link_speed_mbps": 480,
            "direct_to_host": False,
            "reference_link": False,
            "reason": "device path contains USB hub(s), so it is not a Reference Link",
        },
        "host_network": {
            "hardware_ports": [
                {"hardware_port": "USB 10/100/1G/2.5G LAN", "device": "en5"}
            ],
            "services": [
                {"name": "USB 10/100/1G/2.5G LAN", "enabled": True},
                {"name": "Wi-Fi", "enabled": True},
            ],
            "interfaces": [
                {
                    "name": "en5",
                    "flags": "8863<UP,BROADCAST>",
                    "mtu": 1500,
                    "status": "inactive",
                    "ipv4_configured": False,
                }
            ],
            "candidate_interfaces": [],
            "associated_device_interface": None,
        },
        "device_network": {
            "interface": None,
            "driver_family": "none",
            "native_bundled_driver": False,
            "rndis_detected": False,
            "eligible": False,
            "reason": "no NCM/ECM network interface is associated with the target device",
        },
        "device_entry": {
            "status": "not_executed",
            "user_operable": None,
            "developer_mode_required": None,
            "path": None,
            "evidence": [],
        },
        "measurements": {
            "status": "not_executed",
            "reference_link": False,
            "reason": "USB sharing is not enabled and the current path is not a Reference Link",
            "ping": None,
            "throughput": None,
        },
        "decision": {
            "status": "pending",
            "reasons": [
                "device entry is not verified",
                "no NCM/ECM interface",
                "link measurements not executed",
            ],
        },
        "artifacts": [],
    }


def valid_passed_report():
    report = valid_pending_report()
    report["reference_environment"]["device"]["interface_names"] = ["lo", "en7"]
    report["usb_topology"].update(
        {
            "target_device": {
                "name": "MatePad Composite",
                "vendor": "HISILICON",
                "product": "MatePad",
                "serial": "59GYD25716204513",
                "id_vendor": 4817,
                "id_product": 20480,
                "interfaces": [
                    {
                        "name": "NCM Control",
                        "interface_class": 2,
                        "interface_subclass": 13,
                        "interface_protocol": 0,
                    }
                ],
            },
            "ancestors": [
                {"name": "MatePad Composite", "vendor": "HISILICON"},
            ],
            "hubs": [],
            "observed_hubs": [],
            "hub_count": 0,
            "link_speed_mbps": 5000,
            "direct_to_host": True,
            "reference_link": True,
            "reason": "device is directly attached to the Mac USB controller",
        }
    )
    report["host_network"].update(
        {
            "candidate_interfaces": [
                {
                    "name": "en7",
                    "driver_family": "ncm",
                    "driver_class": "AppleUSBNCMControl",
                    "driver_bundle_id": "com.apple.driver.usb.cdc.ncm",
                    "native_bundled_driver": True,
                    "associated_to_target_device": True,
                    "association": "serial_match",
                }
            ],
            "associated_device_interface": "en7",
        }
    )
    report["device_network"] = {
        "interface": "en7",
        "driver_family": "ncm",
        "network_driver_class": "AppleUSBNCMControl",
        "network_driver_bundle_id": "com.apple.driver.usb.cdc.ncm",
        "native_bundled_driver": True,
        "rndis_detected": False,
        "eligible": True,
        "reason": "one native bundled NCM/ECM interface is associated with the target device",
    }
    report["device_entry"] = {
        "status": "verified",
        "device_serial": "59GYD25716204513",
        "observed_at_utc": "2026-11-24T00:00:00Z",
        "operator": "operator",
        "user_operable": True,
        "developer_mode_required": False,
        "path": "Settings > Connected devices > USB network sharing",
        "steps": ["Open the documented Device setting"],
        "evidence": ["evidence/usb-share-entry.png"],
        "notes": "User-operable entry verified without Developer Mode.",
    }
    report["measurements"] = {
        "status": "executed",
        "reference_link": True,
        "reason": "ping and both iperf3 directions completed on the Reference Link",
        "interface": "en7",
        "target": "192.0.2.2",
        "ping": {
            "raw_path": "raw/ping.txt",
            "sha256": "1" * 64,
            "packets_transmitted": 120,
            "packets_received": 120,
            "packet_loss_percent": 0.0,
            "rtt_ms": {
                "min": 0.8,
                "avg": 1.0,
                "max": 1.4,
                "stddev": 0.1,
            },
            "jitter_ms": 0.1,
        },
        "throughput": {
            "runs": [
                {
                    "direction": "host_to_device",
                    "raw_path": "raw/iperf-host-to-device.json",
                    "sha256": "2" * 64,
                    "bits_per_second": 900000000.0,
                },
                {
                    "direction": "device_to_host",
                    "raw_path": "raw/iperf-device-to-host.json",
                    "sha256": "3" * 64,
                    "bits_per_second": 850000000.0,
                },
            ]
        },
        "commands": [
            {
                "purpose": "ping",
                "argv": ["ping", "-b", "en7", "192.0.2.2"],
                "returncode": 0,
            },
            {
                "purpose": "iperf-host-to-device",
                "argv": ["iperf3", "-c", "192.0.2.2", "-J"],
                "returncode": 0,
            },
            {
                "purpose": "iperf-device-to-host",
                "argv": ["iperf3", "-c", "192.0.2.2", "-R", "-J"],
                "returncode": 0,
            },
        ],
    }
    report["decision"] = {
        "status": "passed",
        "reasons": ["all Gate 2A contract requirements are satisfied"],
    }
    report["artifacts"] = [
        {"path": "raw/ping.txt", "sha256": "1" * 64},
        {"path": "raw/iperf-host-to-device.json", "sha256": "2" * 64},
        {"path": "raw/iperf-device-to-host.json", "sha256": "3" * 64},
    ]
    return report


def ioreg_usb_fixture(include_interface=True):
    fixture = """+-o Root  <class IORegistryEntry, id 0x1, retain 32>
  +-o AppleT8132USBXHCI@01000000  <class AppleT8132USBXHCI, id 0x2, retain 20>
    +-o USB2.1 Hub@01100000  <class IOUSBHostDevice, id 0x3, retain 43>
    | {
    |   "USB Vendor Name" = "Bridgesil"
    |   "USB Product Name" = "USB2.1 Hub"
    |   "idVendor" = 10473
    |   "idProduct" = 8921
    |   "UsbLinkSpeed" = 5000000000
    | }
    |
    | +-o USB2.1 Hub@01140000  <class IOUSBHostDevice, id 0x4, retain 37>
    | | {
    | |   "USB Vendor Name" = "GenesysLogic"
    | |   "USB Product Name" = "USB2.1 Hub"
    | |   "idVendor" = 1293
    | |   "idProduct" = 2903
    | |   "UsbLinkSpeed" = 480000000
    | | }
    | |
    | | +-o "HDC Device"@01130000  <class IOUSBHostDevice, id 0x5, retain 38>
    | |   | {
    | |   |   "kUSBSerialNumberString" = "59GYD25716204513"
    | |   |   "USB Serial Number" = "59GYD25716204513"
    | |   |   "USB Vendor Name" = "HISILICON"
    | |   |   "USB Product Name" = ""HDC Device""
    | |   |   "UsbLinkSpeed" = 480000000
    | |   |   "UsbExclusiveOwner" = "pid 67816, hdc"
    | |   |   "idVendor" = 4817
    | |   |   "idProduct" = 20480
    | |   | }
    | |   |
"""
    if include_interface:
        fixture += """    | |   +-o HDC Interface@0  <class IOUSBHostInterface, id 0x6, retain 11>
    | |     {
    | |       "bInterfaceClass" = 255
    | |       "bInterfaceSubClass" = 80
    | |       "bInterfaceProtocol" = 1
    | |       "kUSBString" = "HDC Interface"
    | |     }
"""
    return fixture


def ioreg_interfaces_fixture():
    return """+-o HDC Interface@0  <class IOUSBHostInterface, id 0x6, retain 11>
  | {
  |   "USB Serial Number" = "59GYD25716204513"
  |   "USB Vendor Name" = "HISILICON"
  |   "USB Product Name" = ""HDC Device""
  |   "idVendor" = 4817
  |   "idProduct" = 20480
  |   "bInterfaceClass" = 255
  |   "bInterfaceSubClass" = 80
  |   "bInterfaceProtocol" = 1
  |   "kUSBString" = "HDC Interface"
  | }
"""


def ioreg_realtek_fixture():
    return """+-o Root  <class IORegistryEntry, id 0x1, retain 32>
  +-o AppleT8132USBXHCI@01000000  <class AppleT8132USBXHCI, id 0x2, retain 20>
    +-o USB3.2 Hub@01200000  <class IOUSBHostDevice, id 0x3, retain 40>
      | {
      |   "USB Vendor Name" = "Bridgesil"
      |   "USB Product Name" = "USB3.2 Hub"
      |   "UsbLinkSpeed" = 10000000000
      | }
      |
      +-o USB 10/100/1G/2.5G LAN@01210000  <class IOUSBHostDevice, id 0x4, retain 88>
        | {
        |   "USB Serial Number" = "4013000001"
        |   "USB Vendor Name" = "Realtek"
        |   "USB Product Name" = "USB 10/100/1G/2.5G LAN"
        |   "UsbLinkSpeed" = 5000000000
        |   "idVendor" = 3034
        |   "idProduct" = 33110
        | }
        |
        +-o AppleUSBHostCompositeDevice  <class AppleUSBHostCompositeDevice, id 0x5>
"""


def ioreg_ncm_fixture():
    return """+-o AppleUSBNCMControl  <class AppleUSBNCMControl, id 0x1, retain 8>
  | {
  |   "CFBundleIdentifier" = "com.apple.driver.usb.cdc.ncm"
  |   "IOClass" = "AppleUSBNCMControl"
  |   "bInterfaceClass" = 2
  |   "bInterfaceSubClass" = 13
  |   "bInterfaceProtocol" = "*"
  | }
  |
  +-o AppleUSBNCMData  <class AppleUSBNCMData, id 0x2, retain 7>
    | {
    |   "CFBundleIdentifier" = "com.apple.driver.usb.cdc.ncm"
    |   "idVendor" = 3034
    |   "idProduct" = 33110
    |   "IOVendor" = "USB NCM"
    | }
    |
    +-o en5  <class IOEthernetInterface, id 0x3, retain 11>
      | {
      |   "BSD Name" = "en5"
      |   "IOInterfaceUnit" = 5
      | }
"""


class Gate2AContractTests(unittest.TestCase):
    def test_rndis_is_rejected_even_when_other_evidence_looks_eligible(self):
        capture = direct_ncm_capture(
            interfaces=[
                {
                    "name": "Remote NDIS",
                    "interface_class": 224,
                    "interface_subclass": 1,
                    "interface_protocol": 3,
                }
            ]
        )
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            capture_path = directory / "capture.json"
            output_path = directory / "classification.json"
            capture_path.write_text(json.dumps(capture), encoding="utf-8")

            result = run_cli(
                "classify",
                "--capture",
                capture_path,
                "--output",
                output_path,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            classification = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(classification["driver_family"], "rndis")
            self.assertTrue(classification["rndis_detected"])
            self.assertFalse(classification["native_bundled_driver"])
            self.assertFalse(classification["eligible"])
            self.assertEqual(classification["decision"]["status"], "failed")
            self.assertIn("RNDIS", classification["decision"]["reasons"][0])

    def test_direct_native_ncm_is_an_eligible_reference_link_candidate(self):
        capture = direct_ncm_capture()
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            capture_path = directory / "capture.json"
            output_path = directory / "classification.json"
            capture_path.write_text(json.dumps(capture), encoding="utf-8")

            result = run_cli(
                "classify",
                "--capture",
                capture_path,
                "--output",
                output_path,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            classification = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(classification["driver_family"], "ncm")
            self.assertEqual(classification["interface"], "en7")
            self.assertTrue(classification["native_bundled_driver"])
            self.assertTrue(classification["eligible"])
            self.assertTrue(classification["topology"]["reference_link"])
            self.assertEqual(classification["topology"]["hub_count"], 0)
            self.assertEqual(classification["decision"]["status"], "pending")

    def test_third_party_ncm_driver_is_not_accepted(self):
        capture = direct_ncm_capture(
            network_interfaces=[
                {
                    "name": "en7",
                    "driver_class": "VendorNCMControl",
                    "driver_bundle_id": "com.example.vendor.ncm",
                    "usb_id_vendor": 4817,
                    "usb_id_product": 20480,
                    "usb_serial": "59GYD25716204513",
                    "descriptor_family": "ncm",
                }
            ]
        )
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            capture_path = directory / "capture.json"
            output_path = directory / "classification.json"
            capture_path.write_text(json.dumps(capture), encoding="utf-8")

            result = run_cli(
                "classify",
                "--capture",
                capture_path,
                "--output",
                output_path,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            classification = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(classification["driver_family"], "ncm")
            self.assertFalse(classification["native_bundled_driver"])
            self.assertFalse(classification["eligible"])
            self.assertEqual(classification["decision"]["status"], "failed")

    def test_native_ncm_behind_a_hub_stays_pending_as_non_reference(self):
        capture = direct_ncm_capture()
        capture["usb_device"]["link_speed_mbps"] = 480
        capture["usb_device"]["hubs"] = [
            {"name": "USB2.1 Hub", "vendor": "Bridgesil"}
        ]
        capture["usb_device"]["ancestors"] = [
            {"name": "USB2.1 Hub", "vendor": "Bridgesil"},
            {"name": "MatePad Composite", "product": "MatePad"},
        ]
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            capture_path = directory / "capture.json"
            output_path = directory / "classification.json"
            capture_path.write_text(json.dumps(capture), encoding="utf-8")

            result = run_cli(
                "classify",
                "--capture",
                capture_path,
                "--output",
                output_path,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            classification = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertTrue(classification["native_bundled_driver"])
            self.assertFalse(classification["eligible"])
            self.assertFalse(classification["topology"]["reference_link"])
            self.assertEqual(classification["decision"]["status"], "pending")
            self.assertTrue(
                any(
                    "not a direct Reference Link" in reason
                    for reason in classification["decision"]["reasons"]
                )
            )

    def test_report_requires_environment_topology_and_manual_entry_fields(self):
        report = valid_pending_report()
        del report["reference_environment"]["host"]["os_build"]
        del report["usb_topology"]["hubs"]
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "report.json"
            report_path.write_text(json.dumps(report), encoding="utf-8")

            result = run_cli("validate-report", "--report", report_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("reference_environment.host.os_build", result.stderr)
            self.assertIn("usb_topology.hubs", result.stderr)

    def test_link_summary_is_derived_from_raw_ping_and_iperf_outputs(self):
        ping_output = """PING 192.0.2.2 (192.0.2.2): 56 data bytes
64 bytes from 192.0.2.2: icmp_seq=0 ttl=64 time=1.000 ms
64 bytes from 192.0.2.2: icmp_seq=1 ttl=64 time=2.000 ms
64 bytes from 192.0.2.2: icmp_seq=2 ttl=64 time=3.000 ms
64 bytes from 192.0.2.2: icmp_seq=3 ttl=64 time=5.000 ms

--- 192.0.2.2 ping statistics ---
4 packets transmitted, 4 packets received, 0.0% packet loss
round-trip min/avg/max/stddev = 1.000/2.750/5.000/1.479 ms
"""
        iperf_output = {
            "start": {"test_start": {"protocol": "TCP"}},
            "end": {
                "sum_sent": {"bits_per_second": 987654321.0},
                "sum_received": {"bits_per_second": 123456789.0},
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ping_path = directory / "ping.txt"
            iperf_path = directory / "iperf.json"
            output_path = directory / "link.json"
            ping_path.write_text(ping_output, encoding="utf-8")
            iperf_path.write_text(json.dumps(iperf_output), encoding="utf-8")

            result = run_cli(
                "link-summary",
                "--ping",
                ping_path,
                "--iperf-json",
                iperf_path,
                "--direction",
                "host_to_device",
                "--interface",
                "en7",
                "--target",
                "192.0.2.2",
                "--output",
                output_path,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "executed")
            self.assertEqual(summary["ping"]["packets_transmitted"], 4)
            self.assertEqual(summary["ping"]["packets_received"], 4)
            self.assertEqual(summary["ping"]["packet_loss_percent"], 0.0)
            self.assertAlmostEqual(summary["ping"]["rtt_ms"]["avg"], 2.75)
            self.assertAlmostEqual(summary["ping"]["jitter_ms"], 1.333333, places=5)
            self.assertEqual(
                summary["throughput"]["runs"][0]["bits_per_second"], 123456789.0
            )
            self.assertTrue(Path(summary["ping"]["raw_path"]).is_file())
            self.assertTrue(
                Path(summary["throughput"]["runs"][0]["raw_path"]).is_file()
            )

    def test_usb_topology_records_hub_chain_speed_and_interface_class(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ioreg_path = directory / "ioreg.txt"
            output_path = directory / "topology.json"
            ioreg_path.write_text(ioreg_usb_fixture(), encoding="utf-8")

            result = run_cli(
                "topology",
                "--ioreg",
                ioreg_path,
                "--serial",
                "59GYD25716204513",
                "--output",
                output_path,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            topology = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(topology["target_device"]["name"], "HDC Device")
            self.assertEqual(topology["target_device"]["vendor"], "HISILICON")
            self.assertEqual(topology["target_device"]["serial"], "59GYD25716204513")
            self.assertEqual(topology["target_device"]["link_speed_mbps"], 480)
            self.assertEqual(topology["target_device"]["owner"], "pid 67816, hdc")
            self.assertEqual(
                [hub["vendor"] for hub in topology["hubs"]],
                ["Bridgesil", "GenesysLogic"],
            )
            self.assertEqual(topology["hub_count"], 2)
            self.assertFalse(topology["direct_to_host"])
            self.assertFalse(topology["reference_link"])
            self.assertIn("hub", topology["reason"])
            self.assertEqual(
                topology["target_device"]["interfaces"],
                [
                    {
                        "name": "HDC Interface",
                        "interface_class": 255,
                        "interface_subclass": 80,
                        "interface_protocol": 1,
                        "driver_bundle_id": None,
                        "driver_class": None,
                    }
                ],
            )

    def test_topology_can_merge_interfaces_from_separate_ioreg_query(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            usb_path = directory / "usb.txt"
            interfaces_path = directory / "interfaces.txt"
            output_path = directory / "topology.json"
            usb_path.write_text(
                ioreg_usb_fixture(include_interface=False), encoding="utf-8"
            )
            interfaces_path.write_text(
                ioreg_interfaces_fixture(), encoding="utf-8"
            )

            result = run_cli(
                "topology",
                "--ioreg",
                usb_path,
                "--interfaces-ioreg",
                interfaces_path,
                "--serial",
                "59GYD25716204513",
                "--output",
                output_path,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            topology = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(topology["target_device"]["product"], "HDC Device")
            self.assertEqual(
                topology["target_device"]["interfaces"][0]["interface_class"], 255
            )

    def test_network_inventory_does_not_claim_an_unrelated_native_ncm_adapter(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            usb_path = directory / "usb-ioreg.txt"
            ncm_path = directory / "ncm-ioreg.txt"
            output_path = directory / "network.json"
            usb_path.write_text(ioreg_realtek_fixture(), encoding="utf-8")
            ncm_path.write_text(ioreg_ncm_fixture(), encoding="utf-8")

            result = run_cli(
                "network-inventory",
                "--usb-ioreg",
                usb_path,
                "--ncm-ioreg",
                ncm_path,
                "--device-serial",
                "59GYD25716204513",
                "--output",
                output_path,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            inventory = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(len(inventory["interfaces"]), 1)
            interface = inventory["interfaces"][0]
            self.assertEqual(interface["name"], "en5")
            self.assertEqual(interface["driver_family"], "ncm")
            self.assertEqual(interface["driver_bundle_id"], "com.apple.driver.usb.cdc.ncm")
            self.assertTrue(interface["native_bundled_driver"])
            self.assertEqual(interface["usb_serial"], "4013000001")
            self.assertEqual(interface["usb_vendor"], "Realtek")
            self.assertFalse(interface["associated_to_target_device"])
            self.assertIsNone(inventory["associated_interface"])
            self.assertFalse(inventory["device_network_eligible"])
            self.assertIn("not associated", inventory["reason"])

    def test_ioreg_sanitizer_redacts_text_and_binary_mac_addresses(self):
        output = (
            '    |   "IOMACAddress" = <020000000005>\n'
            "Ethernet Address: 02:00:00:00:00:05\n"
        )

        sanitized = gate2a.sanitize_ioreg_output(output)

        self.assertNotIn("020000000005", sanitized)
        self.assertNotIn("02:00:00:00:00:05", sanitized)
        self.assertIn("<redacted-mac>", sanitized)
        self.assertIn("<redacted>", sanitized)
        self.assertIn('"IOMACAddress" = <redacted-mac>', sanitized)

    def test_report_validation_accepts_a_complete_passed_report(self):
        report = valid_passed_report()
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "report.json"
            report_path.write_text(json.dumps(report), encoding="utf-8")

            result = run_cli("validate-report", "--report", report_path)

            self.assertEqual(result.returncode, 0, result.stderr)

    def test_report_validation_rejects_incomplete_passed_evidence(self):
        report = valid_passed_report()
        report["device_entry"].update(
            {
                "user_operable": None,
                "path": None,
                "evidence": [],
            }
        )
        report["device_network"]["network_driver_bundle_id"] = None
        report["measurements"].update(
            {
                "reference_link": False,
                "ping": None,
                "throughput": None,
            }
        )
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "report.json"
            report_path.write_text(json.dumps(report), encoding="utf-8")

            result = run_cli("validate-report", "--report", report_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("device_entry.user_operable", result.stderr)
            self.assertIn("device_entry.path", result.stderr)
            self.assertIn("device_entry.evidence", result.stderr)
            self.assertIn("network_driver_bundle_id", result.stderr)
            self.assertIn("measurements.reference_link", result.stderr)
            self.assertIn("measurements.ping", result.stderr)
            self.assertIn("measurements.throughput", result.stderr)

    def test_report_validation_rejects_unarchived_measurement_evidence(self):
        report = valid_passed_report()
        report["artifacts"] = report["artifacts"][:1]
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "report.json"
            report_path.write_text(json.dumps(report), encoding="utf-8")

            result = run_cli("validate-report", "--report", report_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("measurement evidence is not archived", result.stderr)

    def test_report_validation_refuses_to_pass_an_rndis_link(self):
        report = valid_pending_report()
        report["device_network"].update(
            {
                "interface": "en9",
                "driver_family": "rndis",
                "rndis_detected": True,
                "eligible": False,
            }
        )
        report["decision"] = {"status": "passed", "reasons": ["incorrectly marked"]}
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "report.json"
            report_path.write_text(json.dumps(report), encoding="utf-8")

            result = run_cli("validate-report", "--report", report_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("RNDIS must produce failed", result.stderr)

    def test_compose_keeps_hdc_only_non_reference_hardware_pending(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            raw_dir = directory / "raw"
            raw_dir.mkdir()
            (raw_dir / "host.json").write_text(
                json.dumps(
                    {
                        "model": "Mac16,12",
                        "chip": "Apple M4",
                        "memory_bytes": 17179869184,
                        "os_version": "27.3",
                        "os_build": "26C5047e",
                        "architecture": "arm64",
                    }
                ),
                encoding="utf-8",
            )
            (raw_dir / "device.json").write_text(
                json.dumps(
                    {
                        "model": "MRDI-W00",
                        "product_name": "HUAWEI MatePad Pro",
                        "serial": "59GYD25716204513",
                        "software_version": "MRDIL-W00 7.0.0.111(SP8C00E111R6P3)",
                        "os_version": "7.0.0",
                        "api_version": "26",
                        "interface_names": ["lo", "wlan0", "chba0"],
                    }
                ),
                encoding="utf-8",
            )
            (raw_dir / "ioreg-usb.txt").write_text(
                ioreg_usb_fixture(), encoding="utf-8"
            )
            (raw_dir / "ioreg-ncm.txt").write_text("", encoding="utf-8")
            (raw_dir / "ioreg-ecm.txt").write_text("", encoding="utf-8")
            (raw_dir / "networksetup-hardware-ports.txt").write_text(
                "\nHardware Port: USB 10/100/1G/2.5G LAN\n"
                "Device: en5\n"
                "Ethernet Address: 02:00:00:00:00:05\n",
                encoding="utf-8",
            )
            (raw_dir / "networksetup-services.txt").write_text(
                "An asterisk (*) denotes that a network service is disabled.\n"
                "USB 10/100/1G/2.5G LAN\nWi-Fi\n",
                encoding="utf-8",
            )
            (raw_dir / "ifconfig.txt").write_text(
                "en5: flags=8863<UP,BROADCAST> mtu 1500\n"
                "\tether 02:00:00:00:00:05\n"
                "\tstatus: inactive\n",
                encoding="utf-8",
            )
            (raw_dir / "hdc-ifconfig.txt").write_text(
                "lo        Link encap:Local Loopback\n"
                "          inet <redacted>  Mask:255.0.0.0\n"
                "wlan0     Link encap:Ethernet  HWaddr <redacted>\n"
                "          inet <redacted>  Bcast:<redacted>  Mask:255.255.255.0\n"
                "Hisilicon0 Link encap:Ethernet  HWaddr 00:00:00:00:00:00\n",
                encoding="utf-8",
            )
            output_path = directory / "report.json"

            result = run_cli(
                "compose",
                "--raw-dir",
                raw_dir,
                "--device-serial",
                "59GYD25716204513",
                "--output",
                output_path,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["probe"], "gate2a-usb-network")
            self.assertFalse(report["scope"]["reverse_input_supported"])
            self.assertFalse(report["scope"]["product_channel"])
            self.assertFalse(report["scope"]["depends_on_aoa_result"])
            self.assertEqual(report["usb_topology"]["hub_count"], 2)
            self.assertEqual(report["usb_topology"]["link_speed_mbps"], 480)
            self.assertFalse(report["usb_topology"]["reference_link"])
            self.assertEqual(report["device_network"]["driver_family"], "none")
            self.assertIsNone(report["device_network"]["interface"])
            self.assertEqual(
                report["reference_environment"]["device"]["interface_names"],
                ["lo", "wlan0", "Hisilicon0"],
            )
            self.assertEqual(report["device_entry"]["status"], "not_executed")
            self.assertEqual(report["measurements"]["status"], "not_executed")
            self.assertEqual(report["decision"]["status"], "pending")
            self.assertEqual(
                report["host_network"]["hardware_ports"][0]["device"], "en5"
            )
            self.assertNotIn(
                "ethernet_address", report["host_network"]["hardware_ports"][0]
            )

            validation = run_cli("validate-report", "--report", output_path)
            self.assertEqual(validation.returncode, 0, validation.stderr)

            measurement_path = directory / "link-measurement.json"
            measurement_path.write_text(
                json.dumps(
                    {
                        "status": "executed",
                        "reference_link": True,
                        "reason": "unverified historical measurement",
                        "interface": "en7",
                        "target": "192.0.2.2",
                        "ping": {},
                        "throughput": {"runs": []},
                        "commands": [],
                    }
                ),
                encoding="utf-8",
            )
            invalid_output = directory / "invalid-report.json"

            inconsistent = run_cli(
                "compose",
                "--raw-dir",
                raw_dir,
                "--device-serial",
                "59GYD25716204513",
                "--measurement",
                measurement_path,
                "--output",
                invalid_output,
            )

            self.assertNotEqual(inconsistent.returncode, 0)
            self.assertIn("Reference Link", inconsistent.stderr)
            self.assertFalse(invalid_output.exists())

    def test_measure_refuses_to_run_on_a_pending_non_reference_report(self):
        report = valid_pending_report()
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            report_path = directory / "report.json"
            output_dir = directory / "measurement"
            report_path.write_text(json.dumps(report), encoding="utf-8")

            result = run_cli(
                "measure",
                "--report",
                report_path,
                "--target",
                "192.0.2.2",
                "--interface",
                "en7",
                "--output",
                output_dir,
                "--execute",
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusing", result.stderr.lower())
            self.assertFalse((output_dir / "ping.txt").exists())

    def test_report_requires_macos_network_service_inventory(self):
        report = valid_pending_report()
        report["host_network"] = {
            "hardware_ports": [],
            "services": [],
            "interfaces": [],
            "candidate_interfaces": [],
        }
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "report.json"
            report_path.write_text(json.dumps(report), encoding="utf-8")

            result = run_cli("validate-report", "--report", report_path)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("host_network.associated_device_interface", result.stderr)


if __name__ == "__main__":
    unittest.main()
