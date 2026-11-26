#include "deskmate/windows/host_controller.hpp"
#include "deskmate/windows/host_session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>  // must follow windows.h for DWORD/DECLARE_HANDLE
#include <avrt.h>
#endif

namespace deskmate::windows {
namespace {

constexpr std::size_t kMaxLogLines = 200;

#ifdef _WIN32
void configure_realtime_process() {
  PROCESS_POWER_THROTTLING_STATE throttle{};
  throttle.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttle.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION |
                         PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  throttle.StateMask = 0;
  (void)SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                              &throttle, sizeof(throttle));
}
#endif

std::string state_badge_for(const std::string& state, bool streaming,
                            const std::string& err) {
  if (!err.empty()) {
    return "出错";
  }
  if (streaming) {
    return "会话已建立";
  }
  if (state == "awaiting_ready" || state == "awaiting_committed") {
    return "协商中";
  }
  if (state == "awaiting_client_hello" || state.rfind("awaiting_", 0) == 0 ||
      state == "paused") {
    return "USB 已连接";
  }
  return "USB 已连接";
}

}  // namespace

HostController::HostController(HostOptions options, std::string settings_path)
    : options_(options), store_(std::move(settings_path)) {
#ifdef _WIN32
  configure_realtime_process();
#endif
  const auto& prefs = store_.prefs();
  auto_virtual_display_ =
      options_.auto_match ? prefs.auto_virtual_display : false;
  default_position_ = prefs.default_position;
  allow_input_ = prefs.allow_input;
  touch_mode_ = prefs.touch_input_mode;
  preferred_refresh_hz_ =
      options_.refresh_hz_explicit
          ? normalize_preferred_refresh_hz(options_.refresh_hz)
          : normalize_preferred_refresh_hz(prefs.preferred_refresh_hz);
  auto_start_service_ = prefs.auto_start_service;
  updater_.set_preferences(prefs.auto_check_updates, prefs.update_mirror_prefix);

#ifdef _WIN32
  hevc_available_ = options_.codec_explicit
                        ? options_.hevc
                        : (options_.hevc && probe_hevc_encoder_available());
#else
  hevc_available_ = options_.hevc;
#endif
  auto probe_sink = make_win32_touch_sink();
  native_touch_available_ = probe_sink && probe_sink->native_touch_available();

  startup_status_ = startup_manager_.current_status();
  {
    std::lock_guard lock(mutex_);
    refresh_monitors_locked();
    if (!prefs.preferred_monitor_name.empty()) {
      for (const auto& m : monitors_) {
        if (m.name == prefs.preferred_monitor_name) {
          selected_monitor_ = m.handle;
          break;
        }
      }
    }
    if (selected_monitor_ == 0 && !monitors_.empty()) {
      for (const auto& m : monitors_) {
        if (!m.primary) {
          selected_monitor_ = m.handle;
          break;
        }
      }
      if (selected_monitor_ == 0) {
        selected_monitor_ = monitors_.front().handle;
      }
    }
  }
}

HostController::~HostController() { stop(); }

void HostController::start() {
  {
    std::lock_guard lock(mutex_);
    if (started_) {
      return;
    }
    started_ = true;
  }
  append_log("DeskMate Windows Host 启动");
  updater_.refresh_legacy_artifacts();
  if (auto_start_service_ || options_.dry_run || options_.headless) {
    start_service_internal();
  }
  if (!options_.dry_run && store_.prefs().auto_check_updates) {
    std::thread([this]() {
      std::this_thread::sleep_for(std::chrono::seconds(4));
      {
        std::lock_guard lock(mutex_);
        if (!started_ || !store_.prefs().auto_check_updates) {
          return;
        }
      }
      updater_.check_for_updates(false);
    }).detach();
  }
}

void HostController::stop() {
  {
    std::lock_guard lock(mutex_);
    if (!started_) {
      return;
    }
    started_ = false;
  }
  stop_service_internal();
}

void HostController::start_service_internal() {
  invalidate_vdd_status();
  {
    std::lock_guard lock(mutex_);
    if (service_running_) {
      return;
    }
    service_running_ = true;
    discovery_error_.clear();
    driver_missing_ = false;
    driver_hint_.clear();
    refresh_monitors_locked();
  }

  if (options_.dry_run) {
    auto capture = make_capture_encoder();
    CaptureOptions cap_opts;
    cap_opts.width = options_.width;
    cap_opts.height = options_.height;
    cap_opts.refresh_hz = options_.refresh_hz;
    cap_opts.hevc = hevc_available_;
    {
      std::lock_guard lock(mutex_);
      cap_opts.monitor_handle = selected_monitor_;
    }
    std::string err;
    if (!capture->start(
            cap_opts, [](EncodedAccessUnit) {},
            [this](std::string msg) {
              std::lock_guard lock(mutex_);
              dry_run_error_ = std::move(msg);
            },
            err)) {
      std::lock_guard lock(mutex_);
      dry_run_error_ = err;
      append_log("[dry-run] 采集启动失败: " + err);
    } else {
      std::lock_guard lock(mutex_);
      dry_run_capture_ = std::move(capture);
      dry_run_error_.clear();
      append_log("[dry-run] 本地采集测试已启动");
    }
    return;
  }

  auto disc = std::make_unique<LibusbAoaDiscovery>();
  disc->set_log_callback([this](std::string msg) { append_log(msg); });
  if (!disc->initialized()) {
    std::lock_guard lock(mutex_);
    discovery_error_ = disc->last_error();
    append_log("[discovery] 初始化失败: " + discovery_error_);
    return;
  }
  discovery_ = std::move(disc);
  discovery_stop_.store(false, std::memory_order_release);
  discovery_thread_ = std::thread([this] { discovery_loop(); });
  append_log("[service] USB AOA 发现服务已启动");
}

void HostController::stop_service_internal() {
  invalidate_vdd_status();
  discovery_stop_.store(true, std::memory_order_release);
  {
    std::lock_guard lock(discovery_mutex_);
    discovery_wakeup_ = true;
  }
  discovery_cv_.notify_all();
  if (discovery_thread_.joinable()) {
    discovery_thread_.join();
  }

  std::unique_ptr<CaptureEncoder> dry_cap;
  std::vector<std::shared_ptr<LinkChannel>> all;
  {
    std::lock_guard lock(mutex_);
    service_running_ = false;
    selected_link_id_ = 0;
    dry_cap = std::move(dry_run_capture_);
    for (auto& [id, ch] : channels_) {
      (void)id;
      all.push_back(ch);
    }
    for (auto& ch : finished_channels_) {
      all.push_back(ch);
    }
    channels_.clear();
    finished_channels_.clear();
    online_device_ids_.clear();
  }
  if (dry_cap != nullptr) {
    dry_cap->stop();
  }

  for (auto& ch : all) {
    ch->stop_requested.store(true, std::memory_order_release);
    wake_channel(ch);
    if (ch->session != nullptr) {
      ch->session->stop();
    }
  }
  for (auto& ch : all) {
    if (ch->worker.joinable()) {
      ch->worker.join();
    }
    stop_capture_for_channel(ch);
    if (ch->owns_virtual_display) {
      vdd_.destroy_display(ch->link_id);
      invalidate_vdd_status();
      ch->owns_virtual_display = false;
    }
    if (ch->session != nullptr) {
      ch->session->close_transport();
    }
    if (ch->injector != nullptr) {
      ch->injector->reset();
    }
  }
  vdd_.destroy_all();
  invalidate_vdd_status();
  discovery_.reset();
  update_power_assertion();
  append_log("[service] USB AOA 服务已停止");
}

void HostController::append_log(const std::string& line) {
  std::lock_guard lock(log_mutex_);
  if (logs_.size() >= kMaxLogLines) {
    logs_.pop_front();
  }
  logs_.push_back(line);
  if (options_.headless) {
    std::fprintf(stderr, "%s\n", line.c_str());
  }
}

