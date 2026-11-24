# Gate 3 device runbook

Target: HUAWEI MatePad Pro `MRDI-W00`, serial `<redacted-serial>`,
OpenHarmony `7.0.0.111`, API 26, 2800x1840. The current USB path is through a
Hub at USB 2.0 and is **not** a Reference Link.

## 1. Build the signed HAP

Enable DevEco Studio automatic signing for `com.deskmate.gate3` and the
connected device. Keep the generated signing block, private key, profile,
passwords, and device UDID out of Git.

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

Verify:

```sh
HAP=entry/build/default/outputs/default/entry-default-signed.hap
shasum -a 256 "$HAP"
unzip -l "$HAP" | grep -E 'libentry.so|gate3-testsrc.h264'
ls -l "$HAP"
```

Do not invent a signature. If no matching signing material exists, an install
failure caused by `no signature file` is a **not executed** device run, not a
failed probe. Do not commit a signing block produced for a local run.

## 2. Reach the target

```sh
HDC=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc
HAP=probes/gate3/app/entry/build/default/outputs/default/entry-default-signed.hap
"$HDC" list targets
"$HDC" -t <redacted-serial> shell param get const.product.model
"$HDC" -t <redacted-serial> shell param get const.ohos.fullname
"$HDC" -t <redacted-serial> shell hidumper -s RenderService -a screen
```

Record the exact output. Do not substitute the task-brief identity for a live
query. If hdc cannot reach the target, the install attempt is `not executed`.

## 3. One official install attempt

Make exactly one attempt with the built artifact:

```sh
timeout 60 "$HDC" -t <redacted-serial> install -r "$HAP"
```

Record the command, exit status, and complete output.

- `Success` marks install `passed`; continue to step 4.
- `code:9568320 error: no signature file` marks install `not executed` with
  the signing reason. Stop. Do not repeat with fake packaging or bypasses.

## 4. Launch

```sh
"$HDC" -t <redacted-serial> shell aa start -a EntryAbility -b com.deskmate.gate3
"$HDC" -t <redacted-serial> shell hilog -r
"$HDC" -t <redacted-serial> shell hilog | grep -i -E 'Gate3|deskmate|avcodec'
```

The page loads `gate3-testsrc.h264` from rawfile and publishes the device
fields shown by `@ohos.deviceInfo`.

## 5. Observe

1. Confirm the XComponent area turns black/renders the test pattern. The probe
   never reads pixels to prove this.
2. Press **Run recovery suite**. Wait for the raw evidence to show
   `suite.state = complete` (or `failed`).
3. Capture the entire raw evidence JSON. Do not transcribe timing fields by
   hand if they can be obtained from the displayed evidence.
4. For the manual Last Good Frame check, press **Freeze for LGF check**, inspect
   the held image, then **Mark LGF stable**. Press **Resume after freeze** and
   confirm the frame during recovery.
5. Keep both USB 2.0 status and any visual observation limitations explicit.

The evidence fields include:

- codec MIME, hardware flag, low-latency feature, width/height ranges, pixel
  formats, and profiles;
- initial render time and input access-unit count;
- per-action ordered stages, durations, first-output latency, SPS/PPS/IDR
  resend, Surface Lease retention, residual callbacks, stale-output frees, and
  control-thread violations;
- router Last Good Frame state and manual observation.

## 6. Update records

Only after real device output exists, update `records/device-pending.json` (or
rename it to a dated device record):

- install `passed` and run `passed`;
- replace supplied identity with live hdc values where available;
- add actual capability and recovery measurements;
- preserve Annex-B Host interoperability as `pending` until issue 05 supplies
  the real sample;
- keep `no_cpu_pixel_readback: true`.

Then run:

```sh
make -C probes/gate3 host-check
```

The validator rejects measured device data when install/run is not `passed`.
