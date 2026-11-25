#!/usr/bin/env python3
"""DeskMate release manifest generator and RFC 8032 Ed25519 signer/verifier.

Zero external dependencies (Python 3 stdlib only).

Canonical per-artifact signed payload (exact ASCII, LF-separated):
    deskmate-update-v1\n<version>\n<platform>\n<sha256_lowercase_hex>\n<size_decimal>

Subcommands:
    self-test
    keygen --out <key_file>
    pubkey --key <key_file_or_env:VAR>
    generate --version <ver> --tag <tag> --release-url <url> --notes <str|@file>
             --key <key_file_or_env:VAR> --artifact <platform>=<file>:<url> [...]
             --out <manifest.json> [--published-at <iso8601>]
    verify --manifest <manifest.json> --pubkey <64-hex-chars>
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import secrets
import sys

# --- RFC 8032 Ed25519 primitives (pure Python stdlib) ---

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493


def _inv(z: int) -> int:
    return pow(z, P - 2, P)


D = (-121665 * _inv(121666)) % P
I = pow(2, (P - 1) // 4, P)

By = (4 * _inv(5)) % P


def _xrecover(y: int) -> int:
    xx = (y * y - 1) * _inv(D * y * y + 1) % P
    x = pow(xx, (P + 3) // 8, P)
    if (x * x - xx) % P != 0:
        x = (x * I) % P
    if x % 2 != 0:
        x = P - x
    return x


Bx = _xrecover(By)
B_EXT = (Bx % P, By % P, 1, (Bx * By) % P)
NEUTRAL_EXT = (0, 1, 1, 0)


def _edwards_add(pt1: tuple[int, int, int, int],
                 pt2: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    x1, y1, z1, t1 = pt1
    x2, y2, z2, t2 = pt2
    a = ((y1 - x1) * (y2 - x2)) % P
    b = ((y1 + x1) * (y2 + x2)) % P
    c = (t1 * 2 * D * t2) % P
    dd = (z1 * 2 * z2) % P
    e = (b - a) % P
    f = (dd - c) % P
    g = (dd + c) % P
    h = (b + a) % P
    return ((e * f) % P, (g * h) % P, (f * g) % P, (e * h) % P)


def _scalarmult(pt: tuple[int, int, int, int], e: int) -> tuple[int, int, int, int]:
    acc = NEUTRAL_EXT
    cur = pt
    while e > 0:
        if e & 1:
            acc = _edwards_add(acc, cur)
        cur = _edwards_add(cur, cur)
        e >>= 1
    return acc


def _encodepoint(pt: tuple[int, int, int, int]) -> bytes:
    x, y, z, _ = pt
    zi = _inv(z)
    xp = (x * zi) % P
    yp = (y * zi) % P
    enc = yp | ((xp & 1) << 255)
    return enc.to_bytes(32, "little")


def _decodepoint(s: bytes) -> tuple[int, int, int, int] | None:
    if len(s) != 32:
        return None
    val = int.from_bytes(s, "little")
    sign = val >> 255
    y = val & ((1 << 255) - 1)
    if y >= P:
        return None
    u = (y * y - 1) % P
    v = (D * y * y + 1) % P
    v3 = (v * v * v) % P
    v7 = (v3 * v3 * v) % P
    x = (u * v3 * pow((u * v7) % P, (P - 5) // 8, P)) % P
    vx2 = (v * x * x) % P
    if vx2 == u:
        pass
    elif vx2 == (-u) % P:
        x = (x * I) % P
    else:
        return None
    if x == 0 and sign == 1:
        return None
    if (x & 1) != sign:
        x = P - x
    return (x, y, 1, (x * y) % P)


def _clamp_scalar(h_prefix: bytes) -> int:
    a = bytearray(h_prefix[:32])
    a[0] &= 248
    a[31] &= 63
    a[31] |= 64
    return int.from_bytes(a, "little")


def ed25519_public_key_from_seed(seed: bytes) -> bytes:
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = _clamp_scalar(h)
    return _encodepoint(_scalarmult(B_EXT, a))


def ed25519_sign(seed: bytes, message: bytes) -> bytes:
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = _clamp_scalar(h)
    prefix = h[32:]
    pk = _encodepoint(_scalarmult(B_EXT, a))
    r = int.from_bytes(hashlib.sha512(prefix + message).digest(), "little") % L
    r_point = _encodepoint(_scalarmult(B_EXT, r))
    k = int.from_bytes(
        hashlib.sha512(r_point + pk + message).digest(), "little"
    ) % L
    s = (r + k * a) % L
    return r_point + s.to_bytes(32, "little")


def ed25519_verify(pubkey: bytes, message: bytes, signature: bytes) -> bool:
    if len(pubkey) != 32 or len(signature) != 64:
        return False
    a_pt = _decodepoint(pubkey)
    r_pt = _decodepoint(signature[:32])
    if a_pt is None or r_pt is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= L:
        return False
    k = int.from_bytes(
        hashlib.sha512(signature[:32] + pubkey + message).digest(), "little"
    ) % L
    lhs = _encodepoint(_scalarmult(B_EXT, s))
    rhs = _encodepoint(_edwards_add(r_pt, _scalarmult(a_pt, k)))
    return lhs == rhs


# --- Manifest helpers ---


def canonical_update_message(version: str, platform: str, sha256_hex: str,
                             size: int) -> bytes:
    payload = f"deskmate-update-v1\n{version}\n{platform}\n{sha256_hex}\n{size}"
    return payload.encode("ascii")


def _load_seed(spec: str) -> bytes:
    if spec.startswith("env:"):
        var_name = spec[4:]
        raw = os.environ.get(var_name, "").strip()
        if not raw:
            raise ValueError(f"environment variable {var_name} is empty")
    else:
        raw = Path(spec).read_text(encoding="utf-8").strip()
    seed = bytes.fromhex(raw)
    if len(seed) != 32:
        raise ValueError("signing key must be 32 bytes (64 hex chars)")
    return seed


def _load_notes(spec: str) -> str:
    if spec.startswith("@"):
        return Path(spec[1:]).read_text(encoding="utf-8").strip()
    return spec


def _sha256_file(path: Path) -> tuple[str, int]:
    h = hashlib.sha256()
    total = 0
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(65536)
            if not chunk:
                break
            total += len(chunk)
            h.update(chunk)
    return h.hexdigest().lower(), total


# --- Subcommands ---


def cmd_self_test() -> int:
    # RFC 8032 Section 7.1 test vectors 1, 2, 3
    vectors = [
        (
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
            "",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        ),
        (
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
            "72",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
        ),
        (
            "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
            "af82",
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
            "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
        ),
    ]
    for idx, (sk_hex, pk_hex, msg_hex, sig_hex) in enumerate(vectors, 1):
        seed = bytes.fromhex(sk_hex)
        pk_exp = bytes.fromhex(pk_hex)
        msg = bytes.fromhex(msg_hex)
        sig_exp = bytes.fromhex(sig_hex)

        pk = ed25519_public_key_from_seed(seed)
        assert pk == pk_exp, f"vector {idx} public key mismatch"
        sig = ed25519_sign(seed, msg)
        assert sig == sig_exp, f"vector {idx} signature mismatch"
        assert ed25519_verify(pk, msg, sig), f"vector {idx} verify failed"

        bad_sig = bytearray(sig)
        bad_sig[0] ^= 0x01
        assert not ed25519_verify(pk, msg, bytes(bad_sig)), (
            f"vector {idx} accepted tampered signature"
        )
        assert not ed25519_verify(pk, msg + b"\x00", sig), (
            f"vector {idx} accepted tampered message"
        )

    print("PASS RFC 8032 Ed25519 self-test (3 vectors + tamper checks)")
    return 0


def cmd_keygen(out_path: str) -> int:
    target = Path(out_path).expanduser()
    if target.exists():
        print(f"Key already exists at {target}; refusing to overwrite.",
              file=sys.stderr)
        seed = _load_seed(str(target))
        pk = ed25519_public_key_from_seed(seed)
        print(f"PUBLIC_KEY_HEX={pk.hex()}")
        return 0
    target.parent.mkdir(parents=True, exist_ok=True)
    seed = secrets.token_bytes(32)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    fd = os.open(str(target), flags, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        fh.write(seed.hex() + "\n")
    os.chmod(target, 0o600)
    pk = ed25519_public_key_from_seed(seed)
    print(f"KEY_PATH={target}")
    print(f"PUBLIC_KEY_HEX={pk.hex()}")
    return 0


def cmd_pubkey(key_spec: str) -> int:
    seed = _load_seed(key_spec)
    pk = ed25519_public_key_from_seed(seed)
    print(f"PUBLIC_KEY_HEX={pk.hex()}")
    return 0


def cmd_generate(args: argparse.Namespace) -> int:
    seed = _load_seed(args.key)
    pk = ed25519_public_key_from_seed(seed)
    published_at = args.published_at or datetime.now(timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )
    notes = _load_notes(args.notes) if args.notes else ""
    artifacts: dict[str, dict[str, object]] = {}
    for entry in args.artifact:
        if "=" not in entry:
            raise ValueError(f"invalid --artifact spec (expected platform=file:url): {entry}")
        platform, rest = entry.split("=", 1)
        if ":https://" in rest:
            idx = rest.index(":https://")
        elif ":http://" in rest:
            idx = rest.index(":http://")
        else:
            raise ValueError(f"artifact spec missing :https:// URL: {entry}")
        file_str = rest[:idx]
        url = rest[idx + 1:]
        local_file = Path(file_str)
        if not local_file.is_file():
            raise FileNotFoundError(f"artifact file not found: {local_file}")
        sha256_hex, size = _sha256_file(local_file)
        msg = canonical_update_message(args.version, platform, sha256_hex, size)
        sig = ed25519_sign(seed, msg)
        assert ed25519_verify(pk, msg, sig)
        artifacts[platform] = {
            "url": url,
            "sha256": sha256_hex,
            "size": size,
            "signature": sig.hex(),
        }

    manifest = {
        "schema": 1,
        "version": args.version,
        "tag": args.tag or f"v{args.version}",
        "published_at": published_at,
        "release_url": args.release_url,
        "notes": notes,
        "artifacts": artifacts,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"Wrote signed update manifest: {out} (pubkey={pk.hex()})")
    return 0


def cmd_verify(manifest_path: str, pubkey_hex: str) -> int:
    pk = bytes.fromhex(pubkey_hex.strip())
    if len(pk) != 32:
        raise ValueError("pubkey must be 32 bytes (64 hex chars)")
    data = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    if data.get("schema") != 1:
        raise ValueError(f"unsupported schema: {data.get('schema')}")
    version = str(data.get("version", ""))
    if not version:
        raise ValueError("missing version")
    artifacts = data.get("artifacts")
    if not isinstance(artifacts, dict) or not artifacts:
        raise ValueError("manifest has no artifacts")
    for platform, item in artifacts.items():
        url = str(item.get("url", ""))
        sha256_hex = str(item.get("sha256", "")).lower()
        size = int(item.get("size", 0))
        sig_hex = str(item.get("signature", ""))
        if not url.startswith("https://"):
            raise ValueError(f"{platform}: URL must start with https://")
        if len(sha256_hex) != 64:
            raise ValueError(f"{platform}: invalid sha256 length")
        if size <= 0:
            raise ValueError(f"{platform}: invalid size {size}")
        sig = bytes.fromhex(sig_hex)
        msg = canonical_update_message(version, platform, sha256_hex, size)
        if not ed25519_verify(pk, msg, sig):
            raise ValueError(f"{platform}: Ed25519 signature verification FAILED")
        print(f"PASS {platform}: v{version} ({size} bytes, sha256={sha256_hex[:12]}...)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="DeskMate signed update manifest tool")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("self-test", help="Verify RFC 8032 Ed25519 test vectors")

    p_kg = sub.add_parser("keygen", help="Generate Ed25519 signing seed")
    p_kg.add_argument("--out", required=True, help="Output key path (0600)")

    p_pk = sub.add_parser("pubkey", help="Print public key hex from seed")
    p_pk.add_argument("--key", required=True, help="Key path or env:VAR")

    p_gen = sub.add_parser("generate", help="Generate and sign update-manifest.json")
    p_gen.add_argument("--version", required=True)
    p_gen.add_argument("--tag", default="")
    p_gen.add_argument("--release-url", required=True)
    p_gen.add_argument("--notes", default="")
    p_gen.add_argument("--published-at", default="")
    p_gen.add_argument("--key", required=True, help="Key file path or env:VAR")
    p_gen.add_argument("--artifact", action="append", required=True,
                       help="platform=local_file:https://download_url")
    p_gen.add_argument("--out", required=True)

    p_ver = sub.add_parser("verify", help="Verify all signatures in a manifest")
    p_ver.add_argument("--manifest", required=True)
    p_ver.add_argument("--pubkey", required=True, help="64-hex-char Ed25519 pubkey")

    args = parser.parse_args(argv)
    if args.cmd == "self-test":
        return cmd_self_test()
    if args.cmd == "keygen":
        return cmd_keygen(args.out)
    if args.cmd == "pubkey":
        return cmd_pubkey(args.key)
    if args.cmd == "generate":
        return cmd_generate(args)
    if args.cmd == "verify":
        return cmd_verify(args.manifest, args.pubkey)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
