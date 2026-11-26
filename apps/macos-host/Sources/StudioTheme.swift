import AppKit
import SwiftUI

private func adaptiveColor(light: UInt32, dark: UInt32) -> Color {
    Color(nsColor: NSColor(name: nil) { appearance in
        let isDark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        let hex = isDark ? dark : light
        let r = CGFloat((hex >> 16) & 0xFF) / 255.0
        let g = CGFloat((hex >> 8) & 0xFF) / 255.0
        let b = CGFloat(hex & 0xFF) / 255.0
        return NSColor(srgbRed: r, green: g, blue: b, alpha: 1.0)
    })
}

enum StudioPalette {
    static let canvas = adaptiveColor(light: 0xF5F5F3, dark: 0x111214)
    static let cardSurface = adaptiveColor(light: 0xFFFFFF, dark: 0x181A1E)
    static let subtleSurface = adaptiveColor(light: 0xECECE8, dark: 0x212429)
    static let hoverSurface = adaptiveColor(light: 0xE5E5E0, dark: 0x292D34)
    static let hairline = adaptiveColor(light: 0xE2E1DC, dark: 0x2A2D33)
    static let softHairline = adaptiveColor(light: 0xECEBE6, dark: 0x22252B)

    static let ink = adaptiveColor(light: 0x18181B, dark: 0xF4F4F5)
    static let inkSecondary = adaptiveColor(light: 0x5A5A60, dark: 0xA1A1AA)
    static let inkMuted = adaptiveColor(light: 0x8B8B92, dark: 0x71717A)

    static let accent = adaptiveColor(light: 0x2563EB, dark: 0x4F8BFF)
    static let accentPressed = adaptiveColor(light: 0x1D4ED8, dark: 0x3B7AF5)
    static let accentForeground = adaptiveColor(light: 0xFFFFFF, dark: 0x091326)
    static let accentSoft = adaptiveColor(light: 0xEFF6FF, dark: 0x18253D)
    static let accentBorder = adaptiveColor(light: 0xBFDBFE, dark: 0x294A82)

    static let positive = adaptiveColor(light: 0x15803D, dark: 0x34D399)
    static let positiveWash = adaptiveColor(light: 0xECFDF5, dark: 0x122921)
    static let positiveBorder = adaptiveColor(light: 0xA7F3D0, dark: 0x1F5441)

    static let warning = adaptiveColor(light: 0xB45309, dark: 0xFBBF24)
    static let warningWash = adaptiveColor(light: 0xFFFBEB, dark: 0x2C2412)
    static let warningBorder = adaptiveColor(light: 0xFDE68A, dark: 0x594518)

    static let danger = adaptiveColor(light: 0xDC2626, dark: 0xF87171)
    static let dangerWash = adaptiveColor(light: 0xFEF2F2, dark: 0x2E1618)
    static let dangerBorder = adaptiveColor(light: 0xFECACA, dark: 0x5E2428)

    static let cornerRadius: CGFloat = 10
    static let tileRadius: CGFloat = 12
    static let cardRadius: CGFloat = 16
}

enum StudioTone {
    case neutral
    case accent
    case positive
    case warning
    case danger

    var foreground: Color {
        switch self {
        case .neutral: return StudioPalette.inkSecondary
        case .accent: return StudioPalette.accent
        case .positive: return StudioPalette.positive
        case .warning: return StudioPalette.warning
        case .danger: return StudioPalette.danger
        }
    }

    var dot: Color {
        switch self {
        case .neutral: return StudioPalette.inkMuted
        case .accent: return StudioPalette.accent
        case .positive: return StudioPalette.positive
        case .warning: return StudioPalette.warning
        case .danger: return StudioPalette.danger
        }
    }

    var background: Color {
        switch self {
        case .neutral: return StudioPalette.subtleSurface
        case .accent: return StudioPalette.accentSoft
        case .positive: return StudioPalette.positiveWash
        case .warning: return StudioPalette.warningWash
        case .danger: return StudioPalette.dangerWash
        }
    }

    var border: Color {
        switch self {
        case .neutral: return StudioPalette.hairline
        case .accent: return StudioPalette.accentBorder
        case .positive: return StudioPalette.positiveBorder
        case .warning: return StudioPalette.warningBorder
        case .danger: return StudioPalette.dangerBorder
        }
    }
}

struct StudioBackground: View {
    var body: some View {
        StudioPalette.canvas
            .ignoresSafeArea()
    }
}

