#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${GATE0_BIN:-$ROOT/build/gate0-probe}"
APP="${GATE0_APP:-$ROOT/build/Gate0Probe.app}"

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_json() {
  local json="$1"
  local filter="$2"
  local message="$3"
  jq -e "$filter" >/dev/null <<<"$json" || fail "$message"
}

[[ -x "$BIN" ]] || fail "missing executable: $BIN (run make build)"

echo "ok - default invocation is a non-live dry run"
json="$("$BIN" --json)"
assert_json "$json" '.probe == "deskmate-gate0"' "probe id missing"
assert_json "$json" '.mode == "dry-run"' "default mode is not dry-run"
assert_json "$json" '.decision == "pending"' "unexecuted gate must remain pending"
assert_json "$json" '.safety.liveActionsExecuted == false' "dry run executed live actions"
assert_json "$json" '.safety.mainDisplayFallbackUsed == false' "probe used the main display"
assert_json "$json" '.display.created == false' "dry run created a display"
assert_json "$json" '.capture.started == false' "dry run started capture"
assert_json "$json" '.capture.audioEnabled == false' "audio capture is not disabled"
assert_json "$json" '.capture.audioOutputAdded == false' "dry run added an audio output"
assert_json "$json" '(.notExecuted | length) > 0' "pending report does not list missing evidence"
assert_json "$json" '.environment.modelIdentifier != "unavailable"' "Mac model was not recorded"
assert_json "$json" '.environment.buildVersion != "unavailable"' "macOS build was not recorded"
assert_json "$json" '.environment.architecture != "unavailable"' "architecture was not recorded"
assert_json "$json" '.videoToolbox.note | length > 0' "VideoToolbox advisory note missing"
assert_json "$json" '.privateAPI.requiredSelectors.CGVirtualDisplaySettings["setRotation:"] == true' "used rotation selector is not checked"
assert_json "$json" '[.artifactPlan[] | select(.codec == "hevc")] | length == 1' "HEVC artifact plan missing"
assert_json "$json" '[.artifactPlan[] | select(.codec == "h264")] | length == 1' "H.264 artifact plan missing"
assert_json "$json" '.artifactPlan[] | select(.annexB == true and .noBFrames == true)' "artifact plan lacks Annex-B/no-B-frame contract"
if nm -u "$BIN" | grep -q 'CGVirtualDisplay'; then
  fail "binary imports a private CGVirtualDisplay class symbol"
fi

echo "ok - live mode requires both explicit destructive opt-ins"
set +e
live_output="$("$BIN" --live --json 2>&1)"
live_status=$?
set -e
[[ $live_status -ne 0 ]] || fail "live mode without opt-ins unexpectedly succeeded"
grep -q -- '--allow-create-display' <<<"$live_output" || fail "missing display opt-in error"
grep -q -- '--allow-screen-capture' <<<"$live_output" || fail "missing capture opt-in error"

echo "ok - dry-run and live modes cannot be combined in either order"
set +e
mode_output="$("$BIN" --dry-run --live --json 2>&1)"
mode_status=$?
set -e
[[ $mode_status -ne 0 ]] || fail "combined dry-run/live mode unexpectedly succeeded"
grep -q 'cannot be combined' <<<"$mode_output" || fail "combined modes were not rejected explicitly"
set +e
mode_reverse_output="$("$BIN" --live --dry-run --json 2>&1)"
mode_reverse_status=$?
set -e
[[ $mode_reverse_status -ne 0 ]] || fail "reversed combined mode unexpectedly succeeded"
grep -q 'cannot be combined' <<<"$mode_reverse_output" || fail "reversed combined modes were not rejected explicitly"

echo "ok - unknown mirror/fallback selector is rejected"
set +e
fallback_output="$("$BIN" --use-main-display --json 2>&1)"
fallback_status=$?
set -e
[[ $fallback_status -ne 0 ]] || fail "main-display fallback option was accepted"
grep -qi 'unknown option' <<<"$fallback_output" || fail "fallback rejection is not explicit"

echo "ok - Annex-B conversion self-test runs without display or capture"
selftest_json="$("$BIN" --self-test --json)"
assert_json "$selftest_json" '.status == "passed"' "Annex-B self-test failed"
assert_json "$selftest_json" '.liveActionsExecuted == false' "self-test executed live actions"
assert_json "$selftest_json" '.cases[] | select(.name == "length-prefixed-to-annex-b") | .passed == true' "Annex-B conversion case missing"
assert_json "$selftest_json" '.cases[] | select(.name == "reject-invalid-nal-length") | .passed == true' "invalid NAL length case missing"

echo "ok - display inventory is read-only"
display_json="$("$BIN" --list-displays --json)"
assert_json "$display_json" '.status == "ok" or .status == "unavailable"' "display inventory failed"
assert_json "$display_json" '.liveActionsExecuted == false' "display inventory changed display state"
assert_json "$display_json" '.note | length > 0' "display inventory note missing"
assert_json "$display_json" 'if .status == "ok" then ([.displays[] | select(.isMain == true)] | length == 1) else true end' "main display was not identified exactly once"

echo "ok - confirmed Reference Host is compared, not hardcoded"
model="$(jq -r '.environment.modelIdentifier' <<<"$json")"
build="$(jq -r '.environment.buildVersion' <<<"$json")"
reference_json="$("$BIN" --reference-model "$model" --reference-build "$build" --json)"
assert_json "$reference_json" '.referenceEnvironment.confirmed == true' "matching Reference Host was not confirmed"
assert_json "$reference_json" '.prerequisites[] | select(.id == "reference-mac") | .state == "passed"' "matching Reference Host prerequisite not passed"
assert_json "$reference_json" '.decision == "pending"' "reference match must not bypass missing evidence"

echo "ok - distribution script defaults to non-executing preflight"
distribution_json="$("$ROOT/scripts/distribution-check.sh" --dry-run --json --app "$APP")"
assert_json "$distribution_json" '.mode == "dry-run"' "distribution script default is not dry-run"
assert_json "$distribution_json" '.liveActionsExecuted == false' "distribution dry-run executed actions"
assert_json "$distribution_json" '.status == "pending"' "distribution dry-run must remain pending"
assert_json "$distribution_json" '.checks.signing == "not-executed"' "distribution dry-run signed an artifact"
assert_json "$distribution_json" '(.notExecuted | length) == 4' "distribution pending steps missing"

echo "ok - distribution script rejects non-Developer ID identities"
wrong_identity_json="$("$ROOT/scripts/distribution-check.sh" --dry-run --json --app "$APP" \
  --identity "Apple Development: Not A Developer ID (TESTTEAM)")"
assert_json "$wrong_identity_json" '.checks.developerIDIdentityFound == false' "non-Developer ID identity was accepted"
assert_json "$wrong_identity_json" '.missing[] | select(. == "Developer ID Application identity")' "wrong identity did not report missing Developer ID"

echo "ok - encoder callback context is retained and drained before teardown"
ENCODER_SOURCE="$ROOT/Sources/Gate0Probe/VideoEncoder.swift"
grep -q 'passRetained(callbackContext)' "$ENCODER_SOURCE" \
  || fail "encoder callback context is not retained"
grep -q 'callbackContext.stopAndWait()' "$ENCODER_SOURCE" \
  || fail "encoder teardown does not wait for in-flight callbacks"
if grep -q 'Unmanaged<Gate0Encoder>.fromOpaque' "$ENCODER_SOURCE"; then
  fail "encoder callback still dereferences an unretained encoder"
fi

echo "PASS: gate0 probe safety contract"
