#include "deskmate/windows/studio_ui.hpp"

#ifndef _WIN32

namespace deskmate::windows {

int run_studio_ui(HostController&) { return 0; }

}  // namespace deskmate::windows

#else

#include "deskmate/windows/host_controller.hpp"
#include "deskmate/windows/resource_ids.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <windows.h>
#include <windowsx.h>
#include <wtsapi32.h>
#include <wrl/client.h>
#include <shellapi.h>  // must follow windows.h

#include <algorithm>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace deskmate::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kMsgTrayIcon = WM_APP + 101;
constexpr UINT_PTR kTimerRefreshId = 1;
constexpr UINT_PTR kTimerCopyResetId = 2;
constexpr int kEditRenameId = 4001;
constexpr int kEditMirrorId = 4002;

constexpr UINT kCmdTrayShow = 1001;
constexpr UINT kCmdTrayToggleService = 1002;
constexpr UINT kCmdTrayCopyStatus = 1003;
constexpr UINT kCmdTrayQuit = 1004;

constexpr UINT kCmdTouchModeBase = 2000;
constexpr UINT kCmdDevPosAuto = 3000;
constexpr UINT kCmdDevPosRight = 3001;
constexpr UINT kCmdDevPosLeft = 3002;
constexpr UINT kCmdDevPosTop = 3003;
constexpr UINT kCmdDevPosBottom = 3004;
constexpr UINT kCmdDevDisconnect = 3010;
constexpr UINT kCmdDevRename = 3011;
constexpr UINT kCmdDevCopyId = 3012;
constexpr UINT kCmdDevRemove = 3013;

constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmCornerRound = 2;

enum class UiTab {
  Display = 0,
  Devices = 1,
  Settings = 2,
  Diagnostics = 3,
};

enum class Tone {
  Neutral,
  Crimson,
  Positive,
  Warning,
};

struct HitRegion {
  D2D1_RECT_F rect{};
  std::function<void()> action;
};

D2D1_COLOR_F rgb(std::uint32_t hex, float alpha = 1.0F) {
  return D2D1::ColorF(static_cast<float>((hex >> 16U) & 0xFFU) / 255.0F,
                      static_cast<float>((hex >> 8U) & 0xFFU) / 255.0F,
                      static_cast<float>(hex & 0xFFU) / 255.0F, alpha);
}

namespace palette {
const D2D1_COLOR_F kCanvasTop = rgb(0xFFFFFF);
const D2D1_COLOR_F kCanvasMid = rgb(0xFCFBF8);
const D2D1_COLOR_F kCanvasEdge = rgb(0xF4F0E8);
const D2D1_COLOR_F kCardSurface = rgb(0xFFFFFF);
const D2D1_COLOR_F kSubtleSurface = rgb(0xFAF7F2);
const D2D1_COLOR_F kHairline = rgb(0xDCD6CC);
const D2D1_COLOR_F kSoftHairline = rgb(0xEAE4DD);
const D2D1_COLOR_F kInk = rgb(0x232026);
const D2D1_COLOR_F kInkSecondary = rgb(0x5A545E);
const D2D1_COLOR_F kInkMuted = rgb(0x8C858E);
const D2D1_COLOR_F kCrimson = rgb(0x9D193D);
const D2D1_COLOR_F kCrimsonPressed = rgb(0x7E1230);
const D2D1_COLOR_F kRoseWash = rgb(0xF7EAEF);
const D2D1_COLOR_F kRoseBorder = rgb(0xD9B6C2);
const D2D1_COLOR_F kChampagne = rgb(0xB9975B);
const D2D1_COLOR_F kJade = rgb(0x2D7D5B);
const D2D1_COLOR_F kJadeWash = rgb(0xEEF7F3);
const D2D1_COLOR_F kJadeBorder = rgb(0xB5DBC9);
const D2D1_COLOR_F kAmber = rgb(0xA8641E);
const D2D1_COLOR_F kAmberWash = rgb(0xFBF3E8);
const D2D1_COLOR_F kAmberBorder = rgb(0xE5C7A1);
const D2D1_COLOR_F kWhite = rgb(0xFFFFFF);
}  // namespace palette

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), needed);
  return out;
}

std::string wide_to_utf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int needed = WideCharToMultiByte(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
      nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), needed, nullptr, nullptr);
  return out;
}

void copy_to_clipboard(HWND owner, const std::string& text) {
  const std::wstring wide = utf8_to_wide(text);
  if (!OpenClipboard(owner)) {
    return;
  }
  EmptyClipboard();
  const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (mem != nullptr) {
    void* ptr = GlobalLock(mem);
    if (ptr != nullptr) {
      std::memcpy(ptr, wide.c_str(), bytes);
      GlobalUnlock(mem);
      SetClipboardData(CF_UNICODETEXT, mem);
    } else {
      GlobalFree(mem);
    }
  }
  CloseClipboard();
}

void open_shell_target(const wchar_t* target) {
  ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL);
}

void open_shell_target_utf8(const std::string& target) {
  if (target.empty()) {
    return;
  }
  const std::wstring w = utf8_to_wide(target);
  open_shell_target(w.c_str());
}

std::string format_bitrate_mbps(double mbps) {
  if (mbps <= 0.0) {
    return {};
  }
  char buf[32]{};
  if (mbps < 0.1) {
    std::snprintf(buf, sizeof(buf), "%.2f", mbps);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f", mbps);
  }
  return std::string(buf);
}

Tone tone_for_badge(const std::string& badge) {
  if (badge == "会话已建立" || badge.find("运行中") != std::string::npos ||
      badge == "测试模式") {
    return Tone::Crimson;
  }
  if (badge == "出错" || badge == "待授权" || badge == "待驱动" ||
      badge == "待装虚拟屏" || badge == "未就绪" || badge == "已拦截") {
    return Tone::Warning;
  }
  if (badge.find("已连接") != std::string::npos || badge == "协商中" ||
      badge == "已开启" || badge == "运行中" || badge == "允许连接" ||
      badge == "USB 已接入" || badge == "原生触摸可用") {
    return Tone::Positive;
  }
  return Tone::Neutral;
}

D2D1_COLOR_F tone_fg(Tone t) {
  switch (t) {
    case Tone::Neutral:
      return palette::kInkSecondary;
    case Tone::Crimson:
      return palette::kCrimson;
    case Tone::Positive:
      return palette::kJade;
    case Tone::Warning:
      return palette::kAmber;
  }
  return palette::kInkSecondary;
}

D2D1_COLOR_F tone_dot(Tone t) {
  switch (t) {
    case Tone::Neutral:
      return palette::kInkMuted;
    case Tone::Crimson:
      return palette::kCrimson;
    case Tone::Positive:
      return palette::kJade;
    case Tone::Warning:
      return palette::kAmber;
  }
  return palette::kInkMuted;
}

D2D1_COLOR_F tone_bg(Tone t) {
  switch (t) {
    case Tone::Neutral:
      return palette::kSubtleSurface;
    case Tone::Crimson:
      return palette::kRoseWash;
    case Tone::Positive:
      return palette::kJadeWash;
    case Tone::Warning:
      return palette::kAmberWash;
  }
  return palette::kSubtleSurface;
}

D2D1_COLOR_F tone_border(Tone t) {
  switch (t) {
    case Tone::Neutral:
      return palette::kHairline;
    case Tone::Crimson:
      return palette::kRoseBorder;
    case Tone::Positive:
      return palette::kJadeBorder;
    case Tone::Warning:
      return palette::kAmberBorder;
  }
  return palette::kHairline;
}

bool point_in_rect(float x, float y, const D2D1_RECT_F& r) {
  return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

// Shared Direct2D + DirectWrite graphics context & widget drawing primitives.
class StudioRenderer {
 public:
  bool init_factories() {
    if (d2d_factory_ == nullptr) {
      if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   d2d_factory_.GetAddressOf()))) {
        return false;
      }
    }
    if (dw_factory_ == nullptr) {
      if (FAILED(DWriteCreateFactory(
              DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
              reinterpret_cast<IUnknown**>(dw_factory_.GetAddressOf())))) {
        return false;
      }
    }
    if (dashed_stroke_ == nullptr) {
      D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
          D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
          D2D1_LINE_JOIN_MITER, 10.0F, D2D1_DASH_STYLE_CUSTOM, 0.0F);
      const float dashes[] = {3.0F, 3.0F};
      d2d_factory_->CreateStrokeStyle(&props, dashes, 2,
                                      dashed_stroke_.GetAddressOf());
    }
    return true;
  }

  ComPtr<ID2D1Factory> d2d_factory_;
  ComPtr<IDWriteFactory> dw_factory_;
  ComPtr<ID2D1StrokeStyle> dashed_stroke_;

  ComPtr<IDWriteTextFormat> make_format(float size, bool mono = false,
                                        bool bold = false,
                                        bool light = false) const {
    ComPtr<IDWriteTextFormat> fmt;
    const wchar_t* family = mono ? L"Consolas" : L"Microsoft YaHei UI";
    const DWRITE_FONT_WEIGHT weight =
        bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD
             : (light ? DWRITE_FONT_WEIGHT_LIGHT : DWRITE_FONT_WEIGHT_NORMAL);
    dw_factory_->CreateTextFormat(family, nullptr, weight,
                                  DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN",
                                  fmt.GetAddressOf());
    if (fmt != nullptr) {
      fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    return fmt;
  }

  D2D1_SIZE_F measure(const std::string& text, IDWriteTextFormat* fmt,
                      float max_w = 10000.0F) const {
    const std::wstring w = utf8_to_wide(text);
    if (w.empty() || fmt == nullptr) {
      return {0.0F, 0.0F};
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dw_factory_->CreateTextLayout(
            w.c_str(), static_cast<UINT32>(w.size()), fmt, max_w, 10000.0F,
            layout.GetAddressOf()))) {
      return {0.0F, 0.0F};
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
  }
};

struct RenderTarget {
  ID2D1HwndRenderTarget* rt{};
  ID2D1SolidColorBrush* brush{};
  StudioRenderer* res{};
  std::vector<HitRegion>* hits{};
  float mouse_x{-1.0F};
  float mouse_y{-1.0F};
  bool mouse_down{false};

  void set_color(D2D1_COLOR_F c) const { brush->SetColor(c); }

  void fill_rect(const D2D1_RECT_F& r, D2D1_COLOR_F c) const {
    set_color(c);
    rt->FillRectangle(r, brush);
  }

  void fill_round(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) const {
    set_color(c);
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
  }

  void stroke_round(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c,
                    float width = 0.85F, bool dashed = false) const {
    set_color(c);
    rt->DrawRoundedRectangle(
        D2D1::RoundedRect(r, radius, radius), brush, width,
        dashed ? res->dashed_stroke_.Get() : nullptr);
  }

  void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c,
            float width = 0.85F, bool dashed = false) const {
    set_color(c);
    rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, width,
                 dashed ? res->dashed_stroke_.Get() : nullptr);
  }

  void draw_text(const std::string& text, const D2D1_RECT_F& r,
                 IDWriteTextFormat* fmt, D2D1_COLOR_F c,
                 DWRITE_TEXT_ALIGNMENT h_align = DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT v_align =
                     DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                 bool ellipsis = false) const {
    if (text.empty() || fmt == nullptr) {
      return;
    }
    const std::wstring w = utf8_to_wide(text);
    fmt->SetTextAlignment(h_align);
    fmt->SetParagraphAlignment(v_align);
    ComPtr<IDWriteTextLayout> layout;
    const float w_lim = std::max(1.0F, r.right - r.left);
    const float h_lim = std::max(1.0F, r.bottom - r.top);
    if (SUCCEEDED(res->dw_factory_->CreateTextLayout(
            w.c_str(), static_cast<UINT32>(w.size()), fmt, w_lim, h_lim,
            layout.GetAddressOf()))) {
      if (ellipsis) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellip;
        res->dw_factory_->CreateEllipsisTrimmingSign(fmt, ellip.GetAddressOf());
        layout->SetTrimming(&trim, ellip.Get());
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      }
      set_color(c);
      rt->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), brush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }

  void add_hit(const D2D1_RECT_F& r, std::function<void()> fn) const {
    if (hits != nullptr && fn) {
      hits->push_back({r, std::move(fn)});
    }
  }

  bool is_hovered(const D2D1_RECT_F& r) const {
    return point_in_rect(mouse_x, mouse_y, r);
  }

  bool is_pressed(const D2D1_RECT_F& r) const {
    return mouse_down && is_hovered(r);
  }
};

void draw_studio_background(const RenderTarget& t, float w, float h) {
  const D2D1_GRADIENT_STOP stops[] = {
      {0.0F, palette::kCanvasTop},
      {0.55F, palette::kCanvasMid},
      {1.0F, palette::kCanvasEdge},
  };
  ComPtr<ID2D1GradientStopCollection> coll;
  if (SUCCEEDED(t.rt->CreateGradientStopCollection(
          stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
          coll.GetAddressOf()))) {
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(t.rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0),
                                                D2D1::Point2F(w, h)),
            coll.Get(), lg.GetAddressOf()))) {
      t.rt->FillRectangle(D2D1::RectF(0, 0, w, h), lg.Get());
      return;
    }
  }
  t.fill_rect(D2D1::RectF(0, 0, w, h), palette::kCanvasMid);
}

void draw_dual_display_glyph(const RenderTarget& t, float x, float y,
                             float size, bool linked, bool streaming) {
  const D2D1_RECT_F box = D2D1::RectF(x, y, x + size, y + size);
  t.fill_round(box, 4.0F,
               streaming ? rgb(0xF7EAEF, 0.65F) : palette::kSubtleSurface);
  t.stroke_round(box, 4.0F,
                 streaming ? palette::kRoseBorder : palette::kHairline, 0.85F);

  const D2D1_RECT_F pc = D2D1::RectF(x + size * 0.17F, y + size * 0.24F,
                                     x + size * 0.69F, y + size * 0.64F);
  t.stroke_round(pc, 1.5F, rgb(0x232026, 0.78F), 1.15F);
  const float mid_x = (pc.left + pc.right) * 0.5F;
  t.line(mid_x, pc.bottom, mid_x, pc.bottom + size * 0.08F, palette::kInkMuted,
         1.0F);
  t.line(mid_x - size * 0.11F, pc.bottom + size * 0.08F,
         mid_x + size * 0.11F, pc.bottom + size * 0.08F, palette::kInkMuted,
         1.0F);

  const D2D1_RECT_F pad = D2D1::RectF(x + size * 0.44F, y + size * 0.41F,
                                      x + size * 0.84F, y + size * 0.75F);
  t.fill_round(pad, 1.5F,
               streaming ? palette::kRoseWash : palette::kCardSurface);
  t.stroke_round(pad, 1.5F,
                 (streaming || linked) ? palette::kCrimson : palette::kInk,
                 (streaming || linked) ? 1.25F : 1.05F);
  t.fill_rect(
      D2D1::RectF(pad.right - 5.5F, pad.top + 3.0F, pad.right - 2.7F,
                  pad.top + 5.8F),
      streaming ? palette::kCrimson
                : (linked ? palette::kChampagne : palette::kInkMuted));
}

void draw_device_glyph(const RenderTarget& t, float x, float y, float size,
                       bool active, bool allowed) {
  const D2D1_RECT_F box = D2D1::RectF(x, y, x + size, y + size);
  t.fill_round(box, 4.0F,
               active ? rgb(0xF7EAEF, 0.7F) : palette::kSubtleSurface);
  t.stroke_round(box, 4.0F,
                 active ? palette::kRoseBorder : palette::kHairline, 0.85F);

  const D2D1_RECT_F frame = D2D1::RectF(x + size * 0.18F, y + size * 0.27F,
                                        x + size * 0.82F, y + size * 0.73F);
  t.fill_round(frame, 2.0F,
               active ? palette::kRoseWash : palette::kCardSurface);
  const D2D1_COLOR_F stroke_col =
      !allowed ? palette::kInkMuted
               : (active ? palette::kCrimson : palette::kInk);
  t.stroke_round(frame, 2.0F, stroke_col, active ? 1.25F : 1.05F);
  const D2D1_RECT_F inner =
      D2D1::RectF(frame.left + 2.4F, frame.top + 2.2F, frame.right - 2.4F,
                  frame.bottom - 2.2F);
  t.stroke_round(inner, 0.8F, palette::kHairline, 0.65F);
  t.fill_rect(D2D1::RectF(inner.right - 3.6F, inner.top + 1.8F,
                          inner.right - 1.4F, inner.top + 4.0F),
              !allowed ? palette::kInkMuted
                       : (active ? palette::kCrimson : palette::kJade));
}

