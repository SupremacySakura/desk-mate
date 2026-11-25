#include "deskmate/windows/host_state.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>  // must follow windows.h
#endif

namespace deskmate::windows {

const char* display_position_id(DisplayPosition pos) noexcept {
  switch (pos) {
    case DisplayPosition::Right:
      return "right";
    case DisplayPosition::Left:
      return "left";
    case DisplayPosition::Top:
      return "top";
    case DisplayPosition::Bottom:
      return "bottom";
  }
  return "right";
}

std::optional<DisplayPosition> display_position_from_id(
    const std::string& id) noexcept {
  if (id == "right") {
    return DisplayPosition::Right;
  }
  if (id == "left") {
    return DisplayPosition::Left;
  }
  if (id == "top") {
    return DisplayPosition::Top;
  }
  if (id == "bottom") {
    return DisplayPosition::Bottom;
  }
  return std::nullopt;
}

std::string display_position_label(DisplayPosition pos) {
  switch (pos) {
    case DisplayPosition::Right:
      return "主屏右侧";
    case DisplayPosition::Left:
      return "主屏左侧";
    case DisplayPosition::Top:
      return "主屏上方";
    case DisplayPosition::Bottom:
      return "主屏下方";
  }
  return "主屏右侧";
}

std::string display_position_short_label(DisplayPosition pos) {
  switch (pos) {
    case DisplayPosition::Right:
      return "右侧";
    case DisplayPosition::Left:
      return "左侧";
    case DisplayPosition::Top:
      return "上方";
    case DisplayPosition::Bottom:
      return "下方";
  }
  return "右侧";
}

namespace {

std::string trim_ws(std::string value) {
  const auto is_ws = [](unsigned char ch) { return std::isspace(ch) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !is_ws(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !is_ws(ch); })
                  .base(),
              value.end());
  return value;
}

std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (unsigned char ch : value) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          char buf[8]{};
          std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
          out += buf;
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return out;
}

// Minimal recursive-descent JSON reader for HostPreferences persistence.
struct JsonValue {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type{Type::Null};
  bool bool_val{false};
  double num_val{0.0};
  std::string str_val;
  std::vector<JsonValue> arr_val;
  std::vector<std::pair<std::string, JsonValue>> obj_val;

