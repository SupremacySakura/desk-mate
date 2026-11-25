#include "deskmate/windows/display_topology.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace deskmate::windows {
namespace {

double clamp_val(double v, double lo, double hi) {
  if (hi < lo) {
    return (lo + hi) * 0.5;
  }
  return std::max(lo, std::min(hi, v));
}

bool overlaps_any(double x, double y, double w, double h,
                  const std::vector<TopologyDisplayItem>& others) {
  for (const auto& o : others) {
    const double ix = std::min(x + w, o.max_x()) - std::max(x, o.x);
    const double iy = std::min(y + h, o.max_y()) - std::max(y, o.y);
    if (ix > 0.5 && iy > 0.5) {
      return true;
    }
  }
  return false;
}

std::vector<TopologyGuideLine> compute_alignment_guides(
    const TopologyDisplayItem& item,
    const std::vector<TopologyDisplayItem>& others) {
  std::vector<TopologyGuideLine> guides;
  constexpr double kEps = 2.0;
  for (const auto& o : others) {
    const double min_x = std::min(item.x, o.x);
    const double max_x = std::max(item.max_x(), o.max_x());
    const double min_y = std::min(item.y, o.y);
    const double max_y = std::max(item.max_y(), o.max_y());
    if (std::abs(item.y - o.y) <= kEps) {
      guides.push_back({true, o.y, min_x, max_x});
    }
    if (std::abs(item.max_y() - o.max_y()) <= kEps) {
      guides.push_back({true, o.max_y(), min_x, max_x});
    }
    if (std::abs(item.mid_y() - o.mid_y()) <= kEps) {
      guides.push_back({true, o.mid_y(), min_x, max_x});
    }
    if (std::abs(item.x - o.x) <= kEps) {
      guides.push_back({false, o.x, min_y, max_y});
    }
    if (std::abs(item.max_x() - o.max_x()) <= kEps) {
      guides.push_back({false, o.max_x(), min_y, max_y});
    }
    if (std::abs(item.mid_x() - o.mid_x()) <= kEps) {
      guides.push_back({false, o.mid_x(), min_y, max_y});
    }
  }
  return guides;
}

std::vector<TopologyDisplayItem> others_except(
    const std::vector<TopologyDisplayItem>& list, std::size_t skip) {
  std::vector<TopologyDisplayItem> out;
  out.reserve(list.size());
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (i != skip) {
      out.push_back(list[i]);
    }
  }
  return out;
}

std::vector<TopologyDisplayItem> resolve_any_overlaps(
    std::vector<TopologyDisplayItem> working, std::size_t idx_a,
    std::size_t idx_b, double snap_tolerance) {
  const std::size_t anchors[2] = {idx_a, idx_b};
  for (std::size_t anchor : anchors) {
    const auto fixed = others_except(working, anchor);
    const auto& cur = working[anchor];
    if (overlaps_any(cur.x, cur.y, cur.width, cur.height, fixed)) {
      working = snap_dragged_display(working, cur.id, cur.x, cur.y,
                                     snap_tolerance)
                    .items;
    }
  }
  for (std::size_t i = 0; i < working.size(); ++i) {
    if (i == idx_a || i == idx_b) {
      continue;
    }
    const auto fixed = others_except(working, i);
    const auto& cur = working[i];
    if (overlaps_any(cur.x, cur.y, cur.width, cur.height, fixed)) {
      working = snap_dragged_display(working, cur.id, cur.x, cur.y,
                                     snap_tolerance)
                    .items;
    }
  }
  return working;
}

}  // namespace

TopologyViewport compute_topology_viewport(
    const std::vector<TopologyDisplayItem>& displays, double canvas_width,
    double canvas_height, double padding) {
  if (displays.empty() || canvas_width <= 20.0 || canvas_height <= 20.0) {
    return {0.05, 0.0, 0.0, padding, padding};
  }
  double min_x = std::numeric_limits<double>::infinity();
  double min_y = std::numeric_limits<double>::infinity();
  double max_x = -std::numeric_limits<double>::infinity();
  double max_y = -std::numeric_limits<double>::infinity();
  for (const auto& d : displays) {
    min_x = std::min(min_x, d.x);
    min_y = std::min(min_y, d.y);
    max_x = std::max(max_x, d.max_x());
    max_y = std::max(max_y, d.max_y());
  }
  const double span_w = std::max(1.0, max_x - min_x);
  const double span_h = std::max(1.0, max_y - min_y);
  const double avail_w = std::max(20.0, canvas_width - padding * 2.0);
  const double avail_h = std::max(20.0, canvas_height - padding * 2.0);
  const double scale = std::min(avail_w / span_w, avail_h / span_h);
  const double drawn_w = span_w * scale;
  const double drawn_h = span_h * scale;
  const double offset_x = (canvas_width - drawn_w) * 0.5;
  const double offset_y = (canvas_height - drawn_h) * 0.5;
  return {scale, min_x, min_y, offset_x, offset_y};
}

