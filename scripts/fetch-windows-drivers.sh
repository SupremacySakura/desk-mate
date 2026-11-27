#!/usr/bin/env bash
# Downloads, SHA-256 verifies, and stages third-party Windows driver tools
# (Parsec VDD + Zadig) + license notices into a target drivers/ directory.
#
# Usage:
#   bash scripts/fetch-windows-drivers.sh [target_drivers_dir]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE_DIR="$REPO/build/windows-drivers"
TARGET_DIR="${1:-$REPO/dist/windows/drivers}"

PARSEC_NAME="parsec-vdd-0.45.0.0-setup.exe"
PARSEC_URL="https://builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe"
PARSEC_SHA256="e23332448fdaf5aa017cb308db5ef6855fac526a7ded05d80c039404126d5362"

ZADIG_NAME="zadig-2.9.exe"
ZADIG_URL="https://github.com/pbatard/libwdi/releases/download/v1.5.1/zadig-2.9.exe"
ZADIG_SHA256="4ecaa95df3da3621486a043aef8b3050b8bafe7c901402871e816229ef82039b"

GPL_NAME="GPL-3.0.txt"
GPL_URL="https://www.gnu.org/licenses/gpl-3.0.txt"

sha256_of() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}

fetch_and_verify() {
  local name="$1"
  local url="$2"
  local expected="$3"
  local cached="$CACHE_DIR/$name"

  mkdir -p "$CACHE_DIR"
  if [[ -f "$cached" ]]; then
    local actual
    actual="$(sha256_of "$cached")"
    if [[ "$actual" == "$expected" ]]; then
      echo "==> Using cached $name (sha256 ok)"
      return 0
    fi
    echo "==> Cached $name checksum mismatch, re-downloading"
    rm -f "$cached"
  fi

  echo "==> Downloading $name"
  local tmp="$cached.tmp.$$"
  curl -fsSL --retry 3 --retry-delay 2 -o "$tmp" "$url"
  local actual
  actual="$(sha256_of "$tmp")"
  if [[ "$actual" != "$expected" ]]; then
    rm -f "$tmp"
    echo "error: SHA-256 mismatch for $name (expected $expected, got $actual)" >&2
    exit 1
  fi
  mv "$tmp" "$cached"
}

fetch_gpl_text() {
  local cached="$CACHE_DIR/$GPL_NAME"
  mkdir -p "$CACHE_DIR"
  if [[ -f "$cached" ]] && grep -q "GNU GENERAL PUBLIC LICENSE" "$cached"; then
    return 0
  fi
  echo "==> Downloading $GPL_NAME"
  local tmp="$cached.tmp.$$"
  curl -fsSL --retry 3 --retry-delay 2 -o "$tmp" "$GPL_URL"
  if ! grep -q "GNU GENERAL PUBLIC LICENSE" "$tmp"; then
    rm -f "$tmp"
    echo "error: downloaded $GPL_NAME does not look like GPL-3.0 text" >&2
    exit 1
  fi
  mv "$tmp" "$cached"
}

fetch_and_verify "$PARSEC_NAME" "$PARSEC_URL" "$PARSEC_SHA256"
fetch_and_verify "$ZADIG_NAME" "$ZADIG_URL" "$ZADIG_SHA256"
fetch_gpl_text

mkdir -p "$TARGET_DIR"
cp "$CACHE_DIR/$PARSEC_NAME" "$TARGET_DIR/$PARSEC_NAME"
cp "$CACHE_DIR/$ZADIG_NAME" "$TARGET_DIR/$ZADIG_NAME"
cp "$CACHE_DIR/$GPL_NAME" "$TARGET_DIR/$GPL_NAME"
chmod 755 "$TARGET_DIR/$PARSEC_NAME" "$TARGET_DIR/$ZADIG_NAME"

cat > "$TARGET_DIR/THIRD_PARTY_LICENSES.txt" <<'EOF'
Third-Party Windows Driver Utilities Bundled with DeskMate
==========================================================

The unmodified standalone executables in this directory are aggregated
alongside DeskMateHost.exe for user convenience ("mere aggregation"; not
linked with or compiled into DeskMateHost.exe). Each utility remains governed
by its own upstream license below.

1. Zadig 2.9 (zadig-2.9.exe)
   - Role: Binds the Microsoft WinUSB driver to Android Open Accessory (AOA)
     USB interfaces (18D1:2D00 / 18D1:2D01, Interface 0).
   - Copyright (c) Pete Batard / Akeo Consulting <https://zadig.akeo.ie/>
   - License: GNU General Public License v3.0 or later (GPL-3.0-or-later)
     Full license text included in GPL-3.0.txt in this directory.
   - Corresponding Source Code:
     https://github.com/pbatard/libwdi/releases/tag/v1.5.1
     (Built on libwdi, licensed under LGPL-3.0-or-later)

2. Parsec Virtual Display Driver 0.45.0.0 (parsec-vdd-0.45.0.0-setup.exe)
   - Role: WHQL-signed virtual display adapter driver used by DeskMateHost.exe
     to create standalone virtual extended monitors on Windows.
   - Driver Copyright (c) Parsec Cloud, Inc. <https://parsec.app/>
   - Upstream community integration reference: nomi-san/parsec-vdd
     <https://github.com/nomi-san/parsec-vdd> (MIT License):

     MIT License

     Copyright (c) 2023-present, Virtual Display Driver contributors

     Permission is hereby granted, free of charge, to any person obtaining a
     copy of this software and associated documentation files (the "Software"),
     to deal in the Software without restriction, including without limitation
     the rights to use, copy, modify, merge, publish, distribute, sublicense,
     and/or sell copies of the Software, and to permit persons to whom the
     Software is furnished to do so, subject to the following conditions:

     The above copyright notice and this permission notice shall be included in
     all copies or substantial portions of the Software.

     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
     IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
     FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
     THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
     OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
     ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
     OTHER DEALINGS IN THE SOFTWARE.
EOF