/// Quiet Squircle dual-display glyph (Host display + extended client display).
struct DualDisplayGlyph: View {
    let linked: Bool
    let streaming: Bool
    var size: CGFloat = 32

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(streaming ? StudioPalette.accentSoft : StudioPalette.subtleSurface)
                .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .stroke(streaming ? StudioPalette.accentBorder : StudioPalette.hairline, lineWidth: 1.0)
                )

            Canvas { context, canvasSize in
                let w = canvasSize.width
                let h = canvasSize.height

                let macRect = CGRect(x: w * 0.16, y: h * 0.24, width: w * 0.50, height: h * 0.38)
                let macPath = Path(roundedRect: macRect, cornerRadius: 2.5)
                context.stroke(macPath, with: .color(StudioPalette.ink.opacity(0.75)), lineWidth: 1.25)

                var stand = Path()
                stand.move(to: CGPoint(x: macRect.midX, y: macRect.maxY))
                stand.addLine(to: CGPoint(x: macRect.midX, y: macRect.maxY + h * 0.09))
                stand.move(to: CGPoint(x: macRect.midX - w * 0.10, y: macRect.maxY + h * 0.09))
                stand.addLine(to: CGPoint(x: macRect.midX + w * 0.10, y: macRect.maxY + h * 0.09))
                context.stroke(stand, with: .color(StudioPalette.inkMuted), lineWidth: 1.1)

                let padRect = CGRect(x: w * 0.44, y: h * 0.41, width: w * 0.40, height: h * 0.35)
                let padBg = Path(roundedRect: padRect, cornerRadius: 2.5)
                context.fill(padBg, with: .color(streaming ? StudioPalette.accentSoft : StudioPalette.cardSurface))
                context.stroke(
                    padBg,
                    with: .color((streaming || linked) ? StudioPalette.accent : StudioPalette.ink.opacity(0.85)),
                    lineWidth: (streaming || linked) ? 1.4 : 1.15
                )

                let dotRect = CGRect(x: padRect.maxX - 5.2, y: padRect.minY + 2.8, width: 2.8, height: 2.8)
                let dotColor = streaming ? StudioPalette.accent : (linked ? StudioPalette.positive : StudioPalette.inkMuted.opacity(0.6))
                context.fill(Path(ellipseIn: dotRect), with: .color(dotColor))
            }
        }
        .frame(width: size, height: size)
    }
}

/// Quiet Squircle tablet/phone glyph for Paired Devices & Active Displays.
struct StudioDeviceGlyph: View {
    let active: Bool
    let allowed: Bool
    var size: CGFloat = 34

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(active ? StudioPalette.accentSoft : StudioPalette.subtleSurface)
                .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .stroke(active ? StudioPalette.accentBorder : StudioPalette.hairline, lineWidth: 1.0)
                )

            Canvas { context, canvasSize in
                let w = canvasSize.width
                let h = canvasSize.height
                let frameRect = CGRect(x: w * 0.20, y: h * 0.27, width: w * 0.60, height: h * 0.46)
                let framePath = Path(roundedRect: frameRect, cornerRadius: 3.0)

                context.fill(framePath, with: .color(active ? StudioPalette.accentSoft : StudioPalette.cardSurface))

                let strokeColor: Color
                if !allowed {
                    strokeColor = StudioPalette.inkMuted.opacity(0.7)
                } else if active {
                    strokeColor = StudioPalette.accent
                } else {
                    strokeColor = StudioPalette.ink.opacity(0.82)
                }
                context.stroke(framePath, with: .color(strokeColor), lineWidth: active ? 1.35 : 1.1)

                let dotRect = CGRect(x: frameRect.maxX - 5.5, y: frameRect.minY + 3.5, width: 2.6, height: 2.6)
                let dotColor = !allowed ? StudioPalette.warning : (active ? StudioPalette.accent : StudioPalette.positive)
                context.fill(Path(ellipseIn: dotRect), with: .color(dotColor))
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
            Circle()
                .fill(tone.dot)
                .frame(width: 6, height: 6)
            Text(text)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(tone.foreground)
                .lineLimit(1)
                .fixedSize(horizontal: true, vertical: false)
        }
        .fixedSize(horizontal: true, vertical: false)
        .padding(.horizontal, 9)
        .padding(.vertical, 3.5)
        .background(Capsule().fill(tone.background))
        .overlay(Capsule().stroke(tone.border, lineWidth: 1.0))
    }
}

struct StudioSectionHeader: View {
    let title: String
    var detail: String? = nil

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(title)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(StudioPalette.ink)

            if let detail, !detail.isEmpty {
                Text(detail)
                    .font(.system(size: 11.5))
                    .foregroundStyle(StudioPalette.inkMuted)
            }

            Spacer(minLength: 0)
        }
    }
}

