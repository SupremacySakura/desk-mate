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

/// Interactive spatial dual-display monitor arranger card.
/// Shows the Mac primary screen at center and lets the user click any cardinal side
/// (top, bottom, left, right) to position virtual extended HarmonyOS displays.
struct SpatialDisplayArrangerView: View {
    let linked: Bool
    let streaming: Bool
    let position: DisplayPosition
    var sessions: [ActiveDisplaySessionState] = []
    var selectedLinkID: UInt64? = nil
    let autoVirtualDisplay: Bool
    let sourceLabel: String
    let streamSummary: String
    let liveFps: Int
    let liveBitrateMbps: Double
    let onSelectPosition: (DisplayPosition) -> Void
    var onSelectSession: (UInt64) -> Void = { _ in }

    private var selectedSession: ActiveDisplaySessionState? {
        if let selectedLinkID, let match = sessions.first(where: { $0.linkID == selectedLinkID }) {
            return match
        }
        return sessions.first
    }

    private func sessions(at slot: DisplayPosition) -> [ActiveDisplaySessionState] {
        if sessions.isEmpty {
            return []
        }
        return sessions.filter { $0.position == slot }
    }

    private func isOccupied(_ slot: DisplayPosition) -> Bool {
        if sessions.isEmpty {
            return slot == position
        }
        return sessions.contains { $0.position == slot }
    }