  const JsonValue* find(const std::string& key) const {
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

  std::string get_string(const std::string& key,
                         const std::string& def = {}) const {
    const auto* v = find(key);
    return (v != nullptr && v->type == Type::String) ? v->str_val : def;
  }

  bool get_bool(const std::string& key, bool def = false) const {
    const auto* v = find(key);
    return (v != nullptr && v->type == Type::Bool) ? v->bool_val : def;
  }

  std::int64_t get_i64(const std::string& key, std::int64_t def = 0) const {
    const auto* v = find(key);
    return (v != nullptr && v->type == Type::Number)
               ? static_cast<std::int64_t>(v->num_val)
               : def;
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string text) : src_(std::move(text)) {}

  bool parse(JsonValue& out) {
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

  bool parse_value(JsonValue& out) {
    skip_ws();
    if (pos_ >= src_.size()) {
      return false;
    }
    const char ch = src_[pos_];
    if (ch == '"') {
      out.type = JsonValue::Type::String;
      return parse_string(out.str_val);
    }
    if (ch == '{') {
      return parse_object(out);
    }
    if (ch == '[') {
      return parse_array(out);
    }
    if (ch == 't' && src_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      out.type = JsonValue::Type::Bool;
      out.bool_val = true;
      return true;
    }
    if (ch == 'f' && src_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      out.type = JsonValue::Type::Bool;
      out.bool_val = false;
      return true;
    }
    if (ch == 'n' && src_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      out.type = JsonValue::Type::Null;
      return true;
    }
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
      return parse_number(out);
    }
    return false;
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
            if (pos_ + 4 > src_.size()) {
              return false;
            }
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              const char hex = src_[pos_++];
              code <<= 4U;
              if (hex >= '0' && hex <= '9') {
                code |= static_cast<unsigned>(hex - '0');
              } else if (hex >= 'a' && hex <= 'f') {
                code |= static_cast<unsigned>(hex - 'a' + 10);
              } else if (hex >= 'A' && hex <= 'F') {
                code |= static_cast<unsigned>(hex - 'A' + 10);
              } else {
                return false;
              }
            }
            if (code < 0x80) {
              out.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
              out.push_back(static_cast<char>(0xC0 | (code >> 6U)));
              out.push_back(static_cast<char>(0x80 | (code & 0x3FU)));
            } else {
              out.push_back(static_cast<char>(0xE0 | (code >> 12U)));
              out.push_back(static_cast<char>(0x80 | ((code >> 6U) & 0x3FU)));
              out.push_back(static_cast<char>(0x80 | (code & 0x3FU)));
            }
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

  bool parse_number(JsonValue& out) {
    const std::size_t start = pos_;
    if (src_[pos_] == '-') {
      ++pos_;
    }
    while (pos_ < src_.size() &&
           std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
      ++pos_;
    }
    if (pos_ < src_.size() && src_[pos_] == '.') {
      ++pos_;
      while (pos_ < src_.size() &&
             std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
        ++pos_;
      }
    }
    if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
        ++pos_;
      }
      while (pos_ < src_.size() &&
             std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
        ++pos_;
      }
    }
    const std::string num_str = src_.substr(start, pos_ - start);
    char* end = nullptr;
    out.num_val = std::strtod(num_str.c_str(), &end);
    if (end == num_str.c_str()) {
      return false;
    }
    out.type = JsonValue::Type::Number;
    return true;
  }

  bool parse_array(JsonValue& out) {
    if (!consume('[')) {
      return false;
    }
    out.type = JsonValue::Type::Array;
    out.arr_val.clear();
    skip_ws();
    if (pos_ < src_.size() && src_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      JsonValue elem;
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

  bool parse_object(JsonValue& out) {
    if (!consume('{')) {
      return false;
    }
    out.type = JsonValue::Type::Object;
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
      JsonValue val;
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

  std::string src_;
  std::size_t pos_{0};
};

std::string default_settings_path() {
#ifdef _WIN32
  wchar_t buffer[MAX_PATH]{};
  const DWORD len = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
  if (len > 0 && len < MAX_PATH) {
    const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0,
                                             nullptr, nullptr);
    if (utf8_len > 1) {
      std::string base(static_cast<std::size_t>(utf8_len - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, buffer, -1, base.data(), utf8_len,
                          nullptr, nullptr);
      return base + "\\DeskMate\\settings.json";
    }
  }
  return "deskmate_settings.json";
#else
  const char* home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    return std::string(home) + "/.config/deskmate/settings.json";
  }
  return "/tmp/deskmate_settings.json";
#endif
}

}  // namespace

std::string PairedDevice::display_name() const {
  const std::string trimmed = trim_ws(custom_name);
  return trimmed.empty() ? default_name : trimmed;
}

bool PairedDevice::is_generic_name(const std::string& name) {
  const std::string t = trim_ws(name);
  return t.empty() || t == "Android" || t == "Accessory" || t == "DeskMate" ||
         t == "DeskMate Extended Display" || t == "DeskMate accessory" ||
         t == "DeskMate device" || t == "DeskMate 平板设备" ||
         t == "DeskMate 设备" || t == "HarmonyOS 平板" ||
         t == "HarmonyOS 设备" || t == "HarmonyOS / Android 设备";
}

SettingsStore::SettingsStore(std::string storage_path)
    : storage_path_(storage_path.empty() ? default_settings_path()
                                         : std::move(storage_path)) {
  load();
}