void HostController::reap_finished_channels() {
  std::vector<std::shared_ptr<LinkChannel>> to_reap;
  {
    std::lock_guard lock(mutex_);
    for (auto it = channels_.begin(); it != channels_.end();) {
      if (it->second->finished.load(std::memory_order_acquire)) {
        to_reap.push_back(it->second);
        it = channels_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& ch : finished_channels_) {
      to_reap.push_back(ch);
    }
    finished_channels_.clear();
    if (selected_link_id_ != 0 && channels_.count(selected_link_id_) == 0) {
      selected_link_id_ =
          channels_.empty() ? 0 : channels_.begin()->first;
    }
  }
  for (auto& ch : to_reap) {
    if (ch->worker.joinable()) {
      ch->worker.join();
    }
    stop_capture_for_channel(ch);
    if (ch->owns_virtual_display) {
      vdd_.destroy_display(ch->link_id);
      ch->owns_virtual_display = false;
      append_log("[link " + std::to_string(ch->link_id) + "] 已移除虚拟扩展屏");
    }
    if (ch->session != nullptr) {
      ch->session->close_transport();
    }
    if (ch->injector != nullptr) {
      ch->injector->reset();
    }
  }
  update_power_assertion();
}

void HostController::discovery_loop() {
  while (!discovery_stop_.load(std::memory_order_acquire)) {
    reap_finished_channels();
    if (discovery_ == nullptr) {
      break;
    }

    std::unordered_set<std::uint16_t> owned_bus_addrs;
    std::unordered_set<std::string> active_keys;
    std::unordered_set<std::string> active_ids;
    {
      std::lock_guard lock(mutex_);
      for (const auto& [id, ch] : channels_) {
        (void)id;
        active_keys.insert(ch->key);
        if (!ch->device_id.empty()) {
          active_ids.insert(ch->device_id);
        }
        owned_bus_addrs.insert(static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(ch->identity.bus) << 8U) |
            static_cast<std::uint16_t>(ch->identity.address)));
      }
    }

    const auto scanned = discovery_->scan_all_devices(owned_bus_addrs);
    std::string pnp_summary;
    bool pnp_driver_missing = false;
    std::string pnp_driver_hint;
    discovery_->probe_pnp_aoa_status(pnp_summary, pnp_driver_missing,
                                     pnp_driver_hint);

    std::string usb_summary;
    for (std::size_t idx = 0; idx < scanned.size(); ++idx) {
      if (idx > 0) {
        usb_summary += "; ";
      }
      char buf[64]{};
      std::snprintf(buf, sizeof(buf), "%04x:%04x", scanned[idx].vendor_id,
                    scanned[idx].product_id);
      usb_summary += buf;
      usb_summary += scanned[idx].in_accessory_mode ? "(aoa" : "(cand";
      if (!scanned[idx].identity.port_path.empty()) {
        usb_summary += ",port=" + scanned[idx].identity.port_path;
      }
      if (!scanned[idx].interfaces_summary.empty()) {
        usb_summary += "," + scanned[idx].interfaces_summary;
      }
      usb_summary += ")";
    }

    bool usb_summary_changed = false;
    bool pnp_summary_changed = false;
    std::set<std::string> next_online;
    std::vector<std::string> next_blocked;

    {
      std::lock_guard lock(mutex_);
      if (usb_summary != last_usb_scan_summary_) {
        last_usb_scan_summary_ = usb_summary;
        usb_summary_changed = !usb_summary.empty();
      }
      if (pnp_summary != last_pnp_aoa_summary_) {
        last_pnp_aoa_summary_ = pnp_summary;
        pnp_summary_changed = !pnp_summary.empty();
      }
      for (const auto& dev : scanned) {
        const std::string dev_id = deskmate::platform::aoa_device_id(dev.identity);
        const std::string dev_name =
            deskmate::platform::aoa_display_name(dev.identity);
        if (!dev_id.empty()) {
          next_online.insert(dev_id);
          store_.record_seen_device(dev_id, dev_name,
                                    dev.identity.manufacturer,
                                    dev.identity.serial, {}, false);
          if (!store_.is_allowed(dev_id)) {
            next_blocked.push_back(store_.display_name_for(dev_id));
          }
        }
      }
      online_device_ids_ = std::move(next_online);
      blocked_device_names_ = next_blocked;
    }
    if (usb_summary_changed) {
      append_log("[discovery] USB 设备枚举: " + usb_summary);
    }
    if (pnp_summary_changed) {
      append_log("[discovery] PnP 设备节点: " + pnp_summary);
    }

    bool any_driver_missing = false;
    std::string latest_driver_hint;
    std::string latest_error;

    for (const auto& dev : scanned) {
      if (discovery_stop_.load(std::memory_order_acquire)) {
        break;
      }
      const std::string key = deskmate::platform::aoa_identity_key(dev.identity);
      const std::string dev_id = deskmate::platform::aoa_device_id(dev.identity);
      {
        std::lock_guard lock(mutex_);
        if (!store_.is_allowed(dev_id)) {
          continue;
        }
        if (active_keys.count(key) != 0 ||
            (!dev_id.empty() && active_ids.count(dev_id) != 0)) {
          continue;
        }
      }

      if (dev.in_accessory_mode) {
        std::string err;
        bool missing = false;
        auto transport = discovery_->open_accessory(dev, err, missing);
        if (transport == nullptr) {
          if (missing) {
            any_driver_missing = true;
            latest_driver_hint = err;
          }
          if (!err.empty()) {
            latest_error = err;
            append_log("[discovery] " + key + ": " + err);
          }
          continue;
        }

        auto ch = std::make_shared<LinkChannel>();
        {
          std::lock_guard lock(mutex_);
          ch->link_id = next_link_id_++;
          ch->key = key;
          ch->identity = dev.identity;
          ch->device_id = dev_id;
          const std::string stored = store_.display_name_for(dev_id);
          ch->device_name = !stored.empty()
                                ? stored
                                : deskmate::platform::aoa_display_name(
                                      dev.identity);
          ch->position = default_position_for_new_channel_locked(dev_id);
          ch->last_metric_time = std::chrono::steady_clock::now();

          store_.record_seen_device(dev_id, ch->device_name,
                                    dev.identity.manufacturer,
                                    dev.identity.serial, {}, true);

          ch->injector = std::make_unique<InputInjector>(InjectorTarget{},
                                                         nullptr, touch_mode_);
          const bool accept_res =
              auto_virtual_display_ && !options_.size_explicit;
          const std::uint16_t advertise_hz =
              static_cast<std::uint16_t>(resolve_advertised_refresh_hz(
                  preferred_refresh_hz_, options_.refresh_hz_explicit,
                  options_.refresh_hz));
          ch->session = std::make_unique<SessionLink>(
              std::move(transport), dev.identity, ch->link_id,
              random_nonzero_u32(), (random_nonzero_u32() & 0x3FFFFFFFU) | 1U,
              options_.width, options_.height, advertise_hz,
              hevc_available_, accept_res);
          std::weak_ptr<LinkChannel> weak_ch = ch;
          ch->session->set_rx_callback([weak_ch] {
            if (auto locked = weak_ch.lock()) {
              {
                std::lock_guard wlock(locked->wake_mutex);
                locked->wakeup = true;
              }
              signal_channel(locked);
            }
          });
          channels_.emplace(ch->link_id, ch);
          if (selected_link_id_ == 0) {
            selected_link_id_ = ch->link_id;
          }
          active_keys.insert(key);
          if (!dev_id.empty()) {
            active_ids.insert(dev_id);
          }
        }
        append_log("[link " + std::to_string(ch->link_id) + "] 已接入 " +
                   ch->device_name + " (" + dev_id + ")");
        ch->worker =
            std::thread([this, ch] { link_worker_loop(ch); });
      } else {
        if (discovery_->is_in_switch_cooldown(dev)) {
          continue;
        }
        std::string err;
        bool missing = false;
        if (!discovery_->trigger_aoa_switch(dev, err, missing)) {
          if (missing) {
            any_driver_missing = true;
            latest_driver_hint = err;
          }
          if (!err.empty()) {
            latest_error = err;
            append_log("[discovery] " + key + ": " + err);
          }
        }
      }
    }

    {
      std::lock_guard lock(mutex_);
      if (!channels_.empty()) {
        driver_missing_ = false;
        driver_hint_.clear();
        discovery_error_.clear();
      } else {
        if (any_driver_missing || pnp_driver_missing) {
          driver_missing_ = true;
          driver_hint_ = !latest_driver_hint.empty() ? latest_driver_hint
                                                     : pnp_driver_hint;
        } else if (scanned.empty() && !pnp_driver_missing) {
          driver_missing_ = false;
          driver_hint_.clear();
        }
        if (!latest_error.empty()) {
          discovery_error_ = latest_error;
        } else if (scanned.empty() && !pnp_driver_missing) {
          discovery_error_.clear();
        }
      }
    }

    std::unique_lock lock(discovery_mutex_);
    discovery_cv_.wait_for(lock, std::chrono::milliseconds(400), [this] {
      return discovery_stop_.load(std::memory_order_acquire) ||
             discovery_wakeup_;
    });
    discovery_wakeup_ = false;
  }
}

