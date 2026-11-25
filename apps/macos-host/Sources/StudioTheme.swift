import AppKit
import SwiftUI

enum StudioPalette {
    static let canvasTop = Color(red: 1.000, green: 1.000, blue: 1.000)
    static let canvasMid = Color(red: 0.988, green: 0.984, blue: 0.973)     // #FCFBF8
    static let canvasEdge = Color(red: 0.957, green: 0.941, blue: 0.910)    // #F4F0E8
    static let cardSurface = Color(red: 1.000, green: 1.000, blue: 1.000)   // #FFFFFF
    static let subtleSurface = Color(red: 0.980, green: 0.969, blue: 0.949) // #FAF7F2
    static let hairline = Color(red: 0.863, green: 0.839, blue: 0.800)      // #DCD6CC
    static let softHairline = Color(red: 0.918, green: 0.898, blue: 0.867)  // #EAE4DD
    static let ink = Color(red: 0.137, green: 0.125, blue: 0.149)           // #232026
    static let inkSecondary = Color(red: 0.353, green: 0.329, blue: 0.369)  // #5A545E
    static let inkMuted = Color(red: 0.549, green: 0.522, blue: 0.557)      // #8C858E
    static let crimson = Color(red: 0.616, green: 0.098, blue: 0.239)       // #9D193D
    static let crimsonPressed = Color(red: 0.494, green: 0.071, blue: 0.188)// #7E1230
    static let roseWash = Color(red: 0.969, green: 0.918, blue: 0.937)      // #F7EAEF
    static let roseBorder = Color(red: 0.851, green: 0.714, blue: 0.761)    // #D9B6C2
    static let champagne = Color(red: 0.725, green: 0.592, blue: 0.357)     // #B9975B
    static let jade = Color(red: 0.176, green: 0.490, blue: 0.357)          // #2D7D5B
    static let jadeWash = Color(red: 0.933, green: 0.969, blue: 0.953)      // #EEF7F3
    static let amber = Color(red: 0.659, green: 0.392, blue: 0.118)         // #A8641E
    static let amberWash = Color(red: 0.984, green: 0.953, blue: 0.910)     // #FBF3E8

    static let cornerRadius: CGFloat = 3
    static let cardRadius: CGFloat = 4
}

enum StudioTone {
    case neutral
    case crimson
    case positive
    case warning

    var foreground: Color {
        switch self {
        case .neutral: return StudioPalette.inkSecondary
        case .crimson: return StudioPalette.crimson
        case .positive: return StudioPalette.jade
        case .warning: return StudioPalette.amber
        }
    }

    var dot: Color {
        switch self {
        case .neutral: return StudioPalette.inkMuted
        case .crimson: return StudioPalette.crimson
        case .positive: return StudioPalette.jade
        case .warning: return StudioPalette.amber
        }
    }

    var background: Color {
        switch self {
        case .neutral: return StudioPalette.subtleSurface
        case .crimson: return StudioPalette.roseWash
        case .positive: return StudioPalette.jadeWash
        case .warning: return StudioPalette.amberWash
        }
    }

    var border: Color {
        switch self {
        case .neutral: return StudioPalette.hairline
        case .crimson: return StudioPalette.roseBorder
        case .positive: return Color(red: 0.710, green: 0.859, blue: 0.788)
        case .warning: return Color(red: 0.898, green: 0.780, blue: 0.631)
        }
    }
}

struct StudioBackground: View {
    var body: some View {
        ZStack {
            LinearGradient(
                colors: [StudioPalette.canvasTop, StudioPalette.canvasMid, StudioPalette.canvasEdge],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            RadialGradient(
                colors: [StudioPalette.canvasTop.opacity(0.95), Color.clear],
                center: .top,
                startRadius: 20,
                endRadius: 520
            )
        }
        .ignoresSafeArea()
    }
}

/// Clean architectural dual-display emblem (Mac host screen + linked HarmonyOS tablet).
struct DualDisplayGlyph: View {
    let linked: Bool
    let streaming: Bool
    var size: CGFloat = 34

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                .fill(streaming ? StudioPalette.roseWash.opacity(0.55) : StudioPalette.subtleSurface)
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(streaming ? StudioPalette.roseBorder : StudioPalette.hairline, lineWidth: 0.75)
                )

