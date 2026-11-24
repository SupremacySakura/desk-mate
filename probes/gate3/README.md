# Gate 3: AVCodec Surface recovery probe

This directory implements issue 07 only. It probes the OpenHarmony API 26
native video decoder with a Surface output, exercises recovery, and records
what was actually observed. It does not modify the DeskMate spec, other issues,
or ADRs.

## Scope

- H.264 Annex-B input is split into access units and submitted to
  `OH_VideoDecoder` with the low-latency configure key.
- Decoded output is rendered only through `OH_VideoDecoder_RenderOutputBuffer`.
  There is no output buffer mapping, pixel copy, or CPU readback.
- A single dedicated control thread owns `Flush`, `Stop`, `Reset`,
  `Destroy`, `Configure`, `SetSurface`, `Prepare`, and `Start`.
- Decoder callbacks only enqueue data: input readiness is copied on the control
  thread; output readiness is posted to a generation-tagged queue.
- `SurfaceLeaseCoordinator` owns and leases native windows. Reference and
  unreference calls are serialized. A destroyed Surface generation is never
  renderable, even if an older lease still exists.
- `RecoveryOutputRouter` frees old Recovery Epoch, surface-invalid, and residual
  destroyed-decoder outputs. They are never passed to the replacement native
  decoder.
- The probe reports Flush, Stop, Reset, and Rebuild state, stage timings,
  parameter-set resend, Surface Lease retention, residual callbacks, and
  Last Good Frame router state.

## Layout

- `core/`: host-compilable lifecycle, Surface Lease, cleanup retry, recovery
  routing, and Annex-B parser.
- `core/tests/`: public-seam contracts.
- `app/`: Stage-model HAP, XComponent Surface, NAPI bridge, and native probe UI.
- `testdata/`: 640x360 Constrained Baseline Annex-B fixture. SHA-256
  `1122a5720ea1f755fd2c940a5ca4d1afbfc87dc1f9dd91cdde0b7bbbf2fba767`.
- `records/local-host-checks.json`: only host-executable results.
- `records/device-pending.json`: supplied device identity plus explicitly
  unexecuted device work.
- `RUNBOOK.md`: build, install, launch, observation, and record-update steps.

## Host checks

```sh
make -C probes/gate3 host-check
```

The target compiles five independent test binaries with C++20, strict warnings,
and pthreads, runs them, then validates `records/*.json`. The tests are
cwd-independent; the Annex-B fixture path is embedded at compile time.

The contracts are:

- Surface references are paired; destroyed generations stop rendering;
  reference operations never overlap.
- A Surface-destroy callback invalidates the epoch without running decoder
  cleanup; prior output is freed.
- Failed output frees remain queued until a later `Drain`, and in-gate Surface
  loss frees the rejected output instead of counting it as rendered.
- Cleanup failures block suite success; failed decoder `Stop`/`Destroy` retains
  the Surface Lease, and failed lease releases retry on each worker pass.
- Callback output commands retain their generation; stale epoch output is freed
  and residual old-decoder callbacks never reach the new instance.
- Flush/Stop/Reset/Rebuild use the specified ordered lifecycle and resend
  SPS/PPS/IDR; failed recovery never opens output.
- The fixture starts on an IDR with SPS/PPS and has repeated parameter sets.
- Source audit verifies encoded-input `GetAddr` is the only buffer address use.

`scripts/validate-records.py` rejects false state, missing evidence, a passed
Annex-B Host claim without the issue-05 sample, and any device-run measurement
when install/run is not `passed`.

## Evidence limits

- Router `last_good_frame.state_visible` is state evidence, not a screenshot or
  pixel capture.
- `surface_retained` means the Surface Lease stayed valid and `SetSurface`
  succeeded. It does not prove compositor scanout or physical-panel behavior.
- `low_latency_key_accepted` means the configure call accepted the key. It does
  not prove actual decoder scheduling latency.
- Issue 05's real Host Annex-B sample is not available. Annex-B, parameter-set,
  and color-metadata Host interoperability therefore remains `pending`.
- The supplied USB path is Hub/USB 2.0, not a Reference Link, so it cannot be
  used as Reference Link evidence.

## Build

The known-good unsigned HAP build is:

```sh
cd probes/gate3/app
/Applications/DevEco-Studio.app/Contents/tools/ohpm/bin/ohpm install --all
env \
  HVIGOR_USER_HOME=/private/tmp/gate3-hvigor-home \
  JAVA_HOME=/Applications/DevEco-Studio.app/Contents/jbr/Contents/Home \
  DEVECO_SDK_HOME=/Applications/DevEco-Studio.app/Contents/sdk \
  NODE_HOME=/Applications/DevEco-Studio.app/Contents/tools/node \
  /Applications/DevEco-Studio.app/Contents/tools/hvigor/bin/hvigorw \
  clean assembleHap --no-daemon -p product=default -p buildMode=debug
```

Expected warning: `No signingConfig found for product default`. The resulting
HAP is unsigned. See `RUNBOOK.md` for the single install attempt and how to
record its real output.