void HostController::link_worker_loop(const std::shared_ptr<LinkChannel>& ch) {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  DWORD task_index = 0;
  const HANDLE mmcs = AvSetMmThreadCharacteristicsW(L"Capture", &task_index);
  if (mmcs != nullptr) {
    (void)AvSetMmThreadPriority(mmcs, AVRT_PRIORITY_HIGH);
  }
  {
    std::lock_guard lock(ch->wake_mutex);
    // Auto-reset: a producer that calls SetEvent() between our wakeup check
    // and WaitForSingleObject() leaves the event signaled, so the wait returns
    // immediately instead of sleeping through a queued frame. A manual-reset
    // event had to be cleared with ResetEvent(), which could swallow a wake
    // that arrived in exactly that window.
    ch->wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }
  const HANDLE wait_event = ch->wake_event;
#endif
  auto next_poll_30hz = std::chrono::steady_clock::now();
  auto next_metric_tick = std::chrono::steady_clock::now();
  std::uint64_t handshake_rx_baseline = ch->session->rx_bytes();
  std::optional<deskmate::protocol::SessionState> last_logged_session_state;
  std::string last_logged_state;
  bool logged_first_rx = false;
  bool logged_first_frame = false;

  while (!ch->stop_requested.load(std::memory_order_acquire) &&
         !discovery_stop_.load(std::memory_order_acquire)) {
    if (ch->disconnect_requested.exchange(false, std::memory_order_acq_rel)) {
      append_log("[link " + std::to_string(ch->link_id) + "] 主控端主动断开");
      break;
    }

    ch->session->pump();

    const auto cur_session_state = ch->session->state();
    const bool state_changed =
        !last_logged_session_state.has_value() ||
        *last_logged_session_state != cur_session_state;
    if (state_changed) {
      last_logged_session_state = cur_session_state;
      const std::string cur_state(
          deskmate::protocol::session_state_name(cur_session_state));
      std::string note = "[link " + std::to_string(ch->link_id) + "] 状态: " +
                         (last_logged_state.empty() ? std::string("init")
                                                    : last_logged_state) +
                         " -> " + cur_state;
      const std::string neg = ch->session->negotiated_summary();
      if (neg != "none") {
        note += " (" + neg + ")";
      }
      append_log(note);
      last_logged_state = cur_state;
    }
    if (!logged_first_rx && ch->session->rx_bytes() > handshake_rx_baseline) {
      logged_first_rx = true;
      append_log("[link " + std::to_string(ch->link_id) + "] 收到设备数据 (rx=" +
                 std::to_string(ch->session->rx_bytes()) + "B)");
    }

    const bool terminal = ch->session->is_terminal();
    if (terminal) {
      const std::string fail_reason = ch->session->failure_reason();
      const bool device_answered =
          ch->session->rx_bytes() > handshake_rx_baseline;
      const bool retryable =
          (fail_reason == "hello_timeout" || fail_reason == "ready_timeout" ||
           fail_reason == "commit_timeout") &&
          ch->session->transport_alive() && !device_answered &&
          discovery_ != nullptr &&
          discovery_->is_accessory_present(ch->identity);
      if (retryable) {
        append_log("[link " + std::to_string(ch->link_id) +
                   "] 握手超时（" + fail_reason + "），原链路重试握手");
        {
          std::lock_guard lock(mutex_);
          ch->last_error = "等待设备授权（" + fail_reason + "，自动重试中）";
        }
        handshake_rx_baseline = ch->session->rx_bytes();
        last_logged_session_state.reset();
        last_logged_state.clear();
        ch->session->restart_session();
        std::unique_lock wlock(ch->wake_mutex);
        ch->wake_cv.wait_for(wlock, std::chrono::milliseconds(300), [&ch] {
          return ch->stop_requested.load(std::memory_order_acquire) ||
                 ch->disconnect_requested.load(std::memory_order_acquire);
        });
        continue;
      }
      const std::string terr = ch->session->transport_last_error();
      std::string full_err =
          fail_reason.empty() ? "session_terminated" : fail_reason;
      if (!terr.empty()) {
        full_err += " (" + terr + ")";
      }
      {
        std::lock_guard lock(mutex_);
        ch->last_error = full_err;
      }
      append_log("[link " + std::to_string(ch->link_id) + "] 会话结束: " +
                 full_err);
      break;
    }

    // Clear transient handshake error once streaming or device answers.
    if (cur_session_state == deskmate::protocol::SessionState::Streaming &&
        state_changed) {
      std::lock_guard lock(mutex_);
      ch->last_error.clear();
    }

    const auto now = std::chrono::steady_clock::now();
    if (state_changed ||
        ch->capture_dirty.exchange(false, std::memory_order_acq_rel) ||
        !ch->capture_active.load(std::memory_order_acquire) ||
        now >= next_poll_30hz) {
      ensure_capture_for_channel(ch);
    }
    if (!logged_first_frame && ch->session->stats().frames_sent > 0) {
      logged_first_frame = true;
      append_log("[link " + std::to_string(ch->link_id) +
                 "] 首帧视频已编码并开始发送到设备");
    }

    // Route incoming reverse touch/key events to this link's InputInjector.
    const auto input_batch = ch->session->take_input();
    if (!input_batch.empty()) {
      bool allow = false;
      {
        std::lock_guard lock(mutex_);
        allow = allow_input_ && ch->session->input_requested();
      }
      if (allow && ch->injector != nullptr) {
        for (const auto& ev : input_batch) {
          (void)ch->injector->inject(ev);
        }
      }
    }

    // The link reports both device-requested IDRs and its own local recovery
    // requests (after a mid-GOP drop). Both must force a fresh keyframe, or
    // the device keeps decoding P-frames with a broken reference chain.
    const bool peer_idr = ch->session->take_keyframe_request() != 0U;
    const bool local_idr = ch->session->consume_local_keyframe_request();
    if (peer_idr || local_idr) {
      std::lock_guard lock(mutex_);
      if (ch->capture != nullptr) {
        ch->capture->request_keyframe();
      }
    }

    if (now >= next_poll_30hz) {
      next_poll_30hz = now + std::chrono::milliseconds(33);
      bool tick_input = false;
      {
        std::lock_guard lock(mutex_);
        tick_input = allow_input_ && ch->session->input_requested();
      }
      if (tick_input && ch->injector != nullptr) {
        ch->injector->tick();
      }
    }

    if (now >= next_metric_tick) {
      next_metric_tick = now + std::chrono::milliseconds(500);
      update_channel_metrics(ch);
    }

    const bool pending_out = ch->session->has_pending_output();
    const bool write_blocked = ch->session->consume_write_blocked();
    // A blocked transport still has data to push, so retry on the short
    // timeout instead of idling a full 5 ms (half a 120 Hz frame).
    const bool retry_soon = pending_out || write_blocked;
    if (!pending_out || write_blocked) {
      std::unique_lock wlock(ch->wake_mutex);
#ifdef _WIN32
      // Consume the pending wake under the lock. A wake that arrived while we
      // were pumping must not be erased by an unconditional ResetEvent, or the
      // worker sleeps a full timeout with a video frame already queued.
      if (ch->wakeup.exchange(false, std::memory_order_acq_rel)) {
        wlock.unlock();
        continue;
      }
      wlock.unlock();
      (void)WaitForSingleObject(wait_event, retry_soon ? 1U : 5U);
#else
      const auto wait_ms = retry_soon ? std::chrono::milliseconds(1)
                                      : std::chrono::milliseconds(5);
      ch->wake_cv.wait_for(wlock, wait_ms, [&ch] {
        return ch->stop_requested.load(std::memory_order_acquire) ||
               ch->disconnect_requested.load(std::memory_order_acquire) ||
               ch->wakeup.load(std::memory_order_acquire);
      });
      ch->wakeup.store(false, std::memory_order_release);
#endif
    }
  }

#ifdef _WIN32
  if (mmcs != nullptr) {
    (void)AvRevertMmThreadCharacteristics(mmcs);
  }
  {
    std::lock_guard lock(ch->wake_mutex);
    ch->wake_event = nullptr;
  }
  if (wait_event != nullptr) {
    (void)CloseHandle(wait_event);
  }
#endif
  ch->finished.store(true, std::memory_order_release);
  {
    std::lock_guard dlock(discovery_mutex_);
    discovery_wakeup_ = true;
  }
  discovery_cv_.notify_one();
}