            Canvas { context, canvasSize in
                let w = canvasSize.width
                let h = canvasSize.height

                // Primary Mac display (back-left)
                let macRect = CGRect(x: w * 0.17, y: h * 0.24, width: w * 0.52, height: h * 0.40)
                let macPath = Path(roundedRect: macRect, cornerRadius: 1.5)
                context.stroke(macPath, with: .color(StudioPalette.ink.opacity(0.78)), lineWidth: 1.15)

                // Stand foot under Mac display
                var stand = Path()
                stand.move(to: CGPoint(x: macRect.midX, y: macRect.maxY))
                stand.addLine(to: CGPoint(x: macRect.midX, y: macRect.maxY + h * 0.08))
                stand.move(to: CGPoint(x: macRect.midX - w * 0.11, y: macRect.maxY + h * 0.08))
                stand.addLine(to: CGPoint(x: macRect.midX + w * 0.11, y: macRect.maxY + h * 0.08))
                context.stroke(stand, with: .color(StudioPalette.inkMuted), lineWidth: 1.0)

                // Secondary Tablet display (front-right overlapping)
                let padRect = CGRect(x: w * 0.44, y: h * 0.41, width: w * 0.40, height: h * 0.34)
                let padBg = Path(roundedRect: padRect, cornerRadius: 1.5)
                let fill = streaming ? StudioPalette.roseWash : StudioPalette.cardSurface
                context.fill(padBg, with: .color(fill))
                context.stroke(
                    padBg,
                    with: .color((streaming || linked) ? StudioPalette.crimson : StudioPalette.ink),
                    lineWidth: (streaming || linked) ? 1.25 : 1.05
                )

                // Status dot inside secondary display
                let dotRect = CGRect(x: padRect.maxX - 5.5, y: padRect.minY + 3.0, width: 2.8, height: 2.8)
                let dotColor = streaming ? StudioPalette.crimson : (linked ? StudioPalette.champagne : StudioPalette.inkMuted.opacity(0.6))
                context.fill(Path(dotRect), with: .color(dotColor))
            }
        }
        .frame(width: size, height: size)
    }
}

/// Clean architectural tablet glyph for the Paired Devices list.
struct StudioDeviceGlyph: View {
    let active: Bool
    let allowed: Bool
    var size: CGFloat = 32

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                .fill(active ? StudioPalette.roseWash.opacity(0.65) : StudioPalette.subtleSurface)
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(active ? StudioPalette.roseBorder : StudioPalette.hairline, lineWidth: 0.75)
                )

            Canvas { context, canvasSize in
                let w = canvasSize.width
                let h = canvasSize.height
                let frameRect = CGRect(x: w * 0.18, y: h * 0.27, width: w * 0.64, height: h * 0.46)
                let framePath = Path(roundedRect: frameRect, cornerRadius: 2.0)

                if active {
                    context.fill(framePath, with: .color(StudioPalette.roseWash))
                } else {
                    context.fill(framePath, with: .color(StudioPalette.cardSurface))
                }

                let strokeColor: Color
                if !allowed {
                    strokeColor = StudioPalette.inkMuted.opacity(0.65)
                } else if active {
                    strokeColor = StudioPalette.crimson
                } else {
                    strokeColor = StudioPalette.ink.opacity(0.8)
                }
                context.stroke(framePath, with: .color(strokeColor), lineWidth: active ? 1.25 : 1.05)

                // Subtle inner screen border
                let innerRect = frameRect.insetBy(dx: 2.4, dy: 2.2)
                context.stroke(
                    Path(roundedRect: innerRect, cornerRadius: 0.8),
                    with: .color(StudioPalette.hairline.opacity(0.85)),
                    lineWidth: 0.6
                )

                // Top-right status square
                let dotRect = CGRect(x: innerRect.maxX - 3.6, y: innerRect.minY + 1.8, width: 2.2, height: 2.2)
                let dotColor = !allowed ? StudioPalette.inkMuted : (active ? StudioPalette.crimson : StudioPalette.jade)
                context.fill(Path(dotRect), with: .color(dotColor))
            }
        }
        .frame(width: size, height: size)
    }
}