struct StudioHairlineToggle: View {
    let isOn: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack(alignment: isOn ? .trailing : .leading) {
                Capsule()
                    .fill(isOn ? StudioPalette.accent : StudioPalette.subtleSurface)
                    .overlay(
                        Capsule()
                            .stroke(isOn ? StudioPalette.accent : StudioPalette.hairline, lineWidth: 1.0)
                    )
                    .frame(width: 38, height: 22)

                Circle()
                    .fill(Color.white)
                    .frame(width: 16, height: 16)
                    .shadow(color: Color.black.opacity(0.16), radius: 1.5, x: 0, y: 1)
                    .padding(3)
            }
            .animation(.easeOut(duration: 0.16), value: isOn)
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
            Text(title)
                .font(.system(size: 11.5, weight: .medium))
                .foregroundStyle(active ? StudioPalette.accent : StudioPalette.inkSecondary)
                .lineLimit(1)
                .fixedSize(horizontal: !expand, vertical: false)
                .padding(.horizontal, expand ? 6 : 11)
                .padding(.vertical, 6)
                .frame(maxWidth: expand ? .infinity : nil)
                .fixedSize(horizontal: !expand, vertical: false)
                .background(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .fill(active ? StudioPalette.accentSoft : StudioPalette.subtleSurface)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .stroke(active ? StudioPalette.accentBorder : StudioPalette.softHairline, lineWidth: 1.0)
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
            .foregroundStyle(StudioPalette.accentForeground)
            .padding(.horizontal, 14)
            .padding(.vertical, 7.5)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .fill(configuration.isPressed ? StudioPalette.accentPressed : StudioPalette.accent)
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
            .padding(.vertical, 6.5)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .fill(configuration.isPressed ? StudioPalette.hoverSurface : StudioPalette.subtleSurface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 1.0)
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
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(configuration.isPressed ? StudioPalette.subtleSurface : Color.clear)
            )
            .contentShape(Rectangle())
    }
}

