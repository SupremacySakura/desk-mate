import AppKit
import CoreGraphics
import SwiftUI

struct TopologyDisplayItem: Identifiable, Equatable {
    let id: UInt32
    var indexNumber: Int
    var name: String
    var x: Double
    var y: Double
    var width: Double
    var height: Double
    var pixelWidth: Int
    var pixelHeight: Int
    var refreshHz: Double
    var isPrimary: Bool
    var isVirtual: Bool
    var isDeskMateVirtual: Bool
    var isGhostPreview: Bool = false
    var linkID: UInt64? = nil
    var streaming: Bool = false
    var liveFps: Int = 0

    var midX: Double { x + width * 0.5 }
    var midY: Double { y + height * 0.5 }
    var maxX: Double { x + width }
    var maxY: Double { y + height }

    var resolutionLabel: String {
        if pixelWidth > 0 && pixelHeight > 0 {
            let hz = Int(refreshHz.rounded())
            return hz > 0 ? "\(pixelWidth)×\(pixelHeight) @ \(hz)Hz" : "\(pixelWidth)×\(pixelHeight)"
        }
        return "\(Int(width.rounded()))×\(Int(height.rounded()))"
    }
}

struct TopologyGuideLine: Equatable {
    let isHorizontal: Bool
    let pos: Double
    let start: Double
    let end: Double
}

struct TopologySnapResult: Equatable {
    var items: [TopologyDisplayItem]
    var guides: [TopologyGuideLine]
    var isSwap: Bool
}

struct TopologyViewport: Equatable {
    let scale: Double
    let originX: Double
    let originY: Double
    let offsetX: Double
    let offsetY: Double

    func desktopToCanvas(x: Double, y: Double) -> CGPoint {
        CGPoint(
            x: offsetX + (x - originX) * scale,
            y: offsetY + (y - originY) * scale
        )
    }

    func desktopRectToCanvas(x: Double, y: Double, width: Double, height: Double) -> CGRect {
        let pt = desktopToCanvas(x: x, y: y)
        return CGRect(x: pt.x, y: pt.y, width: width * scale, height: height * scale)
    }

    func canvasToDesktop(x: Double, y: Double) -> CGPoint {
        let safeScale = max(scale, 1e-6)
        return CGPoint(
            x: originX + (x - offsetX) / safeScale,
            y: originY + (y - offsetY) / safeScale
        )
    }
}

enum DisplayTopologySolver {
    static let ghostDisplayID: UInt32 = 0xFFFF_FFFE

    static func computeViewport(
        displays: [TopologyDisplayItem],
        canvasSize: CGSize,
        padding: Double = 22.0
    ) -> TopologyViewport {
        guard !displays.isEmpty, canvasSize.width > 20, canvasSize.height > 20 else {
            return TopologyViewport(scale: 0.05, originX: 0, originY: 0, offsetX: padding, offsetY: padding)
        }
        var minX = Double.infinity
        var minY = Double.infinity
        var maxX = -Double.infinity
        var maxY = -Double.infinity
        for d in displays {
            minX = min(minX, d.x)
            minY = min(minY, d.y)
            maxX = max(maxX, d.maxX)
            maxY = max(maxY, d.maxY)
        }
        let spanW = max(1.0, maxX - minX)
        let spanH = max(1.0, maxY - minY)
        let availW = max(20.0, Double(canvasSize.width) - padding * 2.0)
        let availH = max(20.0, Double(canvasSize.height) - padding * 2.0)
        let scale = min(availW / spanW, availH / spanH)
        let drawnW = spanW * scale
        let drawnH = spanH * scale
        let offsetX = (Double(canvasSize.width) - drawnW) * 0.5
        let offsetY = (Double(canvasSize.height) - drawnH) * 0.5
        return TopologyViewport(scale: scale, originX: minX, originY: minY, offsetX: offsetX, offsetY: offsetY)
    }