std::int64_t SettingsStore::now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool SettingsStore::load() {
  std::ifstream in(storage_path_, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  JsonValue root;
  JsonParser parser(ss.str());
  if (!parser.parse(root) || root.type != JsonValue::Type::Object) {
    return false;
  }
  prefs_.auto_virtual_display =
      root.get_bool("auto_virtual_display", prefs_.auto_virtual_display);
  if (const auto pos =
          display_position_from_id(root.get_string("default_position", "right"));
      pos.has_value()) {
    prefs_.default_position = *pos;
  }
  prefs_.allow_input = root.get_bool("allow_input", prefs_.allow_input);
  prefs_.touch_input_mode =
      touch_mode_from_id(root.get_string("touch_input_mode", "compatible_gestures"));
  prefs_.auto_start_service =
      root.get_bool("auto_start_service", prefs_.auto_start_service);
  prefs_.auto_check_updates =
      root.get_bool("auto_check_updates", prefs_.auto_check_updates);
  prefs_.update_mirror_prefix = root.get_string("update_mirror_prefix");
  prefs_.preferred_monitor_name = root.get_string("preferred_monitor_name");

  prefs_.paired_devices.clear();
  if (const auto* arr = root.find("paired_devices");
      arr != nullptr && arr->type == JsonValue::Type::Array) {
    for (const auto& item : arr->arr_val) {
      if (item.type != JsonValue::Type::Object) {
        continue;
      }
      PairedDevice dev;
      dev.id = item.get_string("id");
      if (dev.id.empty()) {
        continue;
      }
      dev.default_name = item.get_string("default_name", "HarmonyOS / Android 设备");
      dev.custom_name = item.get_string("custom_name");
      dev.serial = item.get_string("serial");
      dev.manufacturer = item.get_string("manufacturer");
      dev.allowed = item.get_bool("allowed", true);
      dev.last_connected_epoch_s = item.get_i64("last_connected_epoch_s", 0);
      dev.first_paired_epoch_s = item.get_i64("first_paired_epoch_s", 0);
      dev.last_resolution = item.get_string("last_resolution");
      dev.preferred_position =
          display_position_from_id(item.get_string("preferred_position"));
      prefs_.paired_devices.push_back(std::move(dev));
    }
  }
  sort_devices();
  return true;
}

bool SettingsStore::save() const {
  std::error_code ec;
  const std::filesystem::path target(storage_path_);
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path(), ec);
  }
  std::ostringstream out;
  out << "{\n";
  out << "  \"auto_virtual_display\": "
      << (prefs_.auto_virtual_display ? "true" : "false") << ",\n";
  out << "  \"default_position\": \""
      << display_position_id(prefs_.default_position) << "\",\n";
  out << "  \"allow_input\": " << (prefs_.allow_input ? "true" : "false")
      << ",\n";
  out << "  \"touch_input_mode\": \""
      << touch_mode_id(prefs_.touch_input_mode) << "\",\n";
  out << "  \"auto_start_service\": "
      << (prefs_.auto_start_service ? "true" : "false") << ",\n";
  out << "  \"auto_check_updates\": "
      << (prefs_.auto_check_updates ? "true" : "false") << ",\n";
  out << "  \"update_mirror_prefix\": \""
      << json_escape(prefs_.update_mirror_prefix) << "\",\n";
  out << "  \"preferred_monitor_name\": \""
      << json_escape(prefs_.preferred_monitor_name) << "\",\n";
  out << "  \"paired_devices\": [\n";
  for (std::size_t i = 0; i < prefs_.paired_devices.size(); ++i) {
    const auto& d = prefs_.paired_devices[i];
    out << "    {\n";
    out << "      \"id\": \"" << json_escape(d.id) << "\",\n";
    out << "      \"default_name\": \"" << json_escape(d.default_name) << "\",\n";
    out << "      \"custom_name\": \"" << json_escape(d.custom_name) << "\",\n";
    out << "      \"serial\": \"" << json_escape(d.serial) << "\",\n";
    out << "      \"manufacturer\": \"" << json_escape(d.manufacturer) << "\",\n";
    out << "      \"allowed\": " << (d.allowed ? "true" : "false") << ",\n";
    out << "      \"last_connected_epoch_s\": " << d.last_connected_epoch_s
        << ",\n";
    out << "      \"first_paired_epoch_s\": " << d.first_paired_epoch_s << ",\n";
    out << "      \"last_resolution\": \"" << json_escape(d.last_resolution)
        << "\",\n";
    if (d.preferred_position.has_value()) {
      out << "      \"preferred_position\": \""
          << display_position_id(*d.preferred_position) << "\"\n";
    } else {
      out << "      \"preferred_position\": null\n";
    }
    out << "    }" << (i + 1 < prefs_.paired_devices.size() ? "," : "") << "\n";
  }
  out << "  ]\n}\n";

  std::ofstream file(storage_path_, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    return false;
  }
  const std::string text = out.str();
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