void HostController::ensure_capture_for_channel(
    const std::shared_ptr<LinkChannel>& ch) {
  bool want_video = false;
  std::optional<deskmate::protocol::StreamProfile> profile;
  int preferred_hz = 0;
  {
    std::lock_guard lock(mutex_);
    want_video = video_enabled_ && ch->session->capture_requested();
    profile = ch->session->negotiated_profile();
    preferred_hz = preferred_refresh_hz_;
  }
  if (!want_video) {
    stop_capture_for_channel(ch);
    return;
  }
  if (!profile.has_value()) {
    return;
  }

  const int target_hz = resolve_effective_refresh_hz(
      preferred_hz, profile->resolution.refresh_hz, options_.refresh_hz);

  int prev_hz = 0;
  {
    std::lock_guard lock(mutex_);
    if (ch->capture != nullptr && ch->capture_profile == *profile &&
        ch->capture_refresh_hz == target_hz) {
      return;
    }
    if (ch->capture_profile == *profile &&
        ch->capture_refresh_hz == target_hz &&
        std::chrono::steady_clock::now() < ch->retry_after) {
      return;
    }
    prev_hz = ch->capture_refresh_hz;
  }

  stop_capture_for_channel(ch);

  CaptureOptions cap_opts;
  cap_opts.hevc =
      profile->encoder == deskmate::protocol::VideoEncoder::Hevc;
  cap_opts.width = profile->resolution.width;
  cap_opts.height = profile->resolution.height;
  cap_opts.refresh_hz = static_cast<std::uint16_t>(target_hz);

  bool need_virtual_display = false;
  {
    std::lock_guard lock(mutex_);
    if (auto_virtual_display_ && !ch->owns_virtual_display) {
      refresh_monitors_locked();
      bool has_unused_virtual = false;
      for (const auto& m : monitors_) {
        if (m.primary || !m.is_virtual) {
          continue;
        }
        bool in_use = false;
        for (const auto& [id, other] : channels_) {
          if (id != ch->link_id && other->monitor_handle == m.handle) {
            in_use = true;
            break;
          }
        }
        if (!in_use) {
          has_unused_virtual = true;
          break;
        }
      }
      need_virtual_display = !has_unused_virtual;
    }
  }

  if (auto_virtual_display_) {
    std::string topo_log;
    if (vdd_.ensure_extended_topology(topo_log) && !topo_log.empty()) {
      append_log("[link " + std::to_string(ch->link_id) + "] " + topo_log);
    }
  }

  if (need_virtual_display) {
    std::string vdd_log;
    const std::uintptr_t created = vdd_.create_display(
        ch->link_id, cap_opts.width, cap_opts.height, cap_opts.refresh_hz,
        vdd_log);
    invalidate_vdd_status();
    if (!vdd_log.empty()) {
      append_log("[link " + std::to_string(ch->link_id) + "] " + vdd_log);
    }
    if (created != 0) {
      std::lock_guard lock(mutex_);
      ch->owns_virtual_display = true;
    }
  }

  std::string monitor_label = "none";
  bool missing_virtual_display = false;
  std::uintptr_t active_monitor_handle = 0;
  bool active_monitor_is_virtual = false;
  int active_monitor_hz = 0;
  {
    std::lock_guard lock(mutex_);
    refresh_monitors_locked();
    const auto* monitor = pick_monitor_locked(ch);
    if (monitor != nullptr) {
      ch->monitor_handle = monitor->handle;
      ch->monitor_display_id = monitor->display_id;
      monitor_label = monitor->name;
      active_monitor_handle = monitor->handle;
      active_monitor_is_virtual = monitor->is_virtual;
      active_monitor_hz = monitor->refresh_hz;
      if (ch->injector != nullptr) {
        ch->injector->set_target(InjectorTarget{
            monitor->left, monitor->top, monitor->width, monitor->height});
      }
      cap_opts.monitor_handle = monitor->handle;
    } else {
      ch->monitor_handle = 0;
      ch->monitor_display_id = 0;
      if (auto_virtual_display_) {
        missing_virtual_display = true;
        ch->capture_error =
            "无法创建独立虚拟扩展屏：未检测到 Parsec VDD 或 MttVDD 虚拟屏驱动。请安装驱动后点击「重试」，或切换到手动选择已有物理显示器。";
        ch->retry_after =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
      }
    }
    ch->capture_profile = *profile;
    ch->capture_refresh_hz = target_hz;
  }
  if (missing_virtual_display) {
    append_log("[link " + std::to_string(ch->link_id) +
               "] 未检测到虚拟屏驱动，已拒绝占用真实物理显示器，等待安装 Parsec VDD / MttVDD 或切换模式");
    return;
  }

  if (!need_virtual_display && active_monitor_handle != 0 &&
      ((active_monitor_is_virtual && active_monitor_hz != target_hz) ||
       (prev_hz != 0 && prev_hz != target_hz))) {
    std::string mode_log;
    if (vdd_.apply_display_mode(active_monitor_handle, cap_opts.width,
                                cap_opts.height, target_hz, mode_log) &&
        !mode_log.empty()) {
      append_log("[link " + std::to_string(ch->link_id) + "] 切换刷新率" +
                 mode_log);
      std::lock_guard lock(mutex_);
      refresh_monitors_locked();
      for (const auto& m : monitors_) {
        if (m.name == monitor_label) {
          ch->monitor_handle = m.handle;
          cap_opts.monitor_handle = m.handle;
          break;
        }
      }
    }
  }

  auto capture = make_capture_encoder();
  std::string error;
  std::weak_ptr<LinkChannel> weak_ch = ch;
  const bool ok = capture->start(
      cap_opts,
      [weak_ch](EncodedAccessUnit unit) {
        if (auto locked = weak_ch.lock()) {
          locked->session->submit_video(std::move(unit.annex_b),
                                        unit.timestamp_us, unit.keyframe);
          {
            std::lock_guard wlock(locked->wake_mutex);
            locked->wakeup = true;
          }
          signal_channel(locked);
        }
      },
      [this, weak_ch](std::string msg) {
        if (auto locked = weak_ch.lock()) {
          {
            std::lock_guard lock(mutex_);
            locked->capture_error = msg;
            locked->retry_after =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
          }
          append_log("[link " + std::to_string(locked->link_id) +
                     "] 采集异常: " + msg);
        }
      },
      error);

  if (!ok) {
    {
      std::lock_guard lock(mutex_);
      ch->capture_error = error;
      ch->retry_after =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    append_log("[link " + std::to_string(ch->link_id) +
               "] 采集启动失败: " + error);
    return;
  }

  {
    std::lock_guard lock(mutex_);
    ch->capture = std::move(capture);
    ch->capture_active.store(true, std::memory_order_release);
    ch->capture_error.clear();
    const std::string res_text = ch->session->negotiated_summary();
    store_.record_seen_device(ch->device_id, ch->device_name,
                              ch->identity.manufacturer, ch->identity.serial,
                              res_text, true);
  }
  // try_configure_mft_types silently steps the encoder down (120 -> 90 -> 60
  // -> 30) when the hardware MFT rejects a mode. Surface the effective rate so
  // a "90/120 Hz feels slow" report is not misdiagnosed as a transport bug.
  const std::string encoder_summary = ch->capture->status_summary();
  const std::string expected_hz = "@" + std::to_string(cap_opts.refresh_hz) + "Hz";
  if (!encoder_summary.empty() &&
      encoder_summary.find(expected_hz) == std::string::npos) {
    append_log("[link " + std::to_string(ch->link_id) +
               "] 警告：硬件编码器未接受 " +
               std::to_string(cap_opts.refresh_hz) + "Hz，实际为 " +
               encoder_summary);
  }
  rearrange_monitors();
  update_power_assertion();
  // Tell the transport the negotiated rate so it can size its back-off. The
  // async URB timeout itself stays fixed: scaling it by frame rate made every
  // queued URB race the timer, and one expiry aborts the whole WinUSB pipe.
  ch->session->set_stream_fps(cap_opts.refresh_hz);
  append_log("[link " + std::to_string(ch->link_id) + "] 开始采集与编码 " +
             std::to_string(cap_opts.width) + "x" +
             std::to_string(cap_opts.height) + "@" +
             std::to_string(cap_opts.refresh_hz) + " (" + monitor_label + ")");
}

void HostController::stop_capture_for_channel(
    const std::shared_ptr<LinkChannel>& ch) {
  std::unique_ptr<CaptureEncoder> cap;
  {
    std::lock_guard lock(mutex_);
    cap = std::move(ch->capture);
    ch->capture_active.store(false, std::memory_order_release);
    ch->live_fps = 0;
    ch->live_bitrate_mbps = 0.0;
  }
  if (cap != nullptr) {
    cap->stop();
  }
  update_power_assertion();
}

void HostController::restart_all_captures() {
  std::vector<std::shared_ptr<LinkChannel>> list;
  {
    std::lock_guard lock(mutex_);
    for (auto& [id, ch] : channels_) {
      (void)id;
      list.push_back(ch);
    }
  }
  for (auto& ch : list) {
    stop_capture_for_channel(ch);
    {
      std::lock_guard lock(mutex_);
      ch->capture_profile.reset();
      ch->retry_after = {};
    }
    {
      std::lock_guard wlock(ch->wake_mutex);
      ch->wakeup = true;
    }
    wake_channel(ch);
  }
}

void HostController::refresh_monitors_locked() {
  monitors_ = enumerate_monitors();
}

const MonitorInfo* HostController::pick_monitor_locked(
    const std::shared_ptr<LinkChannel>& target) const {
  if (monitors_.empty()) {
    return nullptr;
  }
  if (!auto_virtual_display_) {
    if (selected_monitor_ != 0) {
      for (const auto& m : monitors_) {
        if (m.handle == selected_monitor_) {
          return &m;
        }
      }
    }
    return &monitors_.front();
  }
  const std::uintptr_t created_handle =
      target != nullptr ? vdd_.monitor_for_link(target->link_id) : 0;
  if (created_handle != 0) {
    for (const auto& m : monitors_) {
      if (m.handle == created_handle) {
        return &m;
      }
    }
  }
  // In auto_virtual_display mode, ONLY pick an unused virtual display
  // created by an IddCx driver; never silently hijack a real physical monitor.
  for (const auto& m : monitors_) {
    if (m.primary || !m.is_virtual) {
      continue;
    }
    bool in_use = false;
    for (const auto& [id, ch] : channels_) {
      if (target != nullptr && id != target->link_id &&
          ch->monitor_handle == m.handle) {
        in_use = true;
        break;
      }
    }
    if (!in_use) {
      return &m;
    }
  }
  return nullptr;
}

DisplayPosition HostController::default_position_for_new_channel_locked(
    const std::string& device_id) const {
  if (!device_id.empty()) {
    if (const auto pref = store_.preferred_position_for(device_id);
        pref.has_value()) {
      return *pref;
    }
  }
  std::unordered_set<int> used;
  for (const auto& [id, ch] : channels_) {
    (void)id;
    used.insert(static_cast<int>(ch->position));
  }
  const DisplayPosition candidates[] = {
      default_position_, DisplayPosition::Right, DisplayPosition::Left,
      DisplayPosition::Bottom, DisplayPosition::Top};
  for (DisplayPosition pos : candidates) {
    if (used.count(static_cast<int>(pos)) == 0) {
      return pos;
    }
  }
  return default_position_;
}

void HostController::rearrange_monitors() {
  std::vector<std::pair<std::uintptr_t, int>> placements;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [id, ch] : channels_) {
      (void)id;
      if (ch->monitor_handle != 0) {
        placements.emplace_back(ch->monitor_handle,
                                static_cast<int>(ch->position));
      }
    }
  }
  if (!placements.empty() && arrange_extended_monitors(placements)) {
    std::lock_guard lock(mutex_);
    refresh_monitors_locked();
    for (auto& [id, ch] : channels_) {
      (void)id;
      for (const auto& m : monitors_) {
        if (m.handle == ch->monitor_handle && ch->injector != nullptr) {
          ch->injector->set_target(
              InjectorTarget{m.left, m.top, m.width, m.height});
          break;
        }
      }
    }
  }
}

