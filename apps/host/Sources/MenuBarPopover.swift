import AppKit
import SwiftUI

enum MenuBarIcon {
    static func makeImage(connected: Bool, streaming: Bool) -> NSImage {
        let size = NSSize(width: 22, height: 18)
        let image = NSImage(size: size, flipped: false) { _ in
            NSColor.labelColor.setStroke()
            NSColor.labelColor.setFill()

            // Primary desktop display (upper-left)
            let primaryRect = NSRect(x: 2.0, y: 5.5, width: 12.0, height: 9.0)
            let primaryPath = NSBezierPath(roundedRect: primaryRect, xRadius: 1.5, yRadius: 1.5)
            primaryPath.lineWidth = 1.4
            primaryPath.stroke()

            // Primary stand tick
            let stand = NSBezierPath()
            stand.lineWidth = 1.3
            stand.move(to: NSPoint(x: 6.5, y: 5.5))
            stand.line(to: NSPoint(x: 6.5, y: 3.3))
            stand.move(to: NSPoint(x: 4.0, y: 3.3))
            stand.line(to: NSPoint(x: 9.0, y: 3.3))
            stand.stroke()

            // Extended tablet screen (lower-right overlapping, cleared interior)
            let tabletRect = NSRect(x: 9.5, y: 2.5, width: 10.5, height: 8.2)
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current?.compositingOperation = .clear
            NSBezierPath(roundedRect: tabletRect, xRadius: 1.4, yRadius: 1.4).fill()
            NSGraphicsContext.restoreGraphicsState()

            let tabletPath = NSBezierPath(roundedRect: tabletRect, xRadius: 1.4, yRadius: 1.4)
            tabletPath.lineWidth = streaming ? 1.6 : 1.35
            tabletPath.stroke()

            if streaming {
                // Active inner screen fill cutout
                let innerRect = tabletRect.insetBy(dx: 2.2, dy: 2.1)
                NSBezierPath(roundedRect: innerRect, xRadius: 0.7, yRadius: 0.7).fill()
            } else if connected {
                // Standby connected dot
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
    let setServiceRunning: (Bool) -> Void
    let openPermission: (Bool) -> Void
    let requestAccessibility: (Bool) -> Void
    let showWindow: () -> Void
    let copyStatus: () -> Void
    let quit: () -> Void

    private var state: HostWindowState { model.state }

    private var streaming: Bool {
        state.badge == "会话已建立" || state.liveFps > 0
    }

    private var badgeTone: StudioTone {
        switch state.badge {
        case "会话已建立":
            return .crimson
        case "出错", "待授权", "未就绪", "已拦截":
            return .warning
        case "USB 已连接":
            return .positive
        case "测试模式":
            return .crimson
        default:
            return .neutral
        }
    }

    var body: some View {
        ZStack {
            StudioBackground()

            VStack(alignment: .leading, spacing: 14) {
                // Header
                HStack(spacing: 10) {
                    DualDisplayGlyph(linked: state.linked, streaming: streaming, size: 30)

                    VStack(alignment: .leading, spacing: 1) {
                        Text("DeskMate")
                            .font(.system(size: 13.5, weight: .semibold))
                            .foregroundStyle(StudioPalette.ink)

                        Text(subtitleText)
                            .font(.system(size: 10.5))
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

                    Rectangle().fill(StudioPalette.softHairline).frame(width: 0.75, height: 28)

                    compactMetric(
                        label: "Mbps",
                        value: state.liveBitrateMbps > 0 ? String(format: "%.1f", state.liveBitrateMbps) : "—",
                        highlight: false
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(width: 0.75, height: 28)

                    compactMetric(
                        label: "触控",
                        value: state.inputEnabled ? "\(state.inputEventCount)" : "关",
                        highlight: state.inputEnabled
                    )
                }
                .padding(.vertical, 8)
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 0.75)
                )

                // Compact service or permission callout if required
                if !state.serviceRunning {
                    HStack(spacing: 8) {
                        Text("串流服务已停止")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                        Spacer()
                        Button("启动服务") {
                            setServiceRunning(true)
                        }
                        .buttonStyle(StudioPrimaryButtonStyle())
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 8)
                    .background(
                        RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                            .fill(StudioPalette.subtleSurface)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                            .stroke(StudioPalette.hairline, lineWidth: 0.75)
                    )
                } else if !state.screenPermission || (state.inputEnabled && !state.inputPermission) {
                    HStack(spacing: 8) {
                        Text(!state.screenPermission ? "需要屏幕录制权限" : "需要辅助功能权限")
                            .font(.system(size: 11, weight: .medium))
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
                    .padding(.horizontal, 10)
                    .padding(.vertical, 8)
                    .background(
                        RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                            .fill(StudioPalette.amberWash)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                            .stroke(StudioTone.warning.border, lineWidth: 0.75)
                    )
                }

                // Controls card
                VStack(spacing: 0) {
                    HStack {
                        Text("画面推流")
                            .font(.system(size: 12, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                        Spacer()
                        StudioHairlineToggle(isOn: state.videoEnabled) {
                            setVideo(!state.videoEnabled)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 9)

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("触控回传")
                                .font(.system(size: 12, weight: .medium))
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
                                    .font(.system(size: 10.5))
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

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    VStack(alignment: .leading, spacing: 7) {
                        Text("副屏方位")
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
                }
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 0.75)
                )

                Rectangle()
                    .fill(StudioPalette.hairline)
                    .frame(height: 0.75)

                // Footer actions
                HStack(spacing: 6) {
                    Button {
                        showWindow()
                    } label: {
                        HStack(spacing: 4) {
                            Text("打开主窗口")
                                .font(.system(size: 11.5, weight: .medium))
                                .foregroundStyle(StudioPalette.crimson)
                            Text("⌘O")
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundStyle(StudioPalette.inkMuted)
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
            .padding(16)
        }
        .frame(width: 332)
    }

    private var subtitleText: String {
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
                .foregroundStyle(highlight ? StudioPalette.crimson : StudioPalette.ink)
            Text(label)
                .font(.system(size: 9.5, design: .monospaced))
                .foregroundStyle(StudioPalette.inkMuted)
        }
        .frame(maxWidth: .infinity)
    }
}