bool SettingsStore::record_seen_device(const std::string& id,
                                       const std::string& name,
                                       const std::string& manufacturer,
                                       const std::string& serial,
                                       const std::string& resolution,
                                       bool mark_connected) {
  const std::string clean_id = trim_ws(id);
  if (clean_id.empty()) {
    return false;
  }
  const std::string mfg = trim_ws(manufacturer);
  const std::string fallback =
      mfg.empty() ? "HarmonyOS / Android 设备" : (mfg + " 设备");
  const std::string clean_name = trim_ws(name);
  const std::string resolved_name = clean_name.empty() ? fallback : clean_name;
  const std::int64_t now = now_epoch_seconds();

  for (auto& existing : prefs_.paired_devices) {
    if (existing.id != clean_id) {
      continue;
    }
    bool changed = false;
    if (!PairedDevice::is_generic_name(resolved_name) &&
        (PairedDevice::is_generic_name(existing.default_name) ||
         existing.default_name != resolved_name)) {
      existing.default_name = resolved_name;
      changed = true;
    }
    if (!serial.empty() && existing.serial != serial) {
      existing.serial = serial;
      changed = true;
    }
    if (!mfg.empty() && existing.manufacturer != mfg) {
      existing.manufacturer = mfg;
      changed = true;
    }
    if (!resolution.empty() && resolution != "NONE" &&
        resolution != "none" && existing.last_resolution != resolution) {
      existing.last_resolution = resolution;
      changed = true;
    }
    if (mark_connected && now - existing.last_connected_epoch_s >= 1) {
      existing.last_connected_epoch_s = now;
      changed = true;
    }
    if (changed) {
      sort_devices();
      save();
    }
    return changed;
  }

  PairedDevice created;
  created.id = clean_id;
  created.default_name = resolved_name;
  created.serial = serial;
  created.manufacturer = mfg;
  created.allowed = true;
  created.last_connected_epoch_s = now;
  created.first_paired_epoch_s = now;
  if (!resolution.empty() && resolution != "NONE" && resolution != "none") {
    created.last_resolution = resolution;
  }
  prefs_.paired_devices.push_back(std::move(created));
  sort_devices();
  save();
  return true;
}

void SettingsStore::set_allowed(const std::string& id, bool allowed) {
  for (auto& d : prefs_.paired_devices) {
    if (d.id == id && d.allowed != allowed) {
      d.allowed = allowed;
      save();
      return;
    }
  }
}

void SettingsStore::rename(const std::string& id,
                           const std::string& custom_name) {
  for (auto& d : prefs_.paired_devices) {
    if (d.id == id) {
      d.custom_name = trim_ws(custom_name);
      save();
      return;
    }
  }
}

void SettingsStore::remove(const std::string& id) {
  const auto before = prefs_.paired_devices.size();
  prefs_.paired_devices.erase(
      std::remove_if(prefs_.paired_devices.begin(),
                     prefs_.paired_devices.end(),
                     [&](const PairedDevice& d) { return d.id == id; }),
      prefs_.paired_devices.end());
  if (prefs_.paired_devices.size() != before) {
    save();
  }
}

bool SettingsStore::is_allowed(const std::string& id) const {
  for (const auto& d : prefs_.paired_devices) {
    if (d.id == id) {
      return d.allowed;
    }
  }
  return true;
}

std::vector<std::string> SettingsStore::blocked_device_ids() const {
  std::vector<std::string> out;
  for (const auto& d : prefs_.paired_devices) {
    if (!d.allowed) {
      out.push_back(d.id);
    }
  }
  return out;
}

std::string SettingsStore::display_name_for(const std::string& id) const {
  for (const auto& d : prefs_.paired_devices) {
    if (d.id == id) {
      return d.display_name();
    }
  }
  return {};
}

std::optional<DisplayPosition> SettingsStore::preferred_position_for(
    const std::string& id) const {
  for (const auto& d : prefs_.paired_devices) {
    if (d.id == id) {
      return d.preferred_position;
    }
  }
  return std::nullopt;
}

void SettingsStore::set_preferred_position(
    const std::string& id, std::optional<DisplayPosition> pos) {
  for (auto& d : prefs_.paired_devices) {
    if (d.id == id && d.preferred_position != pos) {
      d.preferred_position = pos;
      save();
      return;
    }
  }
}

