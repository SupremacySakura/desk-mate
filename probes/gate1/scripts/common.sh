#!/usr/bin/env bash

gate1_script_dir() {
  cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P
}

gate1_probe_root() {
  cd "$(gate1_script_dir)/.." && pwd -P
}

gate1_deveco_contents() {
  if [[ -n "${DEVECO_STUDIO_HOME:-}" ]]; then
    printf '%s\n' "$DEVECO_STUDIO_HOME"
    return 0
  fi
  local candidate="/Applications/DevEco-Studio.app/Contents"
  if [[ -d "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi
  return 1
}
gate1_native_sdk_root() {
  local supplied="${DEVECO_SDK_HOME:-}"
  if [[ -z "$supplied" ]]; then
    local contents
    contents="$(gate1_deveco_contents)" || return 1
    supplied="$contents/sdk"
  fi
  if [[ -d "$supplied/openharmony/native" ]]; then
    printf '%s\n' "$supplied"
    return 0
  fi
  if [[ -d "$supplied/default/openharmony/native" ]]; then
    printf '%s\n' "$supplied/default"
    return 0
  fi
  return 1
}

gate1_hvigor_sdk_root() {
  local supplied="${DEVECO_SDK_HOME:-}"
  local local_properties
  local_properties="$(gate1_probe_root)/device/local.properties"
  if [[ -z "$supplied" && -f "$local_properties" ]]; then
    supplied="$(
      sed -n 's/^[[:space:]]*sdk\.dir[[:space:]]*=[[:space:]]*//p' \
        "$local_properties" | tail -n 1
    )"
  fi
  if [[ -z "$supplied" ]]; then
    local contents
    contents="$(gate1_deveco_contents)" || return 1
    supplied="$contents/sdk"
  fi
  if [[ -d "$supplied/openharmony" ]]; then
    printf '%s\n' "$supplied"
    return 0
  fi
  if [[ "$(basename "$supplied")" == "default" &&
        -d "$(dirname "$supplied")/openharmony" ]]; then
    printf '%s\n' "$(dirname "$supplied")"
    return 0
  fi
  if [[ -d "$supplied/default/openharmony" ]]; then
    printf '%s\n' "$supplied"
    return 0
  fi
  return 1
}

gate1_hdc_path() {
  if [[ -n "${HDC:-}" ]]; then
    printf '%s\n' "$HDC"
    return 0
  fi
  local sdk_root
  sdk_root="$(gate1_native_sdk_root)" || return 1
  local candidate="$sdk_root/openharmony/toolchains/hdc"
  if [[ -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi
  return 1
}