struct StudioStatusBadge: View {
    let text: String
    let tone: StudioTone

    var body: some View {
        HStack(spacing: 6) {
            Rectangle()
                .fill(tone.dot)
                .frame(width: 5, height: 5)
            Text(text)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(tone.foreground)
                .lineLimit(1)
                .fixedSize(horizontal: true, vertical: false)
        }
        .fixedSize(horizontal: true, vertical: false)
        .padding(.horizontal, 9)
        .padding(.vertical, 4)
        .background(
            RoundedRectangle(cornerRadius: 2.5, style: .continuous)
                .fill(tone.background)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 2.5, style: .continuous)
                .stroke(tone.border, lineWidth: 0.75)
        )
    }
}

struct StudioSectionHeader: View {
    let english: String
    let chinese: String
    var accent: Color = StudioPalette.crimson

    var body: some View {
        HStack(spacing: 8) {
            HStack(spacing: 5) {
                Text(english)
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .tracking(1.3)
                    .foregroundStyle(accent)
                Text("/")
                    .font(.system(size: 10, weight: .regular))
                    .foregroundStyle(StudioPalette.hairline)
                Text(chinese)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(StudioPalette.ink)
            }
            .fixedSize(horizontal: true, vertical: false)

            Rectangle()
                .fill(StudioPalette.hairline)
                .frame(height: 0.75)
        }
    }
}

struct StudioHairlineToggle: View {
    let isOn: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 9) {
                ZStack(alignment: isOn ? .trailing : .leading) {
                    Rectangle()
                        .fill(StudioPalette.softHairline)
                        .frame(width: 48, height: 2)

                    if isOn {
                        Rectangle()
                            .fill(StudioPalette.crimson)
                            .frame(width: 48, height: 2)
                    }

                    RoundedRectangle(cornerRadius: 1.5, style: .continuous)
                        .fill(isOn ? StudioPalette.crimson : StudioPalette.cardSurface)
                        .frame(width: 10, height: 10)
                        .overlay(
                            RoundedRectangle(cornerRadius: 1.5, style: .continuous)
                                .stroke(isOn ? StudioPalette.crimsonPressed : StudioPalette.inkMuted, lineWidth: 0.9)
                        )
                }
                .frame(width: 48, height: 18)

                Text(isOn ? "开启" : "关闭")
                    .font(.system(size: 11.5, weight: .medium, design: .monospaced))
                    .foregroundStyle(isOn ? StudioPalette.crimson : StudioPalette.inkMuted)
                    .frame(width: 30, alignment: .trailing)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

struct StudioSegmentOption: View {
    let title: String
    let active: Bool
    var expand: Bool = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Rectangle()
                    .fill(active ? StudioPalette.crimson : Color.clear)
                    .frame(width: 4, height: 4)
                    .overlay(
                        Rectangle()
                            .stroke(active ? StudioPalette.crimson : StudioPalette.inkMuted.opacity(0.7), lineWidth: 0.75)
                    )
                Text(title)
                    .font(.system(size: 11.5, weight: active ? .semibold : .regular))
                    .foregroundStyle(active ? StudioPalette.crimson : StudioPalette.inkSecondary)
                    .lineLimit(1)
                    .fixedSize(horizontal: !expand, vertical: false)
            }
            .padding(.horizontal, expand ? 6 : 10)
            .padding(.vertical, 5.5)
            .frame(maxWidth: expand ? .infinity : nil)
            .fixedSize(horizontal: !expand, vertical: false)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .fill(active ? StudioPalette.roseWash : StudioPalette.cardSurface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .stroke(active ? StudioPalette.roseBorder : StudioPalette.hairline, lineWidth: 0.75)
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

struct StudioPrimaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 12, weight: .medium))
            .tracking(0.6)
            .foregroundStyle(Color.white)
            .padding(.horizontal, 16)
            .padding(.vertical, 8.5)
            .background(
                ZStack(alignment: .top) {
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .fill(configuration.isPressed ? StudioPalette.crimsonPressed : StudioPalette.crimson)

                    Rectangle()
                        .fill(StudioPalette.champagne.opacity(0.65))
                        .frame(height: 1)
                        .padding(.horizontal, 1)
                }
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .stroke(StudioPalette.crimsonPressed, lineWidth: 0.75)
            )
            .contentShape(Rectangle())
    }
}