float draw_status_badge(const RenderTarget& t, float x, float y,
                        const std::string& text, Tone tone,
                        IDWriteTextFormat* fmt) {
  const D2D1_SIZE_F sz = t.res->measure(text, fmt);
  const float w = std::ceil(sz.width) + 28.0F;
  const float h = 21.0F;
  const D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
  t.fill_round(r, 2.5F, tone_bg(tone));
  t.stroke_round(r, 2.5F, tone_border(tone), 0.8F);
  t.fill_rect(D2D1::RectF(x + 9.0F, y + 8.0F, x + 14.0F, y + 13.0F),
              tone_dot(tone));
  t.draw_text(text, D2D1::RectF(x + 19.0F, y, x + w - 6.0F, y + h), fmt,
              tone_fg(tone), DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
  return w;
}

void draw_section_header(const RenderTarget& t, float x, float y, float width,
                         const std::string& english,
                         const std::string& chinese,
                         IDWriteTextFormat* mono_fmt,
                         IDWriteTextFormat* zh_fmt) {
  const float en_w = std::ceil(t.res->measure(english, mono_fmt).width);
  const float zh_w = std::ceil(t.res->measure(chinese, zh_fmt).width);
  t.draw_text(english, D2D1::RectF(x, y, x + en_w + 4.0F, y + 16.0F), mono_fmt,
              palette::kCrimson, DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  t.draw_text("/", D2D1::RectF(x + en_w + 5.0F, y, x + en_w + 15.0F, y + 16.0F),
              zh_fmt, palette::kHairline, DWRITE_TEXT_ALIGNMENT_CENTER,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  t.draw_text(chinese,
              D2D1::RectF(x + en_w + 16.0F, y, x + en_w + 20.0F + zh_w, y + 16.0F),
              zh_fmt, palette::kInk, DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  const float line_x = x + en_w + zh_w + 26.0F;
  if (line_x < x + width) {
    t.line(line_x, y + 8.0F, x + width, y + 8.0F, palette::kHairline, 0.8F);
  }
}

void draw_hairline_toggle(const RenderTarget& t, float x, float y, bool is_on,
                          IDWriteTextFormat* mono_fmt,
                          std::function<void()> on_click) {
  const float total_w = 86.0F;
  const float total_h = 20.0F;
  const D2D1_RECT_F hit_r = D2D1::RectF(x, y, x + total_w, y + total_h);
  const float track_y = y + 9.0F;
  t.fill_rect(D2D1::RectF(x, track_y, x + 48.0F, track_y + 2.0F),
              is_on ? palette::kCrimson : palette::kSoftHairline);
  const float knob_x = is_on ? (x + 38.0F) : x;
  const D2D1_RECT_F knob = D2D1::RectF(knob_x, y + 5.0F, knob_x + 10.0F, y + 15.0F);
  t.fill_round(knob, 1.5F, is_on ? palette::kCrimson : palette::kCardSurface);
  t.stroke_round(knob, 1.5F,
                 is_on ? palette::kCrimsonPressed : palette::kInkMuted, 0.9F);
  t.draw_text(is_on ? "开启" : "关闭",
              D2D1::RectF(x + 54.0F, y, x + total_w, y + total_h), mono_fmt,
              is_on ? palette::kCrimson : palette::kInkMuted,
              DWRITE_TEXT_ALIGNMENT_TRAILING,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  t.add_hit(hit_r, std::move(on_click));
}

float draw_primary_button(const RenderTarget& t, float x, float y,
                          const std::string& label, IDWriteTextFormat* fmt,
                          std::function<void()> on_click, float fixed_w = 0.0F) {
  const float w =
      fixed_w > 0.0F ? fixed_w : (std::ceil(t.res->measure(label, fmt).width) + 30.0F);
  const float h = 30.0F;
  const D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
  const bool pressed = t.is_pressed(r);
  const bool hovered = t.is_hovered(r);
  t.fill_round(r, 3.0F,
               pressed ? palette::kCrimsonPressed
                       : (hovered ? rgb(0xAA1F46) : palette::kCrimson));
  t.fill_rect(D2D1::RectF(x + 1.0F, y, x + w - 1.0F, y + 1.0F),
              rgb(0xB9975B, 0.7F));
  t.stroke_round(r, 3.0F, palette::kCrimsonPressed, 0.8F);
  t.draw_text(label, r, fmt, palette::kWhite, DWRITE_TEXT_ALIGNMENT_CENTER,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
  t.add_hit(r, std::move(on_click));
  return w;
}

float draw_secondary_button(const RenderTarget& t, float x, float y,
                            const std::string& label, IDWriteTextFormat* fmt,
                            std::function<void()> on_click,
                            float fixed_w = 0.0F) {
  const float w =
      fixed_w > 0.0F ? fixed_w : (std::ceil(t.res->measure(label, fmt).width) + 22.0F);
  const float h = 28.0F;
  const D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
  const bool pressed = t.is_pressed(r);
  const bool hovered = t.is_hovered(r);
  t.fill_round(r, 3.0F,
               (pressed || hovered) ? palette::kSubtleSurface
                                    : palette::kCardSurface);
  t.stroke_round(r, 3.0F, palette::kHairline, 0.85F);
  t.draw_text(label, r, fmt, palette::kInk, DWRITE_TEXT_ALIGNMENT_CENTER,
              DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
  t.add_hit(r, std::move(on_click));
  return w;
}

void draw_segment_option(const RenderTarget& t, const D2D1_RECT_F& r,
                         const std::string& label, bool active,
                         IDWriteTextFormat* fmt,
                         std::function<void()> on_click) {
  const bool hovered = t.is_hovered(r);
  t.fill_round(r, 3.0F,
               active ? palette::kRoseWash
                      : (hovered ? palette::kSubtleSurface
                                 : palette::kCardSurface));
  t.stroke_round(r, 3.0F, active ? palette::kRoseBorder : palette::kHairline,
                 0.8F);
  const float cy = (r.top + r.bottom) * 0.5F;
  const float cx = (r.left + r.right) * 0.5F;
  const float label_w = t.res->measure(label, fmt).width;
  const float start_x = std::max(r.left + 8.0F, cx - (label_w + 10.0F) * 0.5F);
  const D2D1_RECT_F sq =
      D2D1::RectF(start_x, cy - 2.0F, start_x + 4.0F, cy + 2.0F);
  if (active) {
    t.fill_rect(sq, palette::kCrimson);
  } else {
    t.stroke_round(sq, 0.0F, palette::kInkMuted, 0.8F);
  }
  t.draw_text(label, D2D1::RectF(start_x + 8.0F, r.top, r.right - 4.0F, r.bottom),
              fmt, active ? palette::kCrimson : palette::kInkSecondary,
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
              true);
  t.add_hit(r, std::move(on_click));
}

// Dynamic GDI tray icon matching macOS MenuBarIcon.
HICON create_tray_hicon(bool connected, bool streaming) {
  const int size = 32;
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(BITMAPV5HEADER);
  bi.bV5Width = size;
  bi.bV5Height = -size;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;

  HDC screen = GetDC(nullptr);
  void* bits = nullptr;
  HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi),
                                   DIB_RGB_COLORS, &bits, nullptr, 0);
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  if (color == nullptr || mask == nullptr || bits == nullptr) {
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    ReleaseDC(nullptr, screen);
    return nullptr;
  }
  std::memset(bits, 0, static_cast<std::size_t>(size) * size * 4);
  auto* px = static_cast<std::uint32_t*>(bits);

  auto put = [&](int x, int y, std::uint32_t argb) {
    if (x >= 0 && x < size && y >= 0 && y < size) {
      px[y * size + x] = argb;
    }
  };
  auto stroke_box = [&](int x0, int y0, int x1, int y1, std::uint32_t argb,
                        int thickness = 2) {
    for (int t = 0; t < thickness; ++t) {
      for (int x = x0; x <= x1; ++x) {
        put(x, y0 + t, argb);
        put(x, y1 - t, argb);
      }
      for (int y = y0; y <= y1; ++y) {
        put(x0 + t, y, argb);
        put(x1 - t, y, argb);
      }
    }
  };
  auto fill_box = [&](int x0, int y0, int x1, int y1, std::uint32_t argb) {
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        put(x, y, argb);
      }
    }
  };

  const std::uint32_t ink = 0xFF232026;
  const std::uint32_t crimson = 0xFF9D193D;
  const std::uint32_t jade = 0xFF2D7D5B;
  const std::uint32_t white = 0xFFFAF7F2;

  // Primary display
  fill_box(3, 4, 21, 18, white);
  stroke_box(3, 4, 21, 18, ink, 2);
  fill_box(11, 19, 13, 22, ink);
  fill_box(7, 22, 17, 24, ink);

  // Secondary display overlapping bottom-right
  fill_box(13, 12, 29, 27, white);
  stroke_box(13, 12, 29, 27, streaming ? crimson : ink, 2);
  if (streaming) {
    fill_box(17, 16, 25, 23, crimson);
  } else if (connected) {
    fill_box(19, 18, 23, 21, jade);
  }

  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmMask = mask;
  ii.hbmColor = color;
  HICON icon = CreateIconIndirect(&ii);

  DeleteObject(color);
  DeleteObject(mask);
  ReleaseDC(nullptr, screen);
  return icon;
}

class StudioWindowController {
 public:
  explicit StudioWindowController(HostController& controller)
      : controller_(controller) {}

  ~StudioWindowController() { cleanup(); }

  int run() {
    if (!res_.init_factories()) {
      return 1;
    }
    init_fonts();
    state_ = controller_.snapshot_ui_state();

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    app_icon_big_ = static_cast<HICON>(LoadImageW(
        hinst, MAKEINTRESOURCEW(IDI_DESKMATE_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    app_icon_small_ = static_cast<HICON>(LoadImageW(
        hinst, MAKEINTRESOURCEW(IDI_DESKMATE_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    if (app_icon_big_ == nullptr) {
      app_icon_big_ = create_tray_hicon(false, false);
    }
    if (app_icon_small_ == nullptr) {
      app_icon_small_ = create_tray_hicon(false, false);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &StudioWindowController::main_wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon_big_;
    wc.hIconSm = app_icon_small_;
    wc.lpszClassName = L"DeskMateStudioMainWindow";
    RegisterClassExW(&wc);

    WNDCLASSEXW pwc{};
    pwc.cbSize = sizeof(pwc);
    pwc.style = CS_HREDRAW | CS_VREDRAW;
    pwc.lpfnWndProc = &StudioWindowController::popover_wnd_proc;
    pwc.hInstance = hinst;
    pwc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pwc.lpszClassName = L"DeskMateStudioTrayPopover";
    RegisterClassExW(&pwc);

    const float scale = initial_dpi_scale();
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int avail_w = std::max(720, static_cast<int>(work.right - work.left) - 24);
    const int avail_h = std::max(520, static_cast<int>(work.bottom - work.top) - 24);
    const int win_w =
        std::min(avail_w, static_cast<int>(std::lround(1020.0F * scale)));
    const int win_h =
        std::min(avail_h, static_cast<int>(std::lround(680.0F * scale)));
    const int pos_x = work.left + ((work.right - work.left) - win_w) / 2;
    const int pos_y = work.top + ((work.bottom - work.top) - win_h) / 2;

    main_hwnd_ = CreateWindowExW(
        0, wc.lpszClassName, L"DeskMate // 桌面副屏工作台",
        WS_OVERLAPPEDWINDOW, pos_x, pos_y, win_w, win_h, nullptr, nullptr,
        wc.hInstance, this);
    if (main_hwnd_ == nullptr) {
      return 1;
    }
    if (app_icon_big_ != nullptr) {
      SendMessageW(main_hwnd_, WM_SETICON, ICON_BIG,
                   reinterpret_cast<LPARAM>(app_icon_big_));
    }
    if (app_icon_small_ != nullptr) {
      SendMessageW(main_hwnd_, WM_SETICON, ICON_SMALL,
                   reinterpret_cast<LPARAM>(app_icon_small_));
    }

    const BOOL dark_false = FALSE;
    DwmSetWindowAttribute(main_hwnd_, kDwmUseImmersiveDarkMode, &dark_false,
                          sizeof(dark_false));
    WTSRegisterSessionNotification(main_hwnd_, NOTIFY_FOR_THIS_SESSION);

    popover_hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, pwc.lpszClassName, L"DeskMate Tray",
        WS_POPUP, 0, 0, static_cast<int>(std::lround(332.0F * scale)),
        static_cast<int>(std::lround(420.0F * scale)), nullptr, nullptr,
        pwc.hInstance, this);
    if (popover_hwnd_ != nullptr) {
      const DWORD round_pref = kDwmCornerRound;
      DwmSetWindowAttribute(popover_hwnd_, kDwmWindowCornerPreference,
                            &round_pref, sizeof(round_pref));
    }

    install_tray_icon();
    ShowWindow(main_hwnd_, SW_SHOWNORMAL);
    UpdateWindow(main_hwnd_);
    SetTimer(main_hwnd_, kTimerRefreshId, 250, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (rename_edit_ != nullptr && msg.hwnd == rename_edit_ &&
          msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_RETURN) {
          commit_rename();
          continue;
        }
        if (msg.wParam == VK_ESCAPE) {
          cancel_rename();
          continue;
        }
      }
      if (mirror_edit_ != nullptr && msg.hwnd == mirror_edit_ &&
          msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_RETURN) {
          commit_mirror_edit();
          continue;
        }
        if (msg.wParam == VK_ESCAPE) {
          cancel_mirror_edit();
          continue;
        }
      }
      if (msg.message == WM_KEYDOWN && msg.wParam == 'Q' &&
          (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        request_quit();
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
  }

 private:
  void init_fonts() {
    f_mono_9_ = res_.make_format(9.5F, true, true);
    f_mono_10_ = res_.make_format(10.5F, true, false);
    f_mono_11_ = res_.make_format(11.0F, true, false);
    f_mono_11_b_ = res_.make_format(11.0F, true, true);
    f_mono_20_ = res_.make_format(20.0F, true, true);
    f_ui_10_ = res_.make_format(10.0F, false, false);
    f_ui_10_b_ = res_.make_format(10.0F, false, true);
    f_ui_11_ = res_.make_format(11.0F, false, false);
    f_ui_11_b_ = res_.make_format(11.0F, false, true);
    f_ui_12_ = res_.make_format(12.0F, false, false);
    f_ui_12_b_ = res_.make_format(12.5F, false, true);
    f_ui_14_b_ = res_.make_format(14.5F, false, true);
    f_ui_20_l_ = res_.make_format(20.5F, false, false, true);
  }

  float initial_dpi_scale() const {
    const UINT dpi = GetDpiForSystem();
    return static_cast<float>(dpi > 0 ? dpi : 96U) / 96.0F;
  }

  float dpi_scale(HWND hwnd) const {
    const UINT dpi = GetDpiForWindow(hwnd);
    return static_cast<float>(dpi > 0 ? dpi : 96U) / 96.0F;
  }

  void cleanup() {
    remove_tray_icon();
    if (tray_hicon_ != nullptr) {
      DestroyIcon(tray_hicon_);
      tray_hicon_ = nullptr;
    }
    if (app_icon_big_ != nullptr) {
      DestroyIcon(app_icon_big_);
      app_icon_big_ = nullptr;
    }
    if (app_icon_small_ != nullptr) {
      DestroyIcon(app_icon_small_);
      app_icon_small_ = nullptr;
    }
    if (main_hwnd_ != nullptr) {
      WTSUnRegisterSessionNotification(main_hwnd_);
    }
    main_brush_.Reset();
    main_rt_.Reset();
    pop_brush_.Reset();
    pop_rt_.Reset();
  }

  void install_tray_icon() {
    if (tray_added_) {
      return;
    }
    tray_hicon_ = create_tray_hicon(false, false);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = main_hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kMsgTrayIcon;
    nid.hIcon = tray_hicon_;
    wcsncpy_s(nid.szTip, L"DeskMate - 桌面副屏", _TRUNCATE);
    tray_added_ = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
  }

  void update_tray_icon() {
    const bool streaming =
        state_.badge == "会话已建立" ||
        state_.badge.find("运行中") != std::string::npos || state_.live_fps > 0;
    const bool connected = state_.linked;
    if (!tray_added_ ||
        (connected == last_tray_linked_ && streaming == last_tray_streaming_)) {
      return;
    }
    last_tray_linked_ = connected;
    last_tray_streaming_ = streaming;
    HICON next = create_tray_hicon(connected, streaming);
    if (next == nullptr) {
      return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = main_hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = next;
    const std::wstring tip =
        utf8_to_wide("DeskMate · " + state_.badge);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (tray_hicon_ != nullptr) {
      DestroyIcon(tray_hicon_);
    }
    tray_hicon_ = next;
  }

  void remove_tray_icon() {
    if (!tray_added_) {
      return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = main_hwnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    tray_added_ = false;
  }

  void show_main_window() {
    if (main_hwnd_ == nullptr) {
      return;
    }
    ShowWindow(main_hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(main_hwnd_);
    update_refresh_timer();
  }

  void toggle_tray_popover() {
    if (popover_hwnd_ == nullptr) {
      return;
    }
    if (IsWindowVisible(popover_hwnd_)) {
      ShowWindow(popover_hwnd_, SW_HIDE);
      update_refresh_timer();
      return;
    }
    state_ = controller_.snapshot_ui_state();
    const float s = dpi_scale(popover_hwnd_);
    const float dip_h = compute_popover_height();
    const int pw = static_cast<int>(std::lround(332.0F * s));
    const int ph = static_cast<int>(std::lround(dip_h * s));

    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;

    int x = std::clamp<int>(pt.x - pw / 2, wa.left + 8,
                            std::max<int>(wa.left + 8, wa.right - pw - 8));
    int y = wa.bottom - ph - 8;
    if (pt.y < wa.top + 60) {
      y = wa.top + 8;
    }
    SetWindowPos(popover_hwnd_, HWND_TOPMOST, x, y, pw, ph, SWP_SHOWWINDOW);
    SetForegroundWindow(popover_hwnd_);
    InvalidateRect(popover_hwnd_, nullptr, FALSE);
    update_refresh_timer();
  }

  void update_refresh_timer() {
    if (main_hwnd_ == nullptr) {
      return;
    }
    const bool visible = IsWindowVisible(main_hwnd_) ||
                         (popover_hwnd_ != nullptr &&
                          IsWindowVisible(popover_hwnd_));
    SetTimer(main_hwnd_, kTimerRefreshId, visible ? 250U : 1000U, nullptr);
  }

  void show_tray_context_menu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
      return;
    }
    AppendMenuW(menu, MF_STRING, kCmdTrayShow, L"打开主窗口");
    AppendMenuW(menu, MF_STRING, kCmdTrayToggleService,
                state_.service_running ? L"停止串流服务" : L"启动串流服务");
    AppendMenuW(menu, MF_STRING, kCmdTrayCopyStatus, L"复制诊断状态");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdTrayQuit, L"退出 DeskMate");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(main_hwnd_);
    const UINT cmd = TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, main_hwnd_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
      case kCmdTrayShow:
        show_main_window();
        break;
      case kCmdTrayToggleService:
        controller_.set_service_running(!state_.service_running);
        refresh_now();
        break;
      case kCmdTrayCopyStatus:
        copy_diagnostic();
        break;
      case kCmdTrayQuit:
        request_quit();
        break;
      default:
        break;
    }
  }

  void show_touch_mode_menu(HWND anchor_hwnd, float dip_x, float dip_y) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
      return;
    }
    const TouchInputMode modes[] = {
        TouchInputMode::NativeTouch,
        TouchInputMode::VirtualTrackpad,
        TouchInputMode::CompatibleGestures,
        TouchInputMode::SingleTouchOnly,
    };
    for (int i = 0; i < 4; ++i) {
      const UINT flags =
          MF_STRING | (state_.touch_input_mode == modes[i] ? MF_CHECKED : 0);
      const std::wstring w = utf8_to_wide(touch_mode_display_name(modes[i]));
      AppendMenuW(menu, flags, kCmdTouchModeBase + i, w.c_str());
    }
    const float s = dpi_scale(anchor_hwnd);
    POINT pt{static_cast<LONG>(std::lround(dip_x * s)),
             static_cast<LONG>(std::lround(dip_y * s))};
    ClientToScreen(anchor_hwnd, &pt);
    SetForegroundWindow(anchor_hwnd);
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD, pt.x, pt.y,
                                      anchor_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd >= kCmdTouchModeBase && cmd < kCmdTouchModeBase + 4) {
      controller_.set_touch_input_mode(modes[cmd - kCmdTouchModeBase]);
      refresh_now();
    }
  }

  void show_device_menu(const PairedDevice& dev, float dip_x, float dip_y) {
    HMENU menu = CreatePopupMenu();
    HMENU pos_sub = CreatePopupMenu();
    if (menu == nullptr || pos_sub == nullptr) {
      if (menu) DestroyMenu(menu);
      if (pos_sub) DestroyMenu(pos_sub);
      return;
    }
    AppendMenuW(pos_sub,
                MF_STRING | (!dev.preferred_position.has_value() ? MF_CHECKED : 0),
                kCmdDevPosAuto, L"自动分配空闲侧");
    AppendMenuW(pos_sub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(pos_sub,
                MF_STRING | (dev.preferred_position == DisplayPosition::Right
                                 ? MF_CHECKED
                                 : 0),
                kCmdDevPosRight, L"主屏右侧");
    AppendMenuW(pos_sub,
                MF_STRING | (dev.preferred_position == DisplayPosition::Left
                                 ? MF_CHECKED
                                 : 0),
                kCmdDevPosLeft, L"主屏左侧");
    AppendMenuW(pos_sub,
                MF_STRING | (dev.preferred_position == DisplayPosition::Top
                                 ? MF_CHECKED
                                 : 0),
                kCmdDevPosTop, L"主屏上方");
    AppendMenuW(pos_sub,
                MF_STRING | (dev.preferred_position == DisplayPosition::Bottom
                                 ? MF_CHECKED
                                 : 0),
                kCmdDevPosBottom, L"主屏下方");

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pos_sub),
                L"默认扩展方位");

    std::uint64_t active_link = 0;
    for (const auto& s : state_.active_sessions) {
      if (s.device_id == dev.id) {
        active_link = s.link_id;
        break;
      }
    }
    if (active_link != 0) {
      AppendMenuW(menu, MF_STRING, kCmdDevDisconnect, L"断开当前连接");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdDevRename, L"重命名设备");
    AppendMenuW(menu, MF_STRING, kCmdDevCopyId, L"复制设备标识");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdDevRemove, L"移除设备");

    const float s = dpi_scale(main_hwnd_);
    POINT pt{static_cast<LONG>(std::lround(dip_x * s)),
             static_cast<LONG>(std::lround(dip_y * s))};
    ClientToScreen(main_hwnd_, &pt);
    SetForegroundWindow(main_hwnd_);
    const UINT cmd =
        TrackPopupMenuEx(menu, TPM_RETURNCMD, pt.x, pt.y, main_hwnd_, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
      case kCmdDevPosAuto:
        controller_.set_device_preferred_position(dev.id, std::nullopt);
        refresh_now();
        break;
      case kCmdDevPosRight:
        controller_.set_device_preferred_position(dev.id,
                                                  DisplayPosition::Right);
        refresh_now();
        break;
      case kCmdDevPosLeft:
        controller_.set_device_preferred_position(dev.id, DisplayPosition::Left);
        refresh_now();
        break;
      case kCmdDevPosTop:
        controller_.set_device_preferred_position(dev.id, DisplayPosition::Top);
        refresh_now();
        break;
      case kCmdDevPosBottom:
        controller_.set_device_preferred_position(dev.id,
                                                  DisplayPosition::Bottom);
        refresh_now();
        break;
      case kCmdDevDisconnect:
        if (active_link != 0) {
          controller_.disconnect_link(active_link);
          refresh_now();
        }
        break;
      case kCmdDevRename:
        begin_rename(dev.id, dev.display_name());
        break;
      case kCmdDevCopyId:
        copy_to_clipboard(main_hwnd_, dev.id);
        break;
      case kCmdDevRemove:
        if (renaming_device_id_ == dev.id) {
          cancel_rename();
        }
        controller_.remove_device(dev.id);
        refresh_now();
        break;
      default:
        break;
    }
  }

  void begin_rename(const std::string& id, const std::string& initial) {
    renaming_device_id_ = id;
    if (rename_edit_ == nullptr) {
      rename_edit_ = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", utf8_to_wide(initial).c_str(),
          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 100, 24, main_hwnd_,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditRenameId)),
          GetModuleHandleW(nullptr), nullptr);
      HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Microsoft YaHei UI");
      if (font != nullptr) {
        SendMessageW(rename_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                     TRUE);
      }
    } else {
      SetWindowTextW(rename_edit_, utf8_to_wide(initial).c_str());
      ShowWindow(rename_edit_, SW_SHOW);
    }
    SetFocus(rename_edit_);
    SendMessageW(rename_edit_, EM_SETSEL, 0, -1);
    InvalidateRect(main_hwnd_, nullptr, FALSE);
  }

  void commit_rename() {
    if (renaming_device_id_.empty() || rename_edit_ == nullptr) {
      return;
    }
    wchar_t buf[256]{};
    GetWindowTextW(rename_edit_, buf, 256);
    controller_.rename_device(renaming_device_id_, wide_to_utf8(buf));
    cancel_rename();
    refresh_now();
  }

  void cancel_rename() {
    renaming_device_id_.clear();
    if (rename_edit_ != nullptr) {
      ShowWindow(rename_edit_, SW_HIDE);
    }
    InvalidateRect(main_hwnd_, nullptr, FALSE);
  }

  void begin_mirror_edit(const std::string& initial) {
    editing_mirror_ = true;
    if (mirror_edit_ == nullptr) {
      mirror_edit_ = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", utf8_to_wide(initial).c_str(),
          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 180, 24, main_hwnd_,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditMirrorId)),
          GetModuleHandleW(nullptr), nullptr);
      HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Consolas");
      if (font != nullptr) {
        SendMessageW(mirror_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                     TRUE);
      }
    } else {
      SetWindowTextW(mirror_edit_, utf8_to_wide(initial).c_str());
      ShowWindow(mirror_edit_, SW_SHOW);
    }
    SetFocus(mirror_edit_);
    SendMessageW(mirror_edit_, EM_SETSEL, 0, -1);
    InvalidateRect(main_hwnd_, nullptr, FALSE);
  }

  void commit_mirror_edit() {
    if (!editing_mirror_ || mirror_edit_ == nullptr) {
      return;
    }
    wchar_t buf[512]{};
    GetWindowTextW(mirror_edit_, buf, 512);
    controller_.set_update_mirror_prefix(wide_to_utf8(buf));
    cancel_mirror_edit();
    refresh_now();
  }

  void cancel_mirror_edit() {
    editing_mirror_ = false;
    if (mirror_edit_ != nullptr) {
      ShowWindow(mirror_edit_, SW_HIDE);
    }
    InvalidateRect(main_hwnd_, nullptr, FALSE);
  }

  void copy_diagnostic() {
    copy_to_clipboard(main_hwnd_, controller_.diagnostic_text());
    copied_feedback_ = true;
    SetTimer(main_hwnd_, kTimerCopyResetId, 1800, nullptr);
    InvalidateRect(main_hwnd_, nullptr, FALSE);
  }

  void refresh_now() {
    state_ = controller_.snapshot_ui_state();
    update_tray_icon();
    if (main_hwnd_ != nullptr) {
      InvalidateRect(main_hwnd_, nullptr, FALSE);
    }
    if (popover_hwnd_ != nullptr && IsWindowVisible(popover_hwnd_)) {
      InvalidateRect(popover_hwnd_, nullptr, FALSE);
    }
  }

  void request_quit() {
    quitting_ = true;
    KillTimer(main_hwnd_, kTimerRefreshId);
    KillTimer(main_hwnd_, kTimerCopyResetId);
    remove_tray_icon();
    if (popover_hwnd_ != nullptr) {
      DestroyWindow(popover_hwnd_);
      popover_hwnd_ = nullptr;
    }
    if (main_hwnd_ != nullptr) {
      DestroyWindow(main_hwnd_);
      main_hwnd_ = nullptr;
    }
    PostQuitMessage(0);
  }

  bool ensure_main_rt() {
    if (main_rt_ != nullptr) {
      return true;
    }
    RECT rc{};
    GetClientRect(main_hwnd_, &rc);
    const D2D1_SIZE_U size =
        D2D1::SizeU(std::max<LONG>(1, rc.right - rc.left),
                    std::max<LONG>(1, rc.bottom - rc.top));
    const UINT dpi = GetDpiForWindow(main_hwnd_);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = static_cast<float>(dpi > 0 ? dpi : 96U);
    props.dpiY = static_cast<float>(dpi > 0 ? dpi : 96U);
    if (FAILED(res_.d2d_factory_->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(main_hwnd_, size),
            main_rt_.GetAddressOf()))) {
      return false;
    }
    main_rt_->CreateSolidColorBrush(palette::kInk, main_brush_.GetAddressOf());
    return main_brush_ != nullptr;
  }

  bool ensure_pop_rt() {
    if (pop_rt_ != nullptr) {
      return true;
    }
    RECT rc{};
    GetClientRect(popover_hwnd_, &rc);
    const D2D1_SIZE_U size =
        D2D1::SizeU(std::max<LONG>(1, rc.right - rc.left),
                    std::max<LONG>(1, rc.bottom - rc.top));
    const UINT dpi = GetDpiForWindow(popover_hwnd_);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = static_cast<float>(dpi > 0 ? dpi : 96U);
    props.dpiY = static_cast<float>(dpi > 0 ? dpi : 96U);
    if (FAILED(res_.d2d_factory_->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(popover_hwnd_, size),
            pop_rt_.GetAddressOf()))) {
      return false;
    }
    pop_rt_->CreateSolidColorBrush(palette::kInk, pop_brush_.GetAddressOf());
    return pop_brush_ != nullptr;
  }

  void paint_main() {
    if (!ensure_main_rt()) {
      return;
    }
    const D2D1_SIZE_F sz = main_rt_->GetSize();
    main_hits_.clear();
    RenderTarget t{main_rt_.Get(), main_brush_.Get(), &res_, &main_hits_,
                   main_mouse_x_, main_mouse_y_, main_mouse_down_};

    main_rt_->BeginDraw();
    draw_studio_background(t, sz.width, sz.height);

    // 1. Top Navigation Bar (58 DIPs)
    const float top_h = 58.0F;
    t.fill_rect(D2D1::RectF(0, 0, sz.width, top_h), rgb(0xFFFFFF, 0.78F));
    const bool streaming =
        state_.badge == "会话已建立" ||
        state_.badge.find("运行中") != std::string::npos ||
        state_.live_fps > 0;
    draw_dual_display_glyph(t, 24.0F, 12.0F, 34.0F, state_.linked, streaming);
    t.draw_text("DESKMATE // USB HOST",
                D2D1::RectF(70.0F, 11.0F, 310.0F, 24.0F), f_mono_9_.Get(),
                palette::kInkMuted);
    t.draw_text("桌面副屏工作台", D2D1::RectF(70.0F, 24.0F, 200.0F, 47.0F),
                f_ui_14_b_.Get(), palette::kInk, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    draw_status_badge(t, 198.0F, 25.0F, state_.badge,
                      tone_for_badge(state_.badge), f_ui_11_b_.Get());

    // 4 Top Tabs
    struct TabItem {
      UiTab tab;
      const char* label;
    };
    const TabItem tabs[] = {
        {UiTab::Display, "DISPLAY / 概览"},
        {UiTab::Devices, "DEVICES / 设备"},
        {UiTab::Settings, "SETTINGS / 设置"},
        {UiTab::Diagnostics, "LOGS / 诊断"},
    };
    float tab_x = sz.width - 24.0F;
    for (int i = 3; i >= 0; --i) {
      const float label_w =
          std::ceil(res_.measure(tabs[i].label, f_ui_11_b_.Get()).width) + 14.0F;
      tab_x -= label_w;
      const D2D1_RECT_F tr = D2D1::RectF(tab_x, 14.0F, tab_x + label_w, 46.0F);
      const bool active = (tab_ == tabs[i].tab);
      if (active) {
        t.fill_rect(D2D1::RectF(tab_x, 27.0F, tab_x + 4.0F, 31.0F),
                    palette::kCrimson);
        t.fill_rect(D2D1::RectF(tab_x, 42.0F, tab_x + label_w, 44.0F),
                    palette::kCrimson);
      }
      t.draw_text(tabs[i].label,
                  D2D1::RectF(tab_x + 8.0F, 14.0F, tab_x + label_w, 42.0F),
                  active ? f_ui_11_b_.Get() : f_ui_11_.Get(),
                  active ? palette::kCrimson : palette::kInkMuted,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      const UiTab next_tab = tabs[i].tab;
      t.add_hit(tr, [this, next_tab] {
        if (next_tab != tab_) {
          cancel_rename();
          cancel_mirror_edit();
          tab_ = next_tab;
          right_scroll_y_ = 0.0F;
          InvalidateRect(main_hwnd_, nullptr, FALSE);
        }
      });
      tab_x -= 14.0F;
    }

    t.line(0, top_h, sz.width, top_h, palette::kHairline, 0.85F);

    // 2. Bottom Whisper Bar (30 DIPs)
    const float whisper_y = sz.height - 30.0F;
    t.line(0, whisper_y, sz.width, whisper_y, palette::kHairline, 0.85F);
    t.fill_rect(D2D1::RectF(0, whisper_y + 0.8F, sz.width, sz.height),
                rgb(0xFFFFFF, 0.85F));
    const std::string left_whisper =
        "USB-C AOA 直连  //  " +
        (state_.auto_virtual_display ? std::string("自动匹配扩展屏")
                                     : state_.source);
    t.draw_text(left_whisper,
                D2D1::RectF(24.0F, whisper_y, sz.width - 280.0F, sz.height),
                f_mono_10_.Get(), palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    if (!state_.stream_summary.empty()) {
      t.draw_text(state_.stream_summary,
                  D2D1::RectF(sz.width - 280.0F, whisper_y, sz.width - 104.0F,
                              sz.height),
                  f_mono_10_.Get(), palette::kInkSecondary,
                  DWRITE_TEXT_ALIGNMENT_TRAILING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    }
    const D2D1_RECT_F copy_btn_r =
        D2D1::RectF(sz.width - 96.0F, whisper_y + 4.0F, sz.width - 20.0F,
                    sz.height - 4.0F);
    t.draw_text(copied_feedback_ ? "✓ 已复制" : "复制摘要", copy_btn_r,
                f_ui_10_b_.Get(),
                copied_feedback_ ? palette::kCrimson : palette::kInkSecondary,
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    t.add_hit(copy_btn_r, [this] { copy_diagnostic(); });

    // 3. Left Topology Column (368 DIPs)
    const float body_top = top_h + 12.0F;
    const float body_bottom = whisper_y - 10.0F;
    draw_left_topology_column(t, 20.0F, body_top, 368.0F);

    // 4. Right Content Column (scrollable + clipped)
    const float right_x = 20.0F + 368.0F + 20.0F;
    const float right_w = std::max(300.0F, sz.width - right_x - 20.0F);
    const D2D1_RECT_F right_clip =
        D2D1::RectF(right_x, body_top, right_x + right_w, body_bottom);
    right_viewport_rect_ = right_clip;
    main_rt_->PushAxisAlignedClip(right_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    mirror_box_valid_ = false;
    float content_end_y = body_top - right_scroll_y_;
    switch (tab_) {
      case UiTab::Display:
        content_end_y =
            draw_overview_tab(t, right_x, content_end_y, right_w - 6.0F);
        break;
      case UiTab::Devices:
        content_end_y =
            draw_devices_tab(t, right_x, content_end_y, right_w - 6.0F);
        break;
      case UiTab::Settings:
        content_end_y =
            draw_settings_tab(t, right_x, content_end_y, right_w - 6.0F);
        break;
      case UiTab::Diagnostics:
        content_end_y =
            draw_diagnostics_tab(t, right_x, content_end_y, right_w - 6.0F);
        break;
    }

    const float total_content_h =
        std::max(0.0F, content_end_y - (body_top - right_scroll_y_));
    const float view_h = std::max(1.0F, body_bottom - body_top);
    right_max_scroll_ = std::max(0.0F, total_content_h - view_h + 12.0F);
    if (right_scroll_y_ > right_max_scroll_) {
      right_scroll_y_ = right_max_scroll_;
    }

    if (right_max_scroll_ > 0.0F) {
      const float bar_h = std::max(28.0F, view_h * (view_h / (view_h + right_max_scroll_)));
      const float bar_y =
          body_top + (view_h - bar_h) * (right_scroll_y_ / right_max_scroll_);
      t.fill_round(D2D1::RectF(right_x + right_w - 3.5F, bar_y,
                               right_x + right_w, bar_y + bar_h),
                   1.5F, rgb(0x8C858E, 0.35F));
    }

    main_rt_->PopAxisAlignedClip();

    // Update native EDIT control position during device rename.
    if (rename_edit_ != nullptr && !renaming_device_id_.empty()) {
      if (tab_ == UiTab::Devices && rename_box_valid_) {
        const float s = dpi_scale(main_hwnd_);
        MoveWindow(rename_edit_,
                   static_cast<int>(std::lround(rename_box_rect_.left * s)),
                   static_cast<int>(std::lround(rename_box_rect_.top * s)),
                   static_cast<int>(std::lround(
                       (rename_box_rect_.right - rename_box_rect_.left) * s)),
                   static_cast<int>(std::lround(
                       (rename_box_rect_.bottom - rename_box_rect_.top) * s)),
                   TRUE);
        ShowWindow(rename_edit_, SW_SHOW);
      } else {
        ShowWindow(rename_edit_, SW_HIDE);
      }
    }

    if (mirror_edit_ != nullptr && editing_mirror_) {
      const bool in_bounds =
          mirror_box_valid_ && mirror_box_rect_.top >= body_top &&
          mirror_box_rect_.bottom <= body_bottom;
      if (tab_ == UiTab::Settings && in_bounds) {
        const float s = dpi_scale(main_hwnd_);
        MoveWindow(mirror_edit_,
                   static_cast<int>(std::lround(mirror_box_rect_.left * s)),
                   static_cast<int>(std::lround(mirror_box_rect_.top * s)),
                   static_cast<int>(std::lround(
                       (mirror_box_rect_.right - mirror_box_rect_.left) * s)),
                   static_cast<int>(std::lround(
                       (mirror_box_rect_.bottom - mirror_box_rect_.top) * s)),
                   TRUE);
        ShowWindow(mirror_edit_, SW_SHOW);
      } else {
        ShowWindow(mirror_edit_, SW_HIDE);
      }
    }

    const HRESULT hr = main_rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
      main_brush_.Reset();
      main_rt_.Reset();
    }
  }

  std::string topology_role_label(const TopologyDisplayItem& item) const {
    if (item.is_ghost_preview) return "副屏位置预览";
    if (item.is_deskmate_virtual) return "DeskMate 虚拟屏";
    if (item.is_primary) return "WIN 主屏";
    if (item.is_virtual) return "虚拟显示器";
    return "物理扩展屏";
  }

  void draw_left_topology_column(const RenderTarget& t, float x, float y,
                                 float w) {
    const bool streaming =
        state_.badge == "会话已建立" ||
        state_.badge.find("运行中") != std::string::npos ||
        state_.live_fps > 0;
    const ActiveSessionView* selected = nullptr;
    for (const auto& s : state_.active_sessions) {
      if (s.link_id == state_.selected_link_id) {
        selected = &s;
        break;
      }
    }
    if (selected == nullptr && !state_.active_sessions.empty()) {
      selected = &state_.active_sessions.front();
    }

    // Build base display items (all physical monitors + DeskMate virtual displays)
    std::vector<TopologyDisplayItem> base_items = state_.topology_displays;
    if (base_items.empty()) {
      TopologyDisplayItem fallback;
      fallback.id = 1;
      fallback.index_number = 1;
      fallback.name = "WIN 主屏";
      fallback.x = 0.0;
      fallback.y = 0.0;
      fallback.width = 1920.0;
      fallback.height = 1080.0;
      fallback.pixel_width = 1920;
      fallback.pixel_height = 1080;
      fallback.refresh_hz = 60;
      fallback.is_primary = true;
      base_items.push_back(std::move(fallback));
    }
    bool has_dm = false;
    for (const auto& item : base_items) {
      if (item.is_deskmate_virtual) {
        has_dm = true;
        break;
      }
    }
    if (state_.active_sessions.empty() && state_.auto_virtual_display &&
        !has_dm) {
      const TopologyDisplayItem* primary = &base_items.front();
      double bb_min_x = primary->x;
      double bb_min_y = primary->y;
      double bb_max_x = primary->max_x();
      double bb_max_y = primary->max_y();
      for (const auto& d : base_items) {
        if (d.is_primary) {
          primary = &d;
        }
        bb_min_x = std::min(bb_min_x, d.x);
        bb_min_y = std::min(bb_min_y, d.y);
        bb_max_x = std::max(bb_max_x, d.max_x());
        bb_max_y = std::max(bb_max_y, d.max_y());
      }
      const double gw = std::max(900.0, std::round(primary->width * 0.72));
      const double gh = std::max(600.0, std::round(primary->height * 0.72));
      double gx = bb_max_x;
      double gy = primary->y + (primary->height - gh) * 0.5;
      switch (state_.position) {
        case DisplayPosition::Right:
          gx = bb_max_x;
          gy = primary->y + (primary->height - gh) * 0.5;
          break;
        case DisplayPosition::Left:
          gx = bb_min_x - gw;
          gy = primary->y + (primary->height - gh) * 0.5;
          break;
        case DisplayPosition::Top:
          gx = primary->x + (primary->width - gw) * 0.5;
          gy = bb_min_y - gh;
          break;
        case DisplayPosition::Bottom:
          gx = primary->x + (primary->width - gw) * 0.5;
          gy = bb_max_y;
          break;
      }
      TopologyDisplayItem ghost;
      ghost.id = kGhostPreviewDisplayId;
      ghost.index_number = static_cast<int>(base_items.size() + 1);
      ghost.name = "副屏预览";
      ghost.x = gx;
      ghost.y = gy;
      ghost.width = gw;
      ghost.height = gh;
      ghost.is_virtual = true;
      ghost.is_deskmate_virtual = true;
      ghost.is_ghost_preview = true;
      base_items.push_back(std::move(ghost));
    }

    topo_base_items_ = base_items;
    const std::vector<TopologyDisplayItem>& items =
        (topo_dragging_ && !topo_preview_.empty()) ? topo_preview_ : base_items;
    const std::vector<TopologyDisplayItem>& vp_src =
        (topo_dragging_ && !topo_drag_start_.empty()) ? topo_drag_start_
                                                      : items;

    int physical_count = 0;
    for (const auto& it : base_items) {
      if (!it.is_ghost_preview) ++physical_count;
    }

    const TopologyDisplayItem* active_sel = nullptr;
    if (selected != nullptr) {
      for (const auto& it : items) {
        if (it.link_id == selected->link_id) {
          active_sel = &it;
          break;
        }
      }
    }
    if (active_sel == nullptr && topo_selected_display_id_ != 0) {
      for (const auto& it : items) {
        if (it.id == topo_selected_display_id_) {
          active_sel = &it;
          break;
        }
      }
    }
    if (active_sel == nullptr) {
      for (const auto& it : items) {
        if (it.is_deskmate_virtual) {
          active_sel = &it;
          break;
        }
      }
    }
    if (active_sel == nullptr && !items.empty()) {
      active_sel = &items.front();
    }

    const float footer_h = (streaming && state_.live_fps > 0) ? 84.0F : 66.0F;
    const float canvas_h = 248.0F;
    const float card_h = 33.0F + canvas_h + footer_h;
    const D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + card_h);
    t.fill_round(card, 4.0F, palette::kCardSurface);

    // Top spec header
    t.fill_rect(D2D1::RectF(x + 14.0F, y + 14.0F, x + 19.0F, y + 19.0F),
                streaming ? palette::kCrimson
                          : (state_.linked ? palette::kChampagne
                                           : palette::kInkMuted));
    t.draw_text("TOPOLOGY // 多屏拓扑布局",
                D2D1::RectF(x + 25.0F, y + 8.0F, x + w - 108.0F, y + 25.0F),
                f_mono_9_.Get(), palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);

    const std::string count_pill = std::to_string(physical_count) + " 块屏幕";
    const float pill_w =
        std::ceil(res_.measure(count_pill, f_ui_10_.Get()).width) + 14.0F;
    const D2D1_RECT_F ref_btn =
        D2D1::RectF(x + w - 34.0F, y + 7.5F, x + w - 14.0F, y + 25.5F);
    const D2D1_RECT_F pill_r =
        D2D1::RectF(ref_btn.left - 6.0F - pill_w, y + 7.5F, ref_btn.left - 6.0F,
                    y + 25.5F);
    t.fill_round(pill_r, 2.0F, palette::kSubtleSurface);
    t.stroke_round(pill_r, 2.0F, palette::kHairline, 0.7F);
    t.draw_text(count_pill, pill_r, f_ui_10_.Get(), palette::kInkSecondary,
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    t.fill_round(ref_btn, 2.0F,
                 t.is_hovered(ref_btn) ? palette::kRoseWash
                                       : palette::kSubtleSurface);
    t.stroke_round(ref_btn, 2.0F, palette::kHairline, 0.7F);
    t.draw_text("↻", ref_btn, f_ui_11_b_.Get(), palette::kInkSecondary,
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    t.add_hit(ref_btn, [this] {
      controller_.refresh_displays();
      refresh_now();
    });
    t.line(x, y + 33.0F, x + w, y + 33.0F, palette::kSoftHairline, 0.8F);

    // Interactive Proportional Multi-Display Canvas (248 DIPs)
    const float canvas_y = y + 33.0F;
    const D2D1_RECT_F canvas_rect =
        D2D1::RectF(x + 0.5F, canvas_y, x + w - 0.5F, canvas_y + canvas_h);
    topo_canvas_rect_ = canvas_rect;
    t.fill_rect(canvas_rect, palette::kCanvasMid);

    // Subtle 16-DIP dot grid
    for (float gx = x + 12.0F; gx < x + w - 8.0F; gx += 16.0F) {
      for (float gy = canvas_y + 12.0F; gy < canvas_y + canvas_h - 8.0F;
           gy += 16.0F) {
        t.fill_rect(D2D1::RectF(gx - 0.6F, gy - 0.6F, gx + 0.6F, gy + 0.6F),
                    rgb(0xDCD6CC, 0.48F));
      }
    }

    // Corner registration ticks
    const float ins = 8.0F;
    const float tk = 5.0F;
    t.line(x + ins, canvas_y + ins, x + ins + tk, canvas_y + ins,
           palette::kHairline);
    t.line(x + ins, canvas_y + ins, x + ins, canvas_y + ins + tk,
           palette::kHairline);
    t.line(x + w - ins - tk, canvas_y + ins, x + w - ins, canvas_y + ins,
           palette::kHairline);
    t.line(x + w - ins, canvas_y + ins, x + w - ins, canvas_y + ins + tk,
           palette::kHairline);
    t.line(x + ins, canvas_y + canvas_h - ins, x + ins + tk,
           canvas_y + canvas_h - ins, palette::kHairline);
    t.line(x + ins, canvas_y + canvas_h - ins - tk, x + ins,
           canvas_y + canvas_h - ins, palette::kHairline);
    t.line(x + w - ins - tk, canvas_y + canvas_h - ins, x + w - ins,
           canvas_y + canvas_h - ins, palette::kHairline);
    t.line(x + w - ins, canvas_y + canvas_h - ins - tk, x + w - ins,
           canvas_y + canvas_h - ins, palette::kHairline);

    const TopologyViewport vp =
        compute_topology_viewport(vp_src, w, canvas_h, 20.0);
    topo_viewport_ = vp;

    main_rt_->PushAxisAlignedClip(canvas_rect,
                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Alignment guides while dragging
    if (topo_dragging_) {
      const D2D1_COLOR_F gcol = topo_swap_preview_
                                    ? palette::kChampagne
                                    : rgb(0x9D193D, 0.72F);
      for (const auto& g : topo_guides_) {
        if (g.is_horizontal) {
          const float gy = canvas_y + static_cast<float>(vp.to_canvas_y(g.pos));
          t.line(x + static_cast<float>(vp.to_canvas_x(g.start)), gy,
                 x + static_cast<float>(vp.to_canvas_x(g.end)), gy, gcol, 1.0F,
                 true);
        } else {
          const float gx = x + static_cast<float>(vp.to_canvas_x(g.pos));
          t.line(gx, canvas_y + static_cast<float>(vp.to_canvas_y(g.start)), gx,
                 canvas_y + static_cast<float>(vp.to_canvas_y(g.end)), gcol,
                 1.0F, true);
        }
      }
    }

    // Draw non-dragged tiles first, dragged tile on top
    topo_tile_rects_.clear();
    std::vector<const TopologyDisplayItem*> draw_order;
    draw_order.reserve(items.size());
    const TopologyDisplayItem* dragged_ptr = nullptr;
    for (const auto& it : items) {
      if (topo_dragging_ && it.id == topo_drag_candidate_id_) {
        dragged_ptr = &it;
      } else {
        draw_order.push_back(&it);
      }
    }
    if (dragged_ptr != nullptr) {
      draw_order.push_back(dragged_ptr);
    }

    for (const auto* it : draw_order) {
      const float tw = std::max(32.0F, static_cast<float>(it->width * vp.scale));
      const float th =
          std::max(24.0F, static_cast<float>(it->height * vp.scale));
      const float tx = x + static_cast<float>(vp.to_canvas_x(it->x));
      const float ty = canvas_y + static_cast<float>(vp.to_canvas_y(it->y));
      const D2D1_RECT_F tr = D2D1::RectF(tx, ty, tx + tw, ty + th);
      topo_tile_rects_.push_back({it->id, tr});

      const bool is_dragged =
          (topo_dragging_ && it->id == topo_drag_candidate_id_);
      const bool is_sel = (active_sel != nullptr && active_sel->id == it->id);

      const D2D1_COLOR_F fill =
          it->is_ghost_preview
              ? rgb(0xFFFFFF, 0.72F)
              : (it->is_deskmate_virtual
                     ? (it->streaming ? palette::kRoseWash
                                      : palette::kSubtleSurface)
                     : (it->is_primary ? palette::kCardSurface
                                       : palette::kSubtleSurface));
      const D2D1_COLOR_F border =
          is_dragged
              ? palette::kCrimson
              : (it->is_ghost_preview
                     ? palette::kHairline
                     : (it->is_deskmate_virtual
                            ? ((is_sel || it->streaming) ? palette::kCrimson
                                                         : palette::kRoseBorder)
                            : (is_sel ? palette::kInk
                                      : (it->is_primary ? rgb(0x232026, 0.78F)
                                                        : palette::kChampagne))));
      const float bw = is_dragged ? 1.6F : (is_sel ? 1.35F : 0.95F);

      t.fill_round(tr, 3.0F, fill);
      t.stroke_round(tr, 3.0F, border, bw, it->is_ghost_preview);

      if (it->is_primary && th > 16.0F) {
        t.fill_rect(
            D2D1::RectF(tr.left + 1.0F, tr.top + 1.0F, tr.right - 1.0F,
                        tr.top + 5.0F),
            palette::kSoftHairline);
      }

      const float top_pad = it->is_primary ? 7.0F : 4.0F;
      const std::string num_str = std::to_string(it->index_number);
      const float badge_w =
          std::max(14.0F, std::ceil(res_.measure(num_str, f_mono_9_.Get()).width) +
                              6.0F);
      const D2D1_RECT_F nbadge =
          D2D1::RectF(tr.left + 4.0F, tr.top + top_pad,
                      tr.left + 4.0F + badge_w, tr.top + top_pad + 13.0F);
      const D2D1_COLOR_F nbg =
          it->is_deskmate_virtual
              ? palette::kCrimson
              : (it->is_primary ? palette::kInk : palette::kInkSecondary);
      t.fill_round(nbadge, 2.0F, nbg);
      t.draw_text(num_str, nbadge, f_mono_9_.Get(), palette::kWhite,
                  DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

      if (tw >= 54.0F) {
        t.draw_text(it->name,
                    D2D1::RectF(nbadge.right + 4.0F, tr.top + top_pad - 1.0F,
                                tr.right - 4.0F, tr.top + top_pad + 14.0F),
                    f_ui_10_b_.Get(),
                    it->is_ghost_preview ? palette::kInkMuted : palette::kInk,
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
      }

      if (th >= 38.0F) {
        std::string sub;
        if (it->is_ghost_preview) {
          sub = "拖动设定方位";
        } else if (it->streaming && it->live_fps > 0) {
          sub = std::to_string(it->live_fps) + " FPS";
        } else if (it->is_deskmate_virtual) {
          sub = "DeskMate 副屏";
        } else if (it->is_primary) {
          sub = "主显示器";
        } else {
          sub = std::to_string(it->pixel_width) + "×" +
                std::to_string(it->pixel_height);
        }
        t.draw_text(sub,
                    D2D1::RectF(tr.left + 4.0F, tr.bottom - 16.0F,
                                tr.right - 4.0F, tr.bottom - 3.0F),
                    f_mono_9_.Get(),
                    (it->is_deskmate_virtual && !it->is_ghost_preview)
                        ? palette::kCrimson
                        : palette::kInkMuted,
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
      }
    }

    main_rt_->PopAxisAlignedClip();

    // Footer inside topology card
    const float fy = canvas_y + canvas_h;
    t.line(x, fy, x + w, fy, palette::kSoftHairline, 0.8F);

    if (active_sel != nullptr) {
      const std::string left_lbl = "#" +
                                   std::to_string(active_sel->index_number) +
                                   " " + topology_role_label(*active_sel);
      t.draw_text(left_lbl,
                  D2D1::RectF(x + 14.0F, fy + 7.0F, x + 150.0F, fy + 24.0F),
                  f_ui_10_b_.Get(), palette::kInkMuted,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
      t.draw_text(active_sel->name,
                  D2D1::RectF(x + 150.0F, fy + 7.0F, x + w - 14.0F, fy + 24.0F),
                  f_ui_11_b_.Get(),
                  active_sel->is_deskmate_virtual ? palette::kCrimson
                                                  : palette::kInk,
                  DWRITE_TEXT_ALIGNMENT_TRAILING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);

      const std::string coord_txt =
          "坐标 (" + std::to_string(static_cast<int>(std::llround(active_sel->x))) +
          ", " + std::to_string(static_cast<int>(std::llround(active_sel->y))) +
          ")";
      const std::string spec_txt =
          active_sel->is_ghost_preview
              ? (!state_.stream_summary.empty()
                     ? state_.stream_summary
                     : (state_.auto_virtual_display ? "按设备比例自动匹配"
                                                    : state_.source))
              : (std::to_string(active_sel->pixel_width) + "×" +
                 std::to_string(active_sel->pixel_height) + " @ " +
                 std::to_string(active_sel->refresh_hz) + "Hz");
      t.draw_text(coord_txt,
                  D2D1::RectF(x + 14.0F, fy + 25.0F, x + 155.0F, fy + 42.0F),
                  f_mono_10_.Get(), palette::kInkMuted,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
      t.draw_text(spec_txt,
                  D2D1::RectF(x + 155.0F, fy + 25.0F, x + w - 14.0F, fy + 42.0F),
                  f_mono_10_.Get(), palette::kInkSecondary,
                  DWRITE_TEXT_ALIGNMENT_TRAILING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    }

    float hint_y = fy + 45.0F;
    if (streaming && state_.live_fps > 0) {
      char perf[64]{};
      const std::string rate_txt = format_bitrate_mbps(state_.live_bitrate_mbps);
      if (!rate_txt.empty()) {
        std::snprintf(perf, sizeof(perf), "%d FPS · %s Mbps", state_.live_fps,
                      rate_txt.c_str());
      } else {
        std::snprintf(perf, sizeof(perf), "%d FPS", state_.live_fps);
      }
      t.draw_text(state_.active_sessions.size() > 1 ? "总输出" : "实时输出",
                  D2D1::RectF(x + 14.0F, fy + 44.0F, x + 90.0F, fy + 61.0F),
                  f_ui_11_.Get(), palette::kInkMuted);
      t.draw_text(perf,
                  D2D1::RectF(x + 90.0F, fy + 44.0F, x + w - 14.0F, fy + 61.0F),
                  f_mono_11_b_.Get(), palette::kCrimson,
                  DWRITE_TEXT_ALIGNMENT_TRAILING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
      hint_y = fy + 63.0F;
    }

    t.draw_text(
        topo_swap_preview_ ? "松手将交换两块屏幕的排列顺序"
                           : "拖动屏幕可调整排列位置与顺序，松手自动吸附生效",
        D2D1::RectF(x + 14.0F, hint_y, x + w - 14.0F, hint_y + 16.0F),
        f_ui_10_.Get(),
        topo_swap_preview_ ? palette::kCrimson : palette::kInkMuted,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);

    t.stroke_round(card, 4.0F, palette::kHairline, 0.85F);

    // Compact runtime summary strip below topology
    const float my = y + card_h + 9.0F;
    const float row_h = 28.0F;
    const D2D1_RECT_F meta_card =
        D2D1::RectF(x, my, x + w, my + row_h * 3.0F);
    t.fill_round(meta_card, 4.0F, palette::kCardSurface);

    std::string usb_text = "等待设备接入";
    if (!state_.service_running) {
      usb_text = "服务已停止";
    } else if (!state_.blocked_device_name.empty() &&
               state_.active_sessions.empty()) {
      usb_text = state_.blocked_device_name + " · 已拦截";
    } else if (state_.active_sessions.size() > 1) {
      usb_text =
          "已接入 " + std::to_string(state_.active_sessions.size()) + " 台设备";
    } else if (state_.linked) {
      usb_text = state_.connected_device_name.empty()
                     ? "AOA 直连已就绪"
                     : (state_.connected_device_name + " · AOA 直连");
    }

    auto meta_row = [&](int idx, const char* label, const std::string& val,
                        bool highlight) {
      const float ry = my + idx * row_h;
      if (idx > 0) {
        t.line(x, ry, x + w, ry, palette::kSoftHairline, 0.8F);
      }
      t.draw_text(label, D2D1::RectF(x + 12.0F, ry, x + 90.0F, ry + row_h),
                  f_ui_11_.Get(), palette::kInkMuted,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      t.draw_text(val,
                  D2D1::RectF(x + 90.0F, ry, x + w - 12.0F, ry + row_h),
                  highlight ? f_ui_11_b_.Get() : f_ui_11_.Get(),
                  highlight ? palette::kInk : palette::kInkSecondary,
                  DWRITE_TEXT_ALIGNMENT_TRAILING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    };
    meta_row(0, "USB 通道", usb_text, state_.linked);
    meta_row(1, "画面管线", state_.video, streaming);
    meta_row(2, "触控通道", state_.input, state_.input_enabled);

    t.stroke_round(meta_card, 4.0F, palette::kHairline, 0.85F);
  }

  float draw_overview_tab(const RenderTarget& t, float x, float y, float w) {
    const bool streaming =
        state_.badge == "会话已建立" ||
        state_.badge.find("运行中") != std::string::npos ||
        state_.live_fps > 0;

    t.draw_text("SESSION // 运行状态", D2D1::RectF(x, y, x + w, y + 15.0F),
                f_mono_9_.Get(), palette::kCrimson);
    y += 16.0F;
    t.draw_text(state_.title, D2D1::RectF(x, y, x + w, y + 28.0F),
                f_ui_20_l_.Get(), palette::kInk, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    y += 28.0F;
    const float g_h =
        std::max(18.0F, std::ceil(res_.measure(state_.guidance, f_ui_12_.Get(), w).height));
    t.draw_text(state_.guidance, D2D1::RectF(x, y, x + w, y + g_h + 4.0F),
                f_ui_12_.Get(), palette::kInkSecondary);
    y += g_h + 10.0F;

    // Contextual callout card
    const bool stopped = !state_.service_running;
    const bool has_blocked =
        !state_.blocked_device_name.empty() && !state_.linked;
    if (stopped) {
      const D2D1_RECT_F c = D2D1::RectF(x, y, x + w, y + 58.0F);
      t.fill_round(c, 4.0F, palette::kSubtleSurface);
      t.stroke_round(c, 4.0F, palette::kHairline, 0.85F);
      t.draw_text("串流服务当前处于停止状态",
                  D2D1::RectF(x + 14.0F, y + 10.0F, x + w - 140.0F, y + 28.0F),
                  f_ui_12_b_.Get(), palette::kInk);
      t.draw_text("启动服务后将监听 USB 接入并按偏好创建画面通道。",
                  D2D1::RectF(x + 14.0F, y + 30.0F, x + w - 140.0F, y + 48.0F),
                  f_ui_11_.Get(), palette::kInkSecondary,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
      draw_primary_button(t, x + w - 122.0F, y + 14.0F, "启动串流服务",
                          f_ui_12_.Get(), [this] {
                            controller_.set_service_running(true);
                            refresh_now();
                          }, 108.0F);
      y += 70.0F;
    } else if (has_blocked) {
      const D2D1_RECT_F c = D2D1::RectF(x, y, x + w, y + 58.0F);
      t.fill_round(c, 4.0F, palette::kAmberWash);
      t.stroke_round(c, 4.0F, palette::kAmberBorder, 0.85F);
      t.draw_text("已阻止连接：" + state_.blocked_device_name,
                  D2D1::RectF(x + 14.0F, y + 10.0F, x + w - 132.0F, y + 28.0F),
                  f_ui_12_b_.Get(), palette::kInk);
      t.draw_text("该设备已在「已配对设备」中设为禁止连接。",
                  D2D1::RectF(x + 14.0F, y + 30.0F, x + w - 132.0F, y + 48.0F),
                  f_ui_11_.Get(), palette::kInkSecondary);
      draw_secondary_button(t, x + w - 118.0F, y + 15.0F, "前往设备列表",
                            f_ui_11_b_.Get(), [this] {
                              tab_ = UiTab::Devices;
                              right_scroll_y_ = 0.0F;
                              InvalidateRect(main_hwnd_, nullptr, FALSE);
                            }, 104.0F);
      y += 70.0F;
    } else if (state_.driver_missing) {
      const std::string hint =
          !state_.driver_hint.empty()
              ? state_.driver_hint
              : "请用 Zadig 为 Android Accessory (18D1:2D0x) 接口安装 WinUSB 驱动，或安装 UsbDk 后重试。";
      const float hint_h = std::max(
          28.0F,
          std::ceil(res_.measure(hint, f_ui_11_.Get(), std::max(120.0F, w - 224.0F))
                        .height));
      const float card_h = std::max(72.0F, 38.0F + hint_h);
      const D2D1_RECT_F c = D2D1::RectF(x, y, x + w, y + card_h);
      t.fill_round(c, 4.0F, palette::kAmberWash);
      t.stroke_round(c, 4.0F, palette::kAmberBorder, 0.85F);
      t.draw_text("需要安装 WinUSB 驱动以接管 AOA 接口",
                  D2D1::RectF(x + 14.0F, y + 9.0F, x + w - 210.0F, y + 27.0F),
                  f_ui_12_b_.Get(), palette::kInk);
      t.draw_text(
          hint,
          D2D1::RectF(x + 14.0F, y + 29.0F, x + w - 210.0F, y + card_h - 6.0F),
          f_ui_11_.Get(), palette::kInkSecondary);
      const float btn_y = y + (card_h - 28.0F) * 0.5F;
      draw_secondary_button(t, x + w - 196.0F, btn_y, "设备管理器",
                            f_ui_11_b_.Get(),
                            [] { open_shell_target(L"devmgmt.msc"); }, 96.0F);
      draw_primary_button(t, x + w - 92.0F, btn_y - 1.0F, "重试",
                          f_ui_12_.Get(), [this] {
                            controller_.retry();
                            refresh_now();
                          }, 78.0F);
      y += card_h + 12.0F;
    } else if (state_.auto_virtual_display && !state_.vdd_installed) {
      const std::string title = "需要安装虚拟屏驱动以新建独立扩展屏";
      const std::string sub =
          "Windows 需通过 Parsec VDD / MttVDD 驱动新建独立虚拟屏（不会占用您现有的真实显示器）。";
      const float sub_w = std::max(120.0F, w - 252.0F);
      const float sub_h =
          std::max(28.0F, std::ceil(res_.measure(sub, f_ui_11_.Get(), sub_w).height));
      const float card_h = std::max(68.0F, 38.0F + sub_h);
      const D2D1_RECT_F c = D2D1::RectF(x, y, x + w, y + card_h);
      t.fill_round(c, 4.0F, palette::kAmberWash);
      t.stroke_round(c, 4.0F, palette::kAmberBorder, 0.85F);
      t.draw_text(title,
                  D2D1::RectF(x + 14.0F, y + 8.0F, x + w - 246.0F, y + 26.0F),
                  f_ui_12_b_.Get(), palette::kInk, DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
      t.draw_text(
          sub,
          D2D1::RectF(x + 14.0F, y + 28.0F, x + w - 246.0F, y + card_h - 6.0F),
          f_ui_11_.Get(), palette::kInkSecondary);
      const float btn_y = y + (card_h - 26.0F) * 0.5F;
      draw_secondary_button(t, x + w - 238.0F, btn_y, "用物理屏",
                            f_ui_11_.Get(), [this] {
                              controller_.set_auto_virtual_display(false);
                              tab_ = UiTab::Settings;
                              right_scroll_y_ = 0.0F;
                              refresh_now();
                            }, 76.0F);
      draw_secondary_button(t, x + w - 154.0F, btn_y, "重试",
                            f_ui_11_.Get(), [this] {
                              controller_.retry();
                              refresh_now();
                            }, 56.0F);
      draw_primary_button(
          t, x + w - 90.0F, btn_y - 1.0F, "获取驱动", f_ui_11_b_.Get(),
          [] {
            open_shell_target(
                L"https://github.com/nomi-san/parsec-vdd/releases");
          },
          76.0F);
      y += card_h + 10.0F;
    } else if (state_.can_retry) {
      const D2D1_RECT_F c = D2D1::RectF(x, y, x + w, y + 56.0F);
      t.fill_round(c, 4.0F, palette::kSubtleSurface);
      t.stroke_round(c, 4.0F, palette::kHairline, 0.85F);
      t.draw_text("连接维护",
                  D2D1::RectF(x + 14.0F, y + 9.0F, x + w - 120.0F, y + 27.0F),
                  f_ui_12_b_.Get(), palette::kInk);
      t.draw_text("可手动重新握手 USB 通道并同步显示器配置。",
                  D2D1::RectF(x + 14.0F, y + 29.0F, x + w - 120.0F, y + 47.0F),
                  f_ui_11_.Get(), palette::kInkSecondary);
      draw_primary_button(t, x + w - 108.0F, y + 13.0F, "重新连接",
                          f_ui_12_.Get(), [this] {
                            controller_.retry();
                            refresh_now();
                          }, 94.0F);
      y += 68.0F;
    }

    // Update Banner when new version is available or installing
    {
      const auto& u = state_.update;
      const bool update_active =
          (u.phase == UpdatePhase::Available ||
           u.phase == UpdatePhase::Downloading ||
           u.phase == UpdatePhase::Verifying ||
           u.phase == UpdatePhase::Installing);
      if (update_active) {
        const D2D1_RECT_F uc = D2D1::RectF(x, y, x + w, y + 58.0F);
        t.fill_round(uc, 4.0F, palette::kRoseWash);
        t.stroke_round(uc, 4.0F, palette::kRoseBorder, 0.85F);
        const std::string title = u.available_version.empty()
                                      ? "软件更新"
                                      : ("发现新版本 v" + u.available_version);
        t.draw_text(title,
                    D2D1::RectF(x + 14.0F, y + 9.0F, x + w - 196.0F, y + 27.0F),
                    f_ui_12_b_.Get(), palette::kInk);
        t.draw_text(u.status_text,
                    D2D1::RectF(x + 14.0F, y + 30.0F, x + w - 196.0F, y + 48.0F),
                    f_ui_11_.Get(), palette::kInkSecondary,
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
        if (u.phase == UpdatePhase::Available) {
          draw_secondary_button(t, x + w - 186.0F, y + 15.0F, "查看详情",
                                f_ui_11_.Get(), [this] {
                                  tab_ = UiTab::Settings;
                                  right_scroll_y_ = 480.0F;
                                  InvalidateRect(main_hwnd_, nullptr, FALSE);
                                }, 76.0F);
          draw_primary_button(t, x + w - 102.0F, y + 14.0F, "立即更新",
                              f_ui_12_.Get(), [this] {
                                controller_.install_available_update();
                                refresh_now();
                              }, 88.0F);
        } else {
          const int pct = static_cast<int>(std::lround(u.progress * 100.0));
          t.draw_text(std::to_string(pct) + "%",
                      D2D1::RectF(x + w - 76.0F, y + 16.0F, x + w - 14.0F,
                                  y + 40.0F),
                      f_mono_11_b_.Get(), palette::kCrimson,
                      DWRITE_TEXT_ALIGNMENT_TRAILING,
                      DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        y += 68.0F;
      }
    }

    // Active Displays Section
    if (!state_.active_sessions.empty()) {
      draw_section_header(
          t, x, y, w, "ACTIVE DISPLAYS",
          "在线副屏（" + std::to_string(state_.active_sessions.size()) + "）",
          f_mono_9_.Get(), f_ui_11_b_.Get());
      y += 24.0F;
      for (const auto& session : state_.active_sessions) {
        y = draw_active_session_card(t, x, y, w, session) + 8.0F;
      }
      y += 6.0F;
    }

    // Telemetry Bar
    draw_section_header(t, x, y, w, "TELEMETRY", "实时性能",
                        f_mono_9_.Get(), f_ui_11_b_.Get());
    y += 24.0F;
    const D2D1_RECT_F tele_r = D2D1::RectF(x, y, x + w, y + 60.0F);
    t.fill_round(tele_r, 4.0F, palette::kCardSurface);
    const float col_w = w / 3.0F;
    auto metric_cell = [&](int col, const char* title, const std::string& val,
                           const char* unit, bool accent) {
      const float cx = x + col * col_w;
      if (col > 0) {
        t.line(cx, y + 12.0F, cx, y + 48.0F, palette::kSoftHairline, 0.8F);
      }
      t.draw_text(title, D2D1::RectF(cx + 14.0F, y + 9.0F, cx + col_w - 8.0F, y + 23.0F),
                  f_mono_9_.Get(), palette::kInkMuted);
      const float vw = std::ceil(res_.measure(val, f_mono_20_.Get()).width);
      t.draw_text(val, D2D1::RectF(cx + 14.0F, y + 24.0F, cx + 18.0F + vw, y + 50.0F),
                  f_mono_20_.Get(), accent ? palette::kCrimson : palette::kInk);
      t.draw_text(unit,
                  D2D1::RectF(cx + 20.0F + vw, y + 32.0F, cx + col_w - 8.0F, y + 50.0F),
                  f_ui_10_.Get(), palette::kInkMuted);
    };
    const std::string rate_txt = format_bitrate_mbps(state_.live_bitrate_mbps);
    const std::string mbps_str = rate_txt.empty() ? std::string("—") : rate_txt;
    metric_cell(0, "帧率 FPS",
                state_.live_fps > 0 ? std::to_string(state_.live_fps) : "—",
                state_.live_fps > 0 ? "fps" : "待机",
                streaming && state_.live_fps > 0);
    metric_cell(1, "码率 BITRATE", mbps_str,
                !rate_txt.empty() ? "Mbps" : "待机", false);
    metric_cell(2, "反向触控",
                state_.input_enabled ? std::to_string(state_.input_event_count)
                                     : "关闭",
                state_.input_enabled ? "次事件" : "已禁用", false);
    t.stroke_round(tele_r, 4.0F, palette::kHairline, 0.85F);
    y += 70.0F;

    // Quick Controls
    draw_section_header(t, x, y, w, "CONTROLS", "快速控制",
                        f_mono_9_.Get(), f_ui_11_b_.Get());
    y += 22.0F;
    const float ctrl_top = y;
    const float ctrl_h = 54.0F + 54.0F + 78.0F + 72.0F;
    const D2D1_RECT_F ctrl_r =
        D2D1::RectF(x, ctrl_top, x + w, ctrl_top + ctrl_h);
    t.fill_round(ctrl_r, 4.0F, palette::kCardSurface);

    y = draw_toggle_row(t, x, y, w, "视频画面推流",
                        "将扩展显示器或选定桌面画面编码发送至设备",
                        state_.video_enabled, [this] {
                          controller_.set_video_enabled(!state_.video_enabled);
                          refresh_now();
                        });
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);
    y = draw_toggle_row(t, x, y, w, "平板触控回传",
                        "当前模式：" +
                            touch_mode_display_name(state_.touch_input_mode) +
                            " · 可在「设置」中切换",
                        state_.input_enabled, [this] {
                          controller_.set_input_enabled(!state_.input_enabled);
                          refresh_now();
                        });
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);

    t.draw_text(state_.active_sessions.size() > 1 ? "当前选中副屏方位"
                                                  : "虚拟副屏方位",
                D2D1::RectF(x + 14.0F, y + 10.0F, x + 200.0F, y + 28.0F),
                f_ui_12_b_.Get(), palette::kInk);
    t.draw_text("支持点选左侧拓扑图或下方快速切换",
                D2D1::RectF(x + 190.0F, y + 11.0F, x + w - 14.0F, y + 28.0F),
                f_ui_10_.Get(), palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
    y += 33.0F;
    draw_position_segment_bar(t, x + 14.0F, y, w - 28.0F, state_.position,
                              [this](DisplayPosition p) {
                                controller_.set_default_position(p);
                                refresh_now();
                              });
    y += 45.0F;
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);

    t.draw_text("画面帧率",
                D2D1::RectF(x + 14.0F, y + 9.0F, x + 120.0F, y + 27.0F),
                f_ui_12_b_.Get(), palette::kInk);
    t.draw_text(
        fps_status_subtitle(state_.preferred_refresh_hz,
                            state_.negotiated_refresh_hz),
        D2D1::RectF(x + 120.0F, y + 10.0F, x + w - 14.0F, y + 27.0F),
        f_ui_10_.Get(),
        state_.preferred_refresh_hz > 0 ? palette::kCrimson : palette::kInkMuted,
        DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    draw_fps_segment_bar(t, x + 14.0F, y + 32.0F, w - 28.0F,
                         state_.preferred_refresh_hz, [this](int hz) {
                           controller_.set_preferred_refresh_hz(hz);
                           refresh_now();
                         });
    t.stroke_round(ctrl_r, 4.0F, palette::kHairline, 0.85F);
    return ctrl_top + ctrl_h + 12.0F;
  }

  float draw_active_session_card(const RenderTarget& t, float x, float y,
                                 float w, const ActiveSessionView& s) {
    // Two-row card (86 DIPs) so long device names, status badges, 4 position
    // buttons, and the disconnect button never collide regardless of DPI/width.
    const float h = 86.0F;
    const bool is_sel = (state_.selected_link_id == s.link_id);
    const D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
    t.fill_round(card, 4.0F, palette::kCardSurface);
    draw_device_glyph(t, x + 12.0F, y + 10.0F, 28.0F, s.streaming || is_sel,
                      true);

    const float badge_w =
        std::ceil(res_.measure(s.badge, f_ui_10_b_.Get()).width) + 24.0F;
    const float row1_right_limit = x + w - 72.0F;
    const float name_left = x + 48.0F;
    const float name_max_w =
        std::max(60.0F, row1_right_limit - name_left - badge_w - 8.0F);
    const float name_w = std::min(
        name_max_w,
        std::ceil(res_.measure(s.device_name, f_ui_12_b_.Get()).width) + 4.0F);
    t.draw_text(s.device_name,
                D2D1::RectF(name_left, y + 10.0F, name_left + name_w, y + 36.0F),
                f_ui_12_b_.Get(), palette::kInk, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    draw_status_badge(t, name_left + name_w + 6.0F, y + 13.0F, s.badge,
                      tone_for_badge(s.badge), f_ui_10_b_.Get());

    draw_secondary_button(t, x + w - 62.0F, y + 10.0F, "断开",
                          f_ui_11_b_.Get(), [this, lid = s.link_id] {
                            controller_.disconnect_link(lid);
                            refresh_now();
                          }, 50.0F);

    t.line(x + 12.0F, y + 44.0F, x + w - 12.0F, y + 44.0F,
           palette::kSoftHairline, 0.8F);

    std::string sub = s.stream_summary;
    if (s.streaming && s.live_fps > 0) {
      char buf[64]{};
      const std::string rate_txt = format_bitrate_mbps(s.live_bitrate_mbps);
      if (!rate_txt.empty()) {
        std::snprintf(buf, sizeof(buf), "%d FPS · %s Mbps", s.live_fps,
                      rate_txt.c_str());
      } else {
        std::snprintf(buf, sizeof(buf), "%d FPS", s.live_fps);
      }
      if (!sub.empty()) sub += " · ";
      sub += buf;
    }
    if (s.display_id != 0) {
      if (!sub.empty()) sub += " · ";
      sub += "#" + std::to_string(s.display_id);
    }
    t.draw_text(sub.empty() ? s.state : sub,
                D2D1::RectF(x + 12.0F, y + 51.0F, x + w - 186.0F, y + 77.0F),
                f_mono_10_.Get(),
                s.streaming ? palette::kCrimson : palette::kInkSecondary,
                DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);

    // 4 position mini buttons neatly right-aligned on row 2.
    const DisplayPosition poses[] = {DisplayPosition::Right,
                                     DisplayPosition::Left,
                                     DisplayPosition::Top,
                                     DisplayPosition::Bottom};
    float bx = x + w - 176.0F;
    for (DisplayPosition p : poses) {
      const D2D1_RECT_F pr = D2D1::RectF(bx, y + 52.0F, bx + 38.0F, y + 76.0F);
      const bool active = (s.position == p);
      t.fill_round(pr, 2.5F,
                   active ? palette::kRoseWash : palette::kSubtleSurface);
      t.stroke_round(pr, 2.5F,
                     active ? palette::kRoseBorder : palette::kHairline, 0.7F);
      t.draw_text(display_position_short_label(p), pr,
                  active ? f_ui_10_b_.Get() : f_ui_10_.Get(),
                  active ? palette::kCrimson : palette::kInkMuted,
                  DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      const std::uint64_t lid = s.link_id;
      t.add_hit(pr, [this, lid, p] {
        controller_.set_link_position(lid, p);
        refresh_now();
      });
      bx += 42.0F;
    }

    t.stroke_round(card, 4.0F,
                   is_sel ? palette::kRoseBorder : palette::kHairline,
                   is_sel ? 1.05F : 0.85F);
    t.add_hit(D2D1::RectF(x, y, x + w - 184.0F, y + h),
              [this, lid = s.link_id] {
                controller_.select_link(lid);
                refresh_now();
              });
    return y + h;
  }

  float draw_devices_tab(const RenderTarget& t, float x, float y, float w) {
    rename_box_valid_ = false;
    t.draw_text("DEVICES // 客户端设备管理", D2D1::RectF(x, y, x + w, y + 16.0F),
                f_mono_9_.Get(), palette::kCrimson);
    y += 18.0F;
    t.draw_text("已配对设备", D2D1::RectF(x, y, x + w, y + 28.0F),
                f_ui_20_l_.Get(), palette::kInk);
    y += 28.0F;
    t.draw_text(
        "管理允许通过 USB 连接此 Windows 主机的设备；关闭授权将禁止自动接管并立即断开当前会话。",
        D2D1::RectF(x, y, x + w, y + 22.0F), f_ui_11_.Get(),
        palette::kInkSecondary);
    y += 30.0F;

    draw_status_badge(
        t, x, y + 3.0F,
        std::to_string(state_.paired_devices.size()) + " 台设备",
        state_.paired_devices.empty() ? Tone::Neutral : Tone::Positive,
        f_ui_11_b_.Get());
    draw_secondary_button(t, x + w - 72.0F, y, "↻ 刷新", f_ui_11_b_.Get(),
                          [this] {
                            controller_.refresh_devices();
                            refresh_now();
                          }, 72.0F);
    y += 40.0F;

    if (state_.paired_devices.empty()) {
      const D2D1_RECT_F empty = D2D1::RectF(x, y, x + w, y + 76.0F);
      t.fill_round(empty, 4.0F, palette::kCardSurface);
      t.stroke_round(empty, 4.0F, palette::kHairline, 0.85F);
      t.draw_text("暂无已配对的客户端设备",
                  D2D1::RectF(x + 16.0F, y + 14.0F, x + w - 16.0F, y + 32.0F),
                  f_ui_12_b_.Get(), palette::kInk);
      t.draw_text(
          "使用 USB 数据线连接手机或平板并打开 DeskMate 后，设备会自动记录在这里；也可点击右上角「刷新」手动扫描。",
          D2D1::RectF(x + 16.0F, y + 36.0F, x + w - 16.0F, y + 68.0F),
          f_ui_11_.Get(), palette::kInkMuted);
      return y + 84.0F;
    }

    for (const auto& dev : state_.paired_devices) {
      const bool is_connected =
          state_.connected_device_ids.count(dev.id) != 0 ||
          state_.connected_device_id == dev.id;
      const bool is_online =
          is_connected || state_.online_device_ids.count(dev.id) != 0;
      const bool is_renaming = (renaming_device_id_ == dev.id);
      const float card_h =
          (!dev.last_resolution.empty() ? 72.0F : 58.0F) +
          (is_renaming ? 46.0F : 0.0F);
      const D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + card_h);
      t.fill_round(card, 4.0F, palette::kCardSurface);

      draw_device_glyph(t, x + 14.0F, y + 14.0F, 32.0F, is_connected,
                        dev.allowed);
      t.draw_text(dev.display_name(),
                  D2D1::RectF(x + 56.0F, y + 10.0F, x + w - 140.0F, y + 28.0F),
                  f_ui_12_b_.Get(), palette::kInk, DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);

      float bx = x + 56.0F;
      if (is_connected) {
        bx += draw_status_badge(t, bx, y + 30.0F, "当前连接", Tone::Crimson,
                                f_ui_10_b_.Get()) + 8.0F;
      } else if (!dev.allowed) {
        bx += draw_status_badge(t, bx, y + 30.0F, "已禁止连接", Tone::Warning,
                                f_ui_10_b_.Get()) + 8.0F;
      } else if (is_online) {
        bx += draw_status_badge(t, bx, y + 30.0F, "USB 已接入", Tone::Positive,
                                f_ui_10_b_.Get()) + 8.0F;
      } else {
        bx += draw_status_badge(t, bx, y + 30.0F, "允许连接", Tone::Positive,
                                f_ui_10_b_.Get()) + 8.0F;
      }

      if (dev.preferred_position.has_value()) {
        const std::string ptxt =
            "默认" + display_position_short_label(*dev.preferred_position);
        const float pw =
            std::ceil(res_.measure(ptxt, f_ui_10_.Get()).width) + 12.0F;
        const D2D1_RECT_F pr = D2D1::RectF(bx, y + 31.0F, bx + pw, y + 50.0F);
        t.fill_round(pr, 2.0F, palette::kSubtleSurface);
        t.stroke_round(pr, 2.0F, palette::kHairline, 0.7F);
        t.draw_text(ptxt, pr, f_ui_10_.Get(), palette::kInkSecondary,
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        bx += pw + 8.0F;
      }

      t.draw_text("最近连接 " +
                      SettingsStore::relative_time_text(dev.last_connected_epoch_s),
                  D2D1::RectF(bx, y + 31.0F, x + w - 146.0F, y + 50.0F),
                  f_ui_11_.Get(), palette::kInkMuted,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);

      if (!dev.last_resolution.empty()) {
        t.draw_text(dev.last_resolution,
                    D2D1::RectF(x + 56.0F, y + 52.0F, x + w - 146.0F, y + 68.0F),
                    f_mono_10_.Get(), palette::kInkMuted);
      }

      draw_hairline_toggle(
          t, x + w - 134.0F, y + 18.0F, dev.allowed, f_mono_11_.Get(),
          [this, id = dev.id, allow = !dev.allowed] {
            controller_.set_device_allowed(id, allow);
            refresh_now();
          });

      const D2D1_RECT_F menu_r =
          D2D1::RectF(x + w - 38.0F, y + 16.0F, x + w - 14.0F, y + 40.0F);
      t.fill_round(menu_r, 3.0F, palette::kSubtleSurface);
      t.stroke_round(menu_r, 3.0F, palette::kHairline, 0.8F);
      t.draw_text("···", menu_r, f_ui_11_b_.Get(), palette::kInkSecondary,
                  DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      t.add_hit(menu_r, [this, dev, menu_r] {
        show_device_menu(dev, menu_r.left, menu_r.bottom + 2.0F);
      });

      if (is_renaming) {
        const float ry = y + (!dev.last_resolution.empty() ? 72.0F : 58.0F);
        t.line(x, ry, x + w, ry, palette::kSoftHairline, 0.8F);
        t.fill_rect(D2D1::RectF(x + 0.5F, ry, x + w - 0.5F, y + card_h - 0.5F),
                    palette::kSubtleSurface);
        rename_box_rect_ =
            D2D1::RectF(x + 14.0F, ry + 9.0F, x + w - 156.0F, ry + 35.0F);
        rename_box_valid_ = true;
        draw_primary_button(t, x + w - 144.0F, ry + 8.0F, "保存",
                            f_ui_11_b_.Get(), [this] { commit_rename(); }, 62.0F);
        draw_secondary_button(t, x + w - 74.0F, ry + 9.0F, "取消",
                              f_ui_11_b_.Get(), [this] { cancel_rename(); }, 60.0F);
      }

      t.stroke_round(card, 4.0F,
                     is_connected ? palette::kRoseBorder : palette::kHairline,
                     is_connected ? 1.05F : 0.85F);
      y += card_h + 10.0F;
    }
    return y;
  }

  float draw_settings_tab(const RenderTarget& t, float x, float y, float w) {
    t.draw_text("SETTINGS // 显示与控制配置", D2D1::RectF(x, y, x + w, y + 16.0F),
                f_mono_9_.Get(), palette::kCrimson);
    y += 18.0F;
    t.draw_text("显示与启动设置", D2D1::RectF(x, y, x + w, y + 28.0F),
                f_ui_20_l_.Get(), palette::kInk);
    y += 38.0F;

    // 1. STARTUP
    draw_section_header(t, x, y, w, "STARTUP", "启动行为",
                        f_mono_9_.Get(), f_ui_11_b_.Get());
    y += 24.0F;
    const float startup_top = y;
    const float err_h = state_.startup_error.empty() ? 0.0F : 46.0F;
    const float startup_h = 54.0F + err_h + 54.0F + 54.0F;
    t.fill_round(D2D1::RectF(x, startup_top, x + w, startup_top + startup_h),
                 4.0F, palette::kCardSurface);

    y = draw_toggle_row(t, x, y, w, "登录时打开 DeskMate",
                        state_.startup_summary, state_.launch_at_login, [this] {
                          controller_.set_launch_at_login(
                              !state_.launch_at_login);
                          refresh_now();
                        });
    if (!state_.startup_error.empty()) {
      t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);
      t.fill_rect(D2D1::RectF(x + 0.5F, y, x + w - 0.5F, y + err_h),
                  palette::kAmberWash);
      t.draw_text(state_.startup_error,
                  D2D1::RectF(x + 14.0F, y + 10.0F, x + w - 140.0F, y + err_h - 6.0F),
                  f_mono_10_.Get(), palette::kInkSecondary);
      draw_secondary_button(t, x + w - 128.0F, y + 9.0F, "启动应用设置",
                            f_ui_11_.Get(),
                            [this] { controller_.open_startup_settings(); },
                            114.0F);
      y += err_h;
    }
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);
    y = draw_toggle_row(
        t, x, y, w, "打开应用后自动启动服务",
        "与上方开机启动同时开启后，登录 Windows 即可自动提供 USB 扩展串流服务。",
        state_.auto_start_service, [this] {
          controller_.set_auto_start_service(!state_.auto_start_service);
          refresh_now();
        });
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);

    t.draw_text("当前串流服务",
                D2D1::RectF(x + 14.0F, y + 9.0F, x + 120.0F, y + 27.0F),
                f_ui_12_b_.Get(), palette::kInk);
    draw_status_badge(
        t, x + 114.0F, y + 8.0F,
        state_.service_running ? "运行中" : "已停止",
        state_.service_running ? Tone::Positive : Tone::Neutral,
        f_ui_10_b_.Get());
    t.draw_text(
        state_.service_running ? "正在监听 USB 平板/手机连接并管理画面会话"
                               : "服务已停止，不会自动接管 USB 设备",
        D2D1::RectF(x + 14.0F, y + 30.0F, x + w - 110.0F, y + 48.0F),
        f_ui_11_.Get(), palette::kInkMuted, DWRITE_TEXT_ALIGNMENT_LEADING,
        DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    if (state_.service_running) {
      draw_secondary_button(t, x + w - 98.0F, y + 13.0F, "停止服务",
                            f_ui_11_b_.Get(), [this] {
                              controller_.set_service_running(false);
                              refresh_now();
                            }, 84.0F);
    } else {
      draw_primary_button(t, x + w - 98.0F, y + 12.0F, "启动服务",
                          f_ui_12_.Get(), [this] {
                            controller_.set_service_running(true);
                            refresh_now();
                          }, 84.0F);
    }
    t.stroke_round(D2D1::RectF(x, startup_top, x + w, startup_top + startup_h),
                   4.0F, palette::kHairline, 0.85F);
    y = startup_top + startup_h + 18.0F;

    // 2. TOUCH INPUT
    draw_section_header(t, x, y, w, "TOUCH INPUT", "触控输入",
                        f_mono_9_.Get(), f_ui_11_b_.Get());
    y += 24.0F;
    const float touch_top = y;
    const auto gestures = touch_mode_gesture_rows(state_.touch_input_mode);
    const auto guidance = touch_mode_trackpad_guidance(state_.touch_input_mode);
    const bool show_cap_row =
        touch_mode_uses_native_when_available(state_.touch_input_mode) ||
        state_.touch_input_mode == TouchInputMode::VirtualTrackpad;
    float detail_h = 48.0F;
    if (show_cap_row) detail_h += 34.0F;
    if (!gestures.empty()) detail_h += static_cast<float>(gestures.size()) * 22.0F + 6.0F;
    if (!guidance.empty()) detail_h += static_cast<float>(guidance.size()) * 22.0F + 20.0F;

    const float touch_h = 54.0F + 54.0F + detail_h;
    t.fill_round(D2D1::RectF(x, touch_top, x + w, touch_top + touch_h), 4.0F,
                 palette::kCardSurface);
    y = draw_toggle_row(
        t, x, y, w, "平板触控回传",
        "选择 Windows 如何接收客户端触控；支持按场景切换触控模式并实时生效。",
        state_.input_enabled, [this] {
          controller_.set_input_enabled(!state_.input_enabled);
          refresh_now();
        });
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);

    t.draw_text("输入模式",
                D2D1::RectF(x + 14.0F, y + 9.0F, x + 220.0F, y + 27.0F),
                f_ui_12_b_.Get(), palette::kInk);
    t.draw_text("选择单指、多指手势或原生触摸/触控板映射方式",
                D2D1::RectF(x + 14.0F, y + 30.0F, x + w - 210.0F, y + 48.0F),
                f_ui_11_.Get(), palette::kInkMuted);
    const std::string mode_label =
        touch_mode_display_name(state_.touch_input_mode) + "  ▾";
    const float mode_w =
        std::ceil(res_.measure(mode_label, f_ui_11_b_.Get()).width) + 22.0F;
    const D2D1_RECT_F mode_btn =
        D2D1::RectF(x + w - 14.0F - mode_w, y + 12.0F, x + w - 14.0F, y + 42.0F);
    t.fill_round(mode_btn, 3.0F, palette::kSubtleSurface);
    t.stroke_round(mode_btn, 3.0F, palette::kHairline, 0.8F);
    t.draw_text(mode_label, mode_btn, f_ui_11_b_.Get(), palette::kInk,
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    t.add_hit(mode_btn, [this, mode_btn] {
      show_touch_mode_menu(main_hwnd_, mode_btn.left, mode_btn.bottom + 2.0F);
    });
    y += 54.0F;
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);

    float dy = y + 10.0F;
    if (show_cap_row) {
      const bool native_ok = state_.touch_capability.native_touch_available;
      const float bw = draw_status_badge(
          t, x + 14.0F, dy,
          state_.touch_input_mode == TouchInputMode::NativeTouch
              ? (native_ok ? "原生触摸可用" : "兼容手势映射回退")
              : "相对触控板手势模拟",
          native_ok ? Tone::Positive : Tone::Neutral, f_ui_10_b_.Get());
      t.draw_text(state_.touch_capability.active_backend_label,
                  D2D1::RectF(x + 22.0F + bw, dy, x + w - 120.0F, dy + 22.0F),
                  f_mono_10_.Get(), palette::kInkMuted,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
      if (state_.touch_input_mode == TouchInputMode::VirtualTrackpad) {
        draw_secondary_button(
            t, x + w - 108.0F, dy - 3.0F, "触控板设置", f_ui_11_.Get(),
            [] { open_shell_target(L"ms-settings:devices-touchpad"); }, 94.0F);
      }
      dy += 32.0F;
    }

    t.draw_text(touch_mode_summary_text(state_.touch_input_mode),
                D2D1::RectF(x + 14.0F, dy, x + w - 14.0F, dy + 34.0F),
                f_ui_11_.Get(), palette::kInkSecondary);
    dy += 34.0F;

    for (const auto& row : gestures) {
      t.draw_text(row.fingers,
                  D2D1::RectF(x + 14.0F, dy, x + 98.0F, dy + 20.0F),
                  f_ui_11_b_.Get(), palette::kInk);
      t.draw_text(row.description,
                  D2D1::RectF(x + 104.0F, dy, x + w - 14.0F, dy + 20.0F),
                  f_ui_11_.Get(), palette::kInkSecondary);
      dy += 22.0F;
    }
    if (!guidance.empty()) {
      const float box_h = static_cast<float>(guidance.size()) * 20.0F + 16.0F;
      const D2D1_RECT_F gbox =
          D2D1::RectF(x + 14.0F, dy, x + w - 14.0F, dy + box_h);
      t.fill_round(gbox, 3.0F, palette::kSubtleSurface);
      t.stroke_round(gbox, 3.0F, palette::kSoftHairline, 0.8F);
      float gy = dy + 8.0F;
      for (const auto& line : guidance) {
        t.draw_text(line, D2D1::RectF(x + 24.0F, gy, x + w - 24.0F, gy + 20.0F),
                    f_ui_11_.Get(), palette::kInkMuted,
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
        gy += 20.0F;
      }
    }
    t.stroke_round(D2D1::RectF(x, touch_top, x + w, touch_top + touch_h), 4.0F,
                   palette::kHairline, 0.85F);
    y = touch_top + touch_h + 18.0F;

    // 3. VIRTUAL DISPLAY & SOURCE
    draw_section_header(t, x, y, w, "VIRTUAL DISPLAY", "虚拟显示器与输入源",
                        f_mono_9_.Get(), f_ui_11_b_.Get());
    y += 24.0F;
    const float disp_top = y;
    const float choices_h =
        (!state_.auto_virtual_display && !state_.choices.empty())
            ? (36.0F + static_cast<float>(state_.choices.size()) * 36.0F)
            : 0.0F;
    const float disp_h = 54.0F + 72.0F + 72.0F + choices_h + 48.0F;
    t.fill_round(D2D1::RectF(x, disp_top, x + w, disp_top + disp_h), 4.0F,
                 palette::kCardSurface);
    y = draw_toggle_row(
        t, x, y, w, "自动匹配设备分辨率与扩展屏",
        "连接时按设备协商分辨率编码，并优先使用未占用的扩展显示器",
        state_.auto_virtual_display, [this] {
          controller_.set_auto_virtual_display(!state_.auto_virtual_display);
          refresh_now();
        });
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);

    t.draw_text("默认扩展位置",
                D2D1::RectF(x + 14.0F, y + 9.0F, x + 160.0F, y + 27.0F),
                f_ui_12_b_.Get(), palette::kInk);
    t.draw_text("相对 Windows 主屏的对齐方向",
                D2D1::RectF(x + 170.0F, y + 10.0F, x + w - 14.0F, y + 27.0F),
                f_ui_10_.Get(), palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_position_segment_bar(t, x + 14.0F, y + 32.0F, w - 28.0F,
                              state_.position, [this](DisplayPosition p) {
                                controller_.set_default_position(p);
                                refresh_now();
                              });
    y += 72.0F;
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);

    t.draw_text("画面帧率",
                D2D1::RectF(x + 14.0F, y + 9.0F, x + 120.0F, y + 27.0F),
                f_ui_12_b_.Get(), palette::kInk);
    t.draw_text(
        fps_status_subtitle(state_.preferred_refresh_hz,
                            state_.negotiated_refresh_hz),
        D2D1::RectF(x + 120.0F, y + 10.0F, x + w - 14.0F, y + 27.0F),
        f_ui_10_.Get(),
        state_.preferred_refresh_hz > 0 ? palette::kCrimson : palette::kInkMuted,
        DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    draw_fps_segment_bar(t, x + 14.0F, y + 32.0F, w - 28.0F,
                         state_.preferred_refresh_hz, [this](int hz) {
                           controller_.set_preferred_refresh_hz(hz);
                           refresh_now();
                         });
    y += 72.0F;

    if (!state_.auto_virtual_display && !state_.choices.empty()) {
      t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);
      t.draw_text("选择物理 / 虚拟显示器",
                  D2D1::RectF(x + 14.0F, y + 8.0F, x + 220.0F, y + 26.0F),
                  f_ui_12_b_.Get(), palette::kInk);
      t.draw_text(state_.source,
                  D2D1::RectF(x + 220.0F, y + 9.0F, x + w - 14.0F, y + 26.0F),
                  f_mono_10_.Get(), palette::kInkSecondary,
                  DWRITE_TEXT_ALIGNMENT_TRAILING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
      y += 32.0F;
      for (const auto& ch : state_.choices) {
        const bool sel = (state_.selected_monitor == ch.handle);
        const D2D1_RECT_F cr =
            D2D1::RectF(x + 14.0F, y, x + w - 14.0F, y + 30.0F);
        t.fill_round(cr, 3.0F,
                     sel ? palette::kRoseWash : palette::kSubtleSurface);
        t.stroke_round(cr, 3.0F,
                       sel ? palette::kRoseBorder : palette::kHairline, 0.75F);
        const D2D1_RECT_F sq =
            D2D1::RectF(cr.left + 10.0F, cr.top + 12.0F, cr.left + 15.0F,
                        cr.top + 17.0F);
        if (sel) {
          t.fill_rect(sq, palette::kCrimson);
        } else {
          t.stroke_round(sq, 0.0F, palette::kInkMuted, 0.8F);
        }
        t.draw_text(ch.title,
                    D2D1::RectF(cr.left + 23.0F, cr.top, cr.right - 74.0F,
                                cr.bottom),
                    sel ? f_ui_11_b_.Get() : f_ui_11_.Get(),
                    sel ? palette::kCrimson : palette::kInk,
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
        if (sel) {
          t.draw_text("当前使用",
                      D2D1::RectF(cr.right - 68.0F, cr.top, cr.right - 10.0F,
                                  cr.bottom),
                      f_ui_10_.Get(), palette::kCrimson,
                      DWRITE_TEXT_ALIGNMENT_TRAILING,
                      DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        t.add_hit(cr, [this, h = ch.handle] {
          controller_.choose_monitor(h);
          refresh_now();
        });
        y += 36.0F;
      }
      y += 4.0F;
    }

    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);
    t.draw_text("Windows 显示设置",
                D2D1::RectF(x + 14.0F, y + 8.0F, x + 220.0F, y + 26.0F),
                f_ui_12_b_.Get(), palette::kInk);
    t.draw_text("如需扩展屏幕而非镜像主屏，可在系统中配置多显示器或安装 IddCx 虚拟屏驱动。",
                D2D1::RectF(x + 14.0F, y + 26.0F, x + w - 120.0F, y + 44.0F),
                f_ui_10_.Get(), palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                true);
    draw_secondary_button(
        t, x + w - 114.0F, y + 10.0F, "显示设置", f_ui_11_b_.Get(),
        [] { open_shell_target(L"ms-settings:display"); }, 100.0F);
    t.stroke_round(D2D1::RectF(x, disp_top, x + w, disp_top + disp_h), 4.0F,
                   palette::kHairline, 0.85F);
    y = disp_top + disp_h + 18.0F;

    // 4. SYSTEM CAPABILITIES & DRIVER
    draw_section_header(t, x, y, w, "SYSTEM", "系统环境与驱动",
                        f_mono_9_.Get(), f_ui_11_b_.Get());
    y += 24.0F;
    const float sys_top = y;
    const float sys_h = 204.0F;
    t.fill_round(D2D1::RectF(x, sys_top, x + w, sys_top + sys_h), 4.0F,
                 palette::kCardSurface);
    auto sys_row = [&](int idx, const char* title, const char* badge,
                       Tone tone, const std::string& detail,
                       const char* btn_label, std::function<void()> on_btn) {
      const float ry = sys_top + idx * 50.0F;
      if (idx > 0) t.line(x, ry, x + w, ry, palette::kSoftHairline, 0.8F);
      t.draw_text(title, D2D1::RectF(x + 14.0F, ry + 7.0F, x + 130.0F, ry + 25.0F),
                  f_ui_12_b_.Get(), palette::kInk);
      draw_status_badge(t, x + 128.0F, ry + 6.0F, badge, tone, f_ui_10_b_.Get());
      t.draw_text(detail,
                  D2D1::RectF(x + 14.0F, ry + 28.0F, x + w - 124.0F, ry + 46.0F),
                  f_ui_11_.Get(), palette::kInkMuted,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
      if (btn_label != nullptr && on_btn) {
        draw_secondary_button(t, x + w - 114.0F, ry + 11.0F, btn_label,
                              f_ui_11_.Get(), std::move(on_btn), 100.0F);
      }
    };
    sys_row(0, "画面采集编码",
            state_.hevc_available ? "HEVC / H.264" : "H.264", Tone::Positive,
            "DXGI Desktop Duplication + Media Foundation 硬件/软件回退",
            nullptr, {});
    const std::string vdd_badge =
        state_.vdd_installed
            ? (state_.vdd_backend_name.empty() ? std::string("虚拟屏就绪")
                                               : state_.vdd_backend_name)
            : std::string("未安装");
    sys_row(1, "虚拟屏驱动", vdd_badge.c_str(),
            state_.vdd_installed ? Tone::Positive : Tone::Warning,
            !state_.vdd_status_detail.empty()
                ? state_.vdd_status_detail
                : "安装 Parsec VDD 或 MttVDD 即可为平板创建独立虚拟扩展屏",
            state_.vdd_installed ? "显示设置" : "获取驱动", [this] {
              if (state_.vdd_installed) {
                open_shell_target(L"ms-settings:display");
              } else {
                open_shell_target(
                    L"https://github.com/nomi-san/parsec-vdd/releases");
              }
            });
    sys_row(2, "触控输入注入",
            state_.native_touch_available ? "原生触摸可用" : "SendInput 就绪",
            Tone::Positive,
            state_.native_touch_available
                ? "支持 Win32 多点触摸注入 (InitializeTouchInjection) 与键鼠映射"
                : "支持 SendInput 全桌面鼠标、滚轮与手势快捷键映射",
            nullptr, {});
    sys_row(3, "USB AOA 驱动",
            state_.driver_missing ? "待绑定 WinUSB" : "就绪",
            state_.driver_missing ? Tone::Warning : Tone::Positive,
            state_.driver_missing
                ? (!state_.driver_hint.empty()
                       ? state_.driver_hint
                       : "请使用 Zadig 为 18D1:2D0x AOA 接口绑定 WinUSB 或安装 UsbDk")
                : "libusb + WinUSB / UsbDk 配件枚举通道已启用",
            "设备管理器", [] { open_shell_target(L"devmgmt.msc"); });

    t.stroke_round(D2D1::RectF(x, sys_top, x + w, sys_top + sys_h), 4.0F,
                   palette::kHairline, 0.85F);
    y = sys_top + sys_h + 18.0F;

    // 5. UPDATES
    draw_section_header(t, x, y, w, "UPDATES", "软件更新",
                        f_mono_9_.Get(), f_ui_11_b_.Get());
    y += 24.0F;
    const auto& upd = state_.update;
    const bool upd_busy =
        (upd.phase == UpdatePhase::Checking ||
         upd.phase == UpdatePhase::Downloading ||
         upd.phase == UpdatePhase::Verifying ||
         upd.phase == UpdatePhase::Installing);
    const bool show_prog =
        (upd.phase == UpdatePhase::Downloading ||
         upd.phase == UpdatePhase::Verifying ||
         upd.phase == UpdatePhase::Installing);
    const bool show_notes =
        (upd.phase == UpdatePhase::Available && !upd.release_notes.empty());
    const float top_row_h = show_prog ? 72.0F : 58.0F;
    const float notes_h = show_notes ? 42.0F : 0.0F;
    const float mirror_row_h = editing_mirror_ ? 72.0F : 56.0F;
    const float legacy_row_h = 56.0F;
    const float upd_top = y;
    const float upd_h = top_row_h + notes_h + 54.0F + mirror_row_h + legacy_row_h;
    t.fill_round(D2D1::RectF(x, upd_top, x + w, upd_top + upd_h), 4.0F,
                 palette::kCardSurface);

    const char* upd_badge = upd.current_version.c_str();
    Tone upd_tone = Tone::Neutral;
    std::string upd_badge_buf = "v" + upd.current_version;
    switch (upd.phase) {
      case UpdatePhase::Idle:
        break;
      case UpdatePhase::Checking:
        upd_badge_buf = "检查中";
        break;
      case UpdatePhase::UpToDate:
        upd_badge_buf = "已是最新";
        upd_tone = Tone::Positive;
        break;
      case UpdatePhase::Available:
        upd_badge_buf = "v" + upd.available_version + " 可用";
        upd_tone = Tone::Crimson;
        break;
      case UpdatePhase::Downloading:
        upd_badge_buf =
            "下载中 " +
            std::to_string(static_cast<int>(std::lround(upd.progress * 100.0))) +
            "%";
        upd_tone = Tone::Crimson;
        break;
      case UpdatePhase::Verifying:
        upd_badge_buf = "校验签名中";
        upd_tone = Tone::Crimson;
        break;
      case UpdatePhase::Installing:
        upd_badge_buf = "正在替换重启";
        upd_tone = Tone::Crimson;
        break;
      case UpdatePhase::Failed:
        upd_badge_buf = "更新失败";
        upd_tone = Tone::Warning;
        break;
    }
    upd_badge = upd_badge_buf.c_str();

    t.draw_text("当前版本 v" + upd.current_version,
                D2D1::RectF(x + 14.0F, y + 9.0F, x + 152.0F, y + 27.0F),
                f_ui_12_b_.Get(), palette::kInk);
    draw_status_badge(t, x + 146.0F, y + 8.0F, upd_badge, upd_tone,
                      f_ui_10_b_.Get());
    const std::string upd_sub =
        !upd.error_message.empty() ? upd.error_message : upd.status_text;
    const float upd_btn_reserve =
        (upd.phase == UpdatePhase::Available) ? 286.0F : 196.0F;
    t.draw_text(upd_sub,
                D2D1::RectF(x + 14.0F, y + 31.0F, x + w - upd_btn_reserve,
                            y + 50.0F),
                f_ui_11_.Get(),
                upd.phase == UpdatePhase::Failed ? palette::kAmber
                                                 : palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);

    draw_secondary_button(
        t, x + w - 92.0F, y + 13.0F, "Releases", f_ui_10_b_.Get(),
        [rel = upd.release_url] { open_shell_target_utf8(rel); }, 78.0F);
    if (upd.phase == UpdatePhase::Available) {
      draw_secondary_button(t, x + w - 182.0F, y + 13.0F, "重检",
                            f_ui_11_.Get(), [this] {
                              controller_.check_for_updates(true);
                              refresh_now();
                            }, 56.0F);
      draw_primary_button(t, x + w - 282.0F, y + 12.0F, "立即更新",
                          f_ui_12_.Get(), [this] {
                            controller_.install_available_update();
                            refresh_now();
                          }, 92.0F);
    } else if (!upd_busy) {
      draw_primary_button(t, x + w - 190.0F, y + 12.0F, "检查更新",
                          f_ui_12_.Get(), [this] {
                            controller_.check_for_updates(true);
                            refresh_now();
                          }, 90.0F);
    }

    if (show_prog) {
      const float bar_y = y + 57.0F;
      const float bar_w = w - 28.0F;
      t.fill_round(D2D1::RectF(x + 14.0F, bar_y, x + 14.0F + bar_w, bar_y + 5.0F),
                   2.5F, palette::kSoftHairline);
      const float fill_w =
          std::max(6.0F, bar_w * static_cast<float>(std::clamp(upd.progress, 0.0, 1.0)));
      t.fill_round(D2D1::RectF(x + 14.0F, bar_y, x + 14.0F + fill_w, bar_y + 5.0F),
                   2.5F, palette::kCrimson);
    }
    y += top_row_h;

    if (show_notes) {
      t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);
      t.fill_rect(D2D1::RectF(x + 1.0F, y + 0.5F, x + w - 1.0F, y + notes_h),
                  palette::kSubtleSurface);
      t.draw_text(upd.release_notes,
                  D2D1::RectF(x + 14.0F, y + 6.0F, x + w - 14.0F, y + notes_h - 6.0F),
                  f_ui_11_.Get(), palette::kInkSecondary);
      y += notes_h;
    }

    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);
    y = draw_toggle_row(
        t, x, y, w, "启动时自动检查更新",
        "从 GitHub Releases 检查带 Ed25519 签名的新版本（原地替换，无需重装驱动）",
        upd.auto_check_updates, [this] {
          controller_.set_auto_check_updates(!state_.update.auto_check_updates);
          refresh_now();
        });
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);

    t.draw_text("GitHub 加速前缀（可选）",
                D2D1::RectF(x + 14.0F, y + 8.0F, x + 220.0F, y + 26.0F),
                f_ui_12_b_.Get(), palette::kInk);
    t.draw_text("通过 Ed25519 校验完整性，留空即直连",
                D2D1::RectF(x + 220.0F, y + 9.0F, x + w - 14.0F, y + 26.0F),
                f_ui_10_.Get(), palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (editing_mirror_) {
      mirror_box_rect_ =
          D2D1::RectF(x + 14.0F, y + 32.0F, x + w - 148.0F, y + 58.0F);
      mirror_box_valid_ = true;
      draw_primary_button(t, x + w - 140.0F, y + 31.0F, "保存",
                          f_ui_11_.Get(), [this] { commit_mirror_edit(); },
                          58.0F);
      draw_secondary_button(t, x + w - 74.0F, y + 31.0F, "取消",
                            f_ui_11_.Get(), [this] { cancel_mirror_edit(); },
                            60.0F);
    } else {
      const std::string mirror_text =
          upd.mirror_prefix.empty() ? "当前：直连 GitHub（默认）"
                                    : upd.mirror_prefix;
      t.draw_text(mirror_text,
                  D2D1::RectF(x + 14.0F, y + 29.0F, x + w - 148.0F, y + 48.0F),
                  f_mono_10_.Get(),
                  upd.mirror_prefix.empty() ? palette::kInkMuted
                                            : palette::kInkSecondary,
                  DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
      if (!upd.mirror_prefix.empty()) {
        draw_secondary_button(
            t, x + w - 138.0F, y + 24.0F, "清除", f_ui_11_.Get(),
            [this] {
              controller_.set_update_mirror_prefix("");
              refresh_now();
            },
            56.0F);
      }
      draw_secondary_button(
          t, x + w - 76.0F, y + 24.0F, "设置", f_ui_11_.Get(),
          [this, cur = upd.mirror_prefix] { begin_mirror_edit(cur); },
          62.0F);
    }
    y += mirror_row_h;
    t.line(x, y, x + w, y, palette::kSoftHairline, 0.8F);

    std::string legacy_badge = "已清理";
    Tone legacy_tone = Tone::Positive;
    if (upd.legacy_file_count > 0) {
      char sz_buf[32]{};
      if (upd.legacy_total_bytes >= 1024ULL * 1024ULL) {
        std::snprintf(sz_buf, sizeof(sz_buf), "%.1f MB",
                      static_cast<double>(upd.legacy_total_bytes) /
                          (1024.0 * 1024.0));
      } else if (upd.legacy_total_bytes >= 1024ULL) {
        std::snprintf(sz_buf, sizeof(sz_buf), "%.1f KB",
                      static_cast<double>(upd.legacy_total_bytes) / 1024.0);
      } else {
        std::snprintf(sz_buf, sizeof(sz_buf), "%llu B",
                      static_cast<unsigned long long>(upd.legacy_total_bytes));
      }
      legacy_badge =
          std::to_string(upd.legacy_file_count) + " 个残留 · " + sz_buf;
      legacy_tone = Tone::Warning;
    }
    t.draw_text("旧版本文件清理",
                D2D1::RectF(x + 14.0F, y + 9.0F, x + 132.0F, y + 27.0F),
                f_ui_12_b_.Get(), palette::kInk);
    draw_status_badge(t, x + 126.0F, y + 8.0F, legacy_badge, legacy_tone,
                      f_ui_10_b_.Get());
    const std::string legacy_sub =
        !upd.legacy_cleanup_message.empty()
            ? upd.legacy_cleanup_message
            : (upd.legacy_file_count > 0
                   ? "检测到旧版 .old/.bak 或历史更新临时文件，点击右侧按钮可一键清除"
                   : "更新替换时会自动清除旧版本与临时缓存；当前无残留旧文件");
    t.draw_text(legacy_sub,
                D2D1::RectF(x + 14.0F, y + 30.0F, x + w - 134.0F, y + 49.0F),
                f_ui_11_.Get(), palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                true);
    if (upd.legacy_file_count > 0) {
      draw_primary_button(t, x + w - 124.0F, y + 13.0F, "一键清理旧文件",
                          f_ui_11_b_.Get(), [this] {
                            controller_.cleanup_legacy_update_files();
                            refresh_now();
                          }, 110.0F);
    } else {
      draw_secondary_button(t, x + w - 124.0F, y + 14.0F, "一键清理旧文件",
                            f_ui_11_.Get(), [this] {
                              controller_.cleanup_legacy_update_files();
                              refresh_now();
                            }, 110.0F);
    }

    t.stroke_round(D2D1::RectF(x, upd_top, x + w, upd_top + upd_h), 4.0F,
                   palette::kHairline, 0.85F);
    return upd_top + upd_h + 10.0F;
  }

  float draw_diagnostics_tab(const RenderTarget& t, float x, float y, float w) {
    t.draw_text("DIAGNOSTICS // 连接与编码摘要",
                D2D1::RectF(x, y, x + w, y + 16.0F), f_mono_9_.Get(),
                palette::kCrimson);
    y += 18.0F;
    t.draw_text("运行诊断日志", D2D1::RectF(x, y, x + w, y + 28.0F),
                f_ui_20_l_.Get(), palette::kInk);
    y += 38.0F;

    const float cw = draw_primary_button(
        t, x, y, copied_feedback_ ? "✓ 已复制到剪贴板" : "复制诊断信息",
        f_ui_12_.Get(), [this] { copy_diagnostic(); });
    if (state_.can_retry) {
      draw_secondary_button(t, x + cw + 10.0F, y + 1.0F, "重新建立连接",
                            f_ui_11_b_.Get(), [this] {
                              controller_.retry();
                              refresh_now();
                            });
    }
    y += 44.0F;

    draw_section_header(t, x, y, w, "LOG OUTPUT", "状态快照",
                        f_mono_9_.Get(), f_ui_11_b_.Get());
    y += 24.0F;

    const float log_h = std::max(
        240.0F,
        std::ceil(res_.measure(state_.diagnostic, f_mono_10_.Get(), w - 28.0F)
                      .height) +
            28.0F);
    const D2D1_RECT_F box = D2D1::RectF(x, y, x + w, y + log_h);
    t.fill_round(box, 4.0F, palette::kSubtleSurface);
    t.stroke_round(box, 4.0F, palette::kHairline, 0.85F);
    t.draw_text(state_.diagnostic,
                D2D1::RectF(x + 14.0F, y + 12.0F, x + w - 14.0F, y + log_h - 12.0F),
                f_mono_10_.Get(), palette::kInkSecondary);
    return y + log_h + 10.0F;
  }

  float draw_toggle_row(const RenderTarget& t, float x, float y, float w,
                        const std::string& title, const std::string& sub,
                        bool is_on, std::function<void()> on_toggle) {
    t.draw_text(title, D2D1::RectF(x + 14.0F, y + 9.0F, x + w - 110.0F, y + 27.0F),
                f_ui_12_b_.Get(), palette::kInk);
    t.draw_text(sub, D2D1::RectF(x + 14.0F, y + 29.0F, x + w - 110.0F, y + 48.0F),
                f_ui_11_.Get(), palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                true);
    draw_hairline_toggle(t, x + w - 100.0F, y + 17.0F, is_on, f_mono_11_.Get(),
                         std::move(on_toggle));
    return y + 54.0F;
  }

  void draw_position_segment_bar(const RenderTarget& t, float x, float y,
                                 float w, DisplayPosition current,
                                 std::function<void(DisplayPosition)> on_pick) {
    const DisplayPosition poses[] = {DisplayPosition::Right,
                                     DisplayPosition::Left,
                                     DisplayPosition::Top,
                                     DisplayPosition::Bottom};
    const float gap = 8.0F;
    const float seg_w = (w - gap * 3.0F) / 4.0F;
    for (int i = 0; i < 4; ++i) {
      const float sx = x + i * (seg_w + gap);
      draw_segment_option(t, D2D1::RectF(sx, y, sx + seg_w, y + 28.0F),
                          display_position_short_label(poses[i]),
                          current == poses[i], f_ui_11_b_.Get(),
                          [on_pick, p = poses[i]] { on_pick(p); });
    }
  }

  static std::string fps_status_subtitle(int preferred_hz, int negotiated_hz) {
    if (preferred_hz <= 0) {
      if (negotiated_hz > 0) {
        return "跟随设备协商（当前 " + std::to_string(negotiated_hz) + " Hz）";
      }
      return "默认跟随设备协商帧率";
    }
    if (negotiated_hz > 0) {
      if (preferred_hz > negotiated_hz) {
        return "已按协商上限 " + std::to_string(negotiated_hz) +
               " Hz 输出（请求 " + std::to_string(preferred_hz) + " Hz）";
      }
      return "手动锁定 " + std::to_string(preferred_hz) + " Hz（协商 " +
             std::to_string(negotiated_hz) + " Hz）";
    }
    return "手动锁定 " + std::to_string(preferred_hz) + " Hz";
  }

  void draw_fps_segment_bar(const RenderTarget& t, float x, float y, float w,
                            int current_hz, std::function<void(int)> on_pick) {
    struct Option {
      int hz;
      const char* label;
    };
    const Option opts[] = {
        {0, "自动"},
        {30, "30Hz"},
        {60, "60Hz"},
        {90, "90Hz"},
        {120, "120Hz"},
    };
    const float gap = 6.0F;
    const float seg_w = (w - gap * 4.0F) / 5.0F;
    for (int i = 0; i < 5; ++i) {
      const float sx = x + i * (seg_w + gap);
      draw_segment_option(t, D2D1::RectF(sx, y, sx + seg_w, y + 28.0F),
                          opts[i].label, current_hz == opts[i].hz,
                          f_ui_11_b_.Get(),
                          [on_pick, hz = opts[i].hz] { on_pick(hz); });
    }
  }

  float compute_popover_height() const {
    float h = 342.0F;
    if (!state_.service_running || state_.driver_missing) {
      h += 48.0F;
    }
    if (state_.active_sessions.size() > 1) {
      h += 28.0F + static_cast<float>(state_.active_sessions.size()) * 29.0F;
    }
    return h;
  }

  void paint_popover() {
    if (!ensure_pop_rt()) {
      return;
    }
    const D2D1_SIZE_F sz = pop_rt_->GetSize();
    pop_hits_.clear();
    RenderTarget t{pop_rt_.Get(), pop_brush_.Get(), &res_, &pop_hits_,
                   pop_mouse_x_, pop_mouse_y_, pop_mouse_down_};

    pop_rt_->BeginDraw();
    draw_studio_background(t, sz.width, sz.height);
    t.stroke_round(D2D1::RectF(0.5F, 0.5F, sz.width - 0.5F, sz.height - 0.5F),
                   6.0F, palette::kHairline, 1.0F);

    const bool streaming =
        state_.badge == "会话已建立" ||
        state_.badge.find("运行中") != std::string::npos ||
        state_.live_fps > 0;
    const float pad = 16.0F;
    const float w = sz.width - pad * 2.0F;
    float y = pad;

    // Header
    draw_dual_display_glyph(t, pad, y, 30.0F, state_.linked, streaming);
    t.draw_text("DeskMate", D2D1::RectF(pad + 40.0F, y, pad + 160.0F, y + 17.0F),
                f_ui_12_b_.Get(), palette::kInk);
    std::string sub = "扩展屏 · 主屏" + display_position_short_label(state_.position);
    if (state_.active_sessions.size() > 1) {
      sub = "已连接 " + std::to_string(state_.active_sessions.size()) + " 台副屏设备";
    } else if (state_.linked && !state_.connected_device_name.empty()) {
      sub = state_.connected_device_name + " · 主屏" +
            display_position_short_label(state_.position);
    }
    t.draw_text(sub, D2D1::RectF(pad + 40.0F, y + 17.0F, pad + 185.0F, y + 32.0F),
                f_ui_10_.Get(), palette::kInkMuted, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    const float bw = std::ceil(res_.measure(state_.badge, f_ui_10_b_.Get()).width) + 28.0F;
    draw_status_badge(t, pad + w - bw, y + 4.0F, state_.badge,
                      tone_for_badge(state_.badge), f_ui_10_b_.Get());
    y += 44.0F;

    // 3-column compact telemetry
    const D2D1_RECT_F tele = D2D1::RectF(pad, y, pad + w, y + 48.0F);
    t.fill_round(tele, 4.0F, palette::kCardSurface);
    const float cw = w / 3.0F;
    auto cmet = [&](int col, const std::string& val, const char* lbl, bool acc) {
      const float cx = pad + col * cw;
      if (col > 0) t.line(cx, y + 10.0F, cx, y + 38.0F, palette::kSoftHairline);
      t.draw_text(val, D2D1::RectF(cx, y + 6.0F, cx + cw, y + 27.0F),
                  f_mono_11_b_.Get(), acc ? palette::kCrimson : palette::kInk,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      t.draw_text(lbl, D2D1::RectF(cx, y + 27.0F, cx + cw, y + 43.0F),
                  f_mono_9_.Get(), palette::kInkMuted,
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    const std::string pop_rate = format_bitrate_mbps(state_.live_bitrate_mbps);
    cmet(0, state_.live_fps > 0 ? std::to_string(state_.live_fps) : "—", "FPS",
         streaming && state_.live_fps > 0);
    cmet(1, pop_rate.empty() ? std::string("—") : pop_rate, "Mbps", false);
    cmet(2,
         state_.input_enabled ? std::to_string(state_.input_event_count) : "关",
         "触控", state_.input_enabled);
    t.stroke_round(tele, 4.0F, palette::kHairline, 0.8F);
    y += 60.0F;

    if (!state_.service_running) {
      const D2D1_RECT_F call = D2D1::RectF(pad, y, pad + w, y + 38.0F);
      t.fill_round(call, 4.0F, palette::kSubtleSurface);
      t.stroke_round(call, 4.0F, palette::kHairline, 0.8F);
      t.draw_text("串流服务已停止",
                  D2D1::RectF(pad + 10.0F, y, pad + w - 96.0F, y + 38.0F),
                  f_ui_11_b_.Get(), palette::kInk, DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      draw_primary_button(t, pad + w - 88.0F, y + 4.0F, "启动服务",
                          f_ui_11_b_.Get(), [this] {
                            controller_.set_service_running(true);
                            refresh_now();
                          }, 80.0F);
      y += 48.0F;
    } else if (state_.driver_missing) {
      const D2D1_RECT_F call = D2D1::RectF(pad, y, pad + w, y + 38.0F);
      t.fill_round(call, 4.0F, palette::kAmberWash);
      t.stroke_round(call, 4.0F, palette::kAmberBorder, 0.8F);
      t.draw_text("需要 WinUSB 驱动",
                  D2D1::RectF(pad + 10.0F, y, pad + w - 104.0F, y + 38.0F),
                  f_ui_11_b_.Get(), palette::kInk, DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      draw_secondary_button(t, pad + w - 98.0F, y + 5.0F, "设备管理器",
                            f_ui_10_b_.Get(),
                            [] { open_shell_target(L"devmgmt.msc"); }, 88.0F);
      y += 48.0F;
    }

    // Controls card
    const float multi_h =
        state_.active_sessions.size() > 1
            ? (28.0F + static_cast<float>(state_.active_sessions.size()) * 29.0F)
            : 0.0F;
    const float ctrl_h = 38.0F + 48.0F + 64.0F + multi_h;
    const D2D1_RECT_F ctrl = D2D1::RectF(pad, y, pad + w, y + ctrl_h);
    t.fill_round(ctrl, 4.0F, palette::kCardSurface);

    t.draw_text("画面推流", D2D1::RectF(pad + 12.0F, y, pad + 140.0F, y + 38.0F),
                f_ui_12_b_.Get(), palette::kInk, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    draw_hairline_toggle(t, pad + w - 98.0F, y + 9.0F, state_.video_enabled,
                         f_mono_11_.Get(), [this] {
                           controller_.set_video_enabled(!state_.video_enabled);
                           refresh_now();
                         });
    y += 38.0F;
    t.line(pad, y, pad + w, y, palette::kSoftHairline, 0.8F);

    t.draw_text("触控回传", D2D1::RectF(pad + 12.0F, y + 6.0F, pad + 140.0F, y + 24.0F),
                f_ui_12_b_.Get(), palette::kInk);
    const std::string mode_pop_lbl =
        touch_mode_display_name(state_.touch_input_mode) + " ▾";
    const D2D1_RECT_F mode_pop_r =
        D2D1::RectF(pad + 12.0F, y + 25.0F, pad + w - 104.0F, y + 43.0F);
    t.draw_text(mode_pop_lbl, mode_pop_r, f_ui_10_.Get(), palette::kInkSecondary,
                DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    t.add_hit(mode_pop_r, [this, mode_pop_r] {
      show_touch_mode_menu(popover_hwnd_, mode_pop_r.left,
                           mode_pop_r.bottom + 2.0F);
    });
    draw_hairline_toggle(t, pad + w - 98.0F, y + 14.0F, state_.input_enabled,
                         f_mono_11_.Get(), [this] {
                           controller_.set_input_enabled(!state_.input_enabled);
                           refresh_now();
                         });
    y += 48.0F;
    t.line(pad, y, pad + w, y, palette::kSoftHairline, 0.8F);

    t.draw_text(state_.active_sessions.size() > 1 ? "当前选中副屏方位"
                                                  : "副屏方位",
                D2D1::RectF(pad + 12.0F, y + 7.0F, pad + w - 12.0F, y + 23.0F),
                f_ui_11_.Get(), palette::kInkMuted);
    draw_position_segment_bar(t, pad + 12.0F, y + 27.0F, w - 24.0F,
                              state_.position, [this](DisplayPosition p) {
                                if (state_.selected_link_id != 0) {
                                  controller_.set_link_position(
                                      state_.selected_link_id, p);
                                } else {
                                  controller_.set_default_position(p);
                                }
                                refresh_now();
                              });
    y += 64.0F;

    if (state_.active_sessions.size() > 1) {
      t.line(pad, y, pad + w, y, palette::kSoftHairline, 0.8F);
      t.draw_text("已连接设备 (" +
                      std::to_string(state_.active_sessions.size()) + ")",
                  D2D1::RectF(pad + 12.0F, y + 6.0F, pad + w - 12.0F, y + 22.0F),
                  f_ui_10_.Get(), palette::kInkMuted);
      y += 24.0F;
      for (const auto& s : state_.active_sessions) {
        const bool sel = (state_.selected_link_id == s.link_id);
        const D2D1_RECT_F row_r =
            D2D1::RectF(pad + 12.0F, y, pad + w - 36.0F, y + 24.0F);
        t.fill_round(row_r, 2.5F,
                     sel ? palette::kRoseWash : palette::kSubtleSurface);
        t.stroke_round(row_r, 2.5F,
                       sel ? palette::kRoseBorder : palette::kHairline, 0.65F);
        t.fill_rect(D2D1::RectF(row_r.left + 7.0F, row_r.top + 10.0F,
                                row_r.left + 11.0F, row_r.top + 14.0F),
                    s.streaming ? palette::kCrimson : palette::kJade);
        t.draw_text(s.device_name,
                    D2D1::RectF(row_r.left + 16.0F, row_r.top,
                                row_r.right - 84.0F, row_r.bottom),
                    sel ? f_ui_11_b_.Get() : f_ui_11_.Get(),
                    sel ? palette::kCrimson : palette::kInk,
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
        const std::string right_lbl =
            display_position_short_label(s.position) + " · " +
            (s.live_fps > 0 ? (std::to_string(s.live_fps) + "fps") : s.badge);
        t.draw_text(right_lbl,
                    D2D1::RectF(row_r.right - 82.0F, row_r.top,
                                row_r.right - 6.0F, row_r.bottom),
                    f_mono_9_.Get(), palette::kInkMuted,
                    DWRITE_TEXT_ALIGNMENT_TRAILING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
        t.add_hit(row_r, [this, lid = s.link_id] {
          controller_.select_link(lid);
          refresh_now();
        });

        const D2D1_RECT_F x_r =
            D2D1::RectF(pad + w - 32.0F, y + 2.0F, pad + w - 12.0F, y + 22.0F);
        t.draw_text("×", x_r, f_ui_12_b_.Get(), palette::kInkMuted,
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        t.add_hit(x_r, [this, lid = s.link_id] {
          controller_.disconnect_link(lid);
          refresh_now();
        });
        y += 29.0F;
      }
      y += 4.0F;
    }

    t.stroke_round(ctrl, 4.0F, palette::kHairline, 0.85F);
    y += 12.0F;

    t.line(pad, y, pad + w, y, palette::kHairline, 0.8F);
    y += 6.0F;

    // Footer actions
    const D2D1_RECT_F open_r = D2D1::RectF(pad, y, pad + 88.0F, y + 26.0F);
    t.draw_text("打开主窗口", open_r, f_ui_11_b_.Get(), palette::kCrimson,
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    t.add_hit(open_r, [this] {
      ShowWindow(popover_hwnd_, SW_HIDE);
      show_main_window();
    });

    if (state_.service_running) {
      const D2D1_RECT_F stop_r =
          D2D1::RectF(pad + w - 166.0F, y, pad + w - 104.0F, y + 26.0F);
      t.draw_text("停止服务", stop_r, f_ui_11_.Get(), palette::kInkSecondary,
                  DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      t.add_hit(stop_r, [this] {
        controller_.set_service_running(false);
        refresh_now();
      });
    }
    const D2D1_RECT_F cp_r =
        D2D1::RectF(pad + w - 102.0F, y, pad + w - 40.0F, y + 26.0F);
    t.draw_text("复制状态", cp_r, f_ui_11_.Get(), palette::kInkSecondary,
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    t.add_hit(cp_r, [this] {
      copy_diagnostic();
      ShowWindow(popover_hwnd_, SW_HIDE);
    });

    const D2D1_RECT_F qt_r =
        D2D1::RectF(pad + w - 38.0F, y, pad + w, y + 26.0F);
    t.draw_text("退出", qt_r, f_ui_11_.Get(), palette::kInkMuted,
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    t.add_hit(qt_r, [this] { request_quit(); });

    const HRESULT hr = pop_rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
      pop_brush_.Reset();
      pop_rt_.Reset();
    }
  }

  static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp) {
    StudioWindowController* self = nullptr;
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      self = static_cast<StudioWindowController*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<StudioWindowController*>(
          GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
      return self->handle_main_msg(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  static LRESULT CALLBACK popover_wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                           LPARAM lp) {
    StudioWindowController* self = nullptr;
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      self = static_cast<StudioWindowController*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<StudioWindowController*>(
          GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
      return self->handle_popover_msg(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  LRESULT handle_main_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        const float s = dpi_scale(hwnd);
        mmi->ptMinTrackSize.x = static_cast<LONG>(std::lround(920.0F * s));
        mmi->ptMinTrackSize.y = static_cast<LONG>(std::lround(600.0F * s));
        return 0;
      }
      case WM_DISPLAYCHANGE:
        controller_.refresh_displays();
        refresh_now();
        return 0;
      case WM_DPICHANGED: {
        main_brush_.Reset();
        main_rt_.Reset();
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_SIZE:
        if (main_rt_ != nullptr) {
          main_rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        paint_main();
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_TIMER:
        if (wp == kTimerRefreshId) {
          refresh_now();
          if (state_.update.restart_requested && !quitting_) {
            request_quit();
          }
        } else if (wp == kTimerCopyResetId) {
          copied_feedback_ = false;
          KillTimer(hwnd, kTimerCopyResetId);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      case WM_MOUSEMOVE: {
        const float s = dpi_scale(hwnd);
        main_mouse_x_ = static_cast<float>(GET_X_LPARAM(lp)) / s;
        main_mouse_y_ = static_cast<float>(GET_Y_LPARAM(lp)) / s;
        if (main_mouse_down_ && topo_drag_candidate_id_ != 0) {
          const float dx = main_mouse_x_ - topo_drag_press_x_;
          const float dy = main_mouse_y_ - topo_drag_press_y_;
          if (!topo_dragging_ && std::hypot(dx, dy) >= 4.0F) {
            topo_dragging_ = true;
            topo_selected_display_id_ = topo_drag_candidate_id_;
            for (const auto& item : topo_drag_start_) {
              if (item.id == topo_drag_candidate_id_ && item.link_id != 0) {
                controller_.select_link(item.link_id);
                break;
              }
            }
          }
          if (topo_dragging_) {
            for (const auto& orig : topo_drag_start_) {
              if (orig.id == topo_drag_candidate_id_) {
                const double scale_safe = std::max(topo_viewport_.scale, 1e-6);
                const double cand_x = orig.x + static_cast<double>(dx) / scale_safe;
                const double cand_y = orig.y + static_cast<double>(dy) / scale_safe;
                auto snap = snap_dragged_display(
                    topo_drag_start_, topo_drag_candidate_id_, cand_x, cand_y);
                topo_preview_ = std::move(snap.items);
                topo_guides_ = std::move(snap.guides);
                topo_swap_preview_ = snap.is_swap;
                break;
              }
            }
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
          }
        }
        bool over_tile = false;
        if (point_in_rect(main_mouse_x_, main_mouse_y_, topo_canvas_rect_)) {
          for (const auto& tr : topo_tile_rects_) {
            if (point_in_rect(main_mouse_x_, main_mouse_y_, tr.second)) {
              over_tile = true;
              break;
            }
          }
        }
        bool over_hit = false;
        for (const auto& h : main_hits_) {
          if (point_in_rect(main_mouse_x_, main_mouse_y_, h.rect)) {
            over_hit = true;
            break;
          }
        }
        SetCursor(LoadCursorW(
            nullptr, over_tile ? IDC_SIZEALL : (over_hit ? IDC_HAND : IDC_ARROW)));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_LBUTTONDOWN: {
        const float s = dpi_scale(hwnd);
        main_mouse_x_ = static_cast<float>(GET_X_LPARAM(lp)) / s;
        main_mouse_y_ = static_cast<float>(GET_Y_LPARAM(lp)) / s;
        main_mouse_down_ = true;
        topo_drag_candidate_id_ = 0;
        topo_dragging_ = false;
        if (point_in_rect(main_mouse_x_, main_mouse_y_, topo_canvas_rect_)) {
          for (auto it = topo_tile_rects_.rbegin(); it != topo_tile_rects_.rend();
               ++it) {
            if (point_in_rect(main_mouse_x_, main_mouse_y_, it->second)) {
              topo_drag_candidate_id_ = it->first;
              topo_drag_press_x_ = main_mouse_x_;
              topo_drag_press_y_ = main_mouse_y_;
              topo_drag_start_ = topo_base_items_;
              break;
            }
          }
        }
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_LBUTTONUP: {
        const float s = dpi_scale(hwnd);
        main_mouse_x_ = static_cast<float>(GET_X_LPARAM(lp)) / s;
        main_mouse_y_ = static_cast<float>(GET_Y_LPARAM(lp)) / s;
        const bool was_down = main_mouse_down_;
        main_mouse_down_ = false;
        ReleaseCapture();
        if (topo_drag_candidate_id_ != 0) {
          const std::uint32_t did = topo_drag_candidate_id_;
          const bool was_dragging = topo_dragging_;
          topo_drag_candidate_id_ = 0;
          topo_dragging_ = false;
          if (was_dragging && !topo_preview_.empty()) {
            if (did == kGhostPreviewDisplayId) {
              const TopologyDisplayItem* primary = nullptr;
              const TopologyDisplayItem* ghost = nullptr;
              for (const auto& item : topo_preview_) {
                if (item.is_primary) primary = &item;
                if (item.is_ghost_preview) ghost = &item;
              }
              if (primary != nullptr && ghost != nullptr) {
                const int code = infer_cardinal_position_code(*primary, *ghost);
                controller_.set_default_position(
                    static_cast<DisplayPosition>(std::clamp(code, 0, 3)));
              }
            } else {
              std::vector<TopologyDisplayItem> real_items;
              for (const auto& item : topo_preview_) {
                if (!item.is_ghost_preview) {
                  real_items.push_back(item);
                }
              }
              const auto normalized = normalize_to_primary_origin(real_items);
              std::vector<MonitorPlacement> placements;
              placements.reserve(normalized.size());
              for (const auto& item : normalized) {
                MonitorPlacement mp;
                mp.handle = item.handle;
                mp.device_name = item.device_name;
                mp.left = static_cast<int>(std::llround(item.x));
                mp.top = static_cast<int>(std::llround(item.y));
                mp.primary = item.is_primary;
                placements.push_back(std::move(mp));
              }
              controller_.apply_display_layout(placements);
            }
          } else {
            topo_selected_display_id_ = did;
            for (const auto& item : topo_drag_start_) {
              if (item.id == did && item.link_id != 0) {
                controller_.select_link(item.link_id);
                break;
              }
            }
          }
          topo_preview_.clear();
          topo_guides_.clear();
          topo_swap_preview_ = false;
          refresh_now();
          return 0;
        }
        if (was_down) {
          // Iterate in reverse so inner buttons win over enclosing cards.
          for (auto it = main_hits_.rbegin(); it != main_hits_.rend(); ++it) {
            if (point_in_rect(main_mouse_x_, main_mouse_y_, it->rect)) {
              auto fn = it->action;
              fn();
              break;
            }
          }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        right_scroll_y_ = std::clamp(
            right_scroll_y_ - static_cast<float>(delta) * 0.45F, 0.0F,
            right_max_scroll_);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case kMsgTrayIcon:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
          toggle_tray_popover();
        } else if (LOWORD(lp) == WM_RBUTTONUP ||
                   LOWORD(lp) == WM_CONTEXTMENU) {
          show_tray_context_menu();
        }
        return 0;
      case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) {
          controller_.handle_host_event(
              deskmate::protocol::HostEvent::ScreenLocked);
        } else if (wp == WTS_SESSION_UNLOCK) {
          controller_.handle_host_event(
              deskmate::protocol::HostEvent::ScreenUnlocked);
        }
        return 0;
      case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
          controller_.handle_host_event(
              deskmate::protocol::HostEvent::WillSleep);
        } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
          controller_.handle_host_event(
              deskmate::protocol::HostEvent::ScreenUnlocked);
        }
        return TRUE;
      case WM_CLOSE:
        if (!quitting_ && tray_added_) {
          ShowWindow(hwnd, SW_HIDE);
          update_refresh_timer();
          return 0;
        }
        request_quit();
        return 0;
      case WM_QUERYENDSESSION:
      case WM_ENDSESSION:
        if (msg == WM_ENDSESSION && wp != 0) {
          request_quit();
        }
        return TRUE;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
  }

  LRESULT handle_popover_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
          ShowWindow(hwnd, SW_HIDE);
          update_refresh_timer();
        }
        return 0;
      case WM_SIZE:
        if (pop_rt_ != nullptr) {
          pop_rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        paint_popover();
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_MOUSEMOVE: {
        const float s = dpi_scale(hwnd);
        pop_mouse_x_ = static_cast<float>(GET_X_LPARAM(lp)) / s;
        pop_mouse_y_ = static_cast<float>(GET_Y_LPARAM(lp)) / s;
        bool over = false;
        for (const auto& h : pop_hits_) {
          if (point_in_rect(pop_mouse_x_, pop_mouse_y_, h.rect)) {
            over = true;
            break;
          }
        }
        SetCursor(LoadCursorW(nullptr, over ? IDC_HAND : IDC_ARROW));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_LBUTTONDOWN: {
        const float s = dpi_scale(hwnd);
        pop_mouse_x_ = static_cast<float>(GET_X_LPARAM(lp)) / s;
        pop_mouse_y_ = static_cast<float>(GET_Y_LPARAM(lp)) / s;
        pop_mouse_down_ = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_LBUTTONUP: {
        const float s = dpi_scale(hwnd);
        pop_mouse_x_ = static_cast<float>(GET_X_LPARAM(lp)) / s;
        pop_mouse_y_ = static_cast<float>(GET_Y_LPARAM(lp)) / s;
        const bool was_down = pop_mouse_down_;
        pop_mouse_down_ = false;
        ReleaseCapture();
        if (was_down) {
          for (auto it = pop_hits_.rbegin(); it != pop_hits_.rend(); ++it) {
            if (point_in_rect(pop_mouse_x_, pop_mouse_y_, it->rect)) {
              auto fn = it->action;
              fn();
              break;
            }
          }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
  }

  HostController& controller_;
  StudioRenderer res_;
  HostUiState state_;
  UiTab tab_{UiTab::Display};

  HWND main_hwnd_{nullptr};
  HWND popover_hwnd_{nullptr};
  HWND rename_edit_{nullptr};
  HWND mirror_edit_{nullptr};
  HICON app_icon_big_{nullptr};
  HICON app_icon_small_{nullptr};
  HICON tray_hicon_{nullptr};
  bool tray_added_{false};
  bool last_tray_linked_{false};
  bool last_tray_streaming_{false};
  bool quitting_{false};
  bool copied_feedback_{false};

  std::string renaming_device_id_;
  D2D1_RECT_F rename_box_rect_{};
  bool rename_box_valid_{false};
  bool editing_mirror_{false};
  D2D1_RECT_F mirror_box_rect_{};
  bool mirror_box_valid_{false};

  ComPtr<ID2D1HwndRenderTarget> main_rt_;
  ComPtr<ID2D1SolidColorBrush> main_brush_;
  ComPtr<ID2D1HwndRenderTarget> pop_rt_;
  ComPtr<ID2D1SolidColorBrush> pop_brush_;

  ComPtr<IDWriteTextFormat> f_mono_9_, f_mono_10_, f_mono_11_, f_mono_11_b_,
      f_mono_20_;
  ComPtr<IDWriteTextFormat> f_ui_10_, f_ui_10_b_, f_ui_11_, f_ui_11_b_,
      f_ui_12_, f_ui_12_b_, f_ui_14_b_, f_ui_20_l_;

  std::vector<HitRegion> main_hits_;
  std::vector<HitRegion> pop_hits_;
  float main_mouse_x_{-1.0F}, main_mouse_y_{-1.0F};
  bool main_mouse_down_{false};
  float pop_mouse_x_{-1.0F}, pop_mouse_y_{-1.0F};
  bool pop_mouse_down_{false};
  D2D1_RECT_F right_viewport_rect_{};
  float right_scroll_y_{0.0F};
  float right_max_scroll_{0.0F};

  D2D1_RECT_F topo_canvas_rect_{};
  TopologyViewport topo_viewport_{};
  std::vector<TopologyDisplayItem> topo_base_items_;
  std::vector<TopologyDisplayItem> topo_drag_start_;
  std::vector<TopologyDisplayItem> topo_preview_;
  std::vector<TopologyGuideLine> topo_guides_;
  std::vector<std::pair<std::uint32_t, D2D1_RECT_F>> topo_tile_rects_;
  std::uint32_t topo_selected_display_id_{0};
  std::uint32_t topo_drag_candidate_id_{0};
  bool topo_dragging_{false};
  bool topo_swap_preview_{false};
  float topo_drag_press_x_{0.0F};
  float topo_drag_press_y_{0.0F};
};

}  // namespace

int run_studio_ui(HostController& controller) {
  StudioWindowController app(controller);
  return app.run();
}

}  // namespace deskmate::windows

#endif
