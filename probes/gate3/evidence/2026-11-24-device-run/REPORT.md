# Gate 3 device run — 2026-11-24

## Device and link

- HUAWEI MatePad Pro, model `MRDI-W00`, display version
  `MRDIL-W00 7.0.0.111(<redacted-serial>)`.
- OpenHarmony `7.0.0.111`, API 26, `arm64-v8a`, 2800x1840 at 60 Hz.
- Serial is redacted.
- USB path: Bridgesil `USB2.1 Hub`, USB 2.0 High-Speed. This is **not** an
  OpenHarmony Reference Link and is not Reference Link evidence.

## Build and install

- DevEco Studio 26 automatic signing generated a Huawei-managed debug profile
  for bundle `com.deskmate.gate3` and the connected device.
- The signing material and passwords stayed in local DevEco configuration and
  are not part of this evidence.
- Signed HAP: 2,471,801 bytes, SHA-256
  `633f6115558bfd1da204a3ad5c7f18d8738824161fd10786be7452357652d55e`.
- One install attempt completed with `install bundle successfully`; `bm dump`
  then showed `com.deskmate.gate3` installed as a debug bundle.

## Observations

- `aa start -a EntryAbility -b com.deskmate.gate3` launched the probe and
  reported `start ability successfully`.
- The initial screen showed `Surface output - no CPU pixel readback`, an empty
  black XComponent area, and `waiting for Surface`. The Surface state then
  became ready.
- Pressing **Run recovery suite** produced `suite.state = complete` and
  `verification = passed`. The XComponent rendered the 640x360 color-bar test
  pattern; screenshots are in this directory.
- Pressing **Freeze for LGF check** held the last visible frame. The frozen
  screenshot remained visually identical while the probe reported
  `frozen = true`.
- **Mark LGF stable** recorded
  `last_good_frame.manual_observation = "stable"`.
- **Resume after freeze** set `frozen = false`; the frame remained visible and
  the suite stayed `complete / passed`.
- The probe never read pixels back. The JPEGs were captured externally with
  the system `snapshot_display` command solely to record the manual visual
  check.

## Capability and measurements

- H.264: `OMX.hisi.video.decoder.avc`, hardware, low-latency feature present,
  128..8192 both axes, pixel formats 2 and 3, profiles 0/8/4.
- H.265: `OMX.hisi.video.decoder.hevc`, hardware, low-latency feature present,
  128..8192 both axes, pixel formats 2 and 3, profiles 0/1.
- Fixture: 217,122 bytes, 125 NAL units, 60 access units, 2 keyframes, first
  access unit is IDR with SPS and PPS.
- Low-latency configure key was requested and accepted.
- Initial decode: 2 rendered outputs from 8 access units in 62,103,438 ns.
- Flush: operation 12,129,688 ns; first output 29,166,875 ns.
- Stop: operation 34,138,125 ns; first output 66,287,343 ns.
- Reset: operation 45,245,314 ns; first output 75,296,875 ns.
- Rebuild: operation 74,233,750 ns; first output 103,627,188 ns.
- Every recovery retained the Surface Lease, resubmitted SPS/PPS and IDR,
  produced current-epoch output, and reported zero residual decoder callbacks
  and zero stale-epoch frees.
- Callback audit: 24 output callbacks, 85 total callback events, callbacks
  observed off the control thread, zero control-thread violations, zero
  render failures, zero free failures. Two stale input callbacks were counted
  during the whole run.
- The platform logged one `AVCODEC_JANK_FAULT` with `DURATION:69584055` during
  the flush action. No app crash or fault-log entry was produced. This is
  recorded as a platform jank signal, not hidden as a passing latency result.

## Limits

- `surface_retained` proves lease validity and successful `SetSurface`, not
  compositor scanout.
- `low_latency_key_accepted` proves the configure key was accepted, not
  decoder scheduling latency.
- Annex-B Host interoperability remains `pending` until issue 05 supplies the
  real Host sample.
- Gate 4 latency work remains blocked; this run is a functional recovery probe,
  not a latency baseline.
- USB 2.0 through a hub cannot support Reference Link claims.

## Evidence files

- `hdc-identity.log`: live device parameters and display mode.
- `bundle-info.json`: installed bundle summary from `bm dump`.
- `install.log`: install result and process exit code.
- `suite-evidence.json`: raw evidence JSON displayed by the probe.
- `01-initial.jpeg`, `02-suite-complete.jpeg`, `03-frozen.jpeg`,
  `04-resumed.jpeg`: external screenshots for the manual visual check.
- `hilog-gate3.log`: filtered Gate 3, AVCodec, and XPERF log lines.
- `SHA256SUMS`: hashes of the files above.
