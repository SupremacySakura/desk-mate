#!/usr/bin/env bash
set -euo pipefail

# Creates and imports a local fixed 10-year self-signed Code Signing certificate
# named "DeskMate Local Signer" into the user's login keychain.
#
# Signing DeskMate releases with the SAME certificate (paired with in-app updates
# that avoid com.apple.quarantine) preserves stable Code Signing identity instead
# of rotating an ad-hoc CDHash on every build.
#
# Note: After importing, open Keychain Access -> login -> Certificates ->
# "DeskMate Local Signer" -> Get Info -> Trust -> Code Signing -> Always Trust
# (or trust it via System Settings if macOS prompts).
#
# To export the certificate as base64 .p12 for GitHub Actions secrets
# (MACOS_CERT_P12 / MACOS_CERT_PASSWORD):
#   OUT_P12=/tmp/deskmate-signer.p12 P12_PASSWORD=yourpass bash apps/macos-host/scripts/create-self-signed-cert.sh

CERT_NAME="${CERT_NAME:-DeskMate Local Signer}"
KEYCHAIN="${KEYCHAIN:-$HOME/Library/Keychains/login.keychain-db}"
OUT_P12="${OUT_P12:-}"
P12_PASSWORD="${P12_PASSWORD:-deskmate-local}"

if security find-identity -v -p codesigning "$KEYCHAIN" 2>/dev/null | grep -Fq "\"$CERT_NAME\""; then
  echo "Code signing identity \"$CERT_NAME\" already exists in $KEYCHAIN:"
  security find-identity -v -p codesigning "$KEYCHAIN" | grep -F "\"$CERT_NAME\""
  exit 0
fi

TMP_DIR="$(mktemp -d)"
cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

cat > "$TMP_DIR/ext.cnf" <<EOF
[req]
distinguished_name = req_dn
x509_extensions = codesign_ext
prompt = no

[req_dn]
CN = $CERT_NAME
O = DeskMate Open Source

[codesign_ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
subjectKeyIdentifier = hash
EOF

/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$TMP_DIR/key.pem" \
  -out "$TMP_DIR/cert.pem" \
  -days 3650 \
  -config "$TMP_DIR/ext.cnf" \
  -extensions codesign_ext >/dev/null 2>&1

P12_FILE="${OUT_P12:-$TMP_DIR/signer.p12}"
/usr/bin/openssl pkcs12 -export \
  -legacy \
  -out "$P12_FILE" \
  -inkey "$TMP_DIR/key.pem" \
  -in "$TMP_DIR/cert.pem" \
  -passout "pass:$P12_PASSWORD" >/dev/null 2>&1 || \
/usr/bin/openssl pkcs12 -export \
  -out "$P12_FILE" \
  -inkey "$TMP_DIR/key.pem" \
  -in "$TMP_DIR/cert.pem" \
  -passout "pass:$P12_PASSWORD" >/dev/null 2>&1

security import "$P12_FILE" -k "$KEYCHAIN" -P "$P12_PASSWORD" -T /usr/bin/codesign >/dev/null
echo "Imported \"$CERT_NAME\" into $KEYCHAIN."
security find-identity -v -p codesigning "$KEYCHAIN" | grep -F "\"$CERT_NAME\"" || true

echo
echo "Next steps:"
echo "  1. Open Keychain Access -> login -> Certificates -> \"$CERT_NAME\""
echo "  2. Double-click -> Trust -> Code Signing -> set to \"Always Trust\""
echo "  3. Subsequent 'make -C apps/macos-host dmg' builds will auto-detect and sign with \"$CERT_NAME\"."
if [[ -n "$OUT_P12" ]]; then
  echo "  4. Exported .p12 saved to $OUT_P12 (do NOT commit .p12 files to git!)."
fi