struct StudioSecondaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 11.5, weight: .medium))
            .foregroundStyle(StudioPalette.ink)
            .padding(.horizontal, 12)
            .padding(.vertical, 7)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .fill(configuration.isPressed ? StudioPalette.subtleSurface : StudioPalette.cardSurface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 0.75)
            )
            .contentShape(Rectangle())
    }
}

struct StudioFlatActionStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .fill(configuration.isPressed ? StudioPalette.subtleSurface : Color.clear)
            )
            .contentShape(Rectangle())
    }
}

/// Oink-inspired interactive proportional multi-display topology canvas.
/// Renders all physical and DeskMate virtual displays at true relative sizes,
/// supports dragging to reposition or swap/reorder screens with magnetic edge snapping,
/// and applies normalized OS display coordinates on drop.
struct SpatialDisplayArrangerView: View {
    @ObservedObject var interaction: TopologyInteractionModel
    let linked: Bool
    let streaming: Bool
    let position: DisplayPosition
    var displays: [TopologyDisplayItem] = []
    var sessions: [ActiveDisplaySessionState] = []
    var selectedLinkID: UInt64? = nil
    let autoVirtualDisplay: Bool
    let sourceLabel: String
    let streamSummary: String
    let liveFps: Int
    let liveBitrateMbps: Double
    let onSelectPosition: (DisplayPosition) -> Void
    var onSelectSession: (UInt64) -> Void = { _ in }
    var onApplyLayout: ([(displayID: UInt32, origin: CGPoint)]) -> Void = { _ in }
    var onRefreshDisplays: () -> Void = {}

    private var baseItems: [TopologyDisplayItem] {
        var list = displays
        if list.isEmpty {
            list = [
                TopologyDisplayItem(
                    id: 1,
                    indexNumber: 1,
                    name: "Mac 主屏",
                    x: 0,
                    y: 0,
                    width: 1728,
                    height: 1117,
                    pixelWidth: 3456,
                    pixelHeight: 2234,
                    refreshHz: 120,
                    isPrimary: true,
                    isVirtual: false,
                    isDeskMateVirtual: false
                )
            ]
        }
        // When no device is connected and auto virtual display is enabled, show a draggable preview tile.
        if sessions.isEmpty && autoVirtualDisplay && !list.contains(where: \.isDeskMateVirtual) {
            let primary = list.first(where: \.isPrimary) ?? list[0]
            var bbMinX = primary.x
            var bbMinY = primary.y
            var bbMaxX = primary.maxX
            var bbMaxY = primary.maxY
            for d in list {
                bbMinX = min(bbMinX, d.x)
                bbMinY = min(bbMinY, d.y)
                bbMaxX = max(bbMaxX, d.maxX)
                bbMaxY = max(bbMaxY, d.maxY)
            }
            let gw = max(900.0, (primary.width * 0.72).rounded())
            let gh = max(600.0, (primary.height * 0.72).rounded())
            let gx: Double
            let gy: Double
            switch position {
            case .right:
                gx = bbMaxX
                gy = primary.y + (primary.height - gh) * 0.5
            case .left:
                gx = bbMinX - gw
                gy = primary.y + (primary.height - gh) * 0.5
            case .top:
                gx = primary.x + (primary.width - gw) * 0.5
                gy = bbMinY - gh
            case .bottom:
                gx = primary.x + (primary.width - gw) * 0.5
                gy = bbMaxY
            }
            list.append(TopologyDisplayItem(
                id: DisplayTopologySolver.ghostDisplayID,
                indexNumber: list.count + 1,
                name: "副屏预览",
                x: gx,
                y: gy,
                width: gw,
                height: gh,
                pixelWidth: 0,
                pixelHeight: 0,
                refreshHz: 60,
                isPrimary: false,
                isVirtual: true,
                isDeskMateVirtual: true,
                isGhostPreview: true
            ))
        }
        return list
    }

    private var renderedItems: [TopologyDisplayItem] {
        (interaction.draggedDisplayID != nil && !interaction.previewItems.isEmpty) ? interaction.previewItems : baseItems
    }