    var body: some View {
        VStack(spacing: 0) {
            // Card top spec header
            HStack(spacing: 8) {
                HStack(spacing: 6) {
                    Rectangle()
                        .fill(streaming ? StudioPalette.crimson : (linked ? StudioPalette.champagne : StudioPalette.inkMuted))
                        .frame(width: 5, height: 5)
                    Text("TOPOLOGY // 副屏拓扑布局")
                        .font(.system(size: 9.5, weight: .semibold, design: .monospaced))
                        .tracking(1.0)
                        .foregroundStyle(StudioPalette.inkMuted)
                }

                Spacer(minLength: 6)

                Text(autoVirtualDisplay ? (sessions.count > 1 ? "\(sessions.count) 台独立屏" : "自动虚拟屏") : "物理/指定源")
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
            }
            .padding(.horizontal, 14)
            .padding(.top, 10)
            .padding(.bottom, 7)

            Rectangle()
                .fill(StudioPalette.softHairline)
                .frame(height: 0.75)

            // Interactive 3x3 Spatial Canvas
            ZStack {
                // Background precision crosshair + orthogonal USB bridge lines
                Canvas { context, size in
                    let cx = size.width / 2
                    let cy = size.height / 2

                    // Corner registration ticks (studio calibration aesthetic, no decorative clutter)
                    let inset: CGFloat = 10
                    let tick: CGFloat = 6
                    var corners = Path()
                    // Top-left
                    corners.move(to: CGPoint(x: inset, y: inset + tick))
                    corners.addLine(to: CGPoint(x: inset, y: inset))
                    corners.addLine(to: CGPoint(x: inset + tick, y: inset))
                    // Top-right
                    corners.move(to: CGPoint(x: size.width - inset - tick, y: inset))
                    corners.addLine(to: CGPoint(x: size.width - inset, y: inset))
                    corners.addLine(to: CGPoint(x: size.width - inset, y: inset + tick))
                    // Bottom-left
                    corners.move(to: CGPoint(x: inset, y: size.height - inset - tick))
                    corners.addLine(to: CGPoint(x: inset, y: size.height - inset))
                    corners.addLine(to: CGPoint(x: inset + tick, y: size.height - inset))
                    // Bottom-right
                    corners.move(to: CGPoint(x: size.width - inset - tick, y: size.height - inset))
                    corners.addLine(to: CGPoint(x: size.width - inset, y: size.height - inset))
                    corners.addLine(to: CGPoint(x: size.width - inset, y: size.height - inset - tick))
                    context.stroke(corners, with: .color(StudioPalette.hairline), lineWidth: 0.75)

                    // Subtle orthogonal dashed axis through center
                    var axes = Path()
                    axes.move(to: CGPoint(x: 20, y: cy))
                    axes.addLine(to: CGPoint(x: size.width - 20, y: cy))
                    axes.move(to: CGPoint(x: cx, y: 14))
                    axes.addLine(to: CGPoint(x: cx, y: size.height - 14))
                    context.stroke(
                        axes,
                        with: .color(StudioPalette.softHairline),
                        style: StrokeStyle(lineWidth: 0.75, dash: [2.5, 3.5])
                    )

                    // Active USB data bridge lines between Mac box and every occupied slot
                    for slot in DisplayPosition.allCases where isOccupied(slot) {
                        let slotItems = sessions(at: slot)
                        let hasSelected = slotItems.contains { $0.linkID == selectedSession?.linkID }
                        let slotStreaming = slotItems.contains(where: \.streaming) || (sessions.isEmpty && streaming)
                        let slotLinked = !slotItems.isEmpty || (sessions.isEmpty && linked)
                        let bridgeColor: Color
                        if hasSelected && (slotStreaming || slotLinked) {
                            bridgeColor = StudioPalette.crimson
                        } else if slotStreaming {
                            bridgeColor = StudioPalette.crimson
                        } else if slotLinked {
                            bridgeColor = StudioPalette.champagne
                        } else {
                            bridgeColor = StudioPalette.inkMuted
                        }

                        var bridge = Path()
                        let midDot: CGPoint
                        switch slot {
                        case .top:
                            bridge.move(to: CGPoint(x: cx, y: cy - 28))
                            bridge.addLine(to: CGPoint(x: cx, y: cy - 42))
                            midDot = CGPoint(x: cx, y: cy - 35)
                        case .bottom:
                            bridge.move(to: CGPoint(x: cx, y: cy + 30))
                            bridge.addLine(to: CGPoint(x: cx, y: cy + 42))
                            midDot = CGPoint(x: cx, y: cy + 36)
                        case .left:
                            bridge.move(to: CGPoint(x: cx - 47, y: cy))
                            bridge.addLine(to: CGPoint(x: cx - 64, y: cy))
                            midDot = CGPoint(x: cx - 55.5, y: cy)
                        case .right:
                            bridge.move(to: CGPoint(x: cx + 47, y: cy))
                            bridge.addLine(to: CGPoint(x: cx + 64, y: cy))
                            midDot = CGPoint(x: cx + 55.5, y: cy)
                        }
                        context.stroke(bridge, with: .color(bridgeColor), lineWidth: (slotStreaming || slotLinked) ? 1.5 : 1.0)
                        context.fill(
                            Path(CGRect(x: midDot.x - 2.5, y: midDot.y - 2.5, width: 5, height: 5)),
                            with: .color(bridgeColor)
                        )
                    }
                }

                VStack(spacing: 11) {
                    // TOP SLOT
                    cardinalSlot(.top, width: 108, height: 42)

                    // MIDDLE ROW: LEFT + MAC + RIGHT
                    HStack(spacing: 18) {
                        cardinalSlot(.left, width: 78, height: 56)
                        macPrimaryBox
                        cardinalSlot(.right, width: 78, height: 56)
                    }

                    // BOTTOM SLOT
                    cardinalSlot(.bottom, width: 108, height: 42)
                }
                .padding(.vertical, 8)
            }
            .frame(maxWidth: .infinity)
            .frame(height: 204)
            .background(StudioPalette.canvasMid)

            Rectangle()
                .fill(StudioPalette.softHairline)
                .frame(height: 0.75)

            // Bottom live telemetry & layout footer inside card
            VStack(alignment: .leading, spacing: 4.5) {
                HStack(spacing: 6) {
                    Text(sessions.count > 1 ? "选中副屏" : "当前布局")
                        .font(.system(size: 11))
                        .foregroundStyle(StudioPalette.inkMuted)
                    Spacer()
                    if let selectedSession {
                        Text("\(selectedSession.deviceName) · \(selectedSession.position.shortLabel)")
                            .font(.system(size: 11.5, weight: .semibold))
                            .foregroundStyle(StudioPalette.crimson)
                            .lineLimit(1)
                    } else {
                        Text("主屏\(position.shortLabel)")
                            .font(.system(size: 11.5, weight: .semibold))
                            .foregroundStyle(StudioPalette.crimson)
                    }
                }

                HStack(spacing: 6) {
                    Text(autoVirtualDisplay ? "显示规格" : "输入源")
                        .font(.system(size: 11))
                        .foregroundStyle(StudioPalette.inkMuted)
                    Spacer()
                    Text(specSummary)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(StudioPalette.inkSecondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
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

    private var specSummary: String {
        if let selectedSession, !selectedSession.streamSummary.isEmpty {
            return selectedSession.streamSummary
        }
        if !streamSummary.isEmpty {
            return streamSummary
        }
        if autoVirtualDisplay {
            return "按设备比例自动匹配 HiDPI"
        }
        return sourceLabel
    }

    private var macPrimaryBox: some View {
        VStack(spacing: 0) {
            ZStack(alignment: .top) {
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .fill(StudioPalette.cardSurface)
                    .overlay(
                        RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                            .stroke(StudioPalette.ink.opacity(0.75), lineWidth: 1.0)
                    )

                VStack(spacing: 4) {
                    // Subtle menu bar line
                    Rectangle()
                        .fill(StudioPalette.softHairline)
                        .frame(height: 5)
                        .overlay(
                            HStack(spacing: 3) {
                                Circle().fill(StudioPalette.inkMuted.opacity(0.55)).frame(width: 2, height: 2)
                                Spacer()
                            }
                            .padding(.horizontal, 4)
                        )

                    Spacer(minLength: 2)

                    Text("MAC 主屏")
                        .font(.system(size: 10.5, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)
                    Text("Primary")
                        .font(.system(size: 9, weight: .medium, design: .monospaced))
                        .foregroundStyle(StudioPalette.inkMuted)

                    Spacer(minLength: 2)
                }
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous))
            }
            .frame(width: 92, height: 50)

            // Stand stem + foot
            Rectangle()
                .fill(StudioPalette.hairline)
                .frame(width: 1.5, height: 3.5)
            Rectangle()
                .fill(StudioPalette.inkMuted.opacity(0.75))
                .frame(width: 24, height: 1.5)
        }
    }

    @ViewBuilder
    private func cardinalSlot(_ slotPosition: DisplayPosition, width: CGFloat, height: CGFloat) -> some View {
        let slotSessions = sessions(at: slotPosition)
        let occupied = isOccupied(slotPosition)
        let containsSelected = slotSessions.contains { $0.linkID == selectedSession?.linkID } || (sessions.isEmpty && slotPosition == position)
        let slotStreaming = slotSessions.contains(where: \.streaming) || (sessions.isEmpty && streaming)
        let slotLinked = !slotSessions.isEmpty || (sessions.isEmpty && linked)

        Button {
            if !slotSessions.isEmpty {
                if let currentSelected = selectedSession?.linkID,
                   let idx = slotSessions.firstIndex(where: { $0.linkID == currentSelected }),
                   slotSessions.count > 1 {
                    let next = slotSessions[(idx + 1) % slotSessions.count]
                    onSelectSession(next.linkID)
                } else {
                    onSelectSession(slotSessions[0].linkID)
                }
            } else {
                onSelectPosition(slotPosition)
            }
        } label: {
            if occupied {
                let displayItem = slotSessions.first(where: { $0.linkID == selectedSession?.linkID }) ?? slotSessions.first
                let slotTitle: String = {
                    if slotSessions.count > 1 {
                        return "\(slotSessions.count) 台副屏"
                    }
                    if let item = displayItem {
                        return item.deviceName
                    }
                    return "扩展副屏"
                }()
                let slotSub: String = {
                    if slotSessions.count > 1 {
                        let sumFps = slotSessions.reduce(0) { $0 + $1.liveFps }
                        return sumFps > 0 ? "\(sumFps) FPS" : "点击切换"
                    }
                    if let item = displayItem {
                        if item.streaming && item.liveFps > 0 {
                            return "\(item.liveFps) FPS"
                        }
                        return item.streaming ? "推流中" : "已连接"
                    }
                    return activeSlotSubtitle
                }()

                VStack(spacing: 2) {
                    HStack(spacing: 4) {
                        Rectangle()
                            .fill(slotStreaming ? StudioPalette.crimson : (slotLinked ? StudioPalette.champagne : StudioPalette.inkMuted))
                            .frame(width: 4, height: 4)
                        Text(slotTitle)
                            .font(.system(size: 9.5, weight: .semibold))
                            .foregroundStyle(StudioPalette.ink)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                    .padding(.horizontal, 4)

                    Text(slotSub)
                        .font(.system(size: 9, weight: .medium, design: .monospaced))
                        .foregroundStyle((slotStreaming || slotLinked) ? StudioPalette.crimson : StudioPalette.inkMuted)
                        .lineLimit(1)
                }
                .frame(width: width, height: height)
                .background(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .fill(slotStreaming ? StudioPalette.roseWash : (slotLinked ? StudioPalette.subtleSurface : StudioPalette.cardSurface))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .stroke(
                            containsSelected ? ((slotStreaming || slotLinked) ? StudioPalette.crimson : StudioPalette.ink) : StudioPalette.champagne,
                            lineWidth: containsSelected ? 1.3 : 1.0
                        )
                )
            } else {
                VStack(spacing: 1) {
                    Text("+ \(slotPosition.shortLabel)")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundStyle(StudioPalette.inkMuted)
                }
                .frame(width: width, height: height)
                .background(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .fill(StudioPalette.cardSurface.opacity(0.65))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .strokeBorder(StudioPalette.hairline, style: StrokeStyle(lineWidth: 0.75, dash: [3, 3]))
                )
            }
        }
        .buttonStyle(.plain)
        .help(occupied ? "点击选中主屏\(slotPosition.shortLabel)的设备" : "将当前副屏放置于主屏\(slotPosition.shortLabel)")
    }

    private var activeSlotSubtitle: String {
        if streaming && liveFps > 0 {
            return "\(liveFps) FPS"
        }
        if streaming {
            return "推流中"
        }
        if linked {
            return "已连接"
        }
        return position.shortLabel
    }
}
