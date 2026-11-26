#include "deskmate/windows/capture_encoder.hpp"
#include "deskmate/platform/annex_b.hpp"
#include "deskmate/windows/virtual_display.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <memory>
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
  if (negotiated_hz > 0) {
    return normalized > 0 ? std::min(normalized, negotiated_hz)
                          : negotiated_hz;
  }
  if (normalized > 0) {
    return normalized;
  }
  return fallback_hz > 0 ? fallback_hz : 60;
}

int resolve_advertised_refresh_hz(int preferred_hz, bool explicit_fallback,
                                  int fallback_hz) noexcept {
  (void)preferred_hz;
  if (explicit_fallback) {
    return fallback_hz > 0 ? fallback_hz : 60;
  }
  return 120;
}

FramePacer::FramePacer(int fps)
    : period_us_(1'000'000LL / std::clamp(fps, 1, 240)) {}

std::int64_t FramePacer::next_deadline_us(
    std::int64_t submitted_at_us) noexcept {
  if (next_deadline_us_ < 0 ||
      submitted_at_us - next_deadline_us_ >= period_us_ * 4) {
    next_deadline_us_ = submitted_at_us;
  }
  next_deadline_us_ += period_us_;
  return next_deadline_us_;
}

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
  // macOS uses the same formula but clamps to [8M, 45M]. We adopt the same
  // bounds: the previous 14 Mbps ceiling was too aggressive for 90/120 Hz
  // at high resolutions (e.g. 2848x1320), causing the encoder to starve and
  // produce bursty oversized P-frames that overwhelmed the USB scheduler.
  return static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(std::llround(raw), 8'000'000LL, 45'000'000LL));
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
#include <windows.h>
#include <avrt.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>
#include <strmif.h>  // ICodecAPI (codecapi.h only declares GUIDs)
#include <wrl/client.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "avrt.lib")
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
constexpr GUID kCodecApiCommonBufferSize = {
    0x0db96574,
    0xb6a4,
    0x4c8b,
    {0x81, 0x06, 0x37, 0x73, 0xde, 0x03, 0x10, 0xcd}};
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
  struct PendingFrame {
    Microsoft::WRL::ComPtr<IMFSample> sample;
    std::uint64_t timestamp_us{};
    bool is_gpu_sample{false};
    int pool_slot{-1};
  };

  // The MFT invokes IMFAsyncCallback on its own worker thread. The bridge is
  // shared with the callback so a late notification can never dereference a
  // destroyed encoder; `alive` gates every access to the encoder's event.
  struct EventBridge {
    std::atomic<bool> alive{true};
    // Non-owning: points at the encoder's auto-reset work_event_.
    HANDLE event{nullptr};
    std::mutex mutex;
    // Queue, not a single slot: if the MFT posts a second notification before
    // the encoder thread calls EndGetEvent(), replacing the older result would
    // drop a NeedInput/HaveOutput event and stall the pipeline.
    std::deque<IMFAsyncResult*> results;

    ~EventBridge() {
      for (auto* result : results) {
        if (result != nullptr) {
          result->Release();
        }
      }
    }
  };

  class MftEventCallback final : public IMFAsyncCallback {
   public:
    explicit MftEventCallback(std::shared_ptr<EventBridge> bridge)
        : bridge_(std::move(bridge)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void** object) override {
      if (object == nullptr) {
        return E_POINTER;
      }
      if (riid == IID_IUnknown || riid == IID_IMFAsyncCallback) {
        *object = static_cast<IMFAsyncCallback*>(this);
        AddRef();
        return S_OK;
      }
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return static_cast<ULONG>(++refs_);
    }

    ULONG STDMETHODCALLTYPE Release() override {
      const auto remaining = static_cast<ULONG>(--refs_);
      if (remaining == 0) {
        delete this;
      }
      return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetParameters(DWORD* flags,
                                            DWORD* queue) override {
      if (flags == nullptr || queue == nullptr) {
        return E_POINTER;
      }
      *flags = 0;
      *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override {
      if (result == nullptr ||
          !bridge_->alive.load(std::memory_order_acquire)) {
        return S_OK;
      }
      result->AddRef();
      {
        std::lock_guard lock(bridge_->mutex);
        if (!bridge_->alive.load(std::memory_order_acquire)) {
          result->Release();
          return S_OK;
        }
        bridge_->results.push_back(result);
      }
      (void)SetEvent(bridge_->event);
      return S_OK;
    }

   private:
    ~MftEventCallback() = default;

    std::atomic<long> refs_{1};
    std::shared_ptr<EventBridge> bridge_;
  };

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
    dropped_frames_.store(0, std::memory_order_relaxed);
    pool_forced_reuse_.store(0, std::memory_order_relaxed);
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
    if (work_event_ == nullptr) {
      work_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    }
    if (capture_wait_event_ == nullptr) {
      // Auto-reset and only raised by stop(): pacing uses its own high
      // resolution timer, so a capture wake can never leak into the frame
      // limiter and turn every wait into a 0 ms no-op.
      capture_wait_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    }
    if (work_event_ == nullptr || capture_wait_event_ == nullptr) {
      cleanup_resources();
      error = "CreateEvent failed";
      return false;
    }
    // stop() raises capture_wait_event_ to break the pacing wait; clear it
    // before restarting or every wait_until_us() returns immediately and the
    // capture loop spins at 100% CPU.
    (void)ResetEvent(capture_wait_event_.get());
    if (pacing_timer_ == nullptr) {
      pacing_timer_.reset(CreateWaitableTimerExW(
          nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
          TIMER_ALL_ACCESS));
      if (pacing_timer_ == nullptr) {
        pacing_timer_.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
      }
    }
    running_.store(true, std::memory_order_release);
    encoder_thread_ = std::thread([this] { encoder_loop(); });
    capture_thread_ = std::thread([this] { capture_loop(); });
    return true;
  }

  void request_keyframe() override {
    force_keyframe_.store(true, std::memory_order_release);
    signal_work();
  }

  void stop() override {
    running_.store(false, std::memory_order_release);
    if (capture_wait_event_ != nullptr) {
      (void)SetEvent(capture_wait_event_.get());
    }
    signal_work();
    if (capture_thread_.joinable()) {
      capture_thread_.join();
    }
    if (encoder_thread_.joinable()) {
      encoder_thread_.join();
    }
    pending_frames_.discard();
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

  [[nodiscard]] std::uint64_t dropped_frame_count() const override {
    return dropped_frames_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::string status_summary() const override {
    std::lock_guard lock(name_mutex_);
    const auto forced = pool_forced_reuse_.load(std::memory_order_relaxed);
    if (forced == 0) {
      return encoder_name_;
    }
    // Only shown once the NV12 pool had to break its "never reuse a texture
    // the encoder still holds" rule, which points at a driver/MFT stall.
    return encoder_name_ + " poolReuse=" + std::to_string(forced);
  }

 private:
  void cleanup_resources() {
    shutdown_encoder_only();
    duplication_.Reset();
    output_.Reset();
    bgra_staging_.Reset();
    readback_.Reset();
    vp_input_view_.Reset();
    vp_input_.Reset();
    {
      std::lock_guard lock(pool_mutex_);
      nv12_pool_.clear();
      submitted_slots_.clear();
      cached_slot_ = -1;
      pool_starved_since_ = {};
      pool_ready_ = false;
    }
    transient_gpu_texture_.Reset();
    processor_.Reset();
    enumerator_.Reset();
    video_context_.Reset();
    video_device_.Reset();
    dxgi_manager_.Reset();
    dxgi_reset_token_ = 0;
    device_context_.Reset();
    device_.Reset();
    target_monitor_ = nullptr;
    monitor_refresh_hz_ = 0;
    vp_verified_ = false;
    vp_zero_frames_ = 0;
    disable_d3d11_encoder_ = false;
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
          DEVMODEW mon_mode{};
          mon_mode.dmSize = sizeof(mon_mode);
          if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS,
                                   &mon_mode) != 0 &&
              mon_mode.dmDisplayFrequency > 1) {
            monitor_refresh_hz_ = static_cast<int>(mon_mode.dmDisplayFrequency);
          } else {
            monitor_refresh_hz_ = 60;
          }
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

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread)) && multithread != nullptr) {
      multithread->SetMultithreadProtected(TRUE);
    }
    dxgi_manager_.Reset();
    dxgi_reset_token_ = 0;
    if (SUCCEEDED(
            MFCreateDXGIDeviceManager(&dxgi_reset_token_, &dxgi_manager_)) &&
        dxgi_manager_ != nullptr) {
      if (FAILED(
              dxgi_manager_->ResetDevice(device_.Get(), dxgi_reset_token_))) {
        dxgi_manager_.Reset();
      }
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
    release_async_event_bridge();
    if (encoder_ != nullptr) {
      encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    event_gen_.Reset();
    codec_api_.Reset();
    encoder_.Reset();
    is_async_mft_ = false;
    d3d11_encoder_aware_.store(false, std::memory_order_release);
    capture_gpu_enabled_.store(false, std::memory_order_release);
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
      if (!disable_d3d11_encoder_ && dxgi_manager_ != nullptr) {
        UINT32 d3d11_aware = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware)) &&
            d3d11_aware != 0) {
          if (SUCCEEDED(encoder_->ProcessMessage(
                  MFT_MESSAGE_SET_D3D_MANAGER,
                  reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get())))) {
            d3d11_encoder_aware_.store(true, std::memory_order_release);
            capture_gpu_enabled_.store(true, std::memory_order_release);
          }
        }
      }
    }

    const UINT32 bitrate = compute_target_bitrate_bps(
        options_.width, options_.height, options_.refresh_hz, options_.hevc);
    int chosen_fps = std::max<int>(1, options_.refresh_hz);
    HRESULT out_hr = E_FAIL;
    HRESULT in_hr = E_FAIL;
    bool configured = try_configure_mft_types(
        encoder_.Get(), options_.width, options_.height, options_.refresh_hz,
        options_.hevc, bitrate, chosen_fps, out_hr, in_hr);
    if (!configured && d3d11_encoder_aware_.load(std::memory_order_acquire)) {
      (void)encoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
      d3d11_encoder_aware_.store(false, std::memory_order_release);
      configured = try_configure_mft_types(
          encoder_.Get(), options_.width, options_.height, options_.refresh_hz,
          options_.hevc, bitrate, chosen_fps, out_hr, in_hr);
    }
    if (!configured) {
      if (FAILED(out_hr)) {
        return abort_activate(mft_name + " SetOutputType failed: " +
                              hex_hr(out_hr));
      }
      return abort_activate(mft_name + " SetInputType failed: " +
                            hex_hr(in_hr));
    }
    encoder_fps_.store(chosen_fps, std::memory_order_release);

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

      // Use CBR (0) with a long GOP so bitrate stays smooth over USB without
      // periodic IDR spikes saturating the 64-fragment packet budget.
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = 0;
      (void)codec_api_->SetValue(&kCodecApiCommonRateControlMode, &v);

      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = 0;
      (void)codec_api_->SetValue(&kCodecApiCommonQualityVsSpeed, &v);

      // Match the macOS "long GOP with on-demand IDR" strategy. A periodic
      // IDR every 2 s forced a full-frame burst through the 64-fragment USB
      // budget at 90/120 Hz; 30 s is only a safety net because every
      // congestion event already requests a keyframe explicitly.
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = static_cast<ULONG>(chosen_fps * 30);
      (void)codec_api_->SetValue(&kCodecApiMPVGOPSize, &v);

      // Cap instantaneous bitrate to 1.3x average, matching macOS
      // DataRateLimits burst window. This prevents the encoder from
      // emitting oversized P-frames that exceed the 64-fragment USB budget.
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = static_cast<ULONG>(
          static_cast<std::uint64_t>(bitrate) * 130ULL / 100ULL);
      (void)codec_api_->SetValue(&kCodecApiCommonMaxBitRate, &v);

      // BufferSize controls the leaky-bucket window and is expressed in
      // BYTES, not bits. bitrate / 32 == bitrate / 8 bits-per-byte / 4, i.e.
      // the macOS burstWindowSec=0.25. The previous bitrate / 4 was eight
      // times larger (a 2 s bucket) and let complex scenes emit bursts that
      // overflowed the fragment budget.
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = static_cast<ULONG>(
          static_cast<std::uint64_t>(bitrate) / 32ULL);
      (void)codec_api_->SetValue(&kCodecApiCommonBufferSize, &v);
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
    {
      std::lock_guard nlock(name_mutex_);
      encoder_name_ =
          mft_name + (is_async_mft_ ? " [async]" : " [sync]") +
          (d3d11_encoder_aware_.load(std::memory_order_acquire) ? " [d3d11-zc]"
                                                                 : " [cpu-nv12]") +
          " @" + std::to_string(encoder_fps_.load(std::memory_order_acquire)) +
          "Hz";
    }
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
    DWORD task_index = 0;
    const HANDLE mmcs = AvSetMmThreadCharacteristicsW(L"Capture", &task_index);
    if (mmcs != nullptr) {
      (void)AvSetMmThreadPriority(mmcs, AVRT_PRIORITY_HIGH);
    }
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(com_hr);

    auto last_static_submit =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const int effective_hz =
        std::max<int>(1, std::min<int>(
                               options_.refresh_hz,
                               encoder_fps_.load(std::memory_order_acquire)));
    const UINT acquire_ms =
        static_cast<UINT>(std::clamp(1000 / (effective_hz * 2), 2, 8));
    const bool vsync_paced =
        (monitor_refresh_hz_ > 0 && monitor_refresh_hz_ <= effective_hz + 2);
    FramePacer pacer(effective_hz);
    std::int64_t next_submit_deadline_us = 0;

    while (running_.load(std::memory_order_acquire)) {
      if (gdi_fallback_mode_) {
        const auto now_us = current_time_us();
        wait_until_us(pacer.next_deadline_us(now_us));
        capture_gdi_fallback();
        if (!last_nv12_.empty()) {
          received_frames_.fetch_add(1, std::memory_order_relaxed);
          const auto timestamp =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          submit_nv12(last_nv12_, static_cast<std::uint64_t>(timestamp));
        }
        continue;
      }

      if (duplication_ == nullptr) {
        if (encoded_frames_.load(std::memory_order_relaxed) == 0) {
          const auto now = std::chrono::steady_clock::now();
          if ((now - last_static_submit) >= std::chrono::milliseconds(150)) {
            const auto now_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch())
                    .count());
            if (submit_cached_frame(now_us)) {
              last_static_submit = now;
            }
          }
        }
        (void)wait_for_capture(150);
        (void)reopen_duplication();
        continue;
      }

      DXGI_OUTDUPL_FRAME_INFO info{};
      ComPtr<IDXGIResource> resource;
      const HRESULT hr =
          duplication_->AcquireNextFrame(acquire_ms, &info, &resource);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // AcquireNextFrame already consumed the whole wait budget. Sleeping
        // until next_submit_deadline_us here would skip the DWM frame that
        // landed during that wait and add a full frame of jitter.
        const auto now = std::chrono::steady_clock::now();
        const auto now_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count());
        const bool need_initial_or_idr =
            encoded_frames_.load(std::memory_order_relaxed) == 0 ||
            force_keyframe_.load(std::memory_order_acquire);
        if (need_initial_or_idr &&
            (now - last_static_submit) >= std::chrono::milliseconds(150)) {
          if (submit_cached_frame(now_us)) {
            last_static_submit = now;
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
        const bool no_cached_frame =
            (!capture_gpu_enabled_.load(std::memory_order_acquire) ||
             !has_cached_slot()) &&
            last_nv12_.empty();
        if (no_cached_frame ||
            ((encoded_frames_.load(std::memory_order_relaxed) == 0 ||
              force_keyframe_.load(std::memory_order_acquire)) &&
             (now - last_static_submit) >= std::chrono::milliseconds(150))) {
          if (submit_cached_frame(now_us)) {
            last_static_submit = now;
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
      // Early-skip pacing: if the frame arrives well before the next submit
      // deadline, release it and wait. This caps the encode rate at the
      // negotiated fps when the display runs faster (e.g. 240Hz monitor
      // driving a 120Hz stream).
      if (!need_immediate && !vsync_paced && next_submit_deadline_us != 0 &&
          static_cast<std::int64_t>(now_us) + 1500 <
              next_submit_deadline_us) {
        resource.Reset();
        duplication_->ReleaseFrame();
        wait_until_us(next_submit_deadline_us);
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

      if (copied && encode_copied_frame(src_desc, used_gpu_vp, now_us)) {
        last_static_submit = now;
      }
      next_submit_deadline_us = pacer.next_deadline_us(now_us);
    }

    if (com_owned) {
      CoUninitialize();
    }
    if (mmcs != nullptr) {
      (void)AvRevertMmThreadCharacteristics(mmcs);
    }
  }

  static std::uint64_t current_time_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  void wait_until_us(std::int64_t deadline_us) {
    const auto remaining_us = std::max<std::int64_t>(
        0, deadline_us - static_cast<std::int64_t>(current_time_us()));
    if (remaining_us <= 0) {
      return;
    }
    // WaitForSingleObject has ~15.6 ms resolution without a timer tick bump.
    // At 120 Hz the whole frame budget is 8.3 ms, so pacing must use a
    // high-resolution waitable timer and stay interruptible by stop().
    if (pacing_timer_ == nullptr) {
      std::this_thread::sleep_for(std::chrono::microseconds(remaining_us));
      return;
    }
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(remaining_us * 10);
    if (!SetWaitableTimer(pacing_timer_.get(), &due, 0, nullptr, nullptr,
                          FALSE)) {
      std::this_thread::sleep_for(std::chrono::microseconds(remaining_us));
      return;
    }
    HANDLE handles[2]{pacing_timer_.get(), capture_wait_event_.get()};
    (void)WaitForMultipleObjects(2, handles, FALSE,
                                 static_cast<DWORD>((remaining_us + 999) / 1000));
  }

  void signal_work() {
    if (work_event_ != nullptr) {
      (void)SetEvent(work_event_.get());
    }
  }

  bool wait_for_encoder(DWORD timeout_ms) {
    if (!running_.load(std::memory_order_acquire)) {
      return false;
    }
    return WaitForSingleObject(work_event_.get(), timeout_ms) == WAIT_OBJECT_0 &&
           running_.load(std::memory_order_acquire);
  }

  bool wait_for_capture(DWORD timeout_ms) {
    if (!running_.load(std::memory_order_acquire)) {
      return false;
    }
    return WaitForSingleObject(capture_wait_event_.get(), timeout_ms) ==
               WAIT_OBJECT_0 &&
           running_.load(std::memory_order_acquire);
  }

  void encoder_loop() {
    DWORD task_index = 0;
    const HANDLE mmcs = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (mmcs != nullptr) {
      (void)AvSetMmThreadPriority(mmcs, AVRT_PRIORITY_HIGH);
    }
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(com_hr);
    while (running_.load(std::memory_order_acquire)) {
      auto pending = pending_frames_.take();
      if (!pending.has_value()) {
        pump_async_events(0);
        if (pending_frames_.peek().has_value()) {
          continue;
        }
        // No work queued: sleep until a frame, an IDR request, an async MFT
        // event or shutdown actually arrives. The previous unconditional
        // 4 ms sleep burned half the 120 Hz frame budget every iteration.
        // A 50 ms watchdog (instead of INFINITE) keeps a lost BeginGetEvent()
        // notification from wedging the encoder for the rest of the session
        // while still costing no wakeups in steady state.
        (void)wait_for_encoder(50);
        continue;
      }
      const std::uint64_t timestamp_us = pending->timestamp_us;
      const bool submitted = submit_sample(
          pending->sample.Get(), pending->timestamp_us, pending->is_gpu_sample,
          pending->pool_slot);
      if (!submitted) {
        // Keep the slot reserved while the frame goes back to the queue: the
        // texture must not be reused before the MFT actually accepts it.
        const int slot = pending->pool_slot;
        if (!pending_frames_.restore_if_empty(std::move(*pending)) &&
            slot >= 0) {
          rollback_slot(slot);
        }
      }
      // Drain whatever the MFT produced without blocking the capture thread.
      const bool drained = pump_async_events(timestamp_us);
      // The MFT rejected the input (no NeedInput credit yet) and no event was
      // ready: park on the wake handle instead of spinning, but keep a short
      // watchdog so a lost notification can never wedge the encoder.
      if (!submitted && !drained) {
        (void)wait_for_encoder(5);
      }
    }
    if (com_owned) {
      CoUninitialize();
    }
    if (mmcs != nullptr) {
      (void)AvRevertMmThreadCharacteristics(mmcs);
    }
  }

  bool queue_frame(ComPtr<IMFSample> sample, std::uint64_t timestamp_us,
                   bool is_gpu_sample, int pool_slot = -1) {
    if (sample == nullptr || !running_.load(std::memory_order_acquire)) {
      return false;
    }
    auto evicted = pending_frames_.replace(PendingFrame{
        std::move(sample), timestamp_us, is_gpu_sample, pool_slot});
    if (evicted.has_value()) {
      dropped_frames_.fetch_add(1, std::memory_order_relaxed);
      const int evicted_slot = evicted->pool_slot;
      // Release the sample (and its texture reference) before the slot can be
      // handed out again.
      evicted.reset();
      rollback_slot(evicted_slot);
    }
    signal_work();
    return true;
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
    vp_input_.Reset();
    {
      std::lock_guard lock(pool_mutex_);
      nv12_pool_.clear();
      submitted_slots_.clear();
      nv12_next_ = 0;
      cached_slot_ = -1;
      pool_starved_since_ = {};
      pool_ready_ = false;
    }
    transient_gpu_texture_.Reset();
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
    target.Usage = D3D11_USAGE_STAGING;
    target.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    target.BindFlags = 0;
    if (FAILED(device_->CreateTexture2D(&target, nullptr, &readback_))) {
      processor_.Reset();
      vp_input_.Reset();
      return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(video_device_->CreateVideoProcessorInputView(
            vp_input_.Get(), enumerator_.Get(), &input_desc,
            &vp_input_view_))) {
      vp_input_view_.Reset();
      processor_.Reset();
      vp_input_.Reset();
      readback_.Reset();
      return false;
    }
    create_nv12_pool();
    return true;
  }

  // Reusing a small ring of NV12 render targets removes the per-frame VRAM
  // allocation + output-view creation that cost 120 driver round trips per
  // second at high refresh rates. The ring is only rotated while fewer than
  // kPoolSize - 2 inputs are still inside the MFT, so a slot handed back out
  // can never still be read by the encoder.
  void create_nv12_pool() {
    D3D11_TEXTURE2D_DESC target{};
    target.Width = options_.width;
    target.Height = options_.height;
    target.MipLevels = 1;
    target.ArraySize = 1;
    target.Format = DXGI_FORMAT_NV12;
    target.SampleDesc.Count = 1;
    target.Usage = D3D11_USAGE_DEFAULT;
    target.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    std::vector<Nv12Slot> pool;
    pool.reserve(kNv12PoolSize);
    for (std::size_t i = 0; i < kNv12PoolSize; ++i) {
      Nv12Slot slot;
      if (FAILED(device_->CreateTexture2D(&target, nullptr, &slot.texture))) {
        target.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&target, nullptr,
                                            &slot.texture))) {
          std::lock_guard lock(pool_mutex_);
          nv12_pool_.clear();
          pool_ready_ = false;
          return;
        }
      }
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
      view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      if (FAILED(video_device_->CreateVideoProcessorOutputView(
              slot.texture.Get(), enumerator_.Get(), &view_desc,
              &slot.view))) {
        std::lock_guard lock(pool_mutex_);
        nv12_pool_.clear();
        pool_ready_ = false;
        return;
      }
      // Baseline: the pool plus the output view. Every extra reference means
      // a sample or the MFT is still reading the texture.
      slot.idle_refcount = slot.texture->AddRef();
      (void)slot.texture->Release();
      slot.idle_refcount -= 1U;
      pool.push_back(std::move(slot));
    }
    {
      std::lock_guard lock(pool_mutex_);
      nv12_pool_ = std::move(pool);
      nv12_next_ = 0;
      submitted_slots_.clear();
      cached_slot_ = -1;
      pool_starved_since_ = {};
      pool_ready_ = true;
    }
  }

  // A slot is free exactly when nothing outside the pool holds a reference to
  // its texture. Counting ProcessInput/METransformHaveOutput pairs instead
  // leaks the whole pool as soon as the MFT drops a frame internally or
  // reports MF_E_TRANSFORM_STREAM_CHANGE; the COM refcount cannot desync.
  [[nodiscard]] ULONG slot_refcount_locked(std::size_t index) const {
    auto* texture = nv12_pool_[index].texture.Get();
    if (texture == nullptr) {
      return 0;
    }
    const ULONG with_probe = texture->AddRef();
    (void)texture->Release();
    return with_probe;
  }

  [[nodiscard]] bool slot_idle_locked(std::size_t index) const {
    // +1 for the temporary probe reference taken by slot_refcount_locked().
    return slot_refcount_locked(index) == nv12_pool_[index].idle_refcount + 1U;
  }

  // A texture may only be reused when the encoder dropped its reference *and*
  // the matching HaveOutput has retired it. Some MFTs release the DXGI surface
  // inside ProcessInput while the GPU is still encoding, so the refcount alone
  // is not proof; the two signals together are.
  [[nodiscard]] bool slot_reusable_locked(std::size_t index) const {
    if (!slot_idle_locked(index)) {
      return false;
    }
    return std::find(submitted_slots_.begin(), submitted_slots_.end(),
                     static_cast<int>(index)) == submitted_slots_.end();
  }

  // Returns -1 when every slot is still held by the encoder; the caller then
  // leaves the frame queued instead of overwriting a texture in flight.
  int acquire_nv12_slot() {
    std::lock_guard lock(pool_mutex_);
    if (!pool_ready_ || nv12_pool_.empty()) {
      return -1;
    }
    const std::size_t count = nv12_pool_.size();
    // First pass: rotate through slots that are not retained for static-screen
    // repeats, so a live frame never clobbers the last encoded picture.
    for (std::size_t attempt = 0; attempt < nv12_pool_.size(); ++attempt) {
      const std::size_t index = (nv12_next_ + attempt) % count;
      if (cached_slot_ >= 0 &&
          static_cast<std::size_t>(cached_slot_) == index) {
        continue;
      }
      if (!slot_reusable_locked(index)) {
        continue;
      }
      nv12_next_ = (index + 1) % count;
      pool_starved_since_ = {};
      return static_cast<int>(index);
    }
    // Second pass: the retained static-screen slot may be recycled once every
    // other slot is genuinely in flight.
    if (cached_slot_ >= 0 &&
        static_cast<std::size_t>(cached_slot_) < count &&
        slot_reusable_locked(static_cast<std::size_t>(cached_slot_))) {
      const int index = cached_slot_;
      cached_slot_ = -1;
      nv12_next_ = (static_cast<std::size_t>(index) + 1) % count;
      pool_starved_since_ = {};
      return index;
    }
    // Every slot reports a live reference. If that lasts for a full second the
    // encoder is wedged (or a driver leaks refs); reuse the oldest submission
    // rather than freezing the picture forever, and surface it in the status.
    const auto now = std::chrono::steady_clock::now();
    if (pool_starved_since_.time_since_epoch().count() == 0) {
      pool_starved_since_ = now;
      return -1;
    }
    if (now - pool_starved_since_ < std::chrono::seconds(1)) {
      return -1;
    }
    if (submitted_slots_.empty()) {
      // Nothing was ever submitted through the pool: the reference count is
      // stuck for another reason, so re-baseline instead of stalling.
      for (std::size_t i = 0; i < count; ++i) {
        nv12_pool_[i].idle_refcount = slot_refcount_locked(i) - 1U;
      }
      pool_starved_since_ = {};
      return -1;
    }
    const int index = submitted_slots_.front();
    submitted_slots_.pop_front();
    if (cached_slot_ == index) {
      cached_slot_ = -1;
    }
    pool_forced_reuse_.fetch_add(1, std::memory_order_relaxed);
    pool_starved_since_ = {};
    return index;
  }

  void commit_inflight_slot(int index) {
    std::lock_guard lock(pool_mutex_);
    if (index < 0 || static_cast<std::size_t>(index) >= nv12_pool_.size()) {
      return;
    }
    // A static-screen repeat resubmits the cached slot; never register the
    // same texture twice, or one HaveOutput would look like two retirements.
    if (std::find(submitted_slots_.begin(), submitted_slots_.end(), index) !=
        submitted_slots_.end()) {
      return;
    }
    submitted_slots_.push_back(index);
  }

  void release_inflight_slot() {
    std::lock_guard lock(pool_mutex_);
    if (submitted_slots_.empty()) {
      return;
    }
    // Hardware MFTs may skip an input internally (frame dropping, stream
    // change), so "one HaveOutput = the oldest submission retired" is not
    // guaranteed. Retire the idle prefix instead: several older inputs may
    // have finished with this single output, and out-of-order retirement could
    // free a texture that is still being encoded.
    int last_retired = -1;
    while (!submitted_slots_.empty()) {
      const int index = submitted_slots_.front();
      if (index < 0 || static_cast<std::size_t>(index) >= nv12_pool_.size() ||
          !slot_idle_locked(static_cast<std::size_t>(index))) {
        break;
      }
      last_retired = index;
      submitted_slots_.pop_front();
    }
    if (last_retired < 0) {
      // No prefix entry is idle yet. Bound the list so a driver that never
      // releases its textures cannot grow it forever; acquire()'s 1 s
      // watchdog is the real recovery path for that case.
      if (submitted_slots_.size() <= nv12_pool_.size() * 2) {
        return;
      }
      last_retired = submitted_slots_.front();
      submitted_slots_.pop_front();
    }
    if (last_retired < 0 ||
        static_cast<std::size_t>(last_retired) >= nv12_pool_.size()) {
      return;
    }
    // Only retain a slot for static-screen repeats once the MFT has really let
    // go of it; otherwise the next repeat would resubmit a texture the encoder
    // is still reading. Keep the previous repeat source if this one is busy.
    if (slot_idle_locked(static_cast<std::size_t>(last_retired))) {
      cached_slot_ = last_retired;
    }
  }

  // A frame the encoder never accepted must give its slot straight back.
  void rollback_slot(int index) {
    std::lock_guard lock(pool_mutex_);
    if (index < 0) {
      return;
    }
    if (static_cast<std::size_t>(index) >= nv12_pool_.size()) {
      return;
    }
    submitted_slots_.erase(
        std::remove(submitted_slots_.begin(), submitted_slots_.end(), index),
        submitted_slots_.end());
    // Deliberately keep cached_slot_: the retained texture still holds the
    // last encoded picture, and it is only reused once the pool can prove the
    // encoder released it.
  }

  // Drops all in-flight bookkeeping. Only safe once the MFT has been torn
  // down, because at that point nothing can still be reading a pool texture.
  void reset_slot_bookkeeping() {
    std::lock_guard lock(pool_mutex_);
    submitted_slots_.clear();
    cached_slot_ = -1;
    pool_starved_since_ = {};
    pool_ready_ = !nv12_pool_.empty();
  }

  [[nodiscard]] bool has_cached_slot() const {
    std::lock_guard lock(pool_mutex_);
    return cached_slot_ >= 0;
  }

  // Claims the retained static-screen texture for a repeat submission.
  int take_cached_slot() {
    std::lock_guard lock(pool_mutex_);
    if (cached_slot_ < 0 ||
        static_cast<std::size_t>(cached_slot_) >= nv12_pool_.size()) {
      return -1;
    }
    if (!slot_reusable_locked(static_cast<std::size_t>(cached_slot_))) {
      // The previous repeat is still inside the MFT; resubmitting now would
      // feed the encoder a texture it is concurrently reading.
      return -1;
    }
    // Keep the slot marked as the static-screen source: while the repeat sits
    // in the MFT its buffer reference keeps acquire() away from the texture.
    return cached_slot_;
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

  bool readback_nv12_to_cpu(ID3D11Texture2D* tex,
                            std::vector<std::uint8_t>& out) {
    device_context_->CopyResource(readback_.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(device_context_->Map(readback_.Get(), 0, D3D11_MAP_READ, 0,
                                    &mapped))) {
      return false;
    }
    const std::size_t width = output_width_;
    const std::size_t height = output_height_;
    out.resize(width * height * 3U / 2U);
    for (std::size_t row = 0; row < height; ++row) {
      std::memcpy(
          out.data() + row * width,
          static_cast<const BYTE*>(mapped.pData) + row * mapped.RowPitch,
          width);
    }
    const std::size_t chroma_offset = width * height;
    const BYTE* chroma_source =
        static_cast<const BYTE*>(mapped.pData) + height * mapped.RowPitch;
    for (std::size_t row = 0; row < height / 2; ++row) {
      std::memcpy(out.data() + chroma_offset + row * width,
                  chroma_source + row * mapped.RowPitch, width);
    }
    device_context_->Unmap(readback_.Get(), 0);
    return true;
  }

  bool verify_nv12_nonzero(const std::vector<std::uint8_t>& out,
                           DXGI_FORMAT src_format) {
    if (vp_verified_ || (src_format != DXGI_FORMAT_B8G8R8A8_UNORM &&
                         src_format != DXGI_FORMAT_B8G8R8X8_UNORM)) {
      vp_verified_ = true;
      vp_zero_frames_ = 0;
      return true;
    }
    const std::size_t y_bytes =
        static_cast<std::size_t>(output_width_) * output_height_;
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
      return true;
    }
    // Newly attached virtual displays often output black frames for a moment
    // before DWM paints; only disable GPU VP after 30 consecutive all-zero
    // frames instead of permanently falling back on frame 1.
    ++vp_zero_frames_;
    if (vp_zero_frames_ >= 30) {
      gpu_processor_failed_ = true;
      return false;
    }
    return true;
  }

  bool encode_copied_frame(const D3D11_TEXTURE2D_DESC& desc, bool used_gpu_vp,
                           std::uint64_t timestamp_us) {
    const bool direct_gpu = capture_gpu_enabled_.load(std::memory_order_acquire);
    if (used_gpu_vp && !gpu_processor_failed_ && processor_ != nullptr &&
        vp_input_view_ != nullptr) {
      const int slot_index = acquire_nv12_slot();
      ComPtr<ID3D11Texture2D> transient_texture;
      ComPtr<ID3D11VideoProcessorOutputView> transient_view;
      ID3D11Texture2D* nv12 = nullptr;
      ID3D11VideoProcessorOutputView* output_view = nullptr;
      bool pool_ready = false;
      {
        std::lock_guard lock(pool_mutex_);
        pool_ready = pool_ready_;
      }
      if (slot_index >= 0) {
        // Take strong references instead of holding the pool lock: the
        // VideoProcessorBlt must not run under the bookkeeping mutex, and the
        // extra refs keep acquire_nv12_slot() from handing this texture out
        // again until the sample (and its buffer) is released.
        std::lock_guard lock(pool_mutex_);
        transient_texture =
            nv12_pool_[static_cast<std::size_t>(slot_index)].texture;
        transient_view = nv12_pool_[static_cast<std::size_t>(slot_index)].view;
        nv12 = transient_texture.Get();
        output_view = transient_view.Get();
      } else if (!pool_ready) {
        // Pool creation failed on this adapter: fall back to the old per-frame
        // allocation so streaming still works, just without the VRAM savings.
        D3D11_TEXTURE2D_DESC target{};
        target.Width = options_.width;
        target.Height = options_.height;
        target.MipLevels = 1;
        target.ArraySize = 1;
        target.Format = DXGI_FORMAT_NV12;
        target.SampleDesc.Count = 1;
        target.Usage = D3D11_USAGE_DEFAULT;
        target.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&target, nullptr,
                                            &transient_texture))) {
          target.BindFlags = D3D11_BIND_RENDER_TARGET;
          if (FAILED(device_->CreateTexture2D(&target, nullptr,
                                              &transient_texture))) {
            return false;
          }
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
        view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(video_device_->CreateVideoProcessorOutputView(
                transient_texture.Get(), enumerator_.Get(), &view_desc,
                &transient_view))) {
          return false;
        }
        nv12 = transient_texture.Get();
        output_view = transient_view.Get();
      } else {
        // Every pooled target is still inside the MFT: leave this frame queued
        // rather than clobber an in-flight input.
        return false;
      }
      D3D11_VIDEO_PROCESSOR_STREAM stream{};
      stream.Enable = TRUE;
      stream.pInputSurface = vp_input_view_.Get();
      if (SUCCEEDED(video_context_->VideoProcessorBlt(
              processor_.Get(), output_view, 0, 1, &stream))) {
        if (!vp_verified_) {
          if (!readback_nv12_to_cpu(nv12, last_nv12_) ||
              !verify_nv12_nonzero(last_nv12_, desc.Format)) {
            if (slot_index >= 0) {
              rollback_slot(slot_index);
            }
            return false;
          }
          if (!direct_gpu) {
            if (slot_index >= 0) {
              rollback_slot(slot_index);
            }
            submit_nv12(last_nv12_, timestamp_us);
            return true;
          }
        }
        if (direct_gpu) {
          device_context_->Flush();
          // Only a non-pooled fallback texture needs a retained reference; a
          // pooled slot is pinned by the in-flight FIFO instead.
          transient_gpu_texture_ =
              slot_index >= 0 ? nullptr : transient_texture;
          const bool queued = submit_gpu_nv12(nv12, timestamp_us, slot_index);
          if (!queued && slot_index >= 0) {
            rollback_slot(slot_index);
          }
          return queued;
        }
        if (readback_nv12_to_cpu(nv12, last_nv12_)) {
          if (slot_index >= 0) {
            rollback_slot(slot_index);
          }
          submit_nv12(last_nv12_, timestamp_us);
          return true;
        }
      }
      if (slot_index >= 0) {
        rollback_slot(slot_index);
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
                         options_.height, last_nv12_);
    device_context_->Unmap(bgra_staging_.Get(), 0);
    submit_nv12(last_nv12_, timestamp_us);
    return true;
  }

  bool pump_async_events(std::uint64_t timestamp_us) {
    if (!is_async_mft_ || event_gen_ == nullptr) {
      return false;
    }
    if (event_bridge_ == nullptr) {
      event_bridge_ = std::make_shared<EventBridge>();
      event_bridge_->event = work_event_.get();
    }
    if (!async_event_armed_) {
      if (event_callback_ == nullptr) {
        event_callback_.Attach(new MftEventCallback(event_bridge_));
      }
      const HRESULT arm_hr =
          event_gen_->BeginGetEvent(event_callback_.Get(), nullptr);
      if (FAILED(arm_hr)) {
        return false;
      }
      async_event_armed_ = true;
    }
    bool processed = false;
    while (running_.load(std::memory_order_acquire)) {
      IMFAsyncResult* pending_result = nullptr;
      {
        std::lock_guard lock(event_bridge_->mutex);
        if (event_bridge_->results.empty()) {
          break;
        }
        pending_result = event_bridge_->results.front();
        event_bridge_->results.pop_front();
      }
      ComPtr<IMFAsyncResult> result;
      result.Attach(pending_result);
      ComPtr<IMFMediaEvent> event;
      const HRESULT hr = event_gen_->EndGetEvent(result.Get(), &event);
      async_event_armed_ = false;
      // Re-arm immediately: the MFT stops posting notifications (and the
      // stream stalls) until BeginGetEvent is called again.
      if (running_.load(std::memory_order_acquire) &&
          SUCCEEDED(event_gen_->BeginGetEvent(event_callback_.Get(), nullptr))) {
        async_event_armed_ = true;
      }
      if (!running_.load(std::memory_order_acquire)) {
        // stop() is waiting for this thread: hand the async result back so the
        // callback object can be released cleanly.
        break;
      }
      if (FAILED(hr) || event == nullptr) {
        break;
      }
      processed = true;
      MediaEventType type = MEUnknown;
      if (SUCCEEDED(event->GetType(&type))) {
        if (type == METransformNeedInput) {
          ++need_input_count_;
        } else if (type == METransformHaveOutput) {
          // Drain first, retire second: the input texture is only guaranteed
          // to be free once ProcessOutput has handed the encoded sample back.
          // Retiring before the drain let the capture thread reuse a texture
          // the GPU was still reading.
          drain_output(timestamp_us);
          release_inflight_slot();
        } else if (type == MEError) {
          HRESULT ev_hr = E_FAIL;
          (void)event->GetStatus(&ev_hr);
          last_encode_error_ = "async MFT MEError: " + hex_hr(ev_hr);
          ++consecutive_process_input_errors_;
          if (encoded_frames_.load(std::memory_order_relaxed) == 0 &&
              consecutive_process_input_errors_ >= 5) {
            (void)fallback_to_sync_encoder();
          }
          return processed;
        }
      }
    }
    return processed;
  }

  void release_async_event_bridge() {
    if (event_bridge_ != nullptr) {
      event_bridge_->alive.store(false, std::memory_order_release);
      if (event_bridge_->event != nullptr) {
        (void)SetEvent(event_bridge_->event);
      }
      std::lock_guard lock(event_bridge_->mutex);
      for (auto* result : event_bridge_->results) {
        if (result != nullptr) {
          result->Release();
        }
      }
      event_bridge_->results.clear();
    }
    async_event_armed_ = false;
    event_callback_.Reset();
    event_bridge_.reset();
  }

  bool fallback_to_sync_encoder() {
    if (sync_fallback_attempted_ || !is_async_mft_) {
      return false;
    }
    sync_fallback_attempted_ = true;
    const std::string stalled_name = status_summary();
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
    reset_slot_bookkeeping();
    transient_gpu_texture_.Reset();
    force_keyframe_.store(true, std::memory_order_release);
    return true;
  }

  bool submit_cached_frame(std::uint64_t now_us) {
    if (capture_gpu_enabled_.load(std::memory_order_acquire) &&
        !gpu_processor_failed_ && has_cached_slot()) {
      const int slot = take_cached_slot();
      if (slot < 0) {
        return false;
      }
      ComPtr<ID3D11Texture2D> texture;
      {
        std::lock_guard lock(pool_mutex_);
        if (static_cast<std::size_t>(slot) < nv12_pool_.size()) {
          texture = nv12_pool_[static_cast<std::size_t>(slot)].texture;
        }
      }
      if (texture == nullptr) {
        return false;
      }
      // Track the repeat like any other submission so the pool will not hand
      // this texture out again until its HaveOutput retires it.
      const bool queued = submit_gpu_nv12(texture.Get(), now_us, slot);
      if (!queued) {
        rollback_slot(slot);
      }
      return queued;
    }
    if (capture_gpu_enabled_.load(std::memory_order_acquire) &&
        !gpu_processor_failed_ && transient_gpu_texture_ != nullptr) {
      return submit_gpu_nv12(transient_gpu_texture_.Get(), now_us);
    }
    if (last_nv12_.empty()) {
      capture_gdi_fallback();
    }
    if (!last_nv12_.empty()) {
      submit_nv12(last_nv12_, now_us);
      return true;
    }
    return false;
  }

  bool submit_gpu_nv12(ID3D11Texture2D* nv12, std::uint64_t timestamp_us,
                       int pool_slot = -1) {
    if (nv12 == nullptr || encoder_ == nullptr) {
      return false;
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),
                                          nv12, 0, FALSE,
                                          &buffer)) ||
        buffer == nullptr) {
      return false;
    }
    ComPtr<IMF2DBuffer> buf2d;
    DWORD contiguous_len = 0;
    if (SUCCEEDED(buffer.As(&buf2d)) && buf2d != nullptr &&
        SUCCEEDED(buf2d->GetContiguousLength(&contiguous_len)) &&
        contiguous_len > 0) {
      (void)buffer->SetCurrentLength(contiguous_len);
    }
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample)) || sample == nullptr) {
      return false;
    }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(timestamp_us) * 10);
    sample->SetSampleDuration(
        10'000'000LL /
        std::max<int>(1, encoder_fps_.load(std::memory_order_acquire)));
    return queue_frame(std::move(sample), timestamp_us, true, pool_slot);
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
    sample->SetSampleDuration(
        10'000'000LL /
        std::max<int>(1, encoder_fps_.load(std::memory_order_acquire)));
    (void)queue_frame(std::move(sample), timestamp_us, false);
  }

  bool submit_sample(IMFSample* sample, std::uint64_t timestamp_us,
                     bool is_gpu_sample, int pool_slot = -1) {
    if (sample == nullptr || encoder_ == nullptr) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (first_submit_time_.time_since_epoch().count() == 0) {
      first_submit_time_ = now;
    }

    if (is_async_mft_) {
      pump_async_events(timestamp_us);
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
        if (encoded_frames_.load(std::memory_order_relaxed) == 0 &&
            now - first_submit_time_ >= std::chrono::milliseconds(1500)) {
          (void)fallback_to_sync_encoder();
        }
        return false;
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

    const HRESULT hr = encoder_->ProcessInput(0, sample, 0);
    if (FAILED(hr)) {
      last_encode_error_ = "ProcessInput failed: " + hex_hr(hr);
      if (hr == MF_E_NOTACCEPTING) {
        return false;
      }
      ++consecutive_process_input_errors_;
      if (is_gpu_sample &&
          d3d11_encoder_aware_.load(std::memory_order_acquire) &&
          encoded_frames_.load(std::memory_order_relaxed) == 0 &&
          consecutive_process_input_errors_ >= 3) {
        disable_d3d11_encoder_ = true;
        std::string fb_error;
        if (create_encoder(fb_error, false)) {
          submitted_frames_ = 0;
          first_submit_time_ = {};
          consecutive_process_input_errors_ = 0;
          force_keyframe_.store(true, std::memory_order_release);
        } else if (on_failure_) {
          on_failure_("D3D11 零拷贝编码失败，回退失败: " + fb_error);
        }
      } else if (is_async_mft_ &&
                 encoded_frames_.load(std::memory_order_relaxed) == 0 &&
                 consecutive_process_input_errors_ >= 5) {
        (void)fallback_to_sync_encoder();
      } else if (consecutive_process_input_errors_ == 15 && on_failure_) {
        on_failure_(last_encode_error_);
      }
      return false;
    }
    consecutive_process_input_errors_ = 0;
    ++submitted_frames_;
    if (is_async_mft_) {
      commit_inflight_slot(pool_slot);
    } else if (pool_slot >= 0) {
      rollback_slot(pool_slot);
    }

    if (is_async_mft_) {
      if (need_input_count_ > 0) {
        --need_input_count_;
      }
      pump_async_events(timestamp_us);
      if (encoded_frames_.load(std::memory_order_relaxed) == 0 &&
          (submitted_frames_ >= 20 ||
           (now - first_submit_time_) >= std::chrono::milliseconds(1500))) {
        (void)fallback_to_sync_encoder();
      }
    } else {
      drain_output(timestamp_us);
    }
    return true;
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

      std::uint64_t out_timestamp_us = timestamp_us;
      LONGLONG sample_time_100ns = 0;
      if (SUCCEEDED(produced->GetSampleTime(&sample_time_100ns)) &&
          sample_time_100ns >= 0) {
        out_timestamp_us = static_cast<std::uint64_t>(sample_time_100ns / 10);
      }

      encoded_frames_.fetch_add(1, std::memory_order_relaxed);
      EncodedAccessUnit unit;
      unit.annex_b = std::move(annex_b);
      unit.timestamp_us = out_timestamp_us;
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
  std::atomic<std::uint64_t> dropped_frames_{0};
  std::atomic<std::uint64_t> pool_forced_reuse_{0};
  bool mf_started_{false};
  bool is_async_mft_{false};
  std::atomic<bool> d3d11_encoder_aware_{false};
  std::atomic<bool> capture_gpu_enabled_{false};
  bool disable_d3d11_encoder_{false};
  bool gpu_processor_failed_{false};
  bool vp_verified_{false};
  int vp_zero_frames_{0};
  bool gdi_fallback_mode_{false};
  bool sync_fallback_attempted_{false};
  int need_input_count_{0};
  std::atomic<int> encoder_fps_{60};
  int monitor_refresh_hz_{0};
  int submitted_frames_{0};
  int consecutive_process_input_errors_{0};
  std::chrono::steady_clock::time_point first_submit_time_{};
  mutable std::mutex name_mutex_;
  std::string encoder_name_;
  std::string last_encode_error_;
  HMONITOR target_monitor_{nullptr};
  RECT monitor_rect_{};
  std::thread capture_thread_;
  std::thread encoder_thread_;
  LatestFrameQueue<PendingFrame> pending_frames_;
  struct EventHandleDeleter {
    void operator()(void* handle) const {
      if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(handle);
      }
    }
  };
  std::unique_ptr<void, EventHandleDeleter> work_event_;
  std::unique_ptr<void, EventHandleDeleter> capture_wait_event_;
  std::unique_ptr<void, EventHandleDeleter> pacing_timer_;
  std::shared_ptr<EventBridge> event_bridge_;
  ComPtr<IMFAsyncCallback> event_callback_;
  bool async_event_armed_{false};
  std::chrono::steady_clock::time_point last_duplication_retry_{};
  std::chrono::steady_clock::time_point last_idr_retry_{};

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> device_context_;
  ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
  UINT dxgi_reset_token_{0};
  ComPtr<IDXGIOutput> output_;
  ComPtr<IDXGIOutputDuplication> duplication_;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  ComPtr<ID3D11VideoProcessorInputView> vp_input_view_;
  ComPtr<ID3D11Texture2D> vp_input_;
  struct Nv12Slot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11VideoProcessorOutputView> view;
    // Texture refcount when nothing but the pool and its output view hold it.
    // Comparing against this tells us the MFT has released the input.
    ULONG idle_refcount{};
  };
  static constexpr std::size_t kNv12PoolSize = 6;
  mutable std::mutex pool_mutex_;
  std::vector<Nv12Slot> nv12_pool_;
  std::deque<int> submitted_slots_;
  std::size_t nv12_next_{0};
  int cached_slot_{-1};
  std::chrono::steady_clock::time_point pool_starved_since_{};
  ComPtr<ID3D11Texture2D> transient_gpu_texture_;
  bool pool_ready_{false};
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