TopologySnapResult snap_dragged_display(
    const std::vector<TopologyDisplayItem>& displays, std::uint32_t dragged_id,
    double candidate_x, double candidate_y, double snap_tolerance) {
  std::size_t dragged_idx = displays.size();
  for (std::size_t i = 0; i < displays.size(); ++i) {
    if (displays[i].id == dragged_id) {
      dragged_idx = i;
      break;
    }
  }
  if (dragged_idx >= displays.size()) {
    return {displays, {}, false};
  }

  std::vector<TopologyDisplayItem> result = displays;
  const TopologyDisplayItem& dragged = displays[dragged_idx];
  const double dw = dragged.width;
  const double dh = dragged.height;
  const double dcx = candidate_x + dw * 0.5;
  const double dcy = candidate_y + dh * 0.5;

  // 1. Swap / reorder when dragged center lands inside the central 60% of another display.
  std::size_t target_idx = displays.size();
  for (std::size_t i = 0; i < displays.size(); ++i) {
    if (i == dragged_idx) {
      continue;
    }
    const auto& o = displays[i];
    if (dcx >= o.x + o.width * 0.2 && dcx <= o.x + o.width * 0.8 &&
        dcy >= o.y + o.height * 0.2 && dcy <= o.y + o.height * 0.8) {
      target_idx = i;
      break;
    }
  }

  if (target_idx < displays.size()) {
    const auto& target = displays[target_idx];
    const double dx = dragged.mid_x() - target.mid_x();
    const double dy = dragged.mid_y() - target.mid_y();
    double new_dx = target.x;
    double new_dy = target.y;
    double new_tx = dragged.x;
    double new_ty = dragged.y;

    if (std::abs(dx) >= std::abs(dy)) {
      if (dx < 0.0) {
        new_tx = dragged.x;
        new_ty = dragged.y + (dh - target.height) * 0.5;
        new_dx = new_tx + target.width;
        new_dy = target.y + (target.height - dh) * 0.5;
      } else {
        new_dx = target.x;
        new_dy = target.y + (target.height - dh) * 0.5;
        new_tx = new_dx + dw;
        new_ty = dragged.y + (dh - target.height) * 0.5;
      }
    } else {
      if (dy < 0.0) {
        new_tx = dragged.x + (dw - target.width) * 0.5;
        new_ty = dragged.y;
        new_dx = target.x + (target.width - dw) * 0.5;
        new_dy = new_ty + target.height;
      } else {
        new_dx = target.x + (target.width - dw) * 0.5;
        new_dy = target.y;
        new_tx = dragged.x + (dw - target.width) * 0.5;
        new_ty = new_dy + dh;
      }
    }

    result[dragged_idx].x = std::round(new_dx);
    result[dragged_idx].y = std::round(new_dy);
    result[target_idx].x = std::round(new_tx);
    result[target_idx].y = std::round(new_ty);
    result = resolve_any_overlaps(std::move(result), dragged_idx, target_idx,
                                  snap_tolerance);
    auto guides =
        compute_alignment_guides(result[dragged_idx],
                                 others_except(result, dragged_idx));
    return {std::move(result), std::move(guides), true};
  }

  // 2. Magnetic edge snapping around all other displays.
  const auto others = others_except(displays, dragged_idx);
  if (others.empty()) {
    result[dragged_idx].x = std::round(candidate_x);
    result[dragged_idx].y = std::round(candidate_y);
    return {std::move(result), {}, false};
  }

  std::vector<std::pair<double, double>> candidates;
  candidates.reserve(others.size() * 12);

  for (const auto& other : others) {
    const double min_touch_y =
        std::min(80.0, std::min(dh, other.height) / 3.0);
    const double y_lo = other.y - dh + min_touch_y;
    const double y_hi = other.y + other.height - min_touch_y;
    const double base_y = clamp_val(candidate_y, y_lo, y_hi);
    const double y_edges[3] = {
        other.y,
        other.y + other.height - dh,
        other.y + (other.height - dh) * 0.5,
    };
    double snapped_y = base_y;
    double best_y_diff = snap_tolerance + 1.0;
    for (double edge_y : y_edges) {
      const double diff = std::abs(candidate_y - edge_y);
      if (diff <= snap_tolerance && diff < best_y_diff) {
        best_y_diff = diff;
        snapped_y = clamp_val(edge_y, y_lo, y_hi);
      }
    }
    for (double side_x : {other.x + other.width, other.x - dw}) {
      if (!overlaps_any(side_x, snapped_y, dw, dh, others)) {
        candidates.emplace_back(side_x, snapped_y);
      } else {
        candidates.emplace_back(side_x, base_y);
        for (double edge_y : y_edges) {
          candidates.emplace_back(side_x, clamp_val(edge_y, y_lo, y_hi));
        }
      }
    }

    const double min_touch_x = std::min(80.0, std::min(dw, other.width) / 3.0);
    const double x_lo = other.x - dw + min_touch_x;
    const double x_hi = other.x + other.width - min_touch_x;
    const double base_x = clamp_val(candidate_x, x_lo, x_hi);
    const double x_edges[3] = {
        other.x,
        other.x + other.width - dw,
        other.x + (other.width - dw) * 0.5,
    };
    double snapped_x = base_x;
    double best_x_diff = snap_tolerance + 1.0;
    for (double edge_x : x_edges) {
      const double diff = std::abs(candidate_x - edge_x);
      if (diff <= snap_tolerance && diff < best_x_diff) {
        best_x_diff = diff;
        snapped_x = clamp_val(edge_x, x_lo, x_hi);
      }
    }
    for (double side_y : {other.y + other.height, other.y - dh}) {
      if (!overlaps_any(snapped_x, side_y, dw, dh, others)) {
        candidates.emplace_back(snapped_x, side_y);
      } else {
        candidates.emplace_back(base_x, side_y);
        for (double edge_x : x_edges) {
          candidates.emplace_back(clamp_val(edge_x, x_lo, x_hi), side_y);
        }
      }
    }
  }

  bool found = false;
  double best_x = candidate_x;
  double best_y = candidate_y;
  double best_dist = std::numeric_limits<double>::infinity();

  for (const auto& [cx, cy] : candidates) {
    if (!overlaps_any(cx, cy, dw, dh, others)) {
      const double d2 = (cx - candidate_x) * (cx - candidate_x) +
                        (cy - candidate_y) * (cy - candidate_y);
      if (d2 < best_dist) {
        best_dist = d2;
        best_x = cx;
        best_y = cy;
        found = true;
      }
    }
  }

  if (!found) {
    double bb_min_x = std::numeric_limits<double>::infinity();
    double bb_min_y = std::numeric_limits<double>::infinity();
    double bb_max_x = -std::numeric_limits<double>::infinity();
    double bb_max_y = -std::numeric_limits<double>::infinity();
    for (const auto& o : others) {
      bb_min_x = std::min(bb_min_x, o.x);
      bb_min_y = std::min(bb_min_y, o.y);
      bb_max_x = std::max(bb_max_x, o.max_x());
      bb_max_y = std::max(bb_max_y, o.max_y());
    }
    const std::pair<double, double> fallbacks[4] = {
        {bb_max_x, clamp_val(candidate_y, bb_min_y - dh + 40.0, bb_max_y - 40.0)},
        {bb_min_x - dw, clamp_val(candidate_y, bb_min_y - dh + 40.0, bb_max_y - 40.0)},
        {clamp_val(candidate_x, bb_min_x - dw + 40.0, bb_max_x - 40.0), bb_max_y},
        {clamp_val(candidate_x, bb_min_x - dw + 40.0, bb_max_x - 40.0), bb_min_y - dh},
    };
    for (const auto& [cx, cy] : fallbacks) {
      const double d2 = (cx - candidate_x) * (cx - candidate_x) +
                        (cy - candidate_y) * (cy - candidate_y);
      if (d2 < best_dist) {
        best_dist = d2;
        best_x = cx;
        best_y = cy;
        found = true;
      }
    }
  }

  if (found) {
    result[dragged_idx].x = std::round(best_x);
    result[dragged_idx].y = std::round(best_y);
  }
  auto guides = compute_alignment_guides(result[dragged_idx], others);
  return {std::move(result), std::move(guides), false};
}

