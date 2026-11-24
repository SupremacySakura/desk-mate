#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/build/Gate0Probe.app"
IDENTITY="${DEVELOPER_ID_APPLICATION:-}"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"
REPORT=""
MODE="dry-run"
JSON=0
ALLOW_ANY_APP=0

usage() {
  cat <<'EOF'
DeskMate Gate 0 Developer ID preflight

Default mode is a read-only dry run. It never signs, zips, submits, staples, or
assesses an artifact unless --execute is supplied.

Usage:
  distribution-check.sh [--dry-run] [--json] [--report PATH]
  distribution-check.sh --execute --identity "Developer ID Application: ..." \
    --notary-profile PROFILE [--app PATH]

Options:
  --app PATH              App bundle, default probes/gate0/build/Gate0Probe.app
  --identity NAME         Developer ID Application identity
  --notary-profile NAME   notarytool keychain profile
  --execute               Perform sign, notarize, staple, and Gatekeeper checks
  --allow-any-app         Permit --execute on a bundle not owned by the Gate 0 probe
  --json                  Emit machine-readable JSON
  --report PATH           Also write the JSON report
  --help                  Show this help

Environment fallbacks:
  DEVELOPER_ID_APPLICATION
  NOTARY_PROFILE
EOF
}

fail_usage() {
  printf 'error: %s\n' "$1" >&2
  exit 64
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --app) APP="${2:-}"; shift 2 ;;
    --identity) IDENTITY="${2:-}"; shift 2 ;;
    --notary-profile) NOTARY_PROFILE="${2:-}"; shift 2 ;;
    --report) REPORT="${2:-}"; shift 2 ;;
    --dry-run) MODE="dry-run"; shift ;;
    --execute) MODE="execute"; shift ;;
    --json) JSON=1; shift ;;
    --allow-any-app) ALLOW_ANY_APP=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) fail_usage "unknown option: $1" ;;
  esac
done

APP_EXISTS=0
EXECUTABLE_EXISTS=0
BUNDLE_ID=""
if [[ -d "$APP" ]]; then
  APP_EXISTS=1
  [[ -x "$APP/Contents/MacOS/Gate0Probe" ]] && EXECUTABLE_EXISTS=1
  if [[ -f "$APP/Contents/Info.plist" ]]; then
    BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Contents/Info.plist" 2>/dev/null || true)"
  fi
fi

IDENTITY_FOUND=0
if [[ -n "$IDENTITY" ]]; then
  if [[ "$IDENTITY" == "Developer ID Application:"* ]] &&
    /usr/bin/security find-identity -v -p codesigning 2>/dev/null | /usr/bin/grep -Fq "\"$IDENTITY\""; then
    IDENTITY_FOUND=1
  fi
fi

NOTARYTOOL_AVAILABLE=0
if /usr/bin/xcrun notarytool --version >/dev/null 2>&1; then
  NOTARYTOOL_AVAILABLE=1
fi

GATEKEEPER_ENABLED=0
if /usr/sbin/spctl --status 2>/dev/null | /usr/bin/grep -q enabled; then
  GATEKEEPER_ENABLED=1
fi

PROBE_BUNDLE=0
[[ "$BUNDLE_ID" == "dev.deskmate.gate0-probe" ]] && PROBE_BUNDLE=1

MISSING=()
[[ $APP_EXISTS -eq 1 ]] || MISSING+=("app-bundle")
[[ $EXECUTABLE_EXISTS -eq 1 ]] || MISSING+=("Gate0Probe executable")
[[ $IDENTITY_FOUND -eq 1 ]] || MISSING+=("Developer ID Application identity")
[[ -n "$NOTARY_PROFILE" ]] || MISSING+=("notarytool keychain profile")
[[ $NOTARYTOOL_AVAILABLE -eq 1 ]] || MISSING+=("notarytool")

