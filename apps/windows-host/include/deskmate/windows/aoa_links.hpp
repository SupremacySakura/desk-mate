#pragma once

#include "deskmate/byte_io.hpp"
#include "deskmate/platform/aoa.hpp"
#include "deskmate/protocol.hpp"
#include "deskmate/session.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace deskmate::windows {

struct LinkStats {
  std::uint64_t rx_bytes{};
  std::uint64_t tx_bytes{};
  std::uint64_t frames_submitted{};
  std::uint64_t frames_sent{};
  std::uint64_t frames_dropped{};
  std::uint64_t input_events{};
  std::uint64_t bulk_errors{};
};

// One independent AOA link. The transport is injected so the session pump,
// frame routing and 12 KiB fragment policy can be exercised on any host.
class LinkTransport : public deskmate::protocol::ByteIo {
 public:
  ~LinkTransport() override = default;
  [[nodiscard]] virtual bool alive() const = 0;
  [[nodiscard]] virtual std::uint64_t rx_bytes() const { return 0; }
  [[nodiscard]] virtual std::uint64_t tx_bytes() const { return 0; }
  [[nodiscard]] virtual std::uint64_t bulk_errors() const { return 0; }
  [[nodiscard]] virtual std::string last_error() const { return {}; }
  virtual void set_rx_callback(std::function<void()>) {}
  [[nodiscard]] virtual bool consume_write_blocked() noexcept { return false; }
};

struct PendingVideo {
  std::vector<std::uint8_t> bytes;
  std::uint64_t pts_us{};
  bool keyframe{};
};

class LinkSession {
 public:
  virtual ~LinkSession() = default;

  [[nodiscard]] virtual std::uint64_t id() const = 0;
  [[nodiscard]] virtual const deskmate::platform::AoaIdentity& identity()
      const = 0;
  [[nodiscard]] virtual deskmate::protocol::SessionState state() const = 0;
  [[nodiscard]] virtual std::string negotiated_summary() const = 0;
  [[nodiscard]] virtual LinkStats stats() const = 0;

  virtual void submit_video(std::vector<std::uint8_t> annex_b,
                            std::uint64_t pts_us, bool keyframe) = 0;
  [[nodiscard]] virtual std::vector<deskmate::protocol::InputEvent>
  take_input() = 0;
  [[nodiscard]] virtual std::uint32_t take_keyframe_request() = 0;
  // True once the session negotiated STREAM_COMMITTED and the Host state
  // permits capture/encoding. The capture pipeline starts only then, so it
  // encodes at the negotiated codec and resolution instead of the CLI default.
  [[nodiscard]] virtual bool capture_requested() const = 0;
  [[nodiscard]] virtual bool input_requested() const = 0;
  [[nodiscard]] virtual std::optional<deskmate::protocol::StreamProfile>
  negotiated_profile() const = 0;

  [[nodiscard]] virtual bool has_pending_output() const { return false; }
  [[nodiscard]] virtual bool is_terminal() const { return false; }
  [[nodiscard]] virtual std::string failure_reason() const { return {}; }
  [[nodiscard]] virtual std::string transport_last_error() const { return {}; }
  [[nodiscard]] virtual std::uint64_t reconnect_count() const { return 0; }
  [[nodiscard]] virtual std::uint64_t rx_bytes() const { return stats().rx_bytes; }
  [[nodiscard]] virtual std::uint64_t tx_bytes() const { return stats().tx_bytes; }
  [[nodiscard]] virtual bool transport_alive() const { return true; }
  [[nodiscard]] virtual bool consume_write_blocked() { return false; }
  virtual void set_rx_callback(std::function<void()>) {}
  virtual void set_accept_device_resolution(bool) {}
  virtual void restart_session() {}
  [[nodiscard]] virtual deskmate::protocol::SessionCommandStatus
  handle_host_event(deskmate::protocol::HostEvent) {
    return deskmate::protocol::SessionCommandStatus::InvalidState;
  }

  virtual void pump() = 0;
  virtual void stop() = 0;
  // Releases the USB handle after the pump loop has exited. Keeping this
  // separate from stop() avoids tearing the handle down while pump() is still
  // running on the pump thread.
  virtual void close_transport() {}
};

class LinkFactory {
 public:
  virtual ~LinkFactory() = default;
  [[nodiscard]] virtual std::unique_ptr<LinkSession> attach(
      std::unique_ptr<LinkTransport> transport,
      deskmate::platform::AoaIdentity identity, std::uint64_t link_id) = 0;
};

// Owns all concurrently connected devices. Each link keeps its own session,
// video queue and input queue; one slow device never blocks another.
class LinkManager {
 public:
  explicit LinkManager(LinkFactory& factory);

  [[nodiscard]] std::uint64_t add(std::unique_ptr<LinkTransport> transport,
                                  deskmate::platform::AoaIdentity identity);
  void pump_all();
  void submit_video_to_all(std::vector<std::uint8_t> annex_b,
                           std::uint64_t pts_us, bool keyframe);
  [[nodiscard]] std::vector<std::pair<std::uint64_t,
                                      deskmate::protocol::InputEvent>>
  drain_input();
  void disconnect(std::uint64_t link_id);
  void stop_all();

  [[nodiscard]] std::vector<std::uint64_t> live_ids() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::vector<std::shared_ptr<LinkSession>> snapshot() const;
  // Returns the link with `link_id`, or nullptr when it is gone. Used to route
  // an encoder callback to exactly one device instead of broadcasting.
  [[nodiscard]] std::shared_ptr<LinkSession> find(std::uint64_t link_id) const;

 private:
  LinkFactory& factory_;
  mutable std::mutex mutex_;
  std::uint64_t next_id_{1};
  std::unordered_map<std::uint64_t, std::shared_ptr<LinkSession>> links_;
};

[[nodiscard]] std::uint32_t random_nonzero_u32();

// Real libusb implementation used on Windows. Discovery is split from attach so
// the AOA control handshake can be unit-tested with a fake backend.
class AoaDiscovery {
 public:
  virtual ~AoaDiscovery() = default;
  [[nodiscard]] virtual std::vector<deskmate::platform::AoaIdentity>
  list_candidates() = 0;
  [[nodiscard]] virtual std::unique_ptr<LinkTransport> attach(
      const deskmate::platform::AoaIdentity& identity, std::string& error) = 0;
};

}  // namespace deskmate::windows
