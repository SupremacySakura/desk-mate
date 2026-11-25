#include "deskmate/windows/capture_encoder.hpp"
#include "deskmate/platform/annex_b.hpp"
#include "deskmate/windows/virtual_display.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace deskmate::windows {

int normalize_preferred_refresh_hz(int hz) noexcept {
  switch (hz) {
    case 30:
    case 60:
    case 90:
    case 120:
      return hz;
    default:
      return 0;
  }
}

int resolve_effective_refresh_hz(int preferred_hz, int negotiated_hz,
                                 int fallback_hz) noexcept {
  const int normalized = normalize_preferred_refresh_hz(preferred_hz);
  if (normalized > 0) {
    return normalized;
  }
  if (negotiated_hz > 0) {
    return negotiated_hz;
  }
  return fallback_hz > 0 ? fallback_hz : 60;
}

int resolve_advertised_refresh_hz(int preferred_hz, bool explicit_fallback,
                                  int fallback_hz) noexcept {
  const int normalized = normalize_preferred_refresh_hz(preferred_hz);
  if (normalized > 0 || explicit_fallback) {
    return normalized > 0 ? normalized : fallback_hz;
  }
  return 60;
}

int async_input_wait_budget_ms(int) noexcept { return 0; }

std::uint32_t compute_target_bitrate_bps(int width, int height, int fps,
                                         bool hevc) noexcept {
  const double safe_w = static_cast<double>(std::max(1, width));
  const double safe_h = static_cast<double>(std::max(1, height));
  const double clamped_fps = static_cast<double>(std::clamp(fps, 1, 120));
  // Match macOS VideoEncoder sublinear FPS scaling and USB 2.0 headroom so
  // periodic keyframes stay well under the 64 x 12 KiB = 768 KiB cap.
  const double effective_fps =
      clamped_fps <= 60.0 ? clamped_fps : (60.0 + (clamped_fps - 60.0) * 0.5);
  const double bpp = hevc ? 0.065 : 0.09;
  const double raw = safe_w * safe_h * effective_fps * bpp;
  return static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(std::llround(raw), 4'000'000LL, 14'000'000LL));
}