void HostController::update_channel_metrics(
    const std::shared_ptr<LinkChannel>& ch) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex_);
  const double dt =
      std::chrono::duration<double>(now - ch->last_metric_time).count();
  if (dt < 0.45) {
    return;
  }
  const std::uint64_t encoded =
      ch->capture != nullptr ? ch->capture->encoded_frame_count() : 0;
  const std::uint64_t tx = ch->session->tx_bytes();
  if (ch->capture != nullptr && encoded >= ch->last_metric_frames) {
    ch->live_fps = static_cast<int>(
        std::lround(static_cast<double>(encoded - ch->last_metric_frames) / dt));
  } else {
    ch->live_fps = 0;
  }
  if (tx >= ch->last_metric_tx_bytes) {
    ch->live_bitrate_mbps =
        static_cast<double>(tx - ch->last_metric_tx_bytes) * 8.0 /
        (dt * 1'000'000.0);
  } else {
    ch->live_bitrate_mbps = 0.0;
  }
  ch->last_metric_time = now;
  ch->last_metric_frames = encoded;
  ch->last_metric_tx_bytes = tx;
}

void HostController::update_power_assertion() {
#ifdef _WIN32
  bool any_streaming = false;
  {
    std::lock_guard lock(mutex_);
    if (dry_run_capture_ != nullptr) {
      any_streaming = true;
    }
    for (const auto& [id, ch] : channels_) {
      (void)id;
      if (ch->capture != nullptr) {
        any_streaming = true;
        break;
      }
    }
    if (any_streaming == power_boost_active_) {
      return;
    }
    power_boost_active_ = any_streaming;
  }
  if (any_streaming) {
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                            ES_DISPLAY_REQUIRED);
    timeBeginPeriod(1);
  } else {
    timeEndPeriod(1);
    SetThreadExecutionState(ES_CONTINUOUS);
  }
#endif
}

void HostController::wake_channel(const std::shared_ptr<LinkChannel>& ch) {
  signal_channel(ch);
}

void HostController::signal_channel(const std::shared_ptr<LinkChannel>& ch) {
  std::lock_guard lock(ch->wake_mutex);
#ifdef _WIN32
  if (ch->wake_event != nullptr) {
    (void)SetEvent(ch->wake_event);
  }
#endif
  ch->wake_cv.notify_all();
}

void HostController::set_service_running(bool enabled) {
  if (enabled) {
    start_service_internal();
  } else {
    stop_service_internal();
  }
}

void HostController::set_video_enabled(bool enabled) {
  {
    std::lock_guard lock(mutex_);
    video_enabled_ = enabled;
    for (auto& [id, ch] : channels_) {
      (void)id;
      {
        std::lock_guard wlock(ch->wake_mutex);
        ch->wakeup = true;
      }
      wake_channel(ch);
    }
  }
}

void HostController::set_input_enabled(bool enabled) {
  std::lock_guard lock(mutex_);
  allow_input_ = enabled;
  store_.prefs().allow_input = enabled;
  store_.save();
  if (!enabled) {
    for (auto& [id, ch] : channels_) {
      (void)id;
      if (ch->injector != nullptr) {
        ch->injector->reset();
      }
    }
  }
}

void HostController::set_touch_input_mode(TouchInputMode mode) {
  std::lock_guard lock(mutex_);
  touch_mode_ = mode;
  store_.prefs().touch_input_mode = mode;
  store_.save();
  for (auto& [id, ch] : channels_) {
    (void)id;
    if (ch->injector != nullptr) {
      ch->injector->set_mode(mode);
    }
  }
}

void HostController::set_auto_virtual_display(bool enabled) {
  {
    std::lock_guard lock(mutex_);
    auto_virtual_display_ = enabled;
    store_.prefs().auto_virtual_display = enabled;
    store_.save();
    for (auto& [id, ch] : channels_) {
      (void)id;
      ch->session->set_accept_device_resolution(enabled &&
                                                !options_.size_explicit);
    }
  }
  restart_all_captures();
}

void HostController::set_preferred_refresh_hz(int hz) {
  const int normalized = normalize_preferred_refresh_hz(hz);
  std::vector<std::shared_ptr<LinkChannel>> channels_copy;
  {
    std::lock_guard lock(mutex_);
    if (preferred_refresh_hz_ == normalized) {
      return;
    }
    preferred_refresh_hz_ = normalized;
    store_.prefs().preferred_refresh_hz = normalized;
    store_.save();
    for (auto& [id, ch] : channels_) {
      (void)id;
      channels_copy.push_back(ch);
    }
  }
  append_log(normalized > 0
                 ? ("[fps] 已手动切换目标帧率: " +
                    std::to_string(normalized) + " Hz")
                 : "[fps] 已恢复目标帧率: 自动（跟随设备协商）");

  const int desired_cap = normalized > 0 ? normalized : 120;
  for (auto& ch : channels_copy) {
    const auto neg = ch->session->negotiated_profile();
    const int neg_hz = neg.has_value() ? neg->resolution.refresh_hz : 0;
    const int max_hz = ch->session->max_supported_refresh_hz();
    bool reconf_initiated = false;
    if (neg_hz > 0 &&
        ((normalized > 0 && normalized > neg_hz) ||
         (normalized == 0 && max_hz > neg_hz))) {
      if (ch->session->request_refresh_hz_reconfigure(desired_cap) > 0) {
        reconf_initiated = true;
      }
    }
    if (!reconf_initiated) {
      const int effective_hz = resolve_effective_refresh_hz(
          normalized, neg_hz, options_.refresh_hz);
      bool rate_changed = false;
      {
        std::lock_guard lock(mutex_);
        rate_changed = (ch->capture_refresh_hz != effective_hz);
      }
      if (rate_changed) {
        stop_capture_for_channel(ch);
        {
          std::lock_guard lock(mutex_);
          ch->capture_profile.reset();
          ch->retry_after = {};
        }
        ch->capture_dirty.store(true, std::memory_order_release);
      }
    }
    {
      std::lock_guard wlock(ch->wake_mutex);
      ch->wakeup = true;
    }
    wake_channel(ch);
  }
}

void HostController::choose_monitor(std::uintptr_t handle) {
  {
    std::lock_guard lock(mutex_);
    selected_monitor_ = handle;
    for (const auto& m : monitors_) {
      if (m.handle == handle) {
        store_.prefs().preferred_monitor_name = m.name;
        store_.save();
        break;
      }
    }
  }
  restart_all_captures();
}

void HostController::set_default_position(DisplayPosition pos) {
  {
    std::lock_guard lock(mutex_);
    default_position_ = pos;
    store_.prefs().default_position = pos;
    store_.save();
    if (selected_link_id_ != 0) {
      auto it = channels_.find(selected_link_id_);
      if (it != channels_.end()) {
        it->second->position = pos;
        if (!it->second->device_id.empty()) {
          store_.set_preferred_position(it->second->device_id, pos);
        }
      }
    }
  }
  rearrange_monitors();
}

void HostController::select_link(std::uint64_t link_id) {
  std::lock_guard lock(mutex_);
  if (channels_.count(link_id) != 0) {
    selected_link_id_ = link_id;
  }
}

void HostController::set_link_position(std::uint64_t link_id,
                                       DisplayPosition pos) {
  {
    std::lock_guard lock(mutex_);
    auto it = channels_.find(link_id);
    if (it != channels_.end()) {
      it->second->position = pos;
      selected_link_id_ = link_id;
      if (!it->second->device_id.empty()) {
        store_.set_preferred_position(it->second->device_id, pos);
      }
    } else {
      default_position_ = pos;
      store_.prefs().default_position = pos;
      store_.save();
    }
  }
  rearrange_monitors();
}