    private var activeSelectedItem: TopologyDisplayItem? {
        if let selectedLinkID, let match = renderedItems.first(where: { $0.linkID == selectedLinkID }) {
            return match
        }
        if let selectedDisplayID = interaction.selectedDisplayID, let match = renderedItems.first(where: { $0.id == selectedDisplayID }) {
            return match
        }
        return renderedItems.first(where: \.isDeskMateVirtual) ?? renderedItems.first(where: \.isPrimary) ?? renderedItems.first
    }

    private var physicalCount: Int {
        baseItems.filter { !$0.isGhostPreview }.count
    }

    var body: some View {
        VStack(spacing: 0) {
            // Card top spec header
            HStack(spacing: 8) {
                HStack(spacing: 6) {
                    Rectangle()
                        .fill(streaming ? StudioPalette.crimson : (linked ? StudioPalette.champagne : StudioPalette.inkMuted))
                        .frame(width: 5, height: 5)
                    Text("TOPOLOGY // 多屏拓扑布局")
                        .font(.system(size: 9.5, weight: .semibold, design: .monospaced))
                        .tracking(1.0)
                        .foregroundStyle(StudioPalette.inkMuted)
                }

                Spacer(minLength: 6)

                Text("\(physicalCount) 块屏幕")
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(StudioPalette.inkSecondary)
                    .padding(.horizontal, 7)
                    .padding(.vertical, 2.5)
                    .background(
                        RoundedRectangle(cornerRadius: 2, style: .continuous)
                            .fill(StudioPalette.subtleSurface)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 2, style: .continuous)
                            .stroke(StudioPalette.hairline, lineWidth: 0.6)
                    )

                Button {
                    onRefreshDisplays()
                } label: {
                    Image(systemName: "arrow.clockwise")
                        .font(.system(size: 9.5, weight: .semibold))
                        .foregroundStyle(StudioPalette.inkSecondary)
                        .frame(width: 20, height: 20)
                        .background(
                            RoundedRectangle(cornerRadius: 2, style: .continuous)
                                .fill(StudioPalette.subtleSurface)
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: 2, style: .continuous)
                                .stroke(StudioPalette.hairline, lineWidth: 0.6)
                        )
                }
                .buttonStyle(.plain)
                .help("刷新当前连接的显示器拓扑")
            }
            .padding(.horizontal, 14)
            .padding(.top, 10)
            .padding(.bottom, 7)

            Rectangle()
                .fill(StudioPalette.softHairline)
                .frame(height: 0.75)