bool annex_b_has_vcl_nal(std::span<const std::uint8_t> annex_b,
                         bool hevc) noexcept {
  for (const auto& unit : deskmate::platform::split_annex_b(annex_b)) {
    if (unit.size == 0) {
      continue;
    }
    const std::uint8_t header = annex_b[unit.offset];
    if (hevc) {
      const std::uint8_t type = static_cast<std::uint8_t>((header >> 1U) & 0x3FU);
      if (type <= 31U) {
        return true;
      }
    } else {
      const std::uint8_t type = static_cast<std::uint8_t>(header & 0x1FU);
      if (type >= 1U && type <= 5U) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace deskmate::windows

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>
#include <strmif.h>  // ICodecAPI (codecapi.h only declares GUIDs)
#include <wrl/client.h>
#include <windows.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#endif

// ICodecAPI lives in strmif.h without a DECLSPEC_UUID attribute, so MSVC finds
// its IID through strmif's own __declspec(uuid) and llvm-mingw needs the
// template specialization that backs its __uuidof emulation.
#if defined(__GNUC__) && !defined(_MSC_VER)
__CRT_UUID_DECL(ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2,
                0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda)
#endif

namespace deskmate::windows {
namespace {

using Microsoft::WRL::ComPtr;

// Define CodecAPI GUIDs locally so both MSVC and llvm-mingw compile/link
// without extra strmiids/uuid dependencies.
constexpr GUID kCodecApiCommonRateControlMode = {
    0x1c0608e9,
    0x370c,
    0x4710,
    {0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23}};
constexpr GUID kCodecApiCommonLowLatency = {
    0x9d3ecd55,
    0x89e8,
    0x490a,
    {0x97, 0x0a, 0x0c, 0x95, 0x48, 0xd5, 0xa5, 0x6e}};
constexpr GUID kCodecApiVideoForceKeyFrame = {
    0x3f9aac90,
    0x67aa,
    0x4f1e,
    {0x87, 0x0a, 0x65, 0x0d, 0x39, 0x64, 0x21, 0x8e}};
constexpr GUID kCodecApiMPVDefaultBPictureCount = {
    0x8d390aac,
    0xdc5c,
    0x4200,
    {0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2}};
constexpr GUID kCodecApiMPVGOPSize = {
    0x95f31b26,
    0x95a4,
    0x41aa,
    {0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1}};
constexpr GUID kCodecApiCommonQualityVsSpeed = {
    0x98332df8,
    0x03cd,
    0x476b,
    {0x89, 0xfa, 0x3f, 0x9e, 0x44, 0x2d, 0xec, 0x9f}};
constexpr GUID kCodecApiCommonMaxBitRate = {
    0x9651eae4,
    0x39b9,
    0x4ebf,
    {0x85, 0xef, 0xd7, 0xf4, 0x44, 0xec, 0x74, 0x65}};
constexpr GUID kMftFriendlyNameAttr = {
    0x314ffbae,
    0x5b41,
    0x4c95,
    {0x9c, 0x19, 0x4e, 0x7d, 0x58, 0x6f, 0xac, 0xe3}};
constexpr GUID kMfMtMpeg2Profile = {
    0xad76a80b,
    0x2d5c,
    0x4e0b,
    {0x92, 0xfd, 0x38, 0xc7, 0x25, 0x27, 0xd8, 0xde}};

bool try_configure_mft_types(IMFTransform* encoder, int width, int height,
                             int requested_fps, bool hevc, UINT32 bitrate,
                             int& chosen_fps_out, HRESULT& last_out_hr,
                             HRESULT& last_in_hr) {
  int fps_candidates[4]{requested_fps, 0, 0, 0};
  std::size_t fps_count = 1;
  if (requested_fps > 90) {
    fps_candidates[fps_count++] = 90;
  }
  if (requested_fps > 60) {
    fps_candidates[fps_count++] = 60;
  }
  if (requested_fps > 30) {
    fps_candidates[fps_count++] = 30;
  }

  const UINT32 profiles_h264[3] = {77U /*Main*/, 100U /*High*/, 0U};
  const UINT32 profiles_hevc[2] = {1U /*Main*/, 0U};
  const UINT32* profiles = hevc ? profiles_hevc : profiles_h264;
  const std::size_t profile_count = hevc ? 2U : 3U;

  last_out_hr = E_FAIL;
  last_in_hr = E_FAIL;

  for (std::size_t fi = 0; fi < fps_count; ++fi) {
    const int fps = std::max(1, fps_candidates[fi]);
    for (std::size_t pi = 0; pi < profile_count; ++pi) {
      ComPtr<IMFMediaType> output;
      if (FAILED(MFCreateMediaType(&output)) || output == nullptr) {
        continue;
      }
      output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      output->SetGUID(MF_MT_SUBTYPE,
                      hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
      output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
      output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      if (profiles[pi] != 0U) {
        output->SetUINT32(kMfMtMpeg2Profile, profiles[pi]);
      }
      MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE,
                         static_cast<UINT32>(width),
                         static_cast<UINT32>(height));
      MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE,
                          static_cast<UINT32>(fps), 1);
      MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

      last_out_hr = encoder->SetOutputType(0, output.Get(), 0);
      if (FAILED(last_out_hr)) {
        continue;
      }

      // Configure matching NV12 input type (try direct construct first, then
      // enumerated NV12 available types).
      ComPtr<IMFMediaType> input;
      if (SUCCEEDED(MFCreateMediaType(&input)) && input != nullptr) {
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE,
                           static_cast<UINT32>(width),
                           static_cast<UINT32>(height));
        MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE,
                            static_cast<UINT32>(fps), 1);
        MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        last_in_hr = encoder->SetInputType(0, input.Get(), 0);
        if (SUCCEEDED(last_in_hr)) {
          chosen_fps_out = fps;
          return true;
        }
      }

      for (DWORD type_idx = 0;; ++type_idx) {
        ComPtr<IMFMediaType> avail;
        const HRESULT enum_hr =
            encoder->GetInputAvailableType(0, type_idx, &avail);
        if (FAILED(enum_hr) || avail == nullptr) {
          break;
        }
        GUID subtype = GUID_NULL;
        if (FAILED(avail->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
            subtype != MFVideoFormat_NV12) {
          continue;
        }
        avail->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(avail.Get(), MF_MT_FRAME_SIZE,
                           static_cast<UINT32>(width),
                           static_cast<UINT32>(height));
        MFSetAttributeRatio(avail.Get(), MF_MT_FRAME_RATE,
                            static_cast<UINT32>(fps), 1);
        MFSetAttributeRatio(avail.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        last_in_hr = encoder->SetInputType(0, avail.Get(), 0);
        if (SUCCEEDED(last_in_hr)) {
          chosen_fps_out = fps;
          return true;
        }
      }
    }
  }
  return false;
}

std::string wide_to_utf8(const wchar_t* text) {
  if (text == nullptr || text[0] == L'\0') {
    return {};
  }
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr,
                      nullptr);
  return out;
}

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (needed <= 1) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(needed - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), needed);
  return out;
}

std::uint32_t stable_display_id(const std::string& device_name,
                                std::uint32_t fallback) {
  if (device_name.empty()) {
    return fallback;
  }
  std::uint32_t hash = 2166136261U;
  for (unsigned char ch : device_name) {
    hash ^= ch;
    hash *= 16777619U;
  }
  return hash == 0 ? fallback : hash;
}

std::string hex_hr(HRESULT hr) {
  char buf[32]{};
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return std::string(buf);
}

void convert_bgra_to_nv12(const std::uint8_t* src, int src_pitch, int src_w,
                          int src_h, int dst_w, int dst_h,
                          std::vector<std::uint8_t>& dst) {
  dst.resize(static_cast<std::size_t>(dst_w) * static_cast<std::size_t>(dst_h) *
             3U / 2U);
  std::uint8_t* y_plane = dst.data();
  std::uint8_t* uv_plane = dst.data() + static_cast<std::size_t>(dst_w) * dst_h;

  // BT.601 limited-range integer matrix matching MF NV12 expectations.
  for (int y = 0; y < dst_h; ++y) {
    const int sy = (y * src_h) / std::max(1, dst_h);
    const std::uint8_t* row = src + static_cast<std::size_t>(sy) * src_pitch;
    std::uint8_t* y_row = y_plane + static_cast<std::size_t>(y) * dst_w;
    for (int x = 0; x < dst_w; ++x) {
      const int sx = (x * src_w) / std::max(1, dst_w);
      const std::uint8_t* px = row + static_cast<std::size_t>(sx) * 4U;
      const int b = px[0];
      const int g = px[1];
      const int r = px[2];
      const int luma = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
      y_row[x] = static_cast<std::uint8_t>(std::clamp(luma, 0, 255));
    }
  }

  for (int y = 0; y < dst_h; y += 2) {
    const int sy = (y * src_h) / std::max(1, dst_h);
    const std::uint8_t* row = src + static_cast<std::size_t>(sy) * src_pitch;
    std::uint8_t* uv_row =
        uv_plane + static_cast<std::size_t>(y / 2) * dst_w;
    for (int x = 0; x < dst_w; x += 2) {
      const int sx = (x * src_w) / std::max(1, dst_w);
      const std::uint8_t* px = row + static_cast<std::size_t>(sx) * 4U;
      const int b = px[0];
      const int g = px[1];
      const int r = px[2];
      const int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
      const int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
      uv_row[x] = static_cast<std::uint8_t>(std::clamp(u, 0, 255));
      uv_row[x + 1] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
    }
  }
}

class DxgiCaptureEncoder final : public CaptureEncoder {
 public:
  ~DxgiCaptureEncoder() override { stop(); }

  bool start(const CaptureOptions& options, FrameCallback on_frame,
             FailureCallback on_failure, std::string& error) override {
    stop();
    options_ = options;
    // Video encoders require even dimensions for NV12.
    options_.width =
        static_cast<std::uint16_t>(std::max<int>(2, options_.width & ~1));
    options_.height =
        static_cast<std::uint16_t>(std::max<int>(2, options_.height & ~1));
    options_.refresh_hz =
        static_cast<std::uint16_t>(std::max<int>(1, options_.refresh_hz));

    on_frame_ = std::move(on_frame);
    on_failure_ = std::move(on_failure);
    encoded_frames_.store(0, std::memory_order_relaxed);
    received_frames_.store(0, std::memory_order_relaxed);
    submitted_frames_ = 0;
    first_submit_time_ = {};
    consecutive_process_input_errors_ = 0;
    sync_fallback_attempted_ = false;
    gdi_fallback_mode_ = false;
    vp_zero_frames_ = 0;
    last_duplication_retry_ = {};
    last_idr_retry_ = {};
    last_encode_error_.clear();
    force_keyframe_.store(true, std::memory_order_release);
    cached_param_sets_.clear();
    last_nv12_.clear();

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
      error = "MFStartup failed";
      return false;
    }
    mf_started_ = true;
    if (!create_device_and_duplication(error) ||
        !create_encoder(error, false)) {
      cleanup_resources();
      return false;
    }
    running_.store(true, std::memory_order_release);
    capture_thread_ = std::thread([this] { capture_loop(); });
    return true;
  }

  void request_keyframe() override {
    force_keyframe_.store(true, std::memory_order_release);
  }

  void stop() override {
    running_.store(false, std::memory_order_release);
    if (capture_thread_.joinable()) {
      capture_thread_.join();
    }
    cleanup_resources();
  }

  [[nodiscard]] bool running() const override {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t encoded_frame_count() const override {
    return encoded_frames_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t received_frame_count() const override {
    return received_frames_.load(std::memory_order_relaxed);
  }

 private:
  void cleanup_resources() {
    shutdown_encoder_only();
    duplication_.Reset();
    output_.Reset();
    bgra_staging_.Reset();
    readback_.Reset();
    vp_input_view_.Reset();
    vp_output_view_.Reset();
    vp_input_.Reset();
    nv12_.Reset();
    processor_.Reset();
    enumerator_.Reset();
    video_context_.Reset();
    video_device_.Reset();
    device_context_.Reset();
    device_.Reset();
    target_monitor_ = nullptr;
    vp_verified_ = false;
    vp_zero_frames_ = 0;
    if (mf_started_) {
      MFShutdown();
      mf_started_ = false;
    }
  }

  bool create_device_and_duplication(std::string& error) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
      error = "CreateDXGIFactory1 failed: " + hex_hr(hr);
      return false;
    }

    const HMONITOR target =
        options_.monitor_handle != 0
            ? reinterpret_cast<HMONITOR>(options_.monitor_handle)
            : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

    ComPtr<IDXGIAdapter1> matched_adapter;
    ComPtr<IDXGIOutput> matched_output;
    for (UINT adapter_idx = 0;; ++adapter_idx) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(adapter_idx, &adapter) ==
          DXGI_ERROR_NOT_FOUND) {
        break;
      }
      for (UINT out_idx = 0;; ++out_idx) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(out_idx, &output) == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == target) {
          matched_adapter = adapter;
          matched_output = output;
          monitor_rect_ = desc.DesktopCoordinates;
          break;
        }
      }
      if (matched_output != nullptr) {
        break;
      }
    }

    if (matched_adapter == nullptr || matched_output == nullptr) {
      error = "target monitor is not exposed by DXGI";
      return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    // Create the D3D11 device on the exact adapter that owns the target
    // output (required for hybrid laptops and IddCx virtual display drivers).
    hr = D3D11CreateDevice(
        matched_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device_, &selected, &device_context_);
    if (FAILED(hr)) {
      hr = D3D11CreateDevice(matched_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                             nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                             static_cast<UINT>(std::size(levels)),
                             D3D11_SDK_VERSION, &device_, &selected,
                             &device_context_);
    }
    if (FAILED(hr)) {
      error = "D3D11CreateDevice failed: " + hex_hr(hr);
      return false;
    }

    ComPtr<IDXGIOutput1> output1;
    hr = matched_output.As(&output1);
    if (FAILED(hr)) {
      error = "IDXGIOutput1 query failed: " + hex_hr(hr);
      return false;
    }
    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
      // Hybrid GPUs / remote desktop / certain IddCx drivers return
      // DXGI_ERROR_UNSUPPORTED or E_ACCESSDENIED for DXGI duplication; fall
      // back to GDI BitBlt capture instead of failing stream startup.
      duplication_.Reset();
      gdi_fallback_mode_ = true;
    }
    output_ = matched_output;
    target_monitor_ = target;
    return true;
  }

  // DXGI_ERROR_ACCESS_LOST (display mode change, secure desktop, driver reset)
  // drops the duplication while the D3D device stays usable. Retry on the
  // existing output with a backoff; until it succeeds the loop keeps pushing
  // GDI frames so the stream never stalls.
  void reopen_duplication() {
    const auto now = std::chrono::steady_clock::now();
    if ((now - last_duplication_retry_) < std::chrono::seconds(2)) {
      return;
    }
    last_duplication_retry_ = now;
    if (device_ == nullptr || output_ == nullptr) {
      return;
    }
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output_.As(&output1))) {
      return;
    }
    if (FAILED(output1->DuplicateOutput(device_.Get(), &duplication_))) {
      duplication_.Reset();
    }
  }

  void shutdown_encoder_only() {
    if (encoder_ != nullptr) {
      encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    event_gen_.Reset();
    codec_api_.Reset();
    encoder_.Reset();
    is_async_mft_ = false;
    need_input_count_ = 0;
  }

  bool try_activate_encoder(IMFActivate* activate, std::string& error) {
    shutdown_encoder_only();

    std::string mft_name = "MF-MFT";
    wchar_t* fname = nullptr;
    UINT32 flen = 0;
    if (SUCCEEDED(activate->GetAllocatedString(kMftFriendlyNameAttr, &fname,
                                               &flen)) &&
        fname != nullptr) {
      const std::string parsed = wide_to_utf8(fname);
      if (!parsed.empty()) {
        mft_name = parsed;
      }
      CoTaskMemFree(fname);
    }

    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&encoder_));
    if (FAILED(hr) || encoder_ == nullptr) {
      error = mft_name + " activation failed: " + hex_hr(hr);
      return false;
    }

    auto abort_activate = [&](std::string msg) {
      error = std::move(msg);
      shutdown_encoder_only();
      activate->ShutdownObject();
      return false;
    };

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(encoder_->GetAttributes(&attrs)) && attrs != nullptr) {
      UINT32 async_flag = 0;
      if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async_flag)) &&
          async_flag != 0) {
        if (FAILED(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) {
          return abort_activate(mft_name + " MF_TRANSFORM_ASYNC_UNLOCK failed");
        }
        is_async_mft_ = true;
        encoder_.As(&event_gen_);
        if (event_gen_ == nullptr) {
          return abort_activate(mft_name +
                                " async MFT missing IMFMediaEventGenerator");
        }
      }
    }

    const UINT32 bitrate = compute_target_bitrate_bps(
        options_.width, options_.height, options_.refresh_hz, options_.hevc);
    int chosen_fps = std::max<int>(1, options_.refresh_hz);
    HRESULT out_hr = E_FAIL;
    HRESULT in_hr = E_FAIL;
    if (!try_configure_mft_types(encoder_.Get(), options_.width, options_.height,
                                 options_.refresh_hz, options_.hevc, bitrate,
                                 chosen_fps, out_hr, in_hr)) {
      if (FAILED(out_hr)) {
        return abort_activate(mft_name + " SetOutputType failed: " +
                              hex_hr(out_hr));
      }
      return abort_activate(mft_name + " SetInputType failed: " +
                            hex_hr(in_hr));
    }
    encoder_fps_ = chosen_fps;

    if (SUCCEEDED(encoder_.As(&codec_api_)) && codec_api_ != nullptr) {
      VARIANT v{};
      VariantInit(&v);
      v.vt = VT_BOOL;
      v.boolVal = VARIANT_TRUE;
      (void)codec_api_->SetValue(&kCodecApiCommonLowLatency, &v);

      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = 0;
      (void)codec_api_->SetValue(&kCodecApiMPVDefaultBPictureCount, &v);

      // Use CBR (0) and long GOP so bitrate stays smooth over USB without
      // periodic IDR spikes saturating the 64-fragment packet budget.
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = 0;
      (void)codec_api_->SetValue(&kCodecApiCommonRateControlMode, &v);

      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = 0;
      (void)codec_api_->SetValue(&kCodecApiCommonQualityVsSpeed, &v);

      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = static_cast<ULONG>(std::clamp(chosen_fps * 120, 600, 6000));
      (void)codec_api_->SetValue(&kCodecApiMPVGOPSize, &v);

      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = static_cast<ULONG>(std::min<std::uint64_t>(
          24'000'000ULL, static_cast<std::uint64_t>(bitrate) * 135ULL / 100ULL));
      (void)codec_api_->SetValue(&kCodecApiCommonMaxBitRate, &v);
    }

    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr) && hr != E_NOTIMPL) {
      return abort_activate(mft_name + " BEGIN_STREAMING failed: " +
                            hex_hr(hr));
    }
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr) && hr != E_NOTIMPL) {
      return abort_activate(mft_name + " START_OF_STREAM failed: " +
                            hex_hr(hr));
    }
    encoder_name_ =
        mft_name + (is_async_mft_ ? " [async]" : " [sync]") + " @" +
        std::to_string(encoder_fps_) + "Hz";
    return true;
  }

  bool create_encoder(std::string& error, bool sync_only) {
    MFT_REGISTER_TYPE_INFO input_type{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output_type{
        MFMediaType_Video,
        options_.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};

    const UINT32 all_flags[] = {
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
    };
    const UINT32 sync_flags[] = {
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
    };
    const UINT32* search_flags = sync_only ? sync_flags : all_flags;
    const std::size_t search_count = sync_only ? 1U : 3U;

    for (std::size_t fi = 0; fi < search_count; ++fi) {
      IMFActivate** activates = nullptr;
      UINT32 count = 0;
      const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, search_flags[fi],
                                   &input_type, &output_type, &activates, &count);
      if (FAILED(hr) || count == 0 || activates == nullptr) {
        continue;
      }
      for (UINT32 i = 0; i < count; ++i) {
        std::string attempt_error;
        const bool ok = try_activate_encoder(activates[i], attempt_error);
        if (ok) {
          for (UINT32 j = i; j < count; ++j) {
            activates[j]->Release();
          }
          CoTaskMemFree(activates);
          return true;
        }
        error = attempt_error;
        activates[i]->Release();
      }
      CoTaskMemFree(activates);
    }
    if (error.empty()) {
      error = options_.hevc ? "no usable HEVC Media Foundation encoder"
                            : "no usable H.264 Media Foundation encoder";
    }
    return false;
  }

  void capture_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(com_hr);

    auto last_static_submit =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto last_encode_submit =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const int target_hz = std::max<int>(1, options_.refresh_hz);
    const UINT acquire_ms =
        static_cast<UINT>(std::clamp(1000 / target_hz, 4, 16));
    const auto min_interval = std::chrono::microseconds(
        (1'000'000LL * 100LL) /
        (105LL * static_cast<long long>(target_hz)));

    while (running_.load(std::memory_order_acquire)) {
      if (gdi_fallback_mode_) {
        const auto frame_start = std::chrono::steady_clock::now();
        capture_gdi_fallback();
        if (!last_nv12_.empty()) {
          received_frames_.fetch_add(1, std::memory_order_relaxed);
          const auto timestamp =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  frame_start.time_since_epoch())
                  .count();
          submit_nv12(last_nv12_, static_cast<std::uint64_t>(timestamp));
        }
        const int target_ms =
            std::max(1, 1000 / std::clamp<int>(options_.refresh_hz, 1, 120));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - frame_start);
        if (elapsed.count() < target_ms) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(target_ms - elapsed.count()));
        }
        continue;
      }

      if (duplication_ == nullptr) {
        if (encoded_frames_.load(std::memory_order_relaxed) == 0) {
          const auto now = std::chrono::steady_clock::now();
          if ((now - last_static_submit) >= std::chrono::milliseconds(150)) {
            capture_gdi_fallback();
            if (!last_nv12_.empty()) {
              last_static_submit = now;
              const auto timestamp =
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      now.time_since_epoch())
                      .count();
              submit_nv12(last_nv12_, static_cast<std::uint64_t>(timestamp));
            }
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        (void)reopen_duplication();
        continue;
      }

      DXGI_OUTDUPL_FRAME_INFO info{};
      ComPtr<IDXGIResource> resource;
      const HRESULT hr =
          duplication_->AcquireNextFrame(acquire_ms, &info, &resource);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        const auto now = std::chrono::steady_clock::now();
        const auto now_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count());
        pump_async_events(now_us, false);
        const bool need_initial_or_idr =
            encoded_frames_.load(std::memory_order_relaxed) == 0 ||
            force_keyframe_.load(std::memory_order_acquire);
        if (need_initial_or_idr &&
            (now - last_static_submit) >= std::chrono::milliseconds(150)) {
          if (last_nv12_.empty()) {
            capture_gdi_fallback();
          }
          if (!last_nv12_.empty()) {
            last_static_submit = now;
            last_encode_submit = now;
            submit_nv12(last_nv12_, now_us);
          }
        }
        continue;
      }
      if (hr == DXGI_ERROR_ACCESS_LOST) {
        duplication_.Reset();
        continue;
      }
      if (FAILED(hr)) {
        // Fallback cleanly to GDI capture if DXGI duplication fails mid-stream.
        duplication_.Reset();
        gdi_fallback_mode_ = true;
        continue;
      }
      const bool has_image =
          (info.LastPresentTime.QuadPart != 0 || info.AccumulatedFrames > 0);
      if (!has_image) {
        duplication_->ReleaseFrame();
        const auto now = std::chrono::steady_clock::now();
        const auto now_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count());
        pump_async_events(now_us, false);
        if (last_nv12_.empty() ||
            ((encoded_frames_.load(std::memory_order_relaxed) == 0 ||
              force_keyframe_.load(std::memory_order_acquire)) &&
             (now - last_static_submit) >= std::chrono::milliseconds(150))) {
          if (last_nv12_.empty()) {
            capture_gdi_fallback();
          }
          if (!last_nv12_.empty()) {
            last_static_submit = now;
            last_encode_submit = now;
            submit_nv12(last_nv12_, now_us);
          }
        }
        continue;
      }

      const auto now = std::chrono::steady_clock::now();
      const auto now_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              now.time_since_epoch())
              .count());
      const bool need_immediate =
          encoded_frames_.load(std::memory_order_relaxed) == 0 ||
          force_keyframe_.load(std::memory_order_acquire);
      if (!need_immediate && (now - last_encode_submit) < min_interval) {
        resource.Reset();
        duplication_->ReleaseFrame();
        pump_async_events(now_us, false);
        continue;
      }

      ComPtr<ID3D11Texture2D> texture;
      D3D11_TEXTURE2D_DESC src_desc{};
      bool used_gpu_vp = false;
      bool copied = false;
      if (SUCCEEDED(resource.As(&texture)) && texture != nullptr) {
        received_frames_.fetch_add(1, std::memory_order_relaxed);
        copied = copy_source_for_readback(texture.Get(), src_desc, used_gpu_vp);
      }
      // Release the DXGI frame immediately after the GPU CopyResource so DWM is
      // never stalled while we run VideoProcessorBlt, readback Map, or MFT encode.
      texture.Reset();
      resource.Reset();
      duplication_->ReleaseFrame();

      if (copied && convert_copied_to_nv12(src_desc, used_gpu_vp, last_nv12_)) {
        last_static_submit = now;
        last_encode_submit = now;
        submit_nv12(last_nv12_, now_us);
      } else {
        pump_async_events(now_us, false);
      }
    }

    if (com_owned) {
      CoUninitialize();
    }
  }

  void capture_gdi_fallback() {
    if (target_monitor_ != nullptr) {
      MONITORINFOEXW mi{};
      mi.cbSize = sizeof(mi);
      if (GetMonitorInfoW(target_monitor_, &mi)) {
        monitor_rect_ = mi.rcMonitor;
      }
    }
    const int src_w = std::max(1, static_cast<int>(monitor_rect_.right -
                                                   monitor_rect_.left));
    const int src_h = std::max(1, static_cast<int>(monitor_rect_.bottom -
                                                   monitor_rect_.top));
    HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
      return;
    }
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (mem_dc == nullptr) {
      ReleaseDC(nullptr, screen_dc);
      return;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = src_w;
    bmi.bmiHeader.biHeight = -src_h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp != nullptr && bits != nullptr) {
      HGDIOBJ prev = SelectObject(mem_dc, bmp);
      if (BitBlt(mem_dc, 0, 0, src_w, src_h, screen_dc, monitor_rect_.left,
                 monitor_rect_.top, SRCCOPY | CAPTUREBLT)) {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0 &&
            ci.hCursor != nullptr &&
            PtInRect(&monitor_rect_, ci.ptScreenPos)) {
          ICONINFO ii{};
          if (GetIconInfo(ci.hCursor, &ii)) {
            const int cx =
                ci.ptScreenPos.x - monitor_rect_.left - static_cast<int>(ii.xHotspot);
            const int cy =
                ci.ptScreenPos.y - monitor_rect_.top - static_cast<int>(ii.yHotspot);
            DrawIconEx(mem_dc, cx, cy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
            if (ii.hbmMask != nullptr) DeleteObject(ii.hbmMask);
            if (ii.hbmColor != nullptr) DeleteObject(ii.hbmColor);
          }
        }
        convert_bgra_to_nv12(static_cast<const std::uint8_t*>(bits), src_w * 4,
                             src_w, src_h, options_.width, options_.height,
                             last_nv12_);
      }
      SelectObject(mem_dc, prev);
      DeleteObject(bmp);
    }
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
  }

  bool ensure_processor(const D3D11_TEXTURE2D_DESC& desc) {
    const UINT out_width = options_.width;
    const UINT out_height = options_.height;
    if (processor_ != nullptr && vp_input_ != nullptr &&
        processor_width_ == desc.Width && processor_height_ == desc.Height &&
        output_width_ == out_width && output_height_ == out_height) {
      return true;
    }
    processor_.Reset();
    enumerator_.Reset();
    vp_input_view_.Reset();
    vp_output_view_.Reset();
    vp_input_.Reset();
    nv12_.Reset();
    readback_.Reset();
    vp_verified_ = false;
    vp_zero_frames_ = 0;

    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    device_.As(&video_device);
    device_context_.As(&video_context);
    if (video_device == nullptr || video_context == nullptr) {
      return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = desc.Width;
    content.InputHeight = desc.Height;
    content.OutputWidth = out_width;
    content.OutputHeight = out_height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(video_device->CreateVideoProcessorEnumerator(
            &content, &enumerator_)) ||
        FAILED(video_device->CreateVideoProcessor(enumerator_.Get(), 0,
                                                  &processor_))) {
      return false;
    }
    video_device_ = video_device;
    video_context_ = video_context;
    processor_width_ = desc.Width;
    processor_height_ = desc.Height;
    output_width_ = out_width;
    output_height_ = out_height;

    const RECT src_rc{0, 0, static_cast<LONG>(desc.Width),
                      static_cast<LONG>(desc.Height)};
    const RECT dst_rc{0, 0, static_cast<LONG>(out_width),
                      static_cast<LONG>(out_height)};
    video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE,
                                                      &src_rc);
    video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE,
                                                    &dst_rc);
    video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE,
                                                      &dst_rc);

    D3D11_TEXTURE2D_DESC in_desc{};
    in_desc.Width = desc.Width;
    in_desc.Height = desc.Height;
    in_desc.MipLevels = 1;
    in_desc.ArraySize = 1;
    in_desc.Format = desc.Format;
    in_desc.SampleDesc.Count = 1;
    in_desc.SampleDesc.Quality = 0;
    in_desc.Usage = D3D11_USAGE_DEFAULT;
    in_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&in_desc, nullptr, &vp_input_))) {
      in_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (FAILED(device_->CreateTexture2D(&in_desc, nullptr, &vp_input_))) {
        in_desc.BindFlags = 0;
        if (FAILED(device_->CreateTexture2D(&in_desc, nullptr, &vp_input_))) {
          processor_.Reset();
          return false;
        }
      }
    }

    D3D11_TEXTURE2D_DESC target{};
    target.Width = out_width;
    target.Height = out_height;
    target.MipLevels = 1;
    target.ArraySize = 1;
    target.Format = DXGI_FORMAT_NV12;
    target.SampleDesc.Count = 1;
    target.SampleDesc.Quality = 0;
    target.Usage = D3D11_USAGE_DEFAULT;
    target.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&target, nullptr, &nv12_))) {
      target.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (FAILED(device_->CreateTexture2D(&target, nullptr, &nv12_))) {
        processor_.Reset();
        vp_input_.Reset();
        return false;
      }
    }
    target.Usage = D3D11_USAGE_STAGING;
    target.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    target.BindFlags = 0;
    if (FAILED(device_->CreateTexture2D(&target, nullptr, &readback_))) {
      processor_.Reset();
      vp_input_.Reset();
      nv12_.Reset();
      return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(video_device_->CreateVideoProcessorInputView(
            vp_input_.Get(), enumerator_.Get(), &input_desc,
            &vp_input_view_)) ||
        FAILED(video_device_->CreateVideoProcessorOutputView(
            nv12_.Get(), enumerator_.Get(), &output_desc,
            &vp_output_view_))) {
      vp_input_view_.Reset();
      vp_output_view_.Reset();
      processor_.Reset();
      vp_input_.Reset();
      nv12_.Reset();
      readback_.Reset();
      return false;
    }
    return true;
  }

  bool copy_source_for_readback(ID3D11Texture2D* source,
                                D3D11_TEXTURE2D_DESC& desc_out,
                                bool& used_gpu_vp) {
    source->GetDesc(&desc_out);
    if (!gpu_processor_failed_ && ensure_processor(desc_out)) {
      device_context_->CopyResource(vp_input_.Get(), source);
      used_gpu_vp = true;
      return true;
    }

    used_gpu_vp = false;
    if (bgra_staging_ == nullptr || bgra_w_ != desc_out.Width ||
        bgra_h_ != desc_out.Height) {
      bgra_staging_.Reset();
      D3D11_TEXTURE2D_DESC staging{};
      staging.Width = desc_out.Width;
      staging.Height = desc_out.Height;
      staging.MipLevels = 1;
      staging.ArraySize = 1;
      staging.Format = desc_out.Format;
      staging.SampleDesc.Count = 1;
      staging.Usage = D3D11_USAGE_STAGING;
      staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(device_->CreateTexture2D(&staging, nullptr, &bgra_staging_))) {
        return false;
      }
      bgra_w_ = desc_out.Width;
      bgra_h_ = desc_out.Height;
    }
    device_context_->CopyResource(bgra_staging_.Get(), source);
    return true;
  }

  bool convert_copied_to_nv12(const D3D11_TEXTURE2D_DESC& desc,
                              bool used_gpu_vp,
                              std::vector<std::uint8_t>& out) {
    if (used_gpu_vp && !gpu_processor_failed_ && processor_ != nullptr &&
        vp_input_view_ != nullptr && vp_output_view_ != nullptr) {
      D3D11_VIDEO_PROCESSOR_STREAM stream{};
      stream.Enable = TRUE;
      stream.pInputSurface = vp_input_view_.Get();
      if (SUCCEEDED(video_context_->VideoProcessorBlt(
              processor_.Get(), vp_output_view_.Get(), 0, 1, &stream))) {
        device_context_->CopyResource(readback_.Get(), nv12_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(device_context_->Map(readback_.Get(), 0, D3D11_MAP_READ,
                                           0, &mapped))) {
          const std::size_t width = output_width_;
          const std::size_t height = output_height_;
          out.resize(width * height * 3U / 2U);
          for (std::size_t row = 0; row < height; ++row) {
            std::memcpy(out.data() + row * width,
                        static_cast<const BYTE*>(mapped.pData) +
                            row * mapped.RowPitch,
                        width);
          }
          const std::size_t chroma_offset = width * height;
          const BYTE* chroma_source =
              static_cast<const BYTE*>(mapped.pData) +
              height * mapped.RowPitch;
          for (std::size_t row = 0; row < height / 2; ++row) {
            std::memcpy(out.data() + chroma_offset + row * width,
                        chroma_source + row * mapped.RowPitch, width);
          }
          device_context_->Unmap(readback_.Get(), 0);

          if (!vp_verified_ &&
              (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
               desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM)) {
            const std::size_t y_bytes = width * height;
            bool any_nonzero = false;
            const std::size_t step = std::max<std::size_t>(1, y_bytes / 32U);
            for (std::size_t pos = 0; pos < y_bytes; pos += step) {
              if (out[pos] != 0U) {
                any_nonzero = true;
                break;
              }
            }
            if (any_nonzero) {
              vp_verified_ = true;
              vp_zero_frames_ = 0;
            } else {
              // Newly attached virtual displays often output black frames for a
              // moment before DWM paints; only disable GPU VP after 30 consecutive
              // all-zero frames instead of permanently falling back on frame 1.
              ++vp_zero_frames_;
              if (vp_zero_frames_ >= 30) {
                gpu_processor_failed_ = true;
                return false;
              }
            }
          }
          return true;
        }
      }
      gpu_processor_failed_ = true;
      return false;
    }

    if (bgra_staging_ == nullptr) {
      return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(device_context_->Map(bgra_staging_.Get(), 0, D3D11_MAP_READ, 0,
                                    &mapped))) {
      return false;
    }
    convert_bgra_to_nv12(static_cast<const std::uint8_t*>(mapped.pData),
                         static_cast<int>(mapped.RowPitch),
                         static_cast<int>(desc.Width),
                         static_cast<int>(desc.Height), options_.width,
                         options_.height, out);
    device_context_->Unmap(bgra_staging_.Get(), 0);
    return true;
  }

  void pump_async_events(std::uint64_t timestamp_us, bool wait_for_input) {
    if (!is_async_mft_ || event_gen_ == nullptr) {
      return;
    }
    const int wait_ms =
        wait_for_input && encoded_frames_.load(std::memory_order_relaxed) != 0 &&
                !force_keyframe_.load(std::memory_order_acquire)
            ? async_input_wait_budget_ms(encoder_fps_)
            : 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (running_.load(std::memory_order_acquire)) {
      ComPtr<IMFMediaEvent> event;
      // Never block in GetEvent(0): poll with MF_EVENT_FLAG_NO_WAIT so stop()
      // and capture fallback cannot deadlock on a stalled async MFT.
      const HRESULT hr =
          event_gen_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
      if (hr == MF_E_NO_EVENTS_AVAILABLE) {
        if (std::chrono::steady_clock::now() >= deadline) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (FAILED(hr) || event == nullptr) {
        break;
      }
      MediaEventType type = MEUnknown;
      if (SUCCEEDED(event->GetType(&type))) {
        if (type == METransformNeedInput) {
          ++need_input_count_;
        } else if (type == METransformHaveOutput) {
          drain_output(timestamp_us);
        } else if (type == MEError) {
          HRESULT ev_hr = E_FAIL;
          (void)event->GetStatus(&ev_hr);
          last_encode_error_ = "async MFT MEError: " + hex_hr(ev_hr);
          break;
        }
      }
      if (wait_for_input && need_input_count_ > 0) {
        break;
      }
    }
  }

  bool fallback_to_sync_encoder() {
    if (sync_fallback_attempted_ || !is_async_mft_) {
      return false;
    }
    sync_fallback_attempted_ = true;
    const std::string stalled_name = encoder_name_;
    std::string fb_error;
    if (!create_encoder(fb_error, true)) {
      if (on_failure_) {
        on_failure_("异步编码器未出帧 (" + stalled_name +
                    ")，且同步回退失败: " + fb_error);
      }
      return false;
    }
    submitted_frames_ = 0;
    first_submit_time_ = {};
    consecutive_process_input_errors_ = 0;
    force_keyframe_.store(true, std::memory_order_release);
    return true;
  }

  void submit_nv12(const std::vector<std::uint8_t>& nv12,
                   std::uint64_t timestamp_us) {
    if (nv12.empty() || encoder_ == nullptr) {
      return;
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &buffer))) {
      return;
    }
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) {
      return;
    }
    std::memcpy(dst, nv12.data(), nv12.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(nv12.size()));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) {
      return;
    }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(timestamp_us) * 10);
    sample->SetSampleDuration(10'000'000LL /
                              std::max<int>(1, encoder_fps_));

    const auto now = std::chrono::steady_clock::now();
    if (first_submit_time_.time_since_epoch().count() == 0) {
      first_submit_time_ = now;
    }

    if (is_async_mft_) {
      pump_async_events(timestamp_us, true);
      if (need_input_count_ == 0) {
        if (force_keyframe_.load(std::memory_order_acquire) &&
            now - last_idr_retry_ >= std::chrono::milliseconds(120)) {
          last_idr_retry_ = now;
          if (codec_api_ != nullptr) {
            VARIANT v{};
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = 1;
            (void)codec_api_->SetValue(&kCodecApiVideoForceKeyFrame, &v);
          }
        }
        ++submitted_frames_;
        if (encoded_frames_.load(std::memory_order_relaxed) == 0 &&
            (submitted_frames_ >= 20 ||
             (now - first_submit_time_) >= std::chrono::milliseconds(1500))) {
          (void)fallback_to_sync_encoder();
        }
        return;
      }
    }

    if (force_keyframe_.exchange(false, std::memory_order_acq_rel)) {
      sample->SetUINT32(MFSampleExtension_CleanPoint, 1);
      if (codec_api_ != nullptr) {
        VARIANT v{};
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        (void)codec_api_->SetValue(&kCodecApiVideoForceKeyFrame, &v);
      }
    }

    const HRESULT hr = encoder_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
      last_encode_error_ = "ProcessInput failed: " + hex_hr(hr);
      ++consecutive_process_input_errors_;
      if (is_async_mft_ && encoded_frames_.load(std::memory_order_relaxed) == 0 &&
          consecutive_process_input_errors_ >= 5) {
        (void)fallback_to_sync_encoder();
      } else if (consecutive_process_input_errors_ == 15 && on_failure_) {
        on_failure_(last_encode_error_);
      }
      return;
    }
    consecutive_process_input_errors_ = 0;
    ++submitted_frames_;

    if (is_async_mft_) {
      if (need_input_count_ > 0) {
        --need_input_count_;
      }
      pump_async_events(timestamp_us, false);
      if (encoded_frames_.load(std::memory_order_relaxed) == 0 &&
          (submitted_frames_ >= 20 ||
           (now - first_submit_time_) >= std::chrono::milliseconds(1500))) {
        (void)fallback_to_sync_encoder();
      }
    } else {
      drain_output(timestamp_us);
    }
  }

  void drain_output(std::uint64_t timestamp_us) {
    int stream_changes = 0;
    for (;;) {
      MFT_OUTPUT_STREAM_INFO info{};
      if (FAILED(encoder_->GetOutputStreamInfo(0, &info))) {
        return;
      }
      const bool mft_provides_samples =
          (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
      ComPtr<IMFSample> sample;
      if (!mft_provides_samples) {
        if (FAILED(MFCreateSample(&sample)) || sample == nullptr) {
          return;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(
                std::max<DWORD>(info.cbSize, 64U * 1024U), &buffer)) ||
            buffer == nullptr) {
          return;
        }
        sample->AddBuffer(buffer.Get());
      }
      MFT_OUTPUT_DATA_BUFFER output{};
      output.dwStreamID = 0;
      output.pSample = sample.Get();
      DWORD status = 0;
      const HRESULT hr = encoder_->ProcessOutput(0, 1, &output, &status);
      if (output.pEvents != nullptr) {
        output.pEvents->Release();
      }
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE ||
          (output.dwStatus & MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE) != 0) {
        if (mft_provides_samples && output.pSample != nullptr) {
          output.pSample->Release();
          output.pSample = nullptr;
        }
        if (++stream_changes < 4) {
          ComPtr<IMFMediaType> next_type;
          if (SUCCEEDED(encoder_->GetOutputAvailableType(0, 0, &next_type)) &&
              next_type != nullptr) {
            (void)encoder_->SetOutputType(0, next_type.Get(), 0);
          }
          continue;
        }
        return;
      }
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return;
      }
      if (FAILED(hr)) {
        last_encode_error_ = "ProcessOutput failed: " + hex_hr(hr);
        return;
      }
      ComPtr<IMFSample> produced;
      if (mft_provides_samples) {
        if (output.pSample != nullptr) {
          produced.Attach(output.pSample);
          output.pSample = nullptr;
        }
      } else {
        produced = sample;
      }
      if (produced == nullptr) {
        return;
      }
      ComPtr<IMFMediaBuffer> contiguous;
      produced->ConvertToContiguousBuffer(&contiguous);
      BYTE* data = nullptr;
      DWORD length = 0;
      if (contiguous == nullptr ||
          FAILED(contiguous->Lock(&data, nullptr, &length))) {
        return;
      }
      std::vector<std::uint8_t> raw(data, data + length);
      contiguous->Unlock();

      auto annex_b = deskmate::platform::to_annex_b(raw, options_.hevc);
      const auto inline_sets =
          deskmate::platform::collect_parameter_sets(annex_b, options_.hevc);
      if (!inline_sets.empty()) {
        cached_param_sets_ = inline_sets;
      }

      bool keyframe = false;
      for (const auto& unit : deskmate::platform::split_annex_b(annex_b)) {
        if (unit.size > 0 &&
            deskmate::platform::is_keyframe_nal(annex_b[unit.offset],
                                                options_.hevc)) {
          keyframe = true;
          break;
        }
      }
      // Only prepend cached parameter sets when the MFT did not already emit
      // them inline with this IDR frame (prevents duplicate SPS/PPS).
      if (keyframe && inline_sets.empty() && !cached_param_sets_.empty()) {
        annex_b.insert(annex_b.begin(), cached_param_sets_.begin(),
                       cached_param_sets_.end());
      }

      encoded_frames_.fetch_add(1, std::memory_order_relaxed);
      EncodedAccessUnit unit;
      unit.annex_b = std::move(annex_b);
      unit.timestamp_us = timestamp_us;
      unit.keyframe = keyframe;
      if (on_frame_) {
        on_frame_(std::move(unit));
      }
      if (is_async_mft_) {
        // Async MFT emits one METransformHaveOutput per output sample.
        return;
      }
    }
  }

  CaptureOptions options_{};
  FrameCallback on_frame_;
  FailureCallback on_failure_;
  std::atomic<bool> running_{false};
  std::atomic<bool> force_keyframe_{true};
  std::atomic<std::uint64_t> encoded_frames_{0};
  std::atomic<std::uint64_t> received_frames_{0};
  bool mf_started_{false};
  bool is_async_mft_{false};
  bool gpu_processor_failed_{false};
  bool vp_verified_{false};
  int vp_zero_frames_{0};
  bool gdi_fallback_mode_{false};
  bool sync_fallback_attempted_{false};
  int need_input_count_{0};
  int encoder_fps_{60};
  int submitted_frames_{0};
  int consecutive_process_input_errors_{0};
  std::chrono::steady_clock::time_point first_submit_time_{};
  std::string encoder_name_;
  std::string last_encode_error_;
  HMONITOR target_monitor_{nullptr};
  RECT monitor_rect_{};
  std::thread capture_thread_;
  std::chrono::steady_clock::time_point last_duplication_retry_{};
  std::chrono::steady_clock::time_point last_idr_retry_{};

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> device_context_;
  ComPtr<IDXGIOutput> output_;
  ComPtr<IDXGIOutputDuplication> duplication_;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  ComPtr<ID3D11VideoProcessorInputView> vp_input_view_;
  ComPtr<ID3D11VideoProcessorOutputView> vp_output_view_;
  ComPtr<ID3D11Texture2D> vp_input_;
  ComPtr<ID3D11Texture2D> nv12_;
  ComPtr<ID3D11Texture2D> readback_;
  ComPtr<ID3D11Texture2D> bgra_staging_;
  UINT processor_width_{};
  UINT processor_height_{};
  UINT output_width_{};
  UINT output_height_{};
  UINT bgra_w_{};
  UINT bgra_h_{};

  ComPtr<IMFTransform> encoder_;
  ComPtr<IMFMediaEventGenerator> event_gen_;
  ComPtr<ICodecAPI> codec_api_;
  std::vector<std::uint8_t> cached_param_sets_;
  std::vector<std::uint8_t> last_nv12_;
};

}  // namespace

