#include "deskmate/platform/update_crypto.hpp"
#include "deskmate/platform/update_manifest.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (false)

using deskmate::platform::apply_mirror_prefix;
using deskmate::platform::compare_semver;
using deskmate::platform::ed25519_verify;
using deskmate::platform::hex_decode;
using deskmate::platform::hex_encode;
using deskmate::platform::is_newer_version;
using deskmate::platform::parse_semver;
using deskmate::platform::parse_update_manifest;
using deskmate::platform::SemVer;
using deskmate::platform::sha256;
using deskmate::platform::sha256_file;
using deskmate::platform::sha512;
using deskmate::platform::UpdateManifest;
using deskmate::platform::verify_artifact_signature;

std::vector<std::uint8_t> decode_or_die(std::string_view hex) {
  std::vector<std::uint8_t> out;
  CHECK(hex_decode(hex, out));
  return out;
}

void test_sha256_and_sha512() {
  const std::string empty;
  const auto empty256 = sha256(std::span<const std::uint8_t>{});
  CHECK(hex_encode(empty256) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  const std::string abc = "abc";
  const auto abc256 = sha256(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()));
  CHECK(hex_encode(abc256) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  const auto empty512 = sha512(std::span<const std::uint8_t>{});
  CHECK(hex_encode(empty512) ==
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

  const auto abc512 = sha512(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()));
  CHECK(hex_encode(abc512) ==
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

  // File hashing check
  const std::string tmp = "/tmp/deskmate_update_sha_test.bin";
  {
    std::ofstream f(tmp, std::ios::binary);
    f.write("abc", 3);
  }
  std::array<std::uint8_t, 32> fh{};
  std::uint64_t fsize = 0;
  CHECK(sha256_file(tmp, fh, fsize));
  CHECK(fsize == 3);
  CHECK(fh == abc256);
  std::remove(tmp.c_str());
}

void test_ed25519_rfc8032_vectors() {
  struct Vec {
    const char* pk;
    const char* msg;
    const char* sig;
  };
  static constexpr Vec kVecs[] = {
      {
          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
          "",
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
          "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
      },
      {
          "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
          "72",
          "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
          "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
      },
      {
          "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
          "af82",
          "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
          "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
      },
  };

  for (const auto& v : kVecs) {
    const auto pk_vec = decode_or_die(v.pk);
    const auto msg = decode_or_die(v.msg);
    const auto sig_vec = decode_or_die(v.sig);
    std::array<std::uint8_t, 32> pk{};
    std::array<std::uint8_t, 64> sig{};
    std::copy_n(pk_vec.begin(), 32, pk.begin());
    std::copy_n(sig_vec.begin(), 64, sig.begin());

    CHECK(ed25519_verify(pk, msg, sig));

    auto bad_sig = sig;
    bad_sig[10] ^= 0x80u;
    CHECK(!ed25519_verify(pk, msg, bad_sig));

    auto bad_pk = pk;
    bad_pk[0] ^= 0x01u;
    CHECK(!ed25519_verify(bad_pk, msg, sig));

    std::vector<std::uint8_t> bad_msg = msg;
    bad_msg.push_back(0x42u);
    CHECK(!ed25519_verify(pk, bad_msg, sig));

    // Non-canonical S >= L (set top bit of S) must fail even before curve math
    auto non_canon_s = sig;
    non_canon_s[63] |= 0x20u;
    CHECK(!ed25519_verify(pk, msg, non_canon_s));
  }
}

void test_semver() {
  SemVer a{};
  CHECK(parse_semver("1.2.0", a));
  CHECK(a.major == 1 && a.minor == 2 && a.patch == 0 && a.prerelease.empty());
  CHECK(parse_semver("v1.10.2-beta.1", a));
  CHECK(a.major == 1 && a.minor == 10 && a.patch == 2 &&
        a.prerelease == "beta.1");

  SemVer bad{};
  CHECK(!parse_semver("1.2", bad));
  CHECK(!parse_semver("01.2.0", bad));
  CHECK(!parse_semver("1.2.0-", bad));
  CHECK(!parse_semver("abc", bad));

  CHECK(is_newer_version("1.2.1", "1.2.0"));
  CHECK(is_newer_version("v1.10.0", "1.2.9"));
  CHECK(is_newer_version("1.3.0", "1.3.0-beta.1"));
  CHECK(!is_newer_version("1.2.0", "1.2.0"));
  CHECK(!is_newer_version("1.1.9", "1.2.0"));
  CHECK(!is_newer_version("1.3.0-beta.1", "1.3.0"));
  CHECK(compare_semver({1, 2, 0, ""}, {1, 2, 0, ""}) == 0);
}

void test_manifest_and_mirror() {
  static constexpr const char* kManifest = R"({
    "schema": 1,
    "version": "1.3.0",
    "tag": "v1.3.0",
    "published_at": "2026-11-26T00:00:00Z",
    "release_url": "https://github.com/SupremacySakura/desk-mate/releases/tag/v1.3.0",
    "notes": "Auto updater & stability fixes",
    "artifacts": {
      "windows-x64": {
        "url": "https://github.com/SupremacySakura/desk-mate/releases/download/v1.3.0/DeskMate-v1.3.0-windows-x64.zip",
        "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "size": 1024000,
        "signature": "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
      }
    }
  })";

  UpdateManifest m{};
  std::string err;
  CHECK(parse_update_manifest(kManifest, "windows-x64", m, err));
  CHECK(m.schema == 1);
  CHECK(m.version == "1.3.0");
  CHECK(m.artifact.has_value());
  CHECK(m.artifact->size == 1024000ULL);
  CHECK(!verify_artifact_signature(m.version, "windows-x64", *m.artifact));

  // Missing platform is valid manifest with empty optional artifact.
  UpdateManifest m2{};
  CHECK(parse_update_manifest(kManifest, "macos-arm64", m2, err));
  CHECK(!m2.artifact.has_value());

  // Mirror prefix safety checks.
  const std::string gh =
      "https://github.com/SupremacySakura/desk-mate/releases/latest/download/"
      "update-manifest.json";
  CHECK(apply_mirror_prefix(gh, "") == gh);
  CHECK(apply_mirror_prefix(gh, "  https://ghfast.top  ") ==
        "https://ghfast.top/" + gh);
  CHECK(apply_mirror_prefix("https://ghfast.top/" + gh, "https://ghfast.top/") ==
        "https://ghfast.top/" + gh);
  CHECK(apply_mirror_prefix(gh, "javascript:alert(1)") == gh);
}

}  // namespace

int main() {
  test_sha256_and_sha512();
  test_ed25519_rfc8032_vectors();
  test_semver();
  test_manifest_and_mirror();
  std::printf("all update crypto & manifest tests passed\n");
  return 0;
}