            // Interactive Proportional Multi-Display Canvas
            GeometryReader { geo in
                let items = renderedItems
                let viewportSource = (interaction.draggedDisplayID != nil && !interaction.dragStartItems.isEmpty) ? interaction.dragStartItems : items
                let viewport = DisplayTopologySolver.computeViewport(
                    displays: viewportSource,
                    canvasSize: geo.size,
                    padding: 20.0
                )

                ZStack(alignment: .topLeading) {
                    NonWindowDraggingOverlay()

                    // Dot-grid & calibration corner ticks + magnetic alignment guides
                    Canvas { context, size in
                        let step: CGFloat = 16
                        var gx: CGFloat = 12
                        while gx < size.width - 8 {
                            var gy: CGFloat = 12
                            while gy < size.height - 8 {
                                context.fill(
                                    Path(CGRect(x: gx - 0.6, y: gy - 0.6, width: 1.2, height: 1.2)),
                                    with: .color(StudioPalette.hairline.opacity(0.42))
                                )
                                gy += step
                            }
                            gx += step
                        }

                        let inset: CGFloat = 8
                        let tick: CGFloat = 5
                        var corners = Path()
                        corners.move(to: CGPoint(x: inset, y: inset + tick))
                        corners.addLine(to: CGPoint(x: inset, y: inset))
                        corners.addLine(to: CGPoint(x: inset + tick, y: inset))
                        corners.move(to: CGPoint(x: size.width - inset - tick, y: inset))
                        corners.addLine(to: CGPoint(x: size.width - inset, y: inset))
                        corners.addLine(to: CGPoint(x: size.width - inset, y: inset + tick))
                        corners.move(to: CGPoint(x: inset, y: size.height - inset - tick))
                        corners.addLine(to: CGPoint(x: inset, y: size.height - inset))
                        corners.addLine(to: CGPoint(x: inset + tick, y: size.height - inset))
                        corners.move(to: CGPoint(x: size.width - inset - tick, y: size.height - inset))
                        corners.addLine(to: CGPoint(x: size.width - inset, y: size.height - inset))
                        corners.addLine(to: CGPoint(x: size.width - inset, y: size.height - inset - tick))
                        context.stroke(corners, with: .color(StudioPalette.hairline), lineWidth: 0.75)

                        for guide in interaction.activeGuides {
                            var p = Path()
                            if guide.isHorizontal {
                                let a = viewport.desktopToCanvas(x: guide.start, y: guide.pos)
                                let b = viewport.desktopToCanvas(x: guide.end, y: guide.pos)
                                p.move(to: a)
                                p.addLine(to: b)
                            } else {
                                let a = viewport.desktopToCanvas(x: guide.pos, y: guide.start)
                                let b = viewport.desktopToCanvas(x: guide.pos, y: guide.end)
                                p.move(to: a)
                                p.addLine(to: b)
                            }
                            context.stroke(
                                p,
                                with: .color(interaction.isSwapPreview ? StudioPalette.champagne : StudioPalette.crimson.opacity(0.7)),
                                style: StrokeStyle(lineWidth: 1.0, dash: [3, 3])
                            )
                        }
                    }

                    ForEach(items) { item in
                        let rect = viewport.desktopRectToCanvas(
                            x: item.x,
                            y: item.y,
                            width: item.width,
                            height: item.height
                        )
                        let w = max(32.0, rect.width)
                        let h = max(24.0, rect.height)
                        let isDragged = (interaction.draggedDisplayID == item.id)
                        let isSelected = (activeSelectedItem?.id == item.id)

                        displayTile(item, width: w, height: h, isSelected: isSelected, isDragged: isDragged)
                            .position(x: rect.minX + w * 0.5, y: rect.minY + h * 0.5)
                            .zIndex(isDragged ? 10 : (isSelected ? 5 : 1))
                            .gesture(
                                DragGesture(minimumDistance: 3, coordinateSpace: .named("topologyCanvas"))
                                    .onChanged { value in
                                        if interaction.draggedDisplayID == nil {
                                            interaction.draggedDisplayID = item.id
                                            interaction.dragStartItems = baseItems
                                            interaction.selectedDisplayID = item.id
                                            if let linkID = item.linkID {
                                                onSelectSession(linkID)
                                            }
                                        }
                                        let baseList = interaction.dragStartItems.isEmpty ? baseItems : interaction.dragStartItems
                                        guard let orig = baseList.first(where: { $0.id == item.id }) else { return }
                                        let candX = orig.x + Double(value.translation.width) / max(viewport.scale, 1e-6)
                                        let candY = orig.y + Double(value.translation.height) / max(viewport.scale, 1e-6)
                                        let snapped = DisplayTopologySolver.snapDraggedDisplay(
                                            displays: baseList,
                                            draggedID: item.id,
                                            candidateX: candX,
                                            candidateY: candY
                                        )
                                        interaction.previewItems = snapped.items
                                        interaction.activeGuides = snapped.guides
                                        interaction.isSwapPreview = snapped.isSwap
                                    }
                                    .onEnded { _ in
                                        let finalItems = interaction.previewItems
                                        interaction.draggedDisplayID = nil
                                        interaction.dragStartItems = []
                                        interaction.previewItems = []
                                        interaction.activeGuides = []
                                        interaction.isSwapPreview = false

                                        guard !finalItems.isEmpty else { return }
                                        if item.isGhostPreview {
                                            if let primary = finalItems.first(where: \.isPrimary),
                                               let ghost = finalItems.first(where: \.isGhostPreview) {
                                                let inferred = DisplayTopologySolver.inferCardinalPosition(primary: primary, target: ghost)
                                                onSelectPosition(inferred)
                                            }
                                        } else {
                                            let normalized = DisplayTopologySolver.normalizeToPrimaryOrigin(
                                                finalItems.filter { !$0.isGhostPreview }
                                            )
                                            let payload: [(displayID: UInt32, origin: CGPoint)] = normalized.map {
                                                ($0.id, CGPoint(x: $0.x, y: $0.y))
                                            }
                                            onApplyLayout(payload)
                                        }
                                    }
                            )
                            .simultaneousGesture(
                                TapGesture().onEnded {
                                    interaction.selectedDisplayID = item.id
                                    if let linkID = item.linkID {
                                        onSelectSession(linkID)
                                    }
                                }
                            )
                    }
                }
                .coordinateSpace(name: "topologyCanvas")
            }
            .frame(maxWidth: .infinity)
            .frame(height: 248)
            .background(StudioPalette.canvasMid)
            .clipped()