std::unique_ptr<CaptureEncoder> make_capture_encoder() {
  return std::make_unique<DxgiCaptureEncoder>();
}

std::vector<MonitorInfo> enumerate_monitors() {
  std::vector<MonitorInfo> monitors;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
        auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(context);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
          MonitorInfo entry;
          entry.handle = reinterpret_cast<std::uintptr_t>(monitor);
          entry.name = wide_to_utf8(info.szDevice);
          entry.display_id = stable_display_id(
              entry.name, static_cast<std::uint32_t>(out->size() + 1));

          std::string adapter_name;
          std::string adapter_id;
          DISPLAY_DEVICEW adapter_dd{};
          adapter_dd.cb = sizeof(adapter_dd);
          for (DWORD dev_idx = 0;
               EnumDisplayDevicesW(nullptr, dev_idx, &adapter_dd, 0);
               ++dev_idx) {
            if (_wcsicmp(adapter_dd.DeviceName, info.szDevice) == 0) {
              adapter_name = wide_to_utf8(adapter_dd.DeviceString);
              adapter_id = wide_to_utf8(adapter_dd.DeviceID);
              break;
            }
            ZeroMemory(&adapter_dd, sizeof(adapter_dd));
            adapter_dd.cb = sizeof(adapter_dd);
          }

          std::string monitor_name;
          std::string monitor_id;
          DISPLAY_DEVICEW mon_dd{};
          mon_dd.cb = sizeof(mon_dd);
          if (EnumDisplayDevicesW(info.szDevice, 0, &mon_dd, 0)) {
            monitor_name = wide_to_utf8(mon_dd.DeviceString);
            monitor_id = wide_to_utf8(mon_dd.DeviceID);
          }

          entry.is_virtual = is_virtual_display_adapter_name(adapter_name) ||
                             is_virtual_display_adapter_name(adapter_id) ||
                             is_virtual_display_adapter_name(monitor_name) ||
                             is_virtual_display_adapter_name(monitor_id);
          if (entry.is_virtual && !adapter_name.empty()) {
            entry.friendly_name = adapter_name;
          } else if (!monitor_name.empty()) {
            entry.friendly_name = monitor_name;
          } else if (!adapter_name.empty()) {
            entry.friendly_name = adapter_name;
          } else {
            entry.friendly_name = entry.name;
          }

          entry.left = info.rcMonitor.left;
          entry.top = info.rcMonitor.top;
          entry.width = info.rcMonitor.right - info.rcMonitor.left;
          entry.height = info.rcMonitor.bottom - info.rcMonitor.top;
          DEVMODEW mode{};
          mode.dmSize = sizeof(mode);
          if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS,
                                   &mode) != 0 &&
              mode.dmDisplayFrequency > 1) {
            entry.refresh_hz = static_cast<int>(mode.dmDisplayFrequency);
          } else {
            entry.refresh_hz = 60;
          }
          entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
          out->push_back(std::move(entry));
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitors));
  return monitors;
}