void SettingsStore::sort_devices() {
  std::sort(prefs_.paired_devices.begin(), prefs_.paired_devices.end(),
            [](const PairedDevice& a, const PairedDevice& b) {
              return a.last_connected_epoch_s > b.last_connected_epoch_s;
            });
}

std::string SettingsStore::relative_time_text(std::int64_t epoch_s,
                                              std::int64_t now_epoch_s) {
  if (epoch_s <= 0) {
    return "未知";
  }
  const std::int64_t now =
      now_epoch_s > 0 ? now_epoch_s : now_epoch_seconds();
  const std::int64_t diff = std::max<std::int64_t>(0, now - epoch_s);
  if (diff < 60) {
    return "刚刚";
  }
  if (diff < 3600) {
    return std::to_string(diff / 60) + " 分钟前";
  }
  if (diff < 86400) {
    return std::to_string(diff / 3600) + " 小时前";
  }
  if (diff < 86400 * 30) {
    return std::to_string(diff / 86400) + " 天前";
  }
  std::time_t t = static_cast<std::time_t>(epoch_s);
  std::tm tm_val{};
#ifdef _WIN32
  localtime_s(&tm_val, &t);
#else
  localtime_r(&t, &tm_val);
#endif
  char buf[32]{};
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm_val.tm_year + 1900,
                tm_val.tm_mon + 1, tm_val.tm_mday);
  return std::string(buf);
}

#ifdef _WIN32
namespace {
constexpr wchar_t kRunRegKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"DeskMate";
}  // namespace

StartupStatus StartupManager::current_status() const {
  StartupStatus status;
  status.error_message = last_error_;
  HKEY key = nullptr;
  const LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ, &key);
  if (rc == ERROR_SUCCESS && key != nullptr) {
    DWORD type = 0;
    DWORD size = 0;
    const LONG q = RegQueryValueExW(key, kRunValueName, nullptr, &type, nullptr,
                                    &size);
    RegCloseKey(key);
    if (q == ERROR_SUCCESS && size > 0) {
      status.launch_at_login_enabled = true;
      status.status_description =
          "已添加到 Windows 启动项（HKCU\\...\\Run），登录系统时将自动运行 DeskMate。";
      return status;
    }
  }
  status.launch_at_login_enabled = false;
  status.status_description =
      "未开启。开启后登录 Windows 时将自动运行 DeskMate。";
  return status;
}

StartupStatus StartupManager::set_launch_at_login(bool enabled) {
  last_error_.clear();
  HKEY key = nullptr;
  const LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                  &key, nullptr);
  if (rc != ERROR_SUCCESS || key == nullptr) {
    last_error_ = "无法打开注册表启动项 (err=" + std::to_string(rc) + ")";
    return current_status();
  }
  if (enabled) {
    wchar_t exe[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
      last_error_ = "获取可执行文件路径失败";
      RegCloseKey(key);
      return current_status();
    }
    const std::wstring quoted = std::wstring(L"\"") + exe + L"\"";
    const LONG set_rc = RegSetValueExW(
        key, kRunValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(quoted.c_str()),
        static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    if (set_rc != ERROR_SUCCESS) {
      last_error_ = "写入启动项失败 (err=" + std::to_string(set_rc) + ")";
    }
  } else {
    const LONG del_rc = RegDeleteValueW(key, kRunValueName);
    if (del_rc != ERROR_SUCCESS && del_rc != ERROR_FILE_NOT_FOUND) {
      last_error_ = "移除启动项失败 (err=" + std::to_string(del_rc) + ")";
    }
  }
  RegCloseKey(key);
  return current_status();
}

void StartupManager::open_startup_settings() const {
  ShellExecuteW(nullptr, L"open", L"ms-settings:startupapps", nullptr, nullptr,
                SW_SHOWNORMAL);
}

#else

StartupStatus StartupManager::current_status() const {
  return {false, "仅 Windows 运行时支持开机启动项配置。", last_error_};
}

StartupStatus StartupManager::set_launch_at_login(bool) {
  return current_status();
}

void StartupManager::open_startup_settings() const {}

#endif

}  // namespace deskmate::windows
