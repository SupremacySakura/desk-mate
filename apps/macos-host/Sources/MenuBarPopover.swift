import AppKit
import SwiftUI

enum MenuBarIcon {
    static func makeImage(connected: Bool, streaming: Bool) -> NSImage {
        let size = NSSize(width: 22, height: 18)
        let image = NSImage(size: size, flipped: false) { _ in
            NSColor.labelColor.setStroke()
            NSColor.labelColor.setFill()

            let primaryRect = NSRect(x: 2.0, y: 5.5, width: 12.0, height: 9.0)
            let primaryPath = NSBezierPath(roundedRect: primaryRect, xRadius: 2.0, yRadius: 2.0)
            primaryPath.lineWidth = 1.4
            primaryPath.stroke()

            let stand = NSBezierPath()
            stand.lineWidth = 1.3
            stand.move(to: NSPoint(x: 6.5, y: 5.5))
            stand.line(to: NSPoint(x: 6.5, y: 3.3))
            stand.move(to: NSPoint(x: 4.0, y: 3.3))
            stand.line(to: NSPoint(x: 9.0, y: 3.3))
            stand.stroke()

            let tabletRect = NSRect(x: 9.5, y: 2.5, width: 10.5, height: 8.2)
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current?.compositingOperation = .clear
            NSBezierPath(roundedRect: tabletRect, xRadius: 1.8, yRadius: 1.8).fill()
            NSGraphicsContext.restoreGraphicsState()

            let tabletPath = NSBezierPath(roundedRect: tabletRect, xRadius: 1.8, yRadius: 1.8)
            tabletPath.lineWidth = streaming ? 1.6 : 1.35
            tabletPath.stroke()

            if streaming {
                let innerRect = tabletRect.insetBy(dx: 2.2, dy: 2.1)
                NSBezierPath(roundedRect: innerRect, xRadius: 0.9, yRadius: 0.9).fill()
            } else if connected {
                let dotRect = NSRect(x: tabletRect.midX - 1.5, y: tabletRect.midY - 1.5, width: 3.0, height: 3.0)
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
    let setTouchInputMode: (TouchInputMode) -> Void
    let setPosition: (DisplayPosition) -> Void
    let selectLink: (UInt64) -> Void
    let disconnectLink: (UInt64) -> Void
    let setServiceRunning: (Bool) -> Void
    let openPermission: (Bool) -> Void
    let requestAccessibility: (Bool) -> Void
    let showWindow: () -> Void
    let copyStatus: () -> Void
    let quit: () -> Void

    private var state: HostWindowState { model.state }

    private var streaming: Bool {
        state.badge == "会话已建立" || state.badge.contains("运行中") || state.liveFps > 0
    }

    private var badgeTone: StudioTone {
        if state.badge == "会话已建立" || state.badge.contains("运行中") || state.badge == "测试模式" {
            return .accent
        }
        switch state.badge {
        case "出错":
            return .danger
        case "待授权", "未就绪", "已拦截":
            return .warning
        case "USB 已连接":
            return .positive
        default:
            if state.badge.contains("已连接") {
                return .positive
            }
            return .neutral
        }
    }

    var body: some View {
        ZStack {
            StudioBackground()

            VStack(alignment: .leading, spacing: 12) {
                // Header
                HStack(spacing: 10) {
                    DualDisplayGlyph(linked: state.linked, streaming: streaming, size: 30)

                    VStack(alignment: .leading, spacing: 1) {
                        Text("DeskMate")
                            .font(.system(size: 13.5, weight: .semibold))
                            .foregroundStyle(StudioPalette.ink)

                        Text(subtitleText)
                            .font(.system(size: 11))
                            .foregroundStyle(StudioPalette.inkMuted)
                            .lineLimit(1)
                    }

                    Spacer(minLength: 6)

                    StudioStatusBadge(text: state.badge, tone: badgeTone)
                }

                // 3-column live telemetry bar
                HStack(spacing: 0) {
                    compactMetric(
                        label: "FPS",
                        value: state.liveFps > 0 ? "\(state.liveFps)" : "—",
                        highlight: streaming && state.liveFps > 0
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(width: 1.0, height: 26)

                    compactMetric(
                        label: "Mbps",
                        value: state.liveBitrateMbps > 0 ? String(format: "%.1f", state.liveBitrateMbps) : "—",
                        highlight: false
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(width: 1.0, height: 26)

                    compactMetric(
                        label: "触控",
                        value: state.inputEnabled ? "\(state.inputEventCount)" : "关",
                        highlight: state.inputEnabled
                    )
                }
                .padding(.vertical, 9)
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 1.0)
                )

                if !state.serviceRunning {
                    HStack(spacing: 8) {
                        Text("串流服务已停止")
                            .font(.system(size: 11.5, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                        Spacer()
                        Button("启动服务") {
                            setServiceRunning(true)
                        }
                        .buttonStyle(StudioPrimaryButtonStyle())
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .fill(StudioPalette.subtleSurface)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .stroke(StudioPalette.hairline, lineWidth: 1.0)
                    )
                } else if !state.screenPermission || (state.inputEnabled && !state.inputPermission) {
                    HStack(spacing: 8) {
                        Text(!state.screenPermission ? "需要屏幕录制权限" : "需要辅助功能权限")
                            .font(.system(size: 11.5, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                        Spacer()
                        if !state.screenPermission {
                            Button("前往授权") {
                                openPermission(true)
                            }
                            .buttonStyle(StudioPrimaryButtonStyle())
                        } else {
                            Button("一键授权") {
                                requestAccessibility(false)
                            }
                            .buttonStyle(StudioPrimaryButtonStyle())
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .fill(StudioPalette.warningWash)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .stroke(StudioPalette.warningBorder, lineWidth: 1.0)
                    )
                }

                // Controls card
                VStack(spacing: 0) {
                    HStack {
                        Text("画面推流")
                            .font(.system(size: 12.5, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                        Spacer()
                        StudioHairlineToggle(isOn: state.videoEnabled) {
                            setVideo(!state.videoEnabled)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 9)

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("触控回传")
                                .font(.system(size: 12.5, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)

                            Menu {
                                ForEach(TouchInputMode.allCases) { mode in
                                    Button {
                                        setTouchInputMode(mode)
                                    } label: {
                                        if state.touchInputMode == mode {
                                            Label(mode.displayName, systemImage: "checkmark")
                                        } else {
                                            Text(mode.displayName)
                                        }
                                    }
                                }
                            } label: {
                                Text(state.touchInputMode.displayName)
                                    .font(.system(size: 11))
                                    .foregroundStyle(StudioPalette.inkSecondary)
                                    .lineLimit(1)
                            }
                            .menuStyle(.borderlessButton)
                            .fixedSize()
                        }
                        Spacer()
                        StudioHairlineToggle(isOn: state.inputEnabled) {
                            setInput(!state.inputEnabled)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 9)

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    VStack(alignment: .leading, spacing: 7) {
                        Text(state.activeSessions.count > 1 ? "当前选中副屏方位" : "副屏方位")
                            .font(.system(size: 11))
                            .foregroundStyle(StudioPalette.inkMuted)

                        HStack(spacing: 5) {
                            ForEach(DisplayPosition.allCases, id: \.self) { pos in
                                StudioSegmentOption(
                                    title: pos.shortLabel,
                                    active: state.position == pos,
                                    expand: true,
                                    action: { setPosition(pos) }
                                )
                            }
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 9)

                    if state.activeSessions.count > 1 {
                        Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                        VStack(alignment: .leading, spacing: 5) {
                            Text("已连接设备 (\(state.activeSessions.count))")
                                .font(.system(size: 11))
                                .foregroundStyle(StudioPalette.inkMuted)

                            ForEach(state.activeSessions) { session in
                                let selected = (state.selectedLinkID == session.linkID)
                                HStack(spacing: 6) {
                                    Button {
                                        selectLink(session.linkID)
                                    } label: {
                                        HStack(spacing: 6) {
                                            Circle()
                                                .fill(session.streaming ? StudioPalette.accent : StudioPalette.positive)
                                                .frame(width: 5, height: 5)
                                            Text(session.deviceName)
                                                .font(.system(size: 11.5, weight: selected ? .semibold : .regular))
                                                .foregroundStyle(selected ? StudioPalette.accent : StudioPalette.ink)
                                                .lineLimit(1)
                                            Spacer(minLength: 4)
                                            Text("\(session.position.shortLabel) · \(session.liveFps > 0 ? "\(session.liveFps)fps" : session.badge)")
                                                .font(.system(size: 10.5, design: .monospaced))
                                                .monospacedDigit()
                                                .foregroundStyle(StudioPalette.inkMuted)
                                        }
                                        .padding(.horizontal, 8)
                                        .padding(.vertical, 5)
                                        .background(
                                            RoundedRectangle(cornerRadius: 8, style: .continuous)
                                                .fill(selected ? StudioPalette.accentSoft : StudioPalette.subtleSurface)
                                        )
                                        .overlay(
                                            RoundedRectangle(cornerRadius: 8, style: .continuous)
                                                .stroke(selected ? StudioPalette.accentBorder : StudioPalette.softHairline, lineWidth: 1.0)
                                        )
                                    }
                                    .buttonStyle(.plain)

                                    Button {
                                        disconnectLink(session.linkID)
                                    } label: {
                                        Image(systemName: "xmark")
                                            .font(.system(size: 10, weight: .semibold))
                                            .foregroundStyle(StudioPalette.inkMuted)
                                            .frame(width: 22, height: 22)
                                    }
                                    .buttonStyle(.plain)
                                    .help("断开 \(session.deviceName)")
                                }
                            }
                        }
                        .padding(.horizontal, 12)
                        .padding(.vertical, 9)
                    }
                }
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 14, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 1.0)
                )

                Rectangle()
                    .fill(StudioPalette.hairline)
                    .frame(height: 1.0)

                // Footer actions
                HStack(spacing: 6) {
                    Button {
                        showWindow()
                    } label: {
                        HStack(spacing: 5) {
                            Text("打开主窗口")
                                .font(.system(size: 11.5, weight: .medium))
                                .foregroundStyle(StudioPalette.accent)
                            Text("⌘O")
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundStyle(StudioPalette.inkSecondary)
                                .padding(.horizontal, 5)
                                .padding(.vertical, 1.5)
                                .background(
                                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                                        .fill(StudioPalette.subtleSurface)
                                )
                                .overlay(
                                    RoundedRectangle(cornerRadius: 5, style: .continuous)
                                        .stroke(StudioPalette.hairline, lineWidth: 0.8)
                                )
                        }
                    }
                    .buttonStyle(StudioFlatActionStyle())

                    Spacer(minLength: 4)

                    if state.serviceRunning {
                        Button {
                            setServiceRunning(false)
                        } label: {
                            Text("停止服务")
                                .font(.system(size: 11))
                                .foregroundStyle(StudioPalette.inkSecondary)
                        }
                        .buttonStyle(StudioFlatActionStyle())
                    }

                    Button {
                        copyStatus()
                    } label: {
                        Text("复制状态")
                            .font(.system(size: 11))
                            .foregroundStyle(StudioPalette.inkSecondary)
                    }
                    .buttonStyle(StudioFlatActionStyle())

                    Button {
                        quit()
                    } label: {
                        Text("退出")
                            .font(.system(size: 11))
                            .foregroundStyle(StudioPalette.inkMuted)
                    }
                    .buttonStyle(StudioFlatActionStyle())
                }
            }
            .padding(14)
        }
        .frame(width: 332)
    }

    private var subtitleText: String {
        if state.activeSessions.count > 1 {
            return "已连接 \(state.activeSessions.count) 台副屏设备"
        }
        if let name = state.connectedDeviceName, !name.isEmpty, state.linked {
            return "\(name) · 主屏\(state.position.shortLabel)"
        }
        return state.autoVirtualDisplay ? "虚拟屏 · 主屏\(state.position.shortLabel)" : state.source
    }

    private func compactMetric(label: String, value: String, highlight: Bool) -> some View {
        VStack(spacing: 2) {
            Text(value)
                .font(.system(size: 14, weight: .semibold, design: .monospaced))
                .monospacedDigit()
                .foregroundStyle(highlight ? StudioPalette.accent : StudioPalette.ink)
            Text(label)
                .font(.system(size: 10))
                .foregroundStyle(StudioPalette.inkMuted)
        }
        .frame(maxWidth: .infinity)
    }
}