if [[ "$MODE" == "execute" ]]; then
  [[ ${#MISSING[@]} -eq 0 ]] || {
    printf 'error: cannot execute distribution checks; missing: %s\n' "$(IFS=', '; echo "${MISSING[*]}")" >&2
    exit 2
  }
  [[ $PROBE_BUNDLE -eq 1 || $ALLOW_ANY_APP -eq 1 ]] || {
    printf 'error: refusing to sign bundle %s; pass --allow-any-app only if intentional\n' "$BUNDLE_ID" >&2
    exit 2
  }
fi

STATUS="pending"
EXECUTED=0
CHECKS=()
COMMANDS=()
SIGNING_RESULT="not-executed"
NOTARY_RESULT="not-executed"
STAPLE_RESULT="not-executed"
GATEKEEPER_RESULT="not-executed"

if [[ "$MODE" == "execute" ]]; then
  EXECUTED=1
  ZIP_DIR="$(mktemp -d)"
  trap 'rm -rf "$ZIP_DIR"' EXIT
  SOURCE_ZIP="$ZIP_DIR/Gate0Probe-submit.zip"
  STAPLED_ZIP="$ZIP_DIR/Gate0Probe-notarized.zip"

  /usr/bin/codesign --force --options runtime --timestamp --sign "$IDENTITY" "$APP"
  COMMANDS+=("codesign --force --options runtime --timestamp --sign <redacted> <app>")
  /usr/bin/codesign --verify --deep --strict --verbose=2 "$APP"
  SIGNING_RESULT="passed"
  CHECKS+=("Developer ID signing and strict verification passed")

  /usr/bin/ditto -c -k --keepParent "$APP" "$SOURCE_ZIP"
  COMMANDS+=("ditto -c -k --keepParent <app> <submit.zip>")
  SUBMIT_OUTPUT="$(/usr/bin/xcrun notarytool submit "$SOURCE_ZIP" --keychain-profile "$NOTARY_PROFILE" --wait --timeout 30m 2>&1)"
  printf '%s\n' "$SUBMIT_OUTPUT"
  COMMANDS+=("notarytool submit <submit.zip> --keychain-profile <redacted> --wait --timeout 30m")
  if ! /usr/bin/grep -q 'status: Accepted' <<<"$SUBMIT_OUTPUT"; then
    STATUS="failed"
    NOTARY_RESULT="failed"
    CHECKS+=("Apple notarization was not accepted")
  else
    NOTARY_RESULT="passed"
    CHECKS+=("Apple notarization accepted")
    /usr/bin/xcrun stapler staple "$APP"
    /usr/bin/xcrun stapler validate "$APP"
    STAPLE_RESULT="passed"
    CHECKS+=("Stapler ticket attached and validated")
    /usr/bin/ditto -c -k --keepParent "$APP" "$STAPLED_ZIP"
    COMMANDS+=("stapler staple <app>; stapler validate <app>; ditto notarized zip")

    set +e
    ASSESS_OUTPUT="$(/usr/sbin/spctl --assess --type execute --verbose=4 "$APP" 2>&1)"
    ASSESS_STATUS=$?
    set -e
    printf '%s\n' "$ASSESS_OUTPUT"
    COMMANDS+=("spctl --assess --type execute --verbose=4 <app>")
    if [[ $ASSESS_STATUS -eq 0 ]] && ! /usr/bin/grep -qi rejected <<<"$ASSESS_OUTPUT"; then
      GATEKEEPER_RESULT="passed"
      CHECKS+=("Gatekeeper execute assessment accepted")
      STATUS="passed"
    else
      GATEKEEPER_RESULT="failed"
      STATUS="failed"
      CHECKS+=("Gatekeeper execute assessment failed")
    fi
  fi
fi

if [[ "$JSON" -eq 1 || -n "$REPORT" ]]; then
  if [[ ${#MISSING[@]} -eq 0 ]]; then
    MISSING_JSON='[]'
  else
    MISSING_JSON="$(printf '%s\n' "${MISSING[@]}" | /usr/bin/jq -R . | /usr/bin/jq -s .)"
  fi
  JSON_TEXT="$(/usr/bin/jq -n \
    --arg generatedAt "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --arg mode "$MODE" \
    --arg status "$STATUS" \
    --arg app "$APP" \
    --arg bundleID "$BUNDLE_ID" \
    --arg signing "$SIGNING_RESULT" \
    --arg notary "$NOTARY_RESULT" \
    --arg staple "$STAPLE_RESULT" \
    --arg gatekeeper "$GATEKEEPER_RESULT" \
    --argjson appExists "$APP_EXISTS" \
    --argjson executableExists "$EXECUTABLE_EXISTS" \
    --argjson identityFound "$IDENTITY_FOUND" \
    --argjson notarytoolAvailable "$NOTARYTOOL_AVAILABLE" \
    --argjson gatekeeperEnabled "$GATEKEEPER_ENABLED" \
    --argjson actionsExecuted "$EXECUTED" \
    --argjson missing "$MISSING_JSON" \
    '{
      schemaVersion: 1,
      probe: "deskmate-gate0-distribution",
      generatedAt: $generatedAt,
      mode: $mode,
      status: $status,
      liveActionsExecuted: ($actionsExecuted == 1),
      app: $app,
      appExists: ($appExists == 1),
      executableExists: ($executableExists == 1),
      bundleID: (if $bundleID == "" then null else $bundleID end),
      checks: {
        developerIDIdentityFound: ($identityFound == 1),
        notarytoolAvailable: ($notarytoolAvailable == 1),
        gatekeeperAssessmentEnabled: ($gatekeeperEnabled == 1),
        signing: $signing,
        notarization: $notary,
        stapler: $staple,
        gatekeeper: $gatekeeper
      },
      missing: $missing,
      notExecuted: (if $actionsExecuted == 1 then [] else [
        "Developer ID signing",
        "Apple notarization",
        "Stapler ticket attachment",
        "Gatekeeper launch assessment"
      ] end)
    }')"

  if [[ -n "$REPORT" ]]; then
    mkdir -p "$(dirname "$REPORT")"
    printf '%s\n' "$JSON_TEXT" >"$REPORT"
  fi
  if [[ "$JSON" -eq 1 ]]; then
    printf '%s\n' "$JSON_TEXT"
  fi
fi

if [[ "$JSON" -eq 0 ]]; then
  STATUS_UPPER="$(printf '%s' "$STATUS" | /usr/bin/tr '[:lower:]' '[:upper:]')"
  printf 'DeskMate Gate 0 distribution preflight: %s (%s)\n' "$STATUS_UPPER" "$MODE"
  printf 'App: %s\n' "$APP"
  printf 'Bundle ID: %s\n' "${BUNDLE_ID:-unavailable}"
  printf 'Developer ID identity found: %s\n' "$IDENTITY_FOUND"
  printf 'notarytool available: %s\n' "$NOTARYTOOL_AVAILABLE"
  printf 'Gatekeeper enabled: %s\n' "$GATEKEEPER_ENABLED"
  if [[ ${#MISSING[@]} -gt 0 ]]; then
    printf 'Missing prerequisites:\n'
    printf '  - %s\n' "${MISSING[@]}"
  fi
  if [[ ${#CHECKS[@]} -gt 0 ]]; then
    printf 'Executed checks:\n'
    printf '  - %s\n' "${CHECKS[@]}"
  fi
  printf 'This report is not Gate 0 evidence unless an unsigned dry run was followed by an accepted --execute run on the Reference Host.\n'
fi

if [[ "$MODE" == "execute" && "$STATUS" != "passed" ]]; then
  exit 2
fi