    static func snapDraggedDisplay(
        displays: [TopologyDisplayItem],
        draggedID: UInt32,
        candidateX: Double,
        candidateY: Double,
        snapTolerance: Double = 90.0
    ) -> TopologySnapResult {
        guard let draggedIdx = displays.firstIndex(where: { $0.id == draggedID }) else {
            return TopologySnapResult(items: displays, guides: [], isSwap: false)
        }
        var result = displays
        let dragged = displays[draggedIdx]
        let dw = dragged.width
        let dh = dragged.height
        let dcx = candidateX + dw * 0.5
        let dcy = candidateY + dh * 0.5

        // 1. Swap / reorder if dragged center lands inside the central 60% of another display.
        if let targetIdx = displays.indices.first(where: { idx in
            guard idx != draggedIdx else { return false }
            let o = displays[idx]
            let x0 = o.x + o.width * 0.2
            let x1 = o.x + o.width * 0.8
            let y0 = o.y + o.height * 0.2
            let y1 = o.y + o.height * 0.8
            return dcx >= x0 && dcx <= x1 && dcy >= y0 && dcy <= y1
        }) {
            let target = displays[targetIdx]
            let dx = dragged.midX - target.midX
            let dy = dragged.midY - target.midY
            var newDX = target.x
            var newDY = target.y
            var newTX = dragged.x
            var newTY = dragged.y

            if abs(dx) >= abs(dy) {
                if dx < 0 {
                    // Dragged was on the left, target was on the right -> [T][D]
                    newTX = dragged.x
                    newTY = dragged.y + (dh - target.height) * 0.5
                    newDX = newTX + target.width
                    newDY = target.y + (target.height - dh) * 0.5
                } else {
                    // Dragged was on the right, target was on the left -> [D][T]
                    newDX = target.x
                    newDY = target.y + (target.height - dh) * 0.5
                    newTX = newDX + dw
                    newTY = dragged.y + (dh - target.height) * 0.5
                }
            } else {
                if dy < 0 {
                    // Dragged was above target -> [T] / [D]
                    newTX = dragged.x + (dw - target.width) * 0.5
                    newTY = dragged.y
                    newDX = target.x + (target.width - dw) * 0.5
                    newDY = newTY + target.height
                } else {
                    // Dragged was below target -> [D] / [T]
                    newDX = target.x + (target.width - dw) * 0.5
                    newDY = target.y
                    newTX = dragged.x + (dw - target.width) * 0.5
                    newTY = newDY + dh
                }
            }

            result[draggedIdx].x = newDX.rounded()
            result[draggedIdx].y = newDY.rounded()
            result[targetIdx].x = newTX.rounded()
            result[targetIdx].y = newTY.rounded()

            // Guarantee non-overlap against any third display by snapping collided items to nearest free edge.
            result = resolveAnyOverlaps(items: result, anchorIndices: [draggedIdx, targetIdx], snapTolerance: snapTolerance)
            let guides = alignmentGuides(for: result[draggedIdx], others: result.enumerated().compactMap { $0.offset == draggedIdx ? nil : $0.element })
            return TopologySnapResult(items: result, guides: guides, isSwap: true)
        }

        // 2. Magnetic edge snapping around all other displays.
        let others = displays.enumerated().compactMap { $0.offset == draggedIdx ? nil : $0.element }
        guard !others.isEmpty else {
            result[draggedIdx].x = candidateX.rounded()
            result[draggedIdx].y = candidateY.rounded()
            return TopologySnapResult(items: result, guides: [], isSwap: false)
        }

        var candidates: [(x: Double, y: Double)] = []
        for other in others {
            let minTouchY = min(80.0, min(dh, other.height) / 3.0)
            let yLo = other.y - dh + minTouchY
            let yHi = other.y + other.height - minTouchY
            let baseY = clamp(candidateY, min: yLo, max: yHi)
            let yEdges = [other.y, other.y + other.height - dh, other.y + (other.height - dh) * 0.5]
            var snappedY = baseY
            var bestYDiff = snapTolerance + 1.0
            for edgeY in yEdges {
                let diff = abs(candidateY - edgeY)
                if diff <= snapTolerance && diff < bestYDiff {
                    bestYDiff = diff
                    snappedY = clamp(edgeY, min: yLo, max: yHi)
                }
            }
            for sideX in [other.x + other.width, other.x - dw] {
                if !overlapsAny(x: sideX, y: snappedY, w: dw, h: dh, others: others) {
                    candidates.append((sideX, snappedY))
                } else {
                    candidates.append((sideX, baseY))
                    for edgeY in yEdges {
                        candidates.append((sideX, clamp(edgeY, min: yLo, max: yHi)))
                    }
                }
            }

            let minTouchX = min(80.0, min(dw, other.width) / 3.0)
            let xLo = other.x - dw + minTouchX
            let xHi = other.x + other.width - minTouchX
            let baseX = clamp(candidateX, min: xLo, max: xHi)
            let xEdges = [other.x, other.x + other.width - dw, other.x + (other.width - dw) * 0.5]
            var snappedX = baseX
            var bestXDiff = snapTolerance + 1.0
            for edgeX in xEdges {
                let diff = abs(candidateX - edgeX)
                if diff <= snapTolerance && diff < bestXDiff {
                    bestXDiff = diff
                    snappedX = clamp(edgeX, min: xLo, max: xHi)
                }
            }
            for sideY in [other.y + other.height, other.y - dh] {
                if !overlapsAny(x: snappedX, y: sideY, w: dw, h: dh, others: others) {
                    candidates.append((snappedX, sideY))
                } else {
                    candidates.append((baseX, sideY))
                    for edgeX in xEdges {
                        candidates.append((clamp(edgeX, min: xLo, max: xHi), sideY))
                    }
                }
            }
        }

        var best: (x: Double, y: Double)? = nil
        var bestDist = Double.infinity
        for cand in candidates {
            if !overlapsAny(x: cand.x, y: cand.y, w: dw, h: dh, others: others) {
                let d2 = (cand.x - candidateX) * (cand.x - candidateX) + (cand.y - candidateY) * (cand.y - candidateY)
                if d2 < bestDist {
                    bestDist = d2
                    best = cand
                }
            }
        }

        if best == nil {
            // Outer bounding-box fallback so we always find a valid non-overlapping adjacent spot.
            var bbMinX = Double.infinity
            var bbMinY = Double.infinity
            var bbMaxX = -Double.infinity
            var bbMaxY = -Double.infinity
            for o in others {
                bbMinX = min(bbMinX, o.x)
                bbMinY = min(bbMinY, o.y)
                bbMaxX = max(bbMaxX, o.maxX)
                bbMaxY = max(bbMaxY, o.maxY)
            }
            let fallbacks: [(x: Double, y: Double)] = [
                (bbMaxX, clamp(candidateY, min: bbMinY - dh + 40, max: bbMaxY - 40)),
                (bbMinX - dw, clamp(candidateY, min: bbMinY - dh + 40, max: bbMaxY - 40)),
                (clamp(candidateX, min: bbMinX - dw + 40, max: bbMaxX - 40), bbMaxY),
                (clamp(candidateX, min: bbMinX - dw + 40, max: bbMaxX - 40), bbMinY - dh)
            ]
            for cand in fallbacks {
                let d2 = (cand.x - candidateX) * (cand.x - candidateX) + (cand.y - candidateY) * (cand.y - candidateY)
                if d2 < bestDist {
                    bestDist = d2
                    best = cand
                }
            }
        }

        if let chosen = best {
            result[draggedIdx].x = chosen.x.rounded()
            result[draggedIdx].y = chosen.y.rounded()
        }
        let guides = alignmentGuides(for: result[draggedIdx], others: others)
        return TopologySnapResult(items: result, guides: guides, isSwap: false)
    }

