import AppKit
import SwiftUI

enum MenuBarIcon {
    /// Renders a crisp monochrome template NSImage (dual overlapping displays)
    /// that automatically adapts to macOS light/dark menu bars.
    static func makeImage(connected: Bool, streaming: Bool) -> NSImage {
        let size = NSSize(width: 22, height: 18)
        let image = NSImage(size: size, flipped: false) { _ in
            NSColor.labelColor.setStroke()
            NSColor.labelColor.setFill()

            // Back-left Mac display outline
            let macRect = NSRect(x: 1.5, y: 5.5, width: 12.0, height: 9.0)
            let macPath = NSBezierPath(roundedRect: macRect, xRadius: 2.0, yRadius: 2.0)
            macPath.lineWidth = 1.45
            macPath.stroke()

            // Subtle Mac stand foot
            let stand = NSBezierPath()
            stand.move(to: NSPoint(x: 5.0, y: 3.3))
            stand.line(to: NSPoint(x: 9.0, y: 3.3))
            stand.lineWidth = 1.4
            stand.lineCapStyle = .round
            stand.stroke()

            // Front-right overlapping HarmonyOS tablet display
            let tabRect = NSRect(x: 9.5, y: 2.0, width: 11.0, height: 8.0)
            let clearPath = NSBezierPath(roundedRect: tabRect.insetBy(dx: -0.9, dy: -0.9), xRadius: 2.2, yRadius: 2.2)
            NSGraphicsContext.current?.saveGraphicsState()
            NSGraphicsContext.current?.compositingOperation = .clear
            clearPath.fill()
            NSGraphicsContext.current?.restoreGraphicsState()

            let tabPath = NSBezierPath(roundedRect: tabRect, xRadius: 1.8, yRadius: 1.8)
            tabPath.lineWidth = 1.45
            if streaming {
                tabPath.fill()
                // Clean inner cutout line to suggest an active lit screen
                NSGraphicsContext.current?.saveGraphicsState()
                NSGraphicsContext.current?.compositingOperation = .clear
                let inner = NSBezierPath(roundedRect: tabRect.insetBy(dx: 2.4, dy: 2.1), xRadius: 0.8, yRadius: 0.8)
                inner.fill()
                NSGraphicsContext.current?.restoreGraphicsState()
            } else {
                tabPath.stroke()
            }

            // Tiny corner dot when USB is linked (or streaming)
            if connected || streaming {
                let dotRect = NSRect(x: 17.5, y: 12.5, width: 3.4, height: 3.4)
                NSBezierPath(ovalIn: dotRect).fill()
            }
            return true
        }
        image.isTemplate = true
        return image
    }
}

struct MenuBarPopoverView: View {
    @ObservedObject var model: HostWindowModel
    let setVideo: (Bool) -> Void
    let setInput: (Bool) -> Void
    let setPosition: (DisplayPosition) -> Void
    let openPermission: (Bool) -> Void
    let showWindow: () -> Void
    let copyStatus: () -> Void
    let quit: () -> Void

    private var state: HostWindowState { model.state }

    private var statusColor: Color {
        if state.badge == "会话已建立" { return .green }
        if state.badge == "出错" { return .red }
        if state.linked { return .blue }
        return .secondary
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            // Header
            HStack(spacing: 10) {
                ZStack {
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .fill(statusColor.opacity(0.14))
                        .frame(width: 34, height: 34)
                    Image(systemName: state.linked ? "display.2" : "cable.connector")
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(statusColor)
                }
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        Text("DeskMate")
                            .font(.system(size: 14, weight: .semibold))
                        Text(state.badge)
                            .font(.system(size: 10, weight: .semibold))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(statusColor.opacity(0.16), in: Capsule())
                            .foregroundStyle(statusColor)
                    }
                    Text(state.streamSummary.isEmpty ? state.title : state.streamSummary)
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Spacer()
            }

            // Live telemetry cards
            if state.linked {
                HStack(spacing: 8) {
                    metricBox(title: "编码帧率", value: "\(state.liveFps) fps")
                    metricBox(title: "实时码率", value: String(format: "%.1f Mbps", state.liveBitrateMbps))
                    metricBox(title: "触控事件", value: "\(state.inputEventCount)")
                }
            }

            Divider()

            // Quick toggles
            VStack(spacing: 8) {
                HStack {
                    Label("发送画面", systemImage: "play.display")
                        .font(.system(size: 12.5))
                    Spacer()
                    Toggle("", isOn: Binding(get: { state.videoEnabled }, set: setVideo))
                        .labelsHidden()
                        .toggleStyle(.switch)
                        .controlSize(.small)
                }

                HStack {
                    Label("平板反向触控", systemImage: "hand.tap")
                        .font(.system(size: 12.5))
                    Spacer()
                    if !state.inputPermission {
                        Button("去授权") { openPermission(false) }
                            .buttonStyle(.link)
                            .font(.system(size: 12))
                    } else {
                        Toggle("", isOn: Binding(get: { state.inputEnabled }, set: setInput))
                            .labelsHidden()
                            .toggleStyle(.switch)
                            .controlSize(.small)
                    }
                }
            }

            // Virtual display position selector
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Label("虚拟屏位置", systemImage: "rectangle.inset.filled.and.person.filled")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(state.position.label)
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }
                HStack(spacing: 6) {
                    ForEach(DisplayPosition.allCases) { pos in
                        Button {
                            setPosition(pos)
                        } label: {
                            Text(pos.shortLabel)
                                .font(.system(size: 11.5, weight: state.position == pos ? .semibold : .regular))
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 5)
                                .background(
                                    state.position == pos
                                        ? Color.accentColor.opacity(0.18)
                                        : Color.primary.opacity(0.05),
                                    in: RoundedRectangle(cornerRadius: 6, style: .continuous)
                                )
                                .foregroundStyle(state.position == pos ? Color.accentColor : Color.primary)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }

            Divider()

            // Actions
            VStack(spacing: 2) {
                actionButton("打开主窗口", symbol: "macwindow", shortcut: "⌘O", action: showWindow)
                actionButton("复制诊断信息", symbol: "doc.on.doc", shortcut: "⌘C", action: copyStatus)
                actionButton("退出 DeskMate", symbol: "power", shortcut: "⌘Q", destructive: true, action: quit)
            }
        }
        .padding(14)
        .frame(width: 296)
    }

    private func metricBox(title: String, value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(.system(size: 10))
                .foregroundStyle(.secondary)
            Text(value)
                .font(.system(size: 12, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(Color.primary.opacity(0.05), in: RoundedRectangle(cornerRadius: 7, style: .continuous))
    }

    private func actionButton(
        _ title: String,
        symbol: String,
        shortcut: String,
        destructive: Bool = false,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Image(systemName: symbol)
                    .font(.system(size: 12))
                    .frame(width: 16)
                Text(title)
                    .font(.system(size: 12.5))
                Spacer()
                Text(shortcut)
                    .font(.system(size: 11))
                    .foregroundStyle(.tertiary)
            }
            .foregroundStyle(destructive ? Color.red : Color.primary)
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}