void HostController::apply_display_layout(
    const std::vector<MonitorPlacement>& placements) {
  if (placements.empty()) {
    return;
  }
  if (!apply_monitor_layout(placements)) {
    append_log("[topology] 应用多屏拓扑布局失败");
    return;
  }

  std::lock_guard lock(mutex_);
  refresh_monitors_locked();

  const MonitorInfo* primary = nullptr;
  for (const auto& m : monitors_) {
    if (m.primary) {
      primary = &m;
      break;
    }
  }
  if (primary == nullptr && !monitors_.empty()) {
    primary = &monitors_.front();
  }

  if (primary != nullptr) {
    TopologyDisplayItem p_item;
    p_item.x = static_cast<double>(primary->left);
    p_item.y = static_cast<double>(primary->top);
    p_item.width = static_cast<double>(primary->width);
    p_item.height = static_cast<double>(primary->height);

    for (auto& [id, ch] : channels_) {
      (void)id;
      for (const auto& m : monitors_) {
        if (m.handle == ch->monitor_handle) {
          if (ch->injector != nullptr) {
            ch->injector->set_target(
                InjectorTarget{m.left, m.top, m.width, m.height});
          }
          TopologyDisplayItem t_item;
          t_item.x = static_cast<double>(m.left);
          t_item.y = static_cast<double>(m.top);
          t_item.width = static_cast<double>(m.width);
          t_item.height = static_cast<double>(m.height);
          const int code = infer_cardinal_position_code(p_item, t_item);
          const auto pos = static_cast<DisplayPosition>(std::clamp(code, 0, 3));
          ch->position = pos;
          if (!ch->device_id.empty()) {
            store_.set_preferred_position(ch->device_id, pos);
          }
          break;
        }
      }
    }
  }
  append_log("[topology] 已更新系统多屏拓扑排列 (" +
             std::to_string(placements.size()) + " 块屏幕)");
}

void HostController::refresh_displays() {
  invalidate_vdd_status();
  auto monitors = enumerate_monitors();
  std::lock_guard lock(mutex_);
  monitors_ = std::move(monitors);
}

void HostController::disconnect_link(std::uint64_t link_id) {
  std::lock_guard lock(mutex_);
  auto it = channels_.find(link_id);
  if (it != channels_.end()) {
    it->second->disconnect_requested.store(true, std::memory_order_release);
    wake_channel(it->second);
  }
}

void HostController::set_launch_at_login(bool enabled) {
  std::lock_guard lock(mutex_);
  startup_status_ = startup_manager_.set_launch_at_login(enabled);
}

void HostController::open_startup_settings() {
  startup_manager_.open_startup_settings();
}

void HostController::set_auto_start_service(bool enabled) {
  std::lock_guard lock(mutex_);
  auto_start_service_ = enabled;
  store_.prefs().auto_start_service = enabled;
  store_.save();
}

void HostController::check_for_updates(bool manual) {
  updater_.check_for_updates(manual);
}

void HostController::install_available_update() {
  updater_.download_and_install([this]() {
    append_log("[update] 正在停止后台服务并释放虚拟显示器以完成更新替换...");
    stop_service_internal();
  });
}

void HostController::set_auto_check_updates(bool enabled) {
  std::string mirror;
  {
    std::lock_guard lock(mutex_);
    store_.prefs().auto_check_updates = enabled;
    store_.save();
    mirror = store_.prefs().update_mirror_prefix;
  }
  updater_.set_preferences(enabled, mirror);
}

void HostController::set_update_mirror_prefix(const std::string& prefix) {
  bool auto_check = true;
  std::string trimmed = prefix;
  while (!trimmed.empty() &&
         std::isspace(static_cast<unsigned char>(trimmed.front())) != 0) {
    trimmed.erase(trimmed.begin());
  }
  while (!trimmed.empty() &&
         std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) {
    trimmed.pop_back();
  }
  {
    std::lock_guard lock(mutex_);
    store_.prefs().update_mirror_prefix = trimmed;
    store_.save();
    auto_check = store_.prefs().auto_check_updates;
  }
  updater_.set_preferences(auto_check, trimmed);
}

void HostController::cleanup_legacy_update_files() {
  const auto summary = updater_.cleanup_legacy_artifacts();
  if (!summary.message.empty()) {
    append_log("[update] " + summary.message);
  }
}

void HostController::refresh_devices() {
  if (discovery_ == nullptr) {
    LibusbAoaDiscovery temp;
    if (!temp.initialized()) {
      return;
    }
    const auto scanned = temp.scan_all_devices();
    std::lock_guard lock(mutex_);
    online_device_ids_.clear();
    for (const auto& d : scanned) {
      const std::string id = deskmate::platform::aoa_device_id(d.identity);
      const std::string name = deskmate::platform::aoa_display_name(d.identity);
      if (!id.empty()) {
        online_device_ids_.insert(id);
        store_.record_seen_device(id, name, d.identity.manufacturer,
                                  d.identity.serial, {}, false);
      }
    }
    return;
  }
  {
    std::lock_guard lock(discovery_mutex_);
    discovery_wakeup_ = true;
  }
  discovery_cv_.notify_one();
}

void HostController::set_device_allowed(const std::string& device_id,
                                        bool allowed) {
  std::vector<std::shared_ptr<LinkChannel>> to_disconnect;
  {
    std::lock_guard lock(mutex_);
    store_.set_allowed(device_id, allowed);
    if (!allowed) {
      for (auto& [id, ch] : channels_) {
        (void)id;
        if (ch->device_id == device_id) {
          to_disconnect.push_back(ch);
        }
      }
    }
  }
  for (auto& ch : to_disconnect) {
    ch->disconnect_requested.store(true, std::memory_order_release);
    wake_channel(ch);
  }
  {
    std::lock_guard lock(discovery_mutex_);
    discovery_wakeup_ = true;
  }
  discovery_cv_.notify_one();
}

void HostController::set_device_preferred_position(
    const std::string& device_id, std::optional<DisplayPosition> pos) {
  bool moved = false;
  {
    std::lock_guard lock(mutex_);
    store_.set_preferred_position(device_id, pos);
    if (pos.has_value()) {
      for (auto& [id, ch] : channels_) {
        (void)id;
        if (ch->device_id == device_id) {
          ch->position = *pos;
          moved = true;
        }
      }
    }
  }
  if (moved) {
    rearrange_monitors();
  }
}

void HostController::rename_device(const std::string& device_id,
                                   const std::string& name) {
  std::lock_guard lock(mutex_);
  store_.rename(device_id, name);
  const std::string resolved = store_.display_name_for(device_id);
  for (auto& [id, ch] : channels_) {
    (void)id;
    if (ch->device_id == device_id && !resolved.empty()) {
      ch->device_name = resolved;
    }
  }
}

void HostController::remove_device(const std::string& device_id) {
  std::vector<std::shared_ptr<LinkChannel>> to_disconnect;
  {
    std::lock_guard lock(mutex_);
    for (auto& [id, ch] : channels_) {
      (void)id;
      if (ch->device_id == device_id) {
        to_disconnect.push_back(ch);
      }
    }
    store_.remove(device_id);
  }
  for (auto& ch : to_disconnect) {
    ch->disconnect_requested.store(true, std::memory_order_release);
    wake_channel(ch);
  }
}

void HostController::retry() {
  invalidate_vdd_status();
  bool running_now = false;
  {
    std::lock_guard lock(mutex_);
    running_now = service_running_;
    discovery_error_.clear();
    driver_missing_ = false;
    driver_hint_.clear();
    refresh_monitors_locked();
    for (auto& [id, ch] : channels_) {
      (void)id;
      ch->capture_error.clear();
      ch->retry_after = {};
    }
  }
  if (!running_now) {
    start_service_internal();
    return;
  }
  restart_all_captures();
  {
    std::lock_guard lock(discovery_mutex_);
    discovery_wakeup_ = true;
  }
  discovery_cv_.notify_one();
}

void HostController::handle_host_event(deskmate::protocol::HostEvent event) {
  std::vector<std::shared_ptr<LinkChannel>> list;
  {
    std::lock_guard lock(mutex_);
    for (auto& [id, ch] : channels_) {
      (void)id;
      list.push_back(ch);
    }
  }
  for (auto& ch : list) {
    (void)ch->session->handle_host_event(event);
    if (event == deskmate::protocol::HostEvent::WillSleep) {
      stop_capture_for_channel(ch);
    }
    {
      std::lock_guard wlock(ch->wake_mutex);
      ch->wakeup = true;
    }
    wake_channel(ch);
  }
}

VirtualDisplayStatus HostController::vdd_status_cached(bool force_refresh) {
  constexpr auto kTtl = std::chrono::seconds(30);
  {
    std::lock_guard lock(vdd_status_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!force_refresh && vdd_status_valid_ &&
        now - vdd_status_fetched_at_ < kTtl) {
      return cached_vdd_status_;
    }
  }
  // Probe outside the cache lock: it enumerates monitors and opens the VDD
  // device interface, which can block for tens of milliseconds.
  VirtualDisplayStatus status = vdd_.probe_status();
  {
    std::lock_guard lock(vdd_status_mutex_);
    cached_vdd_status_ = status;
    vdd_status_valid_ = true;
    vdd_status_fetched_at_ = std::chrono::steady_clock::now();
  }
  return status;
}

void HostController::invalidate_vdd_status() {
  std::lock_guard lock(vdd_status_mutex_);
  vdd_status_valid_ = false;
}

