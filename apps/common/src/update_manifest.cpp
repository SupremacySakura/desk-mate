#include "deskmate/platform/update_manifest.hpp"

#include "deskmate/platform/update_crypto.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace deskmate::platform {
namespace {

std::string trim_sv(std::string_view sv) {
  while (!sv.empty() &&
         std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() &&
         std::isspace(static_cast<unsigned char>(sv.back())) != 0) {
    sv.remove_suffix(1);
  }
  return std::string(sv);
}

std::string to_lower_ascii(std::string_view sv) {
  std::string out(sv);
  for (char& ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

bool parse_uint_component(std::string_view part, int& out) {
  if (part.empty()) {
    return false;
  }
  if (part.size() > 1 && part[0] == '0') {
    return false;
  }
  std::uint64_t acc = 0;
  for (char ch : part) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    acc = acc * 10ULL + static_cast<std::uint64_t>(ch - '0');
    if (acc > 999999ULL) {
      return false;
    }
  }
  out = static_cast<int>(acc);
  return true;
}

// Self-contained JSON parser for update-manifest.json.
struct JsonVal {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type{Type::Null};
  bool bool_val{false};
  std::uint64_t uint_val{0};
  bool valid_uint{false};
  std::string str_val;
  std::vector<JsonVal> arr_val;
  std::vector<std::pair<std::string, JsonVal>> obj_val;

  const JsonVal* find(std::string_view key) const {
    if (type != Type::Object) {
      return nullptr;
    }
    for (const auto& [k, v] : obj_val) {
      if (k == key) {
        return &v;
      }
    }
    return nullptr;
  }
};

class MiniJsonParser {
 public:
  explicit MiniJsonParser(std::string_view src) : src_(src) {}

  bool parse(JsonVal& out) {
    skip_ws();
    if (!parse_value(out)) {
      return false;
    }
    skip_ws();
    return pos_ == src_.size();
  }

 private:
  void skip_ws() {
    while (pos_ < src_.size() &&
           std::isspace(static_cast<unsigned char>(src_[pos_])) != 0) {
      ++pos_;
    }
  }

  bool consume(char expected) {
    skip_ws();
    if (pos_ < src_.size() && src_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool parse_value(JsonVal& out) {
    skip_ws();
    if (pos_ >= src_.size()) {
      return false;
    }
    const char ch = src_[pos_];
    if (ch == '"') {
      out.type = JsonVal::Type::String;
      return parse_string(out.str_val);
    }
    if (ch == '{') {
      return parse_object(out);
    }
    if (ch == '[') {
      return parse_array(out);
    }
    if (ch == 't' && src_.substr(pos_, 4) == "true") {
      pos_ += 4;
      out.type = JsonVal::Type::Bool;
      out.bool_val = true;
      return true;
    }
    if (ch == 'f' && src_.substr(pos_, 5) == "false") {
      pos_ += 5;
      out.type = JsonVal::Type::Bool;
      out.bool_val = false;
      return true;
    }
    if (ch == 'n' && src_.substr(pos_, 4) == "null") {
      pos_ += 4;
      out.type = JsonVal::Type::Null;
      return true;
    }
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
      return parse_number(out);
    }
    return false;
  }

  static void append_utf8(std::string& out, unsigned code) {
    if (code < 0x80u) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800u) {
      out.push_back(static_cast<char>(0xC0u | (code >> 6u)));
      out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else if (code < 0x10000u) {
      out.push_back(static_cast<char>(0xE0u | (code >> 12u)));
      out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (code >> 18u)));
      out.push_back(static_cast<char>(0x80u | ((code >> 12u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    }
  }

  bool parse_hex4(unsigned& code) {
    if (pos_ + 4 > src_.size()) {
      return false;
    }
    code = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = src_[pos_++];
      code <<= 4u;
      if (h >= '0' && h <= '9') {
        code |= static_cast<unsigned>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        code |= static_cast<unsigned>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        code |= static_cast<unsigned>(h - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  bool parse_string(std::string& out) {
    if (pos_ >= src_.size() || src_[pos_] != '"') {
      return false;
    }
    ++pos_;
    out.clear();
    while (pos_ < src_.size()) {
      const char ch = src_[pos_++];
      if (ch == '"') {
        return true;
      }
      if (static_cast<unsigned char>(ch) < 0x20u) {
        return false;
      }
      if (ch == '\\') {
        if (pos_ >= src_.size()) {
          return false;
        }
        const char esc = src_[pos_++];
        switch (esc) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            unsigned code = 0;
            if (!parse_hex4(code)) {
              return false;
            }
            if (code >= 0xD800u && code <= 0xDBFFu &&
                pos_ + 6 <= src_.size() && src_[pos_] == '\\' &&
                src_[pos_ + 1] == 'u') {
              const std::size_t saved = pos_;
              pos_ += 2;
              unsigned low = 0;
              if (parse_hex4(low) && low >= 0xDC00u && low <= 0xDFFFu) {
                code = 0x10000u + (((code - 0xD800u) << 10u) | (low - 0xDC00u));
              } else {
                pos_ = saved;
              }
            }
            append_utf8(out, code);
            break;
          }
          default:
            return false;
        }
      } else {
        out.push_back(ch);
      }
    }
    return false;
  }

  bool parse_number(JsonVal& out) {
    const std::size_t start = pos_;
    if (src_[pos_] == '-') {
      ++pos_;
    }
    if (pos_ >= src_.size() ||
        std::isdigit(static_cast<unsigned char>(src_[pos_])) == 0) {
      return false;
    }
    std::uint64_t uval = 0;
    bool ok_uint = (src_[start] != '-');
    while (pos_ < src_.size() &&
           std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
      const unsigned digit = static_cast<unsigned>(src_[pos_] - '0');
      if (ok_uint && uval <= (UINT64_MAX - digit) / 10ULL) {
        uval = uval * 10ULL + digit;
      } else {
        ok_uint = false;
      }
      ++pos_;
    }
    if (pos_ < src_.size() && src_[pos_] == '.') {
      ok_uint = false;
      ++pos_;
      if (pos_ >= src_.size() ||
          std::isdigit(static_cast<unsigned char>(src_[pos_])) == 0) {
        return false;
      }
      while (pos_ < src_.size() &&
             std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
        ++pos_;
      }
    }
    if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
      ok_uint = false;
      ++pos_;
      if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= src_.size() ||
          std::isdigit(static_cast<unsigned char>(src_[pos_])) == 0) {
        return false;
      }
      while (pos_ < src_.size() &&
             std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
        ++pos_;
      }
    }
    out.type = JsonVal::Type::Number;
    out.uint_val = ok_uint ? uval : 0;
    out.valid_uint = ok_uint;
    return true;
  }

  bool parse_array(JsonVal& out) {
    if (!consume('[')) {
      return false;
    }
    out.type = JsonVal::Type::Array;
    out.arr_val.clear();
    skip_ws();
    if (pos_ < src_.size() && src_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      JsonVal elem;
      if (!parse_value(elem)) {
        return false;
      }
      out.arr_val.push_back(std::move(elem));
      if (consume(']')) {
        return true;
      }
      if (!consume(',')) {
        return false;
      }
    }
  }

  bool parse_object(JsonVal& out) {
    if (!consume('{')) {
      return false;
    }
    out.type = JsonVal::Type::Object;
    out.obj_val.clear();
    skip_ws();
    if (pos_ < src_.size() && src_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      skip_ws();
      std::string key;
      if (!parse_string(key)) {
        return false;
      }
      if (!consume(':')) {
        return false;
      }
      JsonVal val;
      if (!parse_value(val)) {
        return false;
      }
      out.obj_val.emplace_back(std::move(key), std::move(val));
      if (consume('}')) {
        return true;
      }
      if (!consume(',')) {
        return false;
      }
    }
  }

  std::string_view src_;
  std::size_t pos_{0};
};

}  // namespace

bool parse_semver(std::string_view text, SemVer& out) {
  std::string s = trim_sv(text);
  if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
    s.erase(s.begin());
  }
  const auto plus = s.find('+');
  if (plus != std::string::npos) {
    s.erase(plus);
  }
  std::string core = s;
  std::string pre;
  const auto dash = s.find('-');
  if (dash != std::string::npos) {
    core = s.substr(0, dash);
    pre = s.substr(dash + 1);
    if (pre.empty()) {
      return false;
    }
  }

  const auto d1 = core.find('.');
  if (d1 == std::string::npos) {
    return false;
  }
  const auto d2 = core.find('.', d1 + 1);
  if (d2 == std::string::npos || core.find('.', d2 + 1) != std::string::npos) {
    return false;
  }

  SemVer parsed;
  if (!parse_uint_component(std::string_view(core).substr(0, d1), parsed.major) ||
      !parse_uint_component(std::string_view(core).substr(d1 + 1, d2 - d1 - 1),
                            parsed.minor) ||
      !parse_uint_component(std::string_view(core).substr(d2 + 1),
                            parsed.patch)) {
    return false;
  }
  parsed.prerelease = std::move(pre);
  out = std::move(parsed);
  return true;
}

int compare_semver(const SemVer& a, const SemVer& b) noexcept {
  if (a.major != b.major) {
    return a.major < b.major ? -1 : 1;
  }
  if (a.minor != b.minor) {
    return a.minor < b.minor ? -1 : 1;
  }
  if (a.patch != b.patch) {
    return a.patch < b.patch ? -1 : 1;
  }
  if (a.prerelease.empty() && b.prerelease.empty()) {
    return 0;
  }
  if (a.prerelease.empty()) {
    return 1;
  }
  if (b.prerelease.empty()) {
    return -1;
  }
  if (a.prerelease == b.prerelease) {
    return 0;
  }
  return a.prerelease < b.prerelease ? -1 : 1;
}

bool is_newer_version(std::string_view candidate, std::string_view current) {
  SemVer cand{};
  SemVer cur{};
  if (!parse_semver(candidate, cand) || !parse_semver(current, cur)) {
    return false;
  }
  return compare_semver(cand, cur) > 0;
}

std::string canonical_update_message(std::string_view version,
                                     std::string_view platform,
                                     std::string_view sha256_hex,
                                     std::uint64_t size) {
  return "deskmate-update-v1\n" + std::string(version) + "\n" +
         std::string(platform) + "\n" + to_lower_ascii(sha256_hex) + "\n" +
         std::to_string(size);
}

bool parse_update_manifest(std::string_view json, std::string_view platform_key,
                           UpdateManifest& out, std::string& error) {
  out = UpdateManifest{};
  error.clear();

  JsonVal root;
  MiniJsonParser parser(json);
  if (!parser.parse(root) || root.type != JsonVal::Type::Object) {
    error = "更新清单 JSON 格式无效";
    return false;
  }

  const auto* schema_val = root.find("schema");
  if (schema_val == nullptr || schema_val->type != JsonVal::Type::Number ||
      !schema_val->valid_uint || schema_val->uint_val != 1ULL) {
    error = "不支持的更新清单版本";
    return false;
  }
  out.schema = 1;

  const auto* ver_val = root.find("version");
  SemVer sv{};
  if (ver_val == nullptr || ver_val->type != JsonVal::Type::String ||
      !parse_semver(ver_val->str_val, sv)) {
    error = "更新清单缺少有效版本号";
    return false;
  }
  out.version = ver_val->str_val;

  if (const auto* tag_val = root.find("tag");
      tag_val != nullptr && tag_val->type == JsonVal::Type::String) {
    out.tag = tag_val->str_val;
  } else {
    out.tag = "v" + out.version;
  }
  if (const auto* pub_val = root.find("published_at");
      pub_val != nullptr && pub_val->type == JsonVal::Type::String) {
    out.published_at = pub_val->str_val;
  }
  if (const auto* rel_val = root.find("release_url");
      rel_val != nullptr && rel_val->type == JsonVal::Type::String &&
      rel_val->str_val.rfind("https://", 0) == 0) {
    out.release_url = rel_val->str_val;
  } else {
    out.release_url = kDefaultReleasesUrl;
  }
  if (const auto* notes_val = root.find("notes");
      notes_val != nullptr && notes_val->type == JsonVal::Type::String) {
    out.notes = notes_val->str_val;
  }

  const auto* artifacts_val = root.find("artifacts");
  if (artifacts_val == nullptr ||
      artifacts_val->type != JsonVal::Type::Object) {
    error = "更新清单缺少 artifacts 字段";
    return false;
  }

  const auto* item = artifacts_val->find(platform_key);
  if (item == nullptr) {
    return true;  // Valid manifest, no package published for this platform yet.
  }
  if (item->type != JsonVal::Type::Object) {
    error = "平台安装包描述格式无效";
    return false;
  }

  const auto* url_v = item->find("url");
  const auto* sha_v = item->find("sha256");
  const auto* size_v = item->find("size");
  const auto* sig_v = item->find("signature");
  if (url_v == nullptr || url_v->type != JsonVal::Type::String ||
      url_v->str_val.rfind("https://", 0) != 0) {
    error = "安装包下载地址必须为 HTTPS";
    return false;
  }

  std::vector<std::uint8_t> tmp;
  if (sha_v == nullptr || sha_v->type != JsonVal::Type::String ||
      sha_v->str_val.size() != 64 || !hex_decode(sha_v->str_val, tmp) ||
      tmp.size() != 32) {
    error = "安装包 SHA-256 格式无效";
    return false;
  }
  if (size_v == nullptr || size_v->type != JsonVal::Type::Number ||
      !size_v->valid_uint || size_v->uint_val == 0 ||
      size_v->uint_val > kMaxUpdateArtifactBytes) {
    error = "安装包文件大小超出允许范围";
    return false;
  }
  if (sig_v == nullptr || sig_v->type != JsonVal::Type::String ||
      sig_v->str_val.size() != 128 || !hex_decode(sig_v->str_val, tmp) ||
      tmp.size() != 64) {
    error = "安装包数字签名格式无效";
    return false;
  }

  UpdateArtifact art;
  art.url = url_v->str_val;
  art.sha256 = to_lower_ascii(sha_v->str_val);
  art.size = size_v->uint_val;
  art.signature = to_lower_ascii(sig_v->str_val);
  out.artifact = std::move(art);
  return true;
}

bool verify_artifact_signature(std::string_view version,
                               std::string_view platform,
                               const UpdateArtifact& artifact,
                               std::string_view pubkey_hex) {
  std::vector<std::uint8_t> pk_bytes;
  std::vector<std::uint8_t> sig_bytes;
  if (!hex_decode(pubkey_hex, pk_bytes) || pk_bytes.size() != 32) {
    return false;
  }
  if (!hex_decode(artifact.signature, sig_bytes) || sig_bytes.size() != 64) {
    return false;
  }
  std::array<std::uint8_t, 32> pk{};
  std::array<std::uint8_t, 64> sig{};
  std::copy_n(pk_bytes.begin(), 32, pk.begin());
  std::copy_n(sig_bytes.begin(), 64, sig.begin());

  const std::string msg = canonical_update_message(
      version, platform, artifact.sha256, artifact.size);
  return ed25519_verify(
      pk,
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size()),
      sig);
}

std::string apply_mirror_prefix(std::string_view url,
                                std::string_view mirror_prefix) {
  std::string prefix = trim_sv(mirror_prefix);
  if (prefix.empty()) {
    return std::string(url);
  }
  if (prefix.rfind("https://", 0) != 0 && prefix.rfind("http://", 0) != 0) {
    return std::string(url);
  }
  if (prefix.back() != '/') {
    prefix.push_back('/');
  }
  if (url.rfind(prefix, 0) == 0) {
    return std::string(url);
  }
  if (url.rfind("https://github.com/", 0) != 0 &&
      url.rfind("https://objects.githubusercontent.com/", 0) != 0 &&
      url.rfind("https://release-assets.githubusercontent.com/", 0) != 0) {
    return std::string(url);
  }
  return prefix + std::string(url);
}

}  // namespace deskmate::platform
