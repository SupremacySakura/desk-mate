#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace deskmate::windows {

struct TopologyDisplayItem {
  std::uint32_t id{};
  std::uintptr_t handle{};
  std::string device_name;
  int index_number{1};
  std::string name;
  double x{0.0};
  double y{0.0};
  double width{0.0};
  double height{0.0};
  int pixel_width{0};
  int pixel_height{0};
  int refresh_hz{60};
  bool is_primary{false};
  bool is_virtual{false};
  bool is_deskmate_virtual{false};
  bool is_ghost_preview{false};
  std::uint64_t link_id{0};
  bool streaming{false};
  int live_fps{0};

  [[nodiscard]] double mid_x() const noexcept { return x + width * 0.5; }
  [[nodiscard]] double mid_y() const noexcept { return y + height * 0.5; }
  [[nodiscard]] double max_x() const noexcept { return x + width; }
  [[nodiscard]] double max_y() const noexcept { return y + height; }
};

struct TopologyGuideLine {
  bool is_horizontal{false};
  double pos{0.0};
  double start{0.0};
  double end{0.0};
};

struct TopologySnapResult {
  std::vector<TopologyDisplayItem> items;
  std::vector<TopologyGuideLine> guides;
  bool is_swap{false};
};

struct TopologyViewport {
  double scale{0.05};
  double origin_x{0.0};
  double origin_y{0.0};
  double offset_x{0.0};
  double offset_y{0.0};

  [[nodiscard]] double to_canvas_x(double dx) const noexcept {
    return offset_x + (dx - origin_x) * scale;
  }
  [[nodiscard]] double to_canvas_y(double dy) const noexcept {
    return offset_y + (dy - origin_y) * scale;
  }
  [[nodiscard]] double to_desktop_x(double cx) const noexcept {
    const double s = scale > 1e-6 ? scale : 1e-6;
    return origin_x + (cx - offset_x) / s;
  }
  [[nodiscard]] double to_desktop_y(double cy) const noexcept {
    const double s = scale > 1e-6 ? scale : 1e-6;
    return origin_y + (cy - offset_y) / s;
  }
};

inline constexpr std::uint32_t kGhostPreviewDisplayId = 0xFFFFFFFEu;

[[nodiscard]] TopologyViewport compute_topology_viewport(
    const std::vector<TopologyDisplayItem>& displays, double canvas_width,
    double canvas_height, double padding = 22.0);

[[nodiscard]] TopologySnapResult snap_dragged_display(
    const std::vector<TopologyDisplayItem>& displays, std::uint32_t dragged_id,
    double candidate_x, double candidate_y, double snap_tolerance = 90.0);

[[nodiscard]] std::vector<TopologyDisplayItem> normalize_to_primary_origin(
    const std::vector<TopologyDisplayItem>& displays);

// Returns DisplayPosition code: 0=Right, 1=Left, 2=Top, 3=Bottom.
[[nodiscard]] int infer_cardinal_position_code(
    const TopologyDisplayItem& primary, const TopologyDisplayItem& target);

}  // namespace deskmate::windows