std::vector<TopologyDisplayItem> normalize_to_primary_origin(
    const std::vector<TopologyDisplayItem>& displays) {
  if (displays.empty()) {
    return displays;
  }
  const TopologyDisplayItem* primary = &displays.front();
  for (const auto& d : displays) {
    if (d.is_primary) {
      primary = &d;
      break;
    }
  }
  const double dx = primary->x;
  const double dy = primary->y;
  std::vector<TopologyDisplayItem> out = displays;
  for (auto& d : out) {
    d.x = std::round(d.x - dx);
    d.y = std::round(d.y - dy);
  }
  return out;
}

int infer_cardinal_position_code(const TopologyDisplayItem& primary,
                                 const TopologyDisplayItem& target) {
  if (target.x >= primary.max_x() - 4.0) {
    return 0;  // Right
  }
  if (target.max_x() <= primary.x + 4.0) {
    return 1;  // Left
  }
  if (target.y >= primary.max_y() - 4.0) {
    return 3;  // Bottom
  }
  if (target.max_y() <= primary.y + 4.0) {
    return 2;  // Top
  }
  const double dx = (target.mid_x() - primary.mid_x()) /
                    std::max(1.0, (primary.width + target.width) * 0.5);
  const double dy = (target.mid_y() - primary.mid_y()) /
                    std::max(1.0, (primary.height + target.height) * 0.5);
  if (std::abs(dx) >= std::abs(dy)) {
    return dx >= 0.0 ? 0 : 1;
  }
  return dy >= 0.0 ? 3 : 2;
}

}  // namespace deskmate::windows