            Rectangle()
                .fill(StudioPalette.softHairline)
                .frame(height: 0.75)

            // Bottom live telemetry & layout footer inside card
            VStack(alignment: .leading, spacing: 4.5) {
                if let sel = activeSelectedItem {
                    HStack(spacing: 6) {
                        Text("#\(sel.indexNumber) \(roleBadgeText(for: sel))")
                            .font(.system(size: 10.5, weight: .medium))
                            .foregroundStyle(StudioPalette.inkMuted)
                        Spacer()
                        Text(sel.name)
                            .font(.system(size: 11.5, weight: .semibold))
                            .foregroundStyle(sel.isDeskMateVirtual ? StudioPalette.crimson : StudioPalette.ink)
                            .lineLimit(1)
                    }

                    HStack(spacing: 6) {
                        Text("坐标 (\(Int(sel.x.rounded())), \(Int(sel.y.rounded())))")
                            .font(.system(size: 10.5, design: .monospaced))
                            .foregroundStyle(StudioPalette.inkMuted)
                        Spacer()
                        Text(sel.isGhostPreview ? specSummary : sel.resolutionLabel)
                            .font(.system(size: 10.5, design: .monospaced))
                            .foregroundStyle(StudioPalette.inkSecondary)
                            .lineLimit(1)
                    }
                } else {
                    HStack(spacing: 6) {
                        Text("当前布局")
                            .font(.system(size: 11))
                            .foregroundStyle(StudioPalette.inkMuted)
                        Spacer()
                        Text("主屏\(position.shortLabel)")
                            .font(.system(size: 11.5, weight: .semibold))
                            .foregroundStyle(StudioPalette.crimson)
                    }
                }

                if streaming && liveFps > 0 {
                    HStack(spacing: 6) {
                        Text(sessions.count > 1 ? "总输出" : "实时输出")
                            .font(.system(size: 11))
                            .foregroundStyle(StudioPalette.inkMuted)
                        Spacer()
                        Text(liveBitrateMbps > 0
                             ? String(format: "%d FPS · %.1f Mbps", liveFps, liveBitrateMbps)
                             : "\(liveFps) FPS")
                            .font(.system(size: 11, weight: .semibold, design: .monospaced))
                            .foregroundStyle(StudioPalette.crimson)
                    }
                }

                Text(interaction.isSwapPreview ? "松手将交换两块屏幕的排列顺序" : "拖动屏幕可调整排列位置与顺序，松手自动吸附生效")
                    .font(.system(size: 10))
                    .foregroundStyle(interaction.isSwapPreview ? StudioPalette.crimson : StudioPalette.inkMuted)
                    .lineLimit(1)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 8.5)
            .background(StudioPalette.cardSurface)
        }
        .background(StudioPalette.cardSurface)
        .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                .stroke(StudioPalette.hairline, lineWidth: 0.75)
        )
    }

    @ViewBuilder
    private func displayTile(
        _ item: TopologyDisplayItem,
        width: CGFloat,
        height: CGFloat,
        isSelected: Bool,
        isDragged: Bool
    ) -> some View {
        let fillColor: Color = {
            if item.isGhostPreview {
                return StudioPalette.cardSurface.opacity(0.72)
            }
            if item.isDeskMateVirtual {
                return item.streaming ? StudioPalette.roseWash : StudioPalette.subtleSurface
            }
            return item.isPrimary ? StudioPalette.cardSurface : StudioPalette.subtleSurface
        }()

        let borderColor: Color = {
            if isDragged {
                return StudioPalette.crimson
            }
            if item.isGhostPreview {
                return StudioPalette.hairline
            }
            if item.isDeskMateVirtual {
                return (isSelected || item.streaming) ? StudioPalette.crimson : StudioPalette.roseBorder
            }
            if isSelected {
                return StudioPalette.ink
            }
            return item.isPrimary ? StudioPalette.ink.opacity(0.75) : StudioPalette.champagne
        }()

        let badgeBg: Color = item.isDeskMateVirtual
            ? StudioPalette.crimson
            : (item.isPrimary ? StudioPalette.ink : StudioPalette.inkSecondary)

        ZStack(alignment: .topLeading) {
            if item.isGhostPreview {
                RoundedRectangle(cornerRadius: 3, style: .continuous)
                    .fill(fillColor)
                    .overlay(
                        RoundedRectangle(cornerRadius: 3, style: .continuous)
                            .strokeBorder(borderColor, style: StrokeStyle(lineWidth: isDragged ? 1.4 : 0.9, dash: [3, 3]))
                    )
            } else {
                RoundedRectangle(cornerRadius: 3, style: .continuous)
                    .fill(fillColor)
                    .overlay(
                        RoundedRectangle(cornerRadius: 3, style: .continuous)
                            .stroke(borderColor, lineWidth: isDragged ? 1.6 : (isSelected ? 1.35 : 0.95))
                    )
            }

            VStack(spacing: 0) {
                if item.isPrimary {
                    Rectangle()
                        .fill(StudioPalette.softHairline)
                        .frame(height: 4)
                }

                VStack(spacing: 1.5) {
                    HStack(spacing: 4) {
                        Text("\(item.indexNumber)")
                            .font(.system(size: 8.5, weight: .bold, design: .monospaced))
                            .foregroundStyle(Color.white)
                            .padding(.horizontal, 4)
                            .padding(.vertical, 1)
                            .background(
                                RoundedRectangle(cornerRadius: 2, style: .continuous)
                                    .fill(badgeBg)
                            )

                        if width >= 54 {
                            Text(item.name)
                                .font(.system(size: 9, weight: .semibold))
                                .foregroundStyle(item.isGhostPreview ? StudioPalette.inkMuted : StudioPalette.ink)
                                .lineLimit(1)
                                .truncationMode(.tail)
                        }

                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 4)
                    .padding(.top, 4)

                    Spacer(minLength: 0)

                    if height >= 36 {
                        let sub: String = {
                            if item.isGhostPreview { return "拖动设定方位" }
                            if item.streaming && item.liveFps > 0 { return "\(item.liveFps) FPS" }
                            if item.isDeskMateVirtual { return "DeskMate 副屏" }
                            if item.isPrimary { return "主显示器" }
                            return "\(item.pixelWidth)×\(item.pixelHeight)"
                        }()
                        Text(sub)
                            .font(.system(size: 8.5, weight: .medium, design: .monospaced))
                            .foregroundStyle(item.isDeskMateVirtual && !item.isGhostPreview ? StudioPalette.crimson : StudioPalette.inkMuted)
                            .lineLimit(1)
                            .padding(.horizontal, 4)
                            .padding(.bottom, 4)
                    }
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 3, style: .continuous))
        }
        .frame(width: width, height: height)
        .shadow(color: isDragged ? Color.black.opacity(0.14) : Color.clear, radius: 5, x: 0, y: 2)
        .contentShape(Rectangle())
    }

    private func roleBadgeText(for item: TopologyDisplayItem) -> String {
        if item.isGhostPreview { return "副屏位置预览" }
        if item.isDeskMateVirtual { return "DeskMate 虚拟屏" }
        if item.isPrimary { return "Mac 主屏" }
        if item.isVirtual { return "虚拟显示器" }
        return "物理扩展屏"
    }

    private var specSummary: String {
        if let selected = sessions.first(where: { $0.linkID == selectedLinkID }) ?? sessions.first,
           !selected.streamSummary.isEmpty {
            return selected.streamSummary
        }
        if !streamSummary.isEmpty {
            return streamSummary
        }
        if autoVirtualDisplay {
            return "按设备比例自动匹配 HiDPI"
        }
        return sourceLabel
    }
}