std::vector<std::string> list_monitors() {
  std::vector<std::string> monitors;
  std::size_t index = 0;
  for (const auto& monitor : enumerate_monitors()) {
    char line[512]{};
    std::snprintf(line, sizeof(line), "%zu|%s|%dx%d@%d%s", index++,
                  monitor.name.c_str(), monitor.width, monitor.height,
                  monitor.refresh_hz, monitor.primary ? "|primary" : "");
    monitors.emplace_back(line);
  }
  return monitors;
}

bool probe_hevc_encoder_available() {
  const HRESULT mf_hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(mf_hr)) {
    return false;
  }
  MFT_REGISTER_TYPE_INFO input_type{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_type{MFMediaType_Video, MFVideoFormat_HEVC};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT |
                       MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
  const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input_type,
                               &output_type, &activates, &count);
  bool available = false;
  if (SUCCEEDED(hr) && count > 0 && activates != nullptr) {
    for (UINT32 i = 0; i < count; ++i) {
      if (!available) {
        ComPtr<IMFTransform> mft;
        if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&mft))) &&
            mft != nullptr) {
          ComPtr<IMFAttributes> attrs;
          UINT32 async_flag = 0;
          bool unlock_ok = true;
          if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs != nullptr &&
              SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async_flag)) &&
              async_flag != 0) {
            unlock_ok =
                SUCCEEDED(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
          }
          if (unlock_ok) {
            int chosen_fps = 60;
            HRESULT out_hr = E_FAIL;
            HRESULT in_hr = E_FAIL;
            available = try_configure_mft_types(
                mft.Get(), 1920, 1080, 60, true, 8'000'000U, chosen_fps,
                out_hr, in_hr);
          }
          mft.Reset();
          activates[i]->ShutdownObject();
        }
      }
      activates[i]->Release();
    }
    CoTaskMemFree(activates);
  }
  MFShutdown();
  return available;
}