HostUiState HostController::snapshot_ui_state() {
  const VirtualDisplayStatus vdd_status = vdd_status_cached(false);
  const StartupStatus startup_status = startup_manager_.current_status();
  HostUiState view;
  std::lock_guard lock(mutex_);
  startup_status_ = startup_status;

  view.vdd_installed = vdd_status.driver_installed;
  view.vdd_backend_name = vdd_status.backend_name;
  view.vdd_status_detail = vdd_status.detail;

  view.service_running = service_running_;
  view.auto_start_service = auto_start_service_;
  view.launch_at_login = startup_status_.launch_at_login_enabled;
  view.startup_summary = startup_status_.status_description;
  view.startup_error = startup_status_.error_message;
  view.paired_devices = store_.prefs().paired_devices;
  view.online_device_ids = online_device_ids_;
  view.blocked_device_names = blocked_device_names_;
  view.blocked_device_name =
      blocked_device_names_.empty() ? std::string{} : blocked_device_names_.front();
  view.video_enabled = video_enabled_;
  view.input_enabled = allow_input_;
  view.touch_input_mode = touch_mode_;
  InputInjector probe_inj(InjectorTarget{}, nullptr, touch_mode_);
  view.touch_capability = probe_inj.capability_status();
  view.auto_virtual_display = auto_virtual_display_;
  view.hevc_available = hevc_available_;
  view.native_touch_available = native_touch_available_;
  view.driver_missing = driver_missing_;
  view.driver_hint = driver_hint_;
  view.selected_monitor = selected_monitor_;

  for (const auto& m : monitors_) {
    DisplayChoice choice;
    choice.handle = m.handle;
    choice.display_id = m.display_id;
    choice.name = m.name;
    choice.width = m.width;
    choice.height = m.height;
    choice.refresh_hz = m.refresh_hz;
    choice.primary = m.primary;
    choice.title = (m.primary ? "[主屏] " : "[扩展] ") +
                   (!m.friendly_name.empty() ? m.friendly_name : m.name) +
                   " · " + std::to_string(m.width) + "×" +
                   std::to_string(m.height) + "@" +
                   std::to_string(m.refresh_hz) + "Hz";
    view.choices.push_back(std::move(choice));
  }

  std::vector<std::shared_ptr<LinkChannel>> sorted;
  for (const auto& [id, ch] : channels_) {
    (void)id;
    sorted.push_back(ch);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a->link_id < b->link_id; });

  std::vector<TopologyDisplayItem> topo;
  topo.reserve(monitors_.size());
  for (const auto& m : monitors_) {
    std::shared_ptr<LinkChannel> matched;
    for (const auto& ch : sorted) {
      if (ch->monitor_handle == m.handle) {
        matched = ch;
        break;
      }
    }
    TopologyDisplayItem item;
    item.id = m.display_id != 0
                  ? m.display_id
                  : static_cast<std::uint32_t>(m.handle & 0xFFFFFFFFu);
    item.handle = m.handle;
    item.device_name = m.name;
    item.is_primary = m.primary;
    item.is_virtual = m.is_virtual;
    item.is_deskmate_virtual = (matched != nullptr);
    if (matched != nullptr) {
      item.name = matched->device_name.empty()
                      ? ("设备 #" + std::to_string(matched->link_id))
                      : matched->device_name;
      item.link_id = matched->link_id;
      const std::string st(
          deskmate::protocol::session_state_name(matched->session->state()));
      item.streaming =
          (st == "streaming" && video_enabled_ && matched->capture != nullptr);
      item.live_fps = matched->live_fps;
    } else if (m.primary) {
      item.name = !m.friendly_name.empty() ? m.friendly_name : "WIN 主屏";
    } else {
      item.name = !m.friendly_name.empty() ? m.friendly_name : m.name;
    }
    item.x = static_cast<double>(m.left);
    item.y = static_cast<double>(m.top);
    item.width = static_cast<double>(m.width);
    item.height = static_cast<double>(m.height);
    item.pixel_width = m.width;
    item.pixel_height = m.height;
    item.refresh_hz = m.refresh_hz;
    topo.push_back(std::move(item));
  }
  std::sort(topo.begin(), topo.end(),
            [](const TopologyDisplayItem& a, const TopologyDisplayItem& b) {
              if (a.is_primary != b.is_primary) return a.is_primary;
              if (std::abs(a.x - b.x) > 8.0) return a.x < b.x;
              if (std::abs(a.y - b.y) > 8.0) return a.y < b.y;
              return a.id < b.id;
            });
  for (std::size_t i = 0; i < topo.size(); ++i) {
    topo[i].index_number = static_cast<int>(i + 1);
  }
  view.topology_displays = std::move(topo);

  std::shared_ptr<LinkChannel> target;
  if (selected_link_id_ != 0) {
    auto it = channels_.find(selected_link_id_);
    if (it != channels_.end()) {
      target = it->second;
    }
  }
  if (target == nullptr && !sorted.empty()) {
    target = sorted.front();
  }
  view.selected_link_id = target != nullptr ? target->link_id : 0;
  view.position = target != nullptr ? target->position : default_position_;
  view.preferred_refresh_hz = preferred_refresh_hz_;
  if (target != nullptr) {
    if (const auto neg_prof = target->session->negotiated_profile();
        neg_prof.has_value()) {
      view.negotiated_refresh_hz = neg_prof->resolution.refresh_hz;
    }
  }

  std::uint64_t total_inputs = 0;
  int total_fps = 0;
  double total_mbps = 0.0;
  bool any_input_wanted = false;
  std::string any_capture_error = dry_run_error_;

  for (const auto& ch : sorted) {
    const auto stats = ch->session->stats();
    const std::string st(
        deskmate::protocol::session_state_name(ch->session->state()));
    const bool is_streaming =
        (st == "streaming" && video_enabled_ && ch->capture != nullptr);
    const std::string err =
        !ch->capture_error.empty() ? ch->capture_error : ch->last_error;
    if (!ch->capture_error.empty() && any_capture_error.empty()) {
      any_capture_error = ch->capture_error;
    }
    if (ch->session->input_requested()) {
      any_input_wanted = true;
    }

    ActiveSessionView sv;
    sv.link_id = ch->link_id;
    sv.device_id = ch->device_id;
    sv.device_name = ch->device_name.empty()
                         ? ("设备 #" + std::to_string(ch->link_id))
                         : ch->device_name;
    sv.state = st;
    sv.linked = (st != "idle" && st != "stopped");
    sv.streaming = is_streaming;
    sv.badge = (!vdd_status.driver_installed && auto_virtual_display_ &&
                !ch->capture_error.empty())
                   ? std::string("待装虚拟屏")
                   : state_badge_for(st, is_streaming, ch->capture_error);
    const std::string neg = ch->session->negotiated_summary();
    if (neg != "none") {
      sv.stream_summary = neg;
      for (char& c : sv.stream_summary) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
    }
    sv.position = ch->position;
    sv.live_fps = ch->live_fps;
    sv.live_bitrate_mbps = ch->live_bitrate_mbps;
    sv.input_event_count = stats.input_events;
    sv.display_id = ch->monitor_display_id;
    sv.error = err;

    total_fps += sv.live_fps;
    total_mbps += sv.live_bitrate_mbps;
    total_inputs += sv.input_event_count;

    if (!sv.device_id.empty()) {
      view.connected_device_ids.insert(sv.device_id);
    }
    view.active_sessions.push_back(std::move(sv));
  }

  view.live_fps = total_fps;
  view.live_bitrate_mbps = total_mbps;
  view.input_event_count = total_inputs;
  if (target != nullptr) {
    view.connected_device_id = target->device_id;
    view.connected_device_name = target->device_name;
  }

  std::vector<std::string> stream_texts;
  for (const auto& s : view.active_sessions) {
    if (!s.stream_summary.empty()) {
      stream_texts.push_back(s.stream_summary);
    }
  }
  if (stream_texts.size() == 1) {
    view.stream_summary = stream_texts[0];
  } else if (stream_texts.size() > 1) {
    view.stream_summary =
        std::to_string(stream_texts.size()) + " 路并发 · " + stream_texts[0];
  }

  view.input = view.input_enabled
                   ? (touch_mode_short_label(touch_mode_) +
                      (any_input_wanted ? " · 已启用" : " · 待会话"))
                   : "未开启";

  const auto* target_mon =
      target != nullptr ? pick_monitor_locked(target) : nullptr;
  view.vdd_active =
      (target_mon != nullptr && target_mon->is_virtual) ||
      vdd_status.active_virtual_display;
  view.mirroring_primary = false;
  if (target_mon != nullptr) {
    if (target_mon->is_virtual) {
      view.source = "虚拟扩展屏 · " + std::to_string(target_mon->width) + "×" +
                    std::to_string(target_mon->height) + "（" +
                    display_position_short_label(view.position) + "）";
    } else {
      view.source = (target_mon->primary ? "主显示器 · " : "物理副屏 · ") +
                    std::to_string(target_mon->width) + "×" +
                    std::to_string(target_mon->height) + "（" +
                    display_position_short_label(view.position) + "）";
    }
  } else {
    view.source = auto_virtual_display_
                      ? (!view.vdd_installed
                             ? std::string("待安装虚拟屏驱动（Parsec VDD / MttVDD）")
                             : ("待创建虚拟扩展屏（默认" +
                                display_position_short_label(default_position_) +
                                "）"))
                      : "未选择显示器";
  }

  const int streaming_count = static_cast<int>(std::count_if(
      view.active_sessions.begin(), view.active_sessions.end(),
      [](const ActiveSessionView& s) { return s.streaming; }));

  if (!service_running_) {
    view.title = "串流服务已停止";
    view.guidance =
        "当前未开启自动服务或已手动暂停；点击「启动服务」即可恢复监听 USB 设备连接。";
    view.badge = "已停止";
    view.can_retry = true;
  } else if (options_.dry_run) {
    view.title = "本机采集测试";
    view.guidance = "仅在本机执行 DXGI 采集与 Media Foundation 编码，不建立 USB 会话。";
    view.badge = "测试模式";
    view.video = dry_run_capture_ != nullptr ? "正在编码" : "未运行";
  } else if (auto_virtual_display_ && !view.vdd_installed &&
             !view.active_sessions.empty()) {
    view.title = "需要安装虚拟屏驱动以新建扩展屏";
    view.guidance =
        "Windows 需要通过 Parsec VDD / MttVDD 创建独立虚拟屏（不会占用您现有的真实显示器）。";
    view.badge = "待装虚拟屏";
    view.video = "等待虚拟屏";
    view.can_retry = true;
  } else if (!any_capture_error.empty()) {
    view.title = "暂时无法发送画面";
    view.guidance = any_capture_error;
    view.badge = "出错";
    view.can_retry = true;
  } else if (view.active_sessions.empty() &&
             !view.blocked_device_names.empty()) {
    view.title = "已拦截未允许的设备";
    view.guidance = "检测到 " + view.blocked_device_name +
                    " 接入，但该设备在「已配对设备」列表中已禁止连接。";
    view.badge = "已拦截";
  } else if (view.active_sessions.empty() && driver_missing_) {
    view.title = "需要安装 WinUSB 驱动";
    view.guidance = driver_hint_;
    view.badge = "待驱动";
    view.can_retry = true;
  } else if (view.active_sessions.empty() && !discovery_error_.empty()) {
    view.title = "USB 连接重试中";
    view.guidance = "USB 通信状态异常（" + discovery_error_ + "），正在自动重连。";
    view.badge = "未连接";
    view.can_retry = true;
  } else if (view.active_sessions.size() >= 2) {
    view.linked = true;
    std::string names;
    for (std::size_t i = 0; i < view.active_sessions.size(); ++i) {
      if (i > 0) {
        names += "、";
      }
      names += view.active_sessions[i].device_name;
    }
    if (streaming_count == static_cast<int>(view.active_sessions.size())) {
      view.title = "正在向 " + std::to_string(view.active_sessions.size()) +
                   " 台设备输出副屏";
      view.guidance =
          "当前同时连接：" + names + "，每台设备拥有独立采集画面与触控通道。";
      view.badge = std::to_string(streaming_count) + " 台副屏运行中";
      view.video = "并发发送 (" + std::to_string(streaming_count) + " 路)";
    } else if (streaming_count > 0) {
      view.title = std::to_string(streaming_count) + "/" +
                   std::to_string(view.active_sessions.size()) +
                   " 台副屏推流中";
      view.guidance = "当前连接：" + names + "，其余设备正在等待画面或确认授权。";
      view.badge =
          std::to_string(view.active_sessions.size()) + " 台已连接";
      view.video = "并发发送 (" + std::to_string(streaming_count) + "/" +
                   std::to_string(view.active_sessions.size()) + ")";
    } else {
      view.title =
          "已接入 " + std::to_string(view.active_sessions.size()) + " 台设备";
      view.guidance =
          "当前连接：" + names + "，请在设备上打开 DeskMate 并允许 USB 配件连接。";
      view.badge =
          std::to_string(view.active_sessions.size()) + " 台已连接";
    }
  } else if (view.active_sessions.size() == 1) {
    const auto& s = view.active_sessions[0];
    view.linked = s.linked;
    if (!s.error.empty()) {
      view.title = "暂时无法发送画面";
      view.guidance = s.error;
      view.badge = "出错";
      view.can_retry = true;
    } else if (s.state == "awaiting_client_hello") {
      view.title = "等待「" + s.device_name + "」确认";
      view.guidance =
          "请在设备上打开 DeskMate，并在系统提示中允许 USB 配件连接。";
      view.badge = "USB 已连接";
    } else if (s.state == "awaiting_ready" ||
               s.state == "awaiting_committed") {
      view.title = "正在与「" + s.device_name + "」协商画面";
      view.guidance = "USB 配件已连通，正在协商分辨率与编码格式…";
      view.badge = "协商中";
    } else if (s.streaming) {
      if (view.mirroring_primary) {
        view.title = "正在向「" + s.device_name + "」镜像主屏";
        view.guidance =
            "未检测到 Parsec VDD / MttVDD 虚拟屏驱动，当前暂时镜像主屏；安装虚拟屏驱动后将自动创建独立扩展副屏。";
      } else {
        view.title = "正在向「" + s.device_name + "」发送画面";
        view.guidance =
            "正在向「" + s.device_name + "」传输扩展桌面画面与反向触控。";
      }
      view.badge = "会话已建立";
      view.video = (target != nullptr && target->capture != nullptr &&
                    target->capture->encoded_frame_count() == 0)
                       ? "等待首帧编码"
                       : "正在发送";
    } else {
      view.title = video_enabled_ ? "正在准备画面" : "画面发送已停止";
      view.guidance =
          video_enabled_
              ? "保持 USB 数据线连接，并确认 Windows 未锁屏。"
              : "需要继续时，开启画面发送。";
      view.badge = "USB 已连接";
    }
  }

  if (!video_enabled_) {
    view.video = "已关闭";
  }

  view.diagnostic = build_diagnostic_locked();
  view.update = updater_.snapshot();
  {
    std::lock_guard llock(log_mutex_);
    view.recent_logs.assign(logs_.begin(), logs_.end());
  }
  return view;
}