/// Oink-inspired interactive proportional multi-display topology canvas.
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
            HStack(spacing: 8) {
                Text("多屏空间拓扑")
                    .font(.system(size: 12.5, weight: .semibold))
                    .foregroundStyle(StudioPalette.ink)

                Text("\(physicalCount) 块屏幕")
                    .font(.system(size: 10.5, weight: .medium))
                    .monospacedDigit()
                    .foregroundStyle(StudioPalette.inkSecondary)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 2.5)
                    .background(Capsule().fill(StudioPalette.subtleSurface))

                Spacer(minLength: 6)

                Button {
                    onRefreshDisplays()
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 10, weight: .medium))
                        Text("刷新拓扑")
                            .font(.system(size: 11, weight: .medium))
                    }
                    .foregroundStyle(StudioPalette.inkSecondary)
                    .padding(.horizontal, 9)
                    .padding(.vertical, 4)
                    .background(
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .fill(StudioPalette.subtleSurface)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .stroke(StudioPalette.softHairline, lineWidth: 1.0)
                    )
                }
                .buttonStyle(.plain)
                .help("刷新当前连接的显示器拓扑")
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)

            Rectangle()
                .fill(StudioPalette.softHairline)
                .frame(height: 1.0)

            GeometryReader { geo in
                let items = renderedItems
                let viewportSource = (interaction.draggedDisplayID != nil && !interaction.dragStartItems.isEmpty) ? interaction.dragStartItems : items
                let viewport = DisplayTopologySolver.computeViewport(
                    displays: viewportSource,
                    canvasSize: geo.size,
                    padding: 22.0
                )

                ZStack(alignment: .topLeading) {
                    NonWindowDraggingOverlay()

                    Canvas { context, size in
                        let step: CGFloat = 16
                        var gx: CGFloat = 12
                        while gx < size.width - 8 {
                            var gy: CGFloat = 12
                            while gy < size.height - 8 {
                                context.fill(
                                    Path(ellipseIn: CGRect(x: gx - 0.8, y: gy - 0.8, width: 1.6, height: 1.6)),
                                    with: .color(StudioPalette.hairline.opacity(0.65))
                                )
                                gy += step
                            }
                            gx += step
                        }

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
                                with: .color(interaction.isSwapPreview ? StudioPalette.warning : StudioPalette.accent.opacity(0.8)),
                                style: StrokeStyle(lineWidth: 1.2, dash: [4, 4])
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
                        let w = max(44.0, rect.width)
                        let h = max(32.0, rect.height)
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
            .frame(height: 212)
            .background(StudioPalette.canvas)
            .clipped()

            Rectangle()
                .fill(StudioPalette.softHairline)
                .frame(height: 1.0)

            HStack(spacing: 12) {
                if let sel = activeSelectedItem {
                    HStack(spacing: 8) {
                        Text("#\(sel.indexNumber) \(roleBadgeText(for: sel))")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundStyle(StudioPalette.inkMuted)
                        Text(sel.name)
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(sel.isDeskMateVirtual ? StudioPalette.accent : StudioPalette.ink)
                            .lineLimit(1)
                        Text("(\(Int(sel.x.rounded())), \(Int(sel.y.rounded())))")
                            .font(.system(size: 11, design: .monospaced))
                            .monospacedDigit()
                            .foregroundStyle(StudioPalette.inkMuted)
                    }
                } else {
                    Text("主屏\(position.shortLabel)")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(StudioPalette.ink)
                }

                Spacer(minLength: 8)

                if interaction.isSwapPreview {
                    Text("松手将交换两块屏幕顺序")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(StudioPalette.accent)
                } else if let sel = activeSelectedItem {
                    Text(sel.isGhostPreview ? specSummary : sel.resolutionLabel)
                        .font(.system(size: 11, design: .monospaced))
                        .monospacedDigit()
                        .foregroundStyle(StudioPalette.inkSecondary)
                        .lineLimit(1)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 9.5)
            .background(StudioPalette.cardSurface)
        }
        .background(StudioPalette.cardSurface)
        .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                .stroke(StudioPalette.hairline, lineWidth: 1.0)
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
                return StudioPalette.cardSurface.opacity(0.65)
            }
            if isSelected || (item.isDeskMateVirtual && item.streaming) {
                return StudioPalette.accentSoft
            }
            return StudioPalette.cardSurface
        }()

        let borderColor: Color = {
            if isDragged || isSelected {
                return StudioPalette.accent
            }
            if item.isGhostPreview {
                return StudioPalette.hairline
            }
            if item.isDeskMateVirtual {
                return item.streaming ? StudioPalette.accent : StudioPalette.accentBorder
            }
            return StudioPalette.hairline
        }()

        let badgeBg: Color = (item.isDeskMateVirtual || isSelected)
            ? StudioPalette.accent
            : StudioPalette.inkSecondary

        ZStack(alignment: .topLeading) {
            if item.isGhostPreview {
                RoundedRectangle(cornerRadius: StudioPalette.tileRadius, style: .continuous)
                    .fill(fillColor)
                    .overlay(
                        RoundedRectangle(cornerRadius: StudioPalette.tileRadius, style: .continuous)
                            .strokeBorder(borderColor, style: StrokeStyle(lineWidth: isDragged ? 1.5 : 1.2, dash: [4, 4]))
                    )
            } else {
                RoundedRectangle(cornerRadius: StudioPalette.tileRadius, style: .continuous)
                    .fill(fillColor)
                    .overlay(
                        RoundedRectangle(cornerRadius: StudioPalette.tileRadius, style: .continuous)
                            .stroke(borderColor, lineWidth: (isDragged || isSelected) ? 1.5 : 1.0)
                    )
            }

            VStack(spacing: 2) {
                HStack(spacing: 5) {
                    Text("\(item.indexNumber)")
                        .font(.system(size: 9, weight: .semibold, design: .monospaced))
                        .foregroundStyle(StudioPalette.accentForeground)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1.5)
                        .background(Capsule().fill(badgeBg))

                    if width >= 62 {
                        Text(item.name)
                            .font(.system(size: 10, weight: .medium))
                            .foregroundStyle(item.isGhostPreview ? StudioPalette.inkMuted : (isSelected ? StudioPalette.accent : StudioPalette.ink))
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }

                    Spacer(minLength: 0)
                }
                .padding(.horizontal, 7)
                .padding(.top, 7)

                Spacer(minLength: 0)

                if height >= 40 {
                    let sub: String = {
                        if item.isGhostPreview { return "拖动设定方位" }
                        if item.streaming && item.liveFps > 0 { return "\(item.liveFps) FPS" }
                        if item.isDeskMateVirtual { return "USB-C 副屏" }
                        if item.isPrimary { return "主屏" }
                        return "\(item.pixelWidth)×\(item.pixelHeight)"
                    }()
                    HStack {
                        Text(sub)
                            .font(.system(size: 9.5, weight: .medium, design: .monospaced))
                            .monospacedDigit()
                            .foregroundStyle(item.isDeskMateVirtual && !item.isGhostPreview ? StudioPalette.accent : StudioPalette.inkMuted)
                            .lineLimit(1)
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 7)
                    .padding(.bottom, 6)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: StudioPalette.tileRadius, style: .continuous))
        }
        .frame(width: width, height: height)
        .shadow(color: isDragged ? Color.black.opacity(0.14) : Color.clear, radius: 6, x: 0, y: 2)
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