bool arrange_extended_monitors(
    const std::vector<std::pair<std::uintptr_t, int>>& monitor_positions) {
  if (monitor_positions.empty()) {
    return false;
  }
  const auto monitors = enumerate_monitors();
  const MonitorInfo* primary = nullptr;
  for (const auto& m : monitors) {
    if (m.primary) {
      primary = &m;
      break;
    }
  }
  if (primary == nullptr && !monitors.empty()) {
    primary = &monitors.front();
  }
  if (primary == nullptr) {
    return false;
  }

  auto find_monitor = [&](std::uintptr_t handle) -> const MonitorInfo* {
    for (const auto& m : monitors) {
      if (m.handle == handle && !m.primary) {
        return &m;
      }
    }
    return nullptr;
  };

  struct Placement {
    const MonitorInfo* info;
    int x;
    int y;
  };
  std::vector<Placement> placements;

  std::vector<const MonitorInfo*> right_list, left_list, top_list, bottom_list;
  for (const auto& [handle, pos] : monitor_positions) {
    const auto* info = find_monitor(handle);
    if (info == nullptr) {
      continue;
    }
    switch (pos) {
      case 0:
        right_list.push_back(info);
        break;
      case 1:
        left_list.push_back(info);
        break;
      case 2:
        top_list.push_back(info);
        break;
      case 3:
        bottom_list.push_back(info);
        break;
      default:
        right_list.push_back(info);
        break;
    }
  }

  // Compute the bounding box of all fixed monitors not managed by this
  // arrangement (primary + any existing physical secondary monitors) so new
  // virtual displays are placed outside them without overlapping.
  int fixed_left = primary->left;
  int fixed_top = primary->top;
  int fixed_right = primary->left + primary->width;
  int fixed_bottom = primary->top + primary->height;
  for (const auto& m : monitors) {
    bool managed = false;
    for (const auto& [handle, pos] : monitor_positions) {
      (void)pos;
      if (m.handle == handle && !m.primary) {
        managed = true;
        break;
      }
    }
    if (!managed) {
      fixed_left = std::min(fixed_left, m.left);
      fixed_top = std::min(fixed_top, m.top);
      fixed_right = std::max(fixed_right, m.left + m.width);
      fixed_bottom = std::max(fixed_bottom, m.top + m.height);
    }
  }

  const int p_top = primary->top;
  const int p_mid_x = fixed_left + (fixed_right - fixed_left) / 2;

  int cursor_x = fixed_right;
  for (const auto* info : right_list) {
    const int y = p_top + (primary->height - info->height) / 2;
    placements.push_back({info, cursor_x, y});
    cursor_x += info->width;
  }

  int left_edge = fixed_left;
  for (const auto* info : left_list) {
    const int x = left_edge - info->width;
    const int y = p_top + (primary->height - info->height) / 2;
    placements.push_back({info, x, y});
    left_edge = x;
  }

  if (!top_list.empty()) {
    int total_w = 0;
    for (const auto* info : top_list) {
      total_w += info->width;
    }
    int x = p_mid_x - total_w / 2;
    int top_edge = fixed_top;
    for (const auto* info : top_list) {
      placements.push_back({info, x, top_edge - info->height});
      x += info->width;
    }
  }

  if (!bottom_list.empty()) {
    int total_w = 0;
    for (const auto* info : bottom_list) {
      total_w += info->width;
    }
    int x = p_mid_x - total_w / 2;
    for (const auto* info : bottom_list) {
      placements.push_back({info, x, fixed_bottom});
      x += info->width;
    }
  }

  if (placements.empty()) {
    return false;
  }

  bool any_changed = false;
  for (const auto& item : placements) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(reinterpret_cast<HMONITOR>(item.info->handle), &mi)) {
      continue;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
      continue;
    }
    if (dm.dmPosition.x == item.x && dm.dmPosition.y == item.y) {
      continue;
    }
    dm.dmFields = DM_POSITION;
    dm.dmPosition.x = item.x;
    dm.dmPosition.y = item.y;
    const LONG res = ChangeDisplaySettingsExW(
        mi.szDevice, &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (res == DISP_CHANGE_SUCCESSFUL) {
      any_changed = true;
    }
  }
  if (!any_changed) {
    return true;
  }
  return ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) ==
         DISP_CHANGE_SUCCESSFUL;
}

