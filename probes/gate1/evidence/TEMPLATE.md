# DeskMate Gate 1 Evidence

- Run ID:
- Date/time (UTC+08:00):
- Operator:
- Result: `PASS | FAIL | NOT EVALUATED`
- Reference Environment: `yes | no`
- Reference Link (direct, no hub/dock): `yes | no | not verified`
- Why:

## Frozen environment

| Item | Observed value | Source |
| --- | --- | --- |
| Mac model | | `host.txt` |
| macOS version/build | | `host.txt` |
| Device model | | `hdc.txt` / UI |
| Device serial | | `ioreg-usb.txt` |
| HarmonyOS build | | `hdc.txt` / UI |
| API level | | `hdc.txt` / UI |
| Display mode | | UI |
| Cable | | physical label |
| DevEco/SDK | | build log |
| HAP SHA-256 | | `SHA256SUMS` |
| Native `.so` SHA-256 | | `SHA256SUMS` |

## USB topology and negotiated speed

- `system_profiler SPUSBDataType -json` result:
- `ioreg-usb.txt` target excerpt:
- Parent chain (root → ... → device):
- `Device Speed` / `UsbLinkSpeed`:
- Hub/dock present:
- Evidence files and SHA-256:

## Build, sign, install, authorize

- Host build command/result:
- Host runtime path:
- Device build command/result:
- ArkTS/native warnings:
- Signing profile/bundle name/device ID:
- Install command/exit/output:
- `requestAccessoryRight` result:
- `openAccessory` result / `accessoryFd` validity (do not publish raw FD if sensitive):

## AOA and duplex

- Normal-mode VID:PID/bus/address/speed:
- `GET_PROTOCOL(51)` result:
- `SEND_STRING(52)` indices/result:
- `START(53)` result:
- Re-enumerated VID:PID/bus/address/speed:
- Interface and Bulk endpoints:
- `HELLO_ACK`:
- Duration and data bytes:
- Echoes/mismatches/unmatched:
- Heartbeats/ACK/loss/RTT:
- Device stats seen:
- Host JSON path:

## Split, coalesced, ZLP

- Split chunks observed:
- Coalesced frames observed:
- `zero_length_out_scheduled/completed`:
- `zero_length_in_completed`:
- Device `zero_reads` / `zero_writes`:
- `parse_errors`:
- Bus analyzer available/result:
- Precise claim supported:

## Cancellation and cleanup

- Simultaneous IN/OUT cancel issued:
- IN/OUT cancel return codes:
- Cancel issue gap:
- IN/OUT callback after cancel:
- `active_on_release`:
- `cleanup_safe`:
- Interface/handle release result:

## Wake branches

### poll/eventfd

- Start/open sequence:
- Device raw JSON:
- `stop_to_wake_us`:
- `stop_to_worker_exit_ms`:
- `wake_reason`:
- Counter evidence:

### Targeted SIGUSR1 without SA_RESTART

- Start/open sequence:
- Device raw JSON:
- `stop_to_wake_us`:
- `stop_to_worker_exit_ms`:
- `wake_reason` / errno:
- Counter evidence:

### Official closeAccessory wake

- Start/open sequence:
- A. reject-new-work JSON:
- B. `closeAccessory` result:
- C. join JSON:
- `stop_to_wake_us` / `stop_to_worker_exit_ms`:
- `wake_reason`:
- Evidence that native did not close `accessoryFd`:

## Background behavior

- Time in background:
- Host heartbeat/loss observation:
- Device lifecycle delta:
- I/O resume/stop result:
- Timestamps and raw files:

## Unplug and reconnect

- Unplug time and Host error:
- Device errno / worker result:
- Unsafe active transfers or retained resources:
- Reconnect enumeration:
- New authorization/HELLO/echo:

## Unexecuted work

| Item | Reason | Consequence |
| --- | --- | --- |
| | | |

## Reviewer conclusion

- Gate 1 claim:
- Missing evidence:
- Follow-up issue/owner:
