#!/usr/bin/env bash
# Uploads the macOS DMG / updater ZIP built by CI to the matching GitHub
# Release, creating the release from CHANGELOG.md when it does not exist yet.
#
# Usage: RELEASE_TAG=v1.5.0 RELEASE_VERSION=1.5.0 \
#        BUILD_DIR=apps/macos-host/build bash scripts/ci-release-macos-assets.sh
set -euo pipefail

RELEASE_TAG="${RELEASE_TAG:?RELEASE_TAG is required}"
RELEASE_VERSION="${RELEASE_VERSION:?RELEASE_VERSION is required}"
BUILD_DIR="${BUILD_DIR:-apps/macos-host/build}"

DMG_FILE="DeskMate-v${RELEASE_VERSION}-macos-arm64.dmg"
ZIP_FILE="DeskMate-v${RELEASE_VERSION}-macos-arm64.zip"
DMG_PATH="${BUILD_DIR}/${DMG_FILE}"
ZIP_PATH="${BUILD_DIR}/${ZIP_FILE}"

for artifact in "$DMG_PATH" "${DMG_PATH}.sha256" "$ZIP_PATH" "${ZIP_PATH}.sha256"; do
  if [[ ! -f "$artifact" ]]; then
    echo "missing build artifact: $artifact" >&2
    exit 1
  fi
done

if gh release view "$RELEASE_TAG" >/dev/null 2>&1; then
  # The signed updater ZIP is referenced by update-manifest.json; never replace
  # it from CI, or the published hashes stop matching the manifest.
  if gh release view "$RELEASE_TAG" --json assets --jq '.assets[].name' |
      grep -qx "$ZIP_FILE"; then
    echo "Release $RELEASE_TAG already contains $ZIP_FILE; skipping upload to preserve manifest hashes."
    exit 0
  fi
  gh release upload "$RELEASE_TAG" \
    "$DMG_PATH" "${DMG_PATH}.sha256" \
    "$ZIP_PATH" "${ZIP_PATH}.sha256" --clobber
  exit 0
fi

NOTES_FILE="$(mktemp)"
trap 'rm -f "$NOTES_FILE"' EXIT

python3 - "$RELEASE_TAG" "$RELEASE_VERSION" "$NOTES_FILE" <<'PYEOF'
import os
import pathlib
import re
import sys

tag, version, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
changelog = pathlib.Path("CHANGELOG.md")
if not changelog.exists():
    sys.exit(1)

text = changelog.read_text(encoding="utf-8")
pattern = re.compile(
    rf"^## \[(?:{re.escape(tag)}|{re.escape(version)})\]\b.*?(?=\n## \[|\Z)",
    re.M | re.S,
)
match = pattern.search(text)
if not match:
    sys.exit(1)

body = match.group(0).strip()
repo = os.environ.get("GITHUB_REPOSITORY", "")
readme_url = (
    f"https://github.com/{repo}/blob/{tag}/README.zh-CN.md"
    if repo
    else "README.zh-CN.md"
)
body += (
    "\n\n---\n"
    f"> 安装步骤、驱动配置与常见问题请查阅 [README.zh-CN.md]({readme_url})。\n"
)
pathlib.Path(out_path).write_text(body, encoding="utf-8")
PYEOF

if [[ -s "$NOTES_FILE" ]]; then
  gh release create "$RELEASE_TAG" \
    "$DMG_PATH" "${DMG_PATH}.sha256" \
    "$ZIP_PATH" "${ZIP_PATH}.sha256" \
    --title "DeskMate ${RELEASE_TAG}" --notes-file "$NOTES_FILE"
else
  gh release create "$RELEASE_TAG" \
    "$DMG_PATH" "${DMG_PATH}.sha256" \
    "$ZIP_PATH" "${ZIP_PATH}.sha256" \
    --title "DeskMate ${RELEASE_TAG}" --generate-notes
fi