    static func normalizeToPrimaryOrigin(_ displays: [TopologyDisplayItem]) -> [TopologyDisplayItem] {
        guard let primary = displays.first(where: \.isPrimary) ?? displays.first else {
            return displays
        }
        let dx = primary.x
        let dy = primary.y
        return displays.map { item in
            var copy = item
            copy.x = (item.x - dx).rounded()
            copy.y = (item.y - dy).rounded()
            return copy
        }
    }

    static func inferCardinalPosition(primary: TopologyDisplayItem, target: TopologyDisplayItem) -> DisplayPosition {
        if target.x >= primary.maxX - 4 { return .right }
        if target.maxX <= primary.x + 4 { return .left }
        if target.y >= primary.maxY - 4 { return .bottom }
        if target.maxY <= primary.y + 4 { return .top }
        let dx = (target.midX - primary.midX) / max(1.0, (primary.width + target.width) * 0.5)
        let dy = (target.midY - primary.midY) / max(1.0, (primary.height + target.height) * 0.5)
        if abs(dx) >= abs(dy) {
            return dx >= 0 ? .right : .left
        }
        return dy >= 0 ? .bottom : .top
    }

    private static func resolveAnyOverlaps(
        items: [TopologyDisplayItem],
        anchorIndices: [Int],
        snapTolerance: Double
    ) -> [TopologyDisplayItem] {
        var working = items
        for anchor in anchorIndices {
            let fixed = working.enumerated().compactMap { $0.offset == anchor ? nil : $0.element }
            let cur = working[anchor]
            if overlapsAny(x: cur.x, y: cur.y, w: cur.width, h: cur.height, others: fixed) {
                let snapped = snapDraggedDisplay(displays: working, draggedID: cur.id, candidateX: cur.x, candidateY: cur.y, snapTolerance: snapTolerance)
                working = snapped.items
            }
        }
        for idx in working.indices where !anchorIndices.contains(idx) {
            let fixed = working.enumerated().compactMap { $0.offset == idx ? nil : $0.element }
            let cur = working[idx]
            if overlapsAny(x: cur.x, y: cur.y, w: cur.width, h: cur.height, others: fixed) {
                let snapped = snapDraggedDisplay(displays: working, draggedID: cur.id, candidateX: cur.x, candidateY: cur.y, snapTolerance: snapTolerance)
                working = snapped.items
            }
        }
        return working
    }