std::string HostController::build_diagnostic_locked() const {
  std::ostringstream ss;
  ss << "服务：" << (service_running_ ? "运行中" : "已停止")
     << "（开机启动=" << (startup_status_.launch_at_login_enabled ? "true" : "false")
     << "，自动启动=" << (auto_start_service_ ? "true" : "false") << "）\n";
  ss << "在线连接数：" << channels_.size() << "（已记录 "
     << store_.prefs().paired_devices.size() << " 台，显示器数 "
     << monitors_.size() << "）\n";
  ss << "编码器能力：HEVC=" << (hevc_available_ ? "true" : "false")
     << "，原生触控注入=" << (native_touch_available_ ? "true" : "false")
     << "，触控模式=" << touch_mode_id(touch_mode_)
     << "，帧率模式="
     << (preferred_refresh_hz_ > 0
             ? (std::to_string(preferred_refresh_hz_) + "Hz")
             : "auto")
     << "\n";
  for (const auto& m : monitors_) {
    ss << "- monitor " << m.name << " (" << m.friendly_name << ") "
       << m.width << "x" << m.height << "@" << m.refresh_hz << "Hz rect=("
       << m.left << "," << m.top << ")"
       << (m.primary ? " primary" : "") << (m.is_virtual ? " virtual" : "")
       << "\n";
  }
  if (driver_missing_) {
    ss << "驱动提示：" << driver_hint_ << "\n";
  }
  if (!discovery_error_.empty()) {
    ss << "发现状态：" << discovery_error_ << "\n";
  }
  if (!last_usb_scan_summary_.empty()) {
    ss << "USB 扫描：" << last_usb_scan_summary_ << "\n";
  }
  if (!last_pnp_aoa_summary_.empty()) {
    ss << "PnP 节点：" << last_pnp_aoa_summary_ << "\n";
  }
  for (const auto& [id, ch] : channels_) {
    const auto st = ch->session->stats();
    ss << "- link=" << id << " device=" << ch->device_name << " ("
       << ch->device_id << ") state="
       << deskmate::protocol::session_state_name(ch->session->state())
       << " pos=" << display_position_short_label(ch->position)
       << " stream=" << ch->session->negotiated_summary()
       << " fps=" << ch->live_fps << " frames=" << st.frames_sent << "/"
       << st.frames_submitted << " dropped=" << st.frames_dropped
       << " inputs=" << st.input_events << " rx=" << st.rx_bytes
       << " tx=" << st.tx_bytes << " bulkErr=" << st.bulk_errors;
    if (ch->capture != nullptr) {
      ss << " captured=" << ch->capture->received_frame_count()
         << " encoded=" << ch->capture->encoded_frame_count()
         << " captureDropped=" << ch->capture->dropped_frame_count();
      const std::string enc = ch->capture->status_summary();
      if (!enc.empty()) {
        ss << " enc=\"" << enc << "\"";
      }
    }
    if (!ch->capture_error.empty()) {
      ss << " captureErr=" << ch->capture_error;
    }
    ss << "\n";
  }
  ss << "\n--- 最近日志 ---\n";
  std::lock_guard llock(log_mutex_);
  for (const auto& line : logs_) {
    ss << line << "\n";
  }
  return ss.str();
}

std::string HostController::diagnostic_text() {
  std::lock_guard lock(mutex_);
  return build_diagnostic_locked();
}

}  // namespace deskmate::windows
