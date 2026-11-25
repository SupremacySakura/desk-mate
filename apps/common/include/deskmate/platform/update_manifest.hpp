#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace deskmate::platform {

inline constexpr const char* kDefaultUpdateManifestUrl =
    "https://github.com/SupremacySakura/desk-mate/releases/latest/download/"
    "update-manifest.json";

inline constexpr const char* kDefaultReleasesUrl =
    "https://github.com/SupremacySakura/desk-mate/releases/latest";

inline constexpr const char* kUpdatePublicKeyHex =
    "4a4128a91fed00f0bb0f5f157e56f8fae6749b4575dff2d21bdc8546e0d98391";

inline constexpr std::uint64_t kMaxUpdateArtifactBytes = 256ULL * 1024ULL * 1024ULL;

struct SemVer {
  int major{0};
  int minor{0};
  int patch{0};
  std::string prerelease;
};

[[nodiscard]] bool parse_semver(std::string_view text, SemVer& out);
[[nodiscard]] int compare_semver(const SemVer& a, const SemVer& b) noexcept;
[[nodiscard]] bool is_newer_version(std::string_view candidate,
                                    std::string_view current);

struct UpdateArtifact {
  std::string url;
  std::string sha256;
  std::uint64_t size{0};
  std::string signature;
};

struct UpdateManifest {
  int schema{0};
  std::string version;
  std::string tag;
  std::string published_at;
  std::string release_url;
  std::string notes;
  std::optional<UpdateArtifact> artifact;
};

[[nodiscard]] std::string canonical_update_message(std::string_view version,
                                                   std::string_view platform,
                                                   std::string_view sha256_hex,
                                                   std::uint64_t size);

[[nodiscard]] bool parse_update_manifest(std::string_view json,
                                         std::string_view platform_key,
                                         UpdateManifest& out,
                                         std::string& error);

[[nodiscard]] bool verify_artifact_signature(
    std::string_view version, std::string_view platform,
    const UpdateArtifact& artifact,
    std::string_view pubkey_hex = kUpdatePublicKeyHex);

[[nodiscard]] std::string apply_mirror_prefix(std::string_view url,
                                              std::string_view mirror_prefix);

}  // namespace deskmate::platform