    private static func overlapsAny(x: Double, y: Double, w: Double, h: Double, others: [TopologyDisplayItem]) -> Bool {
        for o in others {
            let ix = min(x + w, o.maxX) - max(x, o.x)
            let iy = min(y + h, o.maxY) - max(y, o.y)
            if ix > 0.5 && iy > 0.5 {
                return true
            }
        }
        return false
    }

    private static func alignmentGuides(for item: TopologyDisplayItem, others: [TopologyDisplayItem]) -> [TopologyGuideLine] {
        var guides: [TopologyGuideLine] = []
        let eps = 2.0
        for o in others {
            let minX = min(item.x, o.x)
            let maxX = max(item.maxX, o.maxX)
            let minY = min(item.y, o.y)
            let maxY = max(item.maxY, o.maxY)
            if abs(item.y - o.y) <= eps {
                guides.append(TopologyGuideLine(isHorizontal: true, pos: o.y, start: minX, end: maxX))
            }
            if abs(item.maxY - o.maxY) <= eps {
                guides.append(TopologyGuideLine(isHorizontal: true, pos: o.maxY, start: minX, end: maxX))
            }
            if abs(item.midY - o.midY) <= eps {
                guides.append(TopologyGuideLine(isHorizontal: true, pos: o.midY, start: minX, end: maxX))
            }
            if abs(item.x - o.x) <= eps {
                guides.append(TopologyGuideLine(isHorizontal: false, pos: o.x, start: minY, end: maxY))
            }
            if abs(item.maxX - o.maxX) <= eps {
                guides.append(TopologyGuideLine(isHorizontal: false, pos: o.maxX, start: minY, end: maxY))
            }
            if abs(item.midX - o.midX) <= eps {
                guides.append(TopologyGuideLine(isHorizontal: false, pos: o.midX, start: minY, end: maxY))
            }
        }
        return guides
    }

    private static func clamp(_ v: Double, min lo: Double, max hi: Double) -> Double {
        if hi < lo { return (lo + hi) * 0.5 }
        return max(lo, min(hi, v))
    }
}

final class TopologyInteractionModel: ObservableObject {
    @Published var selectedDisplayID: UInt32? = nil
    @Published var draggedDisplayID: UInt32? = nil
    @Published var dragStartItems: [TopologyDisplayItem] = []
    @Published var previewItems: [TopologyDisplayItem] = []
    @Published var activeGuides: [TopologyGuideLine] = []
    @Published var isSwapPreview: Bool = false
}

/// Prevents NSWindow.isMovableByWindowBackground from hijacking drag gestures inside the topology canvas.
final class NonWindowDraggingNSView: NSView {
    override var mouseDownCanMoveWindow: Bool { false }
}

struct NonWindowDraggingOverlay: NSViewRepresentable {
    func makeNSView(context: Context) -> NonWindowDraggingNSView {
        let view = NonWindowDraggingNSView()
        view.wantsLayer = true
        view.layer?.backgroundColor = NSColor.clear.cgColor
        return view
    }

    func updateNSView(_ nsView: NonWindowDraggingNSView, context: Context) {}
}