bool apply_monitor_layout(const std::vector<MonitorPlacement>& placements) {
  if (placements.empty()) {
    return false;
  }
  const auto monitors = enumerate_monitors();
  bool any_changed = false;
  for (const auto& p : placements) {
    std::wstring dev_name_w;
    if (!p.device_name.empty()) {
      dev_name_w = utf8_to_wide(p.device_name);
    } else if (p.handle != 0) {
      MONITORINFOEXW mi{};
      mi.cbSize = sizeof(mi);
      if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(p.handle), &mi)) {
        dev_name_w = mi.szDevice;
      }
    }
    if (dev_name_w.empty()) {
      for (const auto& m : monitors) {
        if (m.handle == p.handle && !m.name.empty()) {
          dev_name_w = utf8_to_wide(m.name);
          break;
        }
      }
    }
    if (dev_name_w.empty()) {
      continue;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(dev_name_w.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
      continue;
    }
    if (dm.dmPosition.x == p.left && dm.dmPosition.y == p.top) {
      continue;
    }
    dm.dmFields = DM_POSITION;
    dm.dmPosition.x = p.left;
    dm.dmPosition.y = p.top;
    const LONG res = ChangeDisplaySettingsExW(
        dev_name_w.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET,
        nullptr);
    if (res == DISP_CHANGE_SUCCESSFUL) {
      any_changed = true;
    }
  }
  if (!any_changed) {
    return true;
  }
  return ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) ==
         DISP_CHANGE_SUCCESSFUL;
}

}  // namespace deskmate::windows

#else

namespace deskmate::windows {

std::unique_ptr<CaptureEncoder> make_capture_encoder() {
  class Unsupported final : public CaptureEncoder {
   public:
    bool start(const CaptureOptions&, FrameCallback, FailureCallback,
               std::string& error) override {
      error = "Windows capture is only available in a Windows build";
      return false;
    }
    void request_keyframe() override {}
    void stop() override {}
    [[nodiscard]] bool running() const override { return false; }
  };
  return std::make_unique<Unsupported>();
}

std::vector<MonitorInfo> enumerate_monitors() { return {}; }

std::vector<std::string> list_monitors() { return {}; }

bool probe_hevc_encoder_available() { return false; }

bool arrange_extended_monitors(
    const std::vector<std::pair<std::uintptr_t, int>>&) {
  return false;
}

bool apply_monitor_layout(const std::vector<MonitorPlacement>&) {
  return false;
}

}  // namespace deskmate::windows

#endif
