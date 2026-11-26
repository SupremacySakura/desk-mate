import AppKit
import SwiftUI

struct DisplayChoice: Identifiable, Equatable {
    let id: UInt32
    let title: String
}

enum FrameRateOption: Int, CaseIterable, Identifiable {
    case auto = 0
    case fps30 = 30
    case fps60 = 60
    case fps90 = 90
    case fps120 = 120

    var id: Int { rawValue }

    var shortLabel: String {
        switch self {
        case .auto: return "自动"
        case .fps30: return "30 Hz"
        case .fps60: return "60 Hz"
        case .fps90: return "90 Hz"
        case .fps120: return "120 Hz"
        }
    }
}

struct ActiveDisplaySessionState: Identifiable, Equatable {
    let linkID: UInt64
    let deviceID: String
    let deviceName: String
    let state: String
    let badge: String
    let linked: Bool
    let streaming: Bool
    let streamSummary: String
    let position: DisplayPosition
    let liveFps: Int
    let liveBitrateMbps: Double
    let inputEventCount: Int
    let displayID: UInt32?
    let error: String?

    var id: UInt64 { linkID }
}

struct HostWindowState: Equatable {
    var title = "等待设备连接"
    var guidance = "打开设备上的 DeskMate，再接上 USB 数据线。"
    var badge = "未连接"
    var linked = false
    var serviceRunning = true
    var autoStartService = true
    var launchAtLogin = false
    var loginItemNeedsApproval = false
    var loginItemSummary = ""
    var loginItemError: String? = nil
    var pairedDevices: [PairedDevice] = []
    var activeSessions: [ActiveDisplaySessionState] = []
    var topologyDisplays: [TopologyDisplayItem] = []
    var selectedLinkID: UInt64? = nil
    var connectedDeviceIDs: Set<String> = []
    var connectedDeviceID: String? = nil
    var onlineDeviceIDs: Set<String> = []
    var connectedDeviceName: String? = nil
    var blockedDeviceNames: [String] = []
    var blockedDeviceName: String? = nil
    var source = "自动创建虚拟显示器"
    var streamSummary = ""
    var video = "等待连接"
    var input = "未开启"
    var screenPermission = false
    var inputPermission = false
    var videoEnabled = true
    var inputEnabled = false
    var touchInputMode: TouchInputMode = .compatibleGestures
    var touchCapability = TouchCapabilityStatus(
        virtualHIDAvailable: false,
        fallbackActive: false,
        activeBackendLabel: "兼容手势映射",
        statusDetail: ""
    )
    var autoVirtualDisplay = true
    var position: DisplayPosition = .right
    var preferredFrameRateHz = 0
    var negotiatedRefreshHz = 0
    var maxSupportedRefreshHz = 0
    var liveFps = 0
    var liveBitrateMbps = 0.0
    var inputEventCount = 0
    var choices: [DisplayChoice] = []
    var selected: UInt32?
    var fixedSource = false
    var needsRestart = false
    var canRetry = false
    var update = UpdateStateSnapshot()
    var diagnostic = ""
}

final class HostWindowModel: ObservableObject {
    @Published var state = HostWindowState()
    @Published var page: HostWindow.Page = .display
    @Published var showDiagnostics = false
    @Published var copiedDiagnostics = false
    @Published var renamingDeviceID: String? = nil
    @Published var renameDraft: String = ""
    @Published var mirrorDraft: String = ""
    let topologyInteraction = TopologyInteractionModel()
}

struct HostWindow: View {
    enum Page: String, CaseIterable {
        case display = "概览"
        case devices = "设备"
        case settings = "设置"
        case diagnostics = "诊断"
    }

    @ObservedObject var model: HostWindowModel
    let chooseSource: (UInt32) -> Void
    let setAutoVirtual: (Bool) -> Void
    let setPosition: (DisplayPosition) -> Void
    let setDefaultPosition: (DisplayPosition) -> Void
    let setPreferredFrameRate: (Int) -> Void
    let selectLink: (UInt64) -> Void
    let setLinkPosition: (UInt64, DisplayPosition) -> Void
    let disconnectLink: (UInt64) -> Void
    let applyDisplayLayout: ([(displayID: UInt32, origin: CGPoint)]) -> Void
    let refreshDisplays: () -> Void
    let setVideo: (Bool) -> Void
    let setInput: (Bool) -> Void
    let setTouchInputMode: (TouchInputMode) -> Void
    let openTrackpadSettings: () -> Void
    let openPermission: (Bool) -> Void
    let requestAccessibility: (Bool) -> Void
    let setLaunchAtLogin: (Bool) -> Void
    let openLoginItemsSettings: () -> Void
    let setAutoStartService: (Bool) -> Void
    let setServiceRunning: (Bool) -> Void
    let refreshDevices: () -> Void
    let setDeviceAllowed: (String, Bool) -> Void
    let setDevicePreferredPosition: (String, DisplayPosition?) -> Void
    let renameDevice: (String, String) -> Void
    let removeDevice: (String) -> Void
    let copyDeviceID: (String) -> Void
    let checkForUpdates: () -> Void
    let installUpdate: () -> Void
    let setAutoCheckUpdates: (Bool) -> Void
    let setUpdateMirrorPrefix: (String) -> Void
    let openReleasesPage: () -> Void
    let retry: () -> Void
    let copy: () -> Void

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

            VStack(spacing: 0) {
                topBar

                Rectangle()
                    .fill(StudioPalette.hairline)
                    .frame(height: 1.0)

                ScrollView(.vertical, showsIndicators: false) {
                    VStack(alignment: .leading, spacing: 16) {
                        switch model.page {
                        case .display:
                            displayOverviewContent
                        case .devices:
                            devicesContent
                        case .settings:
                            settingsContent
                        case .diagnostics:
                            diagnosticsContent
                        }
                    }
                    .frame(maxWidth: model.page == .display ? 940 : 800, alignment: .leading)
                    .padding(.horizontal, 24)
                    .padding(.vertical, 18)
                    .frame(maxWidth: .infinity)
                }

                Rectangle()
                    .fill(StudioPalette.hairline)
                    .frame(height: 1.0)

                whisperBar
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(minWidth: 820, minHeight: 540)
    }

    // MARK: - Top Bar (Quiet Squircle Centered Pill Nav)

    private var topBar: some View {
        ZStack {
            HStack(spacing: 12) {
                HStack(spacing: 10) {
                    DualDisplayGlyph(linked: state.linked, streaming: streaming, size: 32)

                    Text("DeskMate")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)

                    StudioStatusBadge(text: state.badge, tone: badgeTone)
                }

                Spacer(minLength: 16)

                Text(usbChannelText)
                    .font(.system(size: 11.5))
                    .foregroundStyle(StudioPalette.inkMuted)
                    .lineLimit(1)
            }

            // Centered pill navigation
            HStack(spacing: 2) {
                ForEach(Page.allCases, id: \.self) { tab in
                    topTabButton(tab)
                }
            }
            .padding(3)
            .background(Capsule().fill(StudioPalette.subtleSurface))
            .overlay(Capsule().stroke(StudioPalette.softHairline, lineWidth: 1.0))
        }
        .padding(.leading, 78)
        .padding(.trailing, 20)
        .padding(.vertical, 10)
        .background(StudioPalette.cardSurface)
    }

    private func topTabButton(_ tab: Page) -> some View {
        let active = (model.page == tab)
        return Button {
            model.page = tab
        } label: {
            Text(tab.rawValue)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(active ? StudioPalette.ink : StudioPalette.inkSecondary)
                .padding(.horizontal, 14)
                .padding(.vertical, 5.5)
                .background(
                    Capsule()
                        .fill(active ? StudioPalette.cardSurface : Color.clear)
                        .shadow(color: active ? Color.black.opacity(0.06) : Color.clear, radius: 2, x: 0, y: 1)
                )
                .contentShape(Capsule())
        }
        .buttonStyle(.plain)
    }

    private var usbChannelText: String {
        if !state.serviceRunning {
            return "服务已停止"
        }
        if let blocked = state.blockedDeviceName, !blocked.isEmpty, state.activeSessions.isEmpty {
            return "\(blocked) · 已拦截"
        }
        if state.activeSessions.count > 1 {
            return "已接入 \(state.activeSessions.count) 台设备"
        }
        if state.linked {
            if let name = state.connectedDeviceName, !name.isEmpty {
                return "\(name) · USB-C 直连"
            }
            return "USB-C 直连就绪"
        }
        return "等待 USB 接入"
    }

    // MARK: - Overview Page (Canvas-First + Twin Flat Control Dock)

    private var displayOverviewContent: some View {
        VStack(alignment: .leading, spacing: 14) {
            actionCalloutIfNeeded
            updateCalloutIfNeeded

            SpatialDisplayArrangerView(
                interaction: model.topologyInteraction,
                linked: state.linked,
                streaming: streaming,
                position: state.position,
                displays: state.topologyDisplays,
                sessions: state.activeSessions,
                selectedLinkID: state.selectedLinkID,
                autoVirtualDisplay: state.autoVirtualDisplay,
                sourceLabel: state.source,
                streamSummary: state.streamSummary,
                liveFps: state.liveFps,
                liveBitrateMbps: state.liveBitrateMbps,
                onSelectPosition: setPosition,
                onSelectSession: selectLink,
                onApplyLayout: applyDisplayLayout,
                onRefreshDisplays: refreshDisplays
            )

            if !state.activeSessions.isEmpty {
                activeSessionsSection
            }

            // 3-column live telemetry bar
            HStack(spacing: 0) {
                metricCell(
                    title: "实时帧率",
                    value: state.liveFps > 0 ? "\(state.liveFps)" : "—",
                    unit: state.liveFps > 0 ? "FPS" : "待机",
                    accent: streaming && state.liveFps > 0
                )

                Rectangle()
                    .fill(StudioPalette.softHairline)
                    .frame(width: 1.0, height: 34)

                metricCell(
                    title: "传输码率",
                    value: state.liveBitrateMbps > 0 ? String(format: "%.1f", state.liveBitrateMbps) : "—",
                    unit: state.liveBitrateMbps > 0 ? "Mbps" : "待机",
                    accent: false
                )

                Rectangle()
                    .fill(StudioPalette.softHairline)
                    .frame(width: 1.0, height: 34)

                metricCell(
                    title: "反向触控",
                    value: state.inputEnabled ? "\(state.inputEventCount)" : "关闭",
                    unit: state.inputEnabled ? "次事件" : "已禁用",
                    accent: false
                )
            }
            .padding(.vertical, 12)
            .background(StudioPalette.cardSurface)
            .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 1.0)
            )

            // Twin Flat Control Dock
            HStack(alignment: .top, spacing: 14) {
                // Left column: Video + Touch toggles + pipeline summary
                VStack(spacing: 0) {
                    controlToggleRow(
                        title: "画面推流",
                        subtitle: "编码发送扩展桌面至设备",
                        isOn: state.videoEnabled,
                        action: { setVideo(!state.videoEnabled) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    controlToggleRow(
                        title: "平板触控回传",
                        subtitle: state.touchInputMode.displayName,
                        isOn: state.inputEnabled,
                        action: { setInput(!state.inputEnabled) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    HStack(spacing: 10) {
                        Text("画面管线")
                            .font(.system(size: 11.5))
                            .foregroundStyle(StudioPalette.inkMuted)
                        Spacer()
                        Text(state.video)
                            .font(.system(size: 11.5, weight: .medium))
                            .foregroundStyle(streaming ? StudioPalette.ink : StudioPalette.inkSecondary)
                            .lineLimit(1)
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                }
                .frame(maxWidth: .infinity)
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 1.0)
                )

                // Right column: Position + Frame rate segments
                VStack(alignment: .leading, spacing: 14) {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Text(state.activeSessions.count > 1 ? "当前选中副屏方位" : "扩展方位")
                                .font(.system(size: 12.5, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                            Spacer()
                            Text("主屏\(state.position.shortLabel)")
                                .font(.system(size: 11))
                                .foregroundStyle(StudioPalette.inkMuted)
                        }

                        HStack(spacing: 6) {
                            ForEach(DisplayPosition.allCases, id: \.self) { pos in
                                StudioSegmentOption(
                                    title: pos.shortLabel,
                                    active: state.position == pos,
                                    expand: true,
                                    action: { setDefaultPosition(pos) }
                                )
                            }
                        }
                    }

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Text("输出帧率上限")
                                .font(.system(size: 12.5, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                            Spacer()
                            Text(frameRateSubtitleText)
                                .font(.system(size: 11))
                                .foregroundStyle(state.preferredFrameRateHz > 0 ? StudioPalette.accent : StudioPalette.inkMuted)
                                .lineLimit(1)
                        }

                        HStack(spacing: 6) {
                            ForEach(FrameRateOption.allCases) { opt in
                                StudioSegmentOption(
                                    title: opt.shortLabel,
                                    active: state.preferredFrameRateHz == opt.rawValue,
                                    expand: true,
                                    action: { setPreferredFrameRate(opt.rawValue) }
                                )
                            }
                        }
                    }
                }
                .padding(14)
                .frame(maxWidth: .infinity)
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 1.0)
                )
            }
        }
    }

    private var activeSessionsSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            StudioSectionHeader(title: "在线副屏（\(state.activeSessions.count)）", detail: "点选或拖动拓扑块切换目标")

            VStack(spacing: 8) {
                ForEach(state.activeSessions) { session in
                    activeSessionCard(session)
                }
            }
        }
    }

    private func activeSessionCard(_ session: ActiveDisplaySessionState) -> some View {
        let isSelected = (state.selectedLinkID == session.linkID)
        let sessionTone: StudioTone = session.streaming ? .accent : (session.error != nil ? .warning : .positive)

        return HStack(spacing: 12) {
            StudioDeviceGlyph(active: session.streaming || isSelected, allowed: true, size: 32)

            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 7) {
                    Text(session.deviceName)
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(StudioPalette.ink)
                        .lineLimit(1)

                    StudioStatusBadge(text: session.badge, tone: sessionTone)

                    if isSelected && state.activeSessions.count > 1 {
                        Text("已选中")
                            .font(.system(size: 10, weight: .medium))
                            .foregroundStyle(StudioPalette.accent)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Capsule().fill(StudioPalette.accentSoft))
                    }
                }

                HStack(spacing: 8) {
                    if !session.streamSummary.isEmpty {
                        Text(session.streamSummary)
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(StudioPalette.inkSecondary)
                    }
                    if session.streaming && session.liveFps > 0 {
                        Text(session.liveBitrateMbps > 0
                             ? String(format: "%d FPS · %.1f Mbps", session.liveFps, session.liveBitrateMbps)
                             : "\(session.liveFps) FPS")
                            .font(.system(size: 11, weight: .medium, design: .monospaced))
                            .monospacedDigit()
                            .foregroundStyle(StudioPalette.accent)
                    }
                    if let displayID = session.displayID {
                        Text("#\(displayID)")
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(StudioPalette.inkMuted)
                    }
                }
            }

            Spacer(minLength: 8)

            HStack(spacing: 4) {
                ForEach(DisplayPosition.allCases, id: \.self) { pos in
                    let active = (session.position == pos)
                    Button {
                        setLinkPosition(session.linkID, pos)
                    } label: {
                        Text(pos.shortLabel)
                            .font(.system(size: 10.5, weight: .medium))
                            .foregroundStyle(active ? StudioPalette.accent : StudioPalette.inkSecondary)
                            .padding(.horizontal, 7)
                            .padding(.vertical, 4)
                            .background(
                                RoundedRectangle(cornerRadius: 7, style: .continuous)
                                    .fill(active ? StudioPalette.accentSoft : StudioPalette.subtleSurface)
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: 7, style: .continuous)
                                    .stroke(active ? StudioPalette.accentBorder : StudioPalette.softHairline, lineWidth: 1.0)
                            )
                    }
                    .buttonStyle(.plain)
                }
            }

            Button("断开") {
                disconnectLink(session.linkID)
            }
            .buttonStyle(StudioSecondaryButtonStyle())
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(StudioPalette.cardSurface)
        .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                .stroke(isSelected ? StudioPalette.accentBorder : StudioPalette.hairline, lineWidth: isSelected ? 1.4 : 1.0)
        )
        .contentShape(Rectangle())
        .onTapGesture {
            selectLink(session.linkID)
        }
    }

    @ViewBuilder
    private var actionCalloutIfNeeded: some View {
        let stopped = !state.serviceRunning
        let hasBlocked = (state.blockedDeviceName?.isEmpty == false) && !state.linked
        let missingScreen = !state.screenPermission
        let missingInput = state.inputEnabled && !state.inputPermission

        if stopped {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("串流服务当前处于停止状态")
                        .font(.system(size: 12.5, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)
                    Text("启动服务后将监听 USB 接入并按偏好创建虚拟显示器。")
                        .font(.system(size: 11.5))
                        .foregroundStyle(StudioPalette.inkSecondary)
                }
                Spacer(minLength: 10)
                Button("启动串流服务") { setServiceRunning(true) }
                    .buttonStyle(StudioPrimaryButtonStyle())
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .fill(StudioPalette.subtleSurface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 1.0)
            )
        } else if hasBlocked, let blocked = state.blockedDeviceName {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("已阻止连接：\(blocked)")
                        .font(.system(size: 12.5, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)
                    Text("该设备已在「设备」列表中设为禁止连接。")
                        .font(.system(size: 11.5))
                        .foregroundStyle(StudioPalette.inkSecondary)
                }
                Spacer(minLength: 10)
                Button("前往设备列表") { model.page = .devices }
                    .buttonStyle(StudioSecondaryButtonStyle())
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .fill(StudioPalette.warningWash)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.warningBorder, lineWidth: 1.0)
            )
        } else if missingScreen || missingInput || state.canRetry {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(missingScreen ? "需要屏幕录制权限" : (missingInput ? "需要辅助功能权限" : "连接维护"))
                        .font(.system(size: 12.5, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)

                    Text(
                        missingScreen
                        ? (state.needsRestart ? "如已在系统设置中勾选，请重新打开 DeskMate 使其生效。" : "DeskMate 需要截取扩展屏幕并编码传输至平板。")
                        : (missingInput ? "开启触控回传需要辅助功能权限以注入鼠标与滚轮事件。" : "可手动重新握手 USB 通道并同步虚拟显示器。")
                    )
                    .font(.system(size: 11.5))
                    .foregroundStyle(StudioPalette.inkSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                }

                Spacer(minLength: 10)

                HStack(spacing: 8) {
                    if missingScreen {
                        Button("授权屏幕录制") { openPermission(true) }
                            .buttonStyle(StudioPrimaryButtonStyle())
                            .fixedSize()
                    } else if missingInput {
                        Button("一键获取") { requestAccessibility(false) }
                            .buttonStyle(StudioPrimaryButtonStyle())
                            .fixedSize()
                        Button("重置旧授权") { requestAccessibility(true) }
                            .buttonStyle(StudioSecondaryButtonStyle())
                            .fixedSize()
                    }

                    if state.canRetry {
                        if !missingScreen && !missingInput {
                            Button("重新连接") { retry() }
                                .buttonStyle(StudioPrimaryButtonStyle())
                        } else {
                            Button("重试") { retry() }
                                .buttonStyle(StudioSecondaryButtonStyle())
                        }
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .fill((missingScreen || missingInput) ? StudioPalette.warningWash : StudioPalette.cardSurface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke((missingScreen || missingInput) ? StudioPalette.warningBorder : StudioPalette.hairline, lineWidth: 1.0)
            )
        }
    }

    private func metricCell(title: String, value: String, unit: String, accent: Bool) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title)
                .font(.system(size: 11))
                .foregroundStyle(StudioPalette.inkMuted)

            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Text(value)
                    .font(.system(size: 20, weight: .semibold, design: .monospaced))
                    .monospacedDigit()
                    .foregroundStyle(accent ? StudioPalette.accent : StudioPalette.ink)
                Text(unit)
                    .font(.system(size: 11))
                    .foregroundStyle(StudioPalette.inkMuted)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 16)
    }

    private func controlToggleRow(title: String, subtitle: String, isOn: Bool, action: @escaping () -> Void) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(StudioPalette.ink)
                Text(subtitle)
                    .font(.system(size: 11.5))
                    .foregroundStyle(StudioPalette.inkMuted)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Spacer(minLength: 10)

            StudioHairlineToggle(isOn: isOn, action: action)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 11)
    }

    private var frameRateSubtitleText: String {
        if state.preferredFrameRateHz == 0 {
            if state.negotiatedRefreshHz > 0 {
                return "跟随设备协商（当前 \(state.negotiatedRefreshHz) Hz）"
            }
            return "默认跟随设备协商"
        }
        if state.negotiatedRefreshHz > 0 {
            if state.preferredFrameRateHz > state.negotiatedRefreshHz {
                return "协商上限 \(state.negotiatedRefreshHz) Hz（请求 \(state.preferredFrameRateHz) Hz）"
            }
            return "锁定 \(state.preferredFrameRateHz) Hz（协商 \(state.negotiatedRefreshHz) Hz）"
        }
        return "锁定 \(state.preferredFrameRateHz) Hz"
    }

    // MARK: - Paired Devices Page

    private var devicesContent: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .center, spacing: 12) {
                VStack(alignment: .leading, spacing: 3) {
                    Text("已配对客户端设备")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)

                    Text("管理允许通过 USB 连接此 Mac 的平板或手机；关闭授权将禁止自动接管并立即断开会话。")
                        .font(.system(size: 12))
                        .foregroundStyle(StudioPalette.inkSecondary)
                        .fixedSize(horizontal: false, vertical: true)
                }

                Spacer(minLength: 12)

                StudioStatusBadge(
                    text: "\(state.pairedDevices.count) 台设备",
                    tone: state.pairedDevices.isEmpty ? .neutral : .positive
                )

                Button {
                    refreshDevices()
                } label: {
                    HStack(spacing: 5) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 10.5, weight: .medium))
                        Text("刷新")
                    }
                }
                .buttonStyle(StudioSecondaryButtonStyle())
            }

            if state.pairedDevices.isEmpty {
                VStack(alignment: .leading, spacing: 6) {
                    Text("暂无已配对的客户端设备")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(StudioPalette.ink)
                    Text("使用 USB 数据线连接平板并打开 DeskMate 后，设备会自动记录在这里；也可点击右上角「刷新」手动扫描已接入设备。")
                        .font(.system(size: 12))
                        .foregroundStyle(StudioPalette.inkMuted)
                        .lineSpacing(2.5)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(18)
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 1.0)
                )
            } else {
                VStack(spacing: 10) {
                    ForEach(state.pairedDevices) { device in
                        pairedDeviceCard(device)
                    }
                }
            }
        }
    }

    private func pairedDeviceCard(_ device: PairedDevice) -> some View {
        let isConnected = state.connectedDeviceIDs.contains(device.id) || (state.connectedDeviceID == device.id)
        let isOnline = state.onlineDeviceIDs.contains(device.id) || isConnected
        let isRenaming = (model.renamingDeviceID == device.id)

        return VStack(spacing: 0) {
            HStack(spacing: 12) {
                StudioDeviceGlyph(active: isConnected, allowed: device.allowed, size: 36)

                VStack(alignment: .leading, spacing: 4) {
                    Text(device.displayName)
                        .font(.system(size: 13.5, weight: .medium))
                        .foregroundStyle(StudioPalette.ink)
                        .lineLimit(1)

                    HStack(spacing: 8) {
                        if isConnected {
                            StudioStatusBadge(text: "当前连接", tone: .accent)
                        } else if !device.allowed {
                            StudioStatusBadge(text: "已禁止连接", tone: .warning)
                        } else if isOnline {
                            StudioStatusBadge(text: "USB 已接入", tone: .positive)
                        } else {
                            StudioStatusBadge(text: "允许连接", tone: .positive)
                        }

                        if let prefPos = device.preferredPosition {
                            Text("默认\(prefPos.shortLabel)")
                                .font(.system(size: 10.5, weight: .medium))
                                .foregroundStyle(StudioPalette.inkSecondary)
                                .padding(.horizontal, 7)
                                .padding(.vertical, 2)
                                .background(Capsule().fill(StudioPalette.subtleSurface))
                        }

                        Text("最近连接 \(PairedDeviceStore.relativeTimeText(from: device.lastConnectedAt))")
                            .font(.system(size: 11.5))
                            .foregroundStyle(StudioPalette.inkMuted)
                            .lineLimit(1)
                            .fixedSize(horizontal: true, vertical: false)
                    }

                    if let resolution = device.lastResolution, !resolution.isEmpty {
                        Text(resolution)
                            .font(.system(size: 11, design: .monospaced))
                            .monospacedDigit()
                            .foregroundStyle(StudioPalette.inkMuted)
                            .lineLimit(1)
                    }
                }

                Spacer(minLength: 8)

                HStack(spacing: 10) {
                    StudioHairlineToggle(isOn: device.allowed) {
                        setDeviceAllowed(device.id, !device.allowed)
                    }

                    Menu {
                        Menu("默认扩展方位") {
                            Button {
                                setDevicePreferredPosition(device.id, nil)
                            } label: {
                                if device.preferredPosition == nil {
                                    Label("自动分配空闲侧", systemImage: "checkmark")
                                } else {
                                    Text("自动分配空闲侧")
                                }
                            }
                            Divider()
                            ForEach(DisplayPosition.allCases, id: \.self) { pos in
                                Button {
                                    setDevicePreferredPosition(device.id, pos)
                                } label: {
                                    if device.preferredPosition == pos {
                                        Label(pos.label, systemImage: "checkmark")
                                    } else {
                                        Text(pos.label)
                                    }
                                }
                            }
                        }
                        if isConnected, let session = state.activeSessions.first(where: { $0.deviceID == device.id }) {
                            Button("断开当前连接") {
                                disconnectLink(session.linkID)
                            }
                        }
                        Divider()
                        Button("重命名设备") {
                            model.renamingDeviceID = device.id
                            model.renameDraft = device.displayName
                        }
                        Button("复制设备标识") {
                            copyDeviceID(device.id)
                        }
                        Divider()
                        Button(role: .destructive) {
                            if model.renamingDeviceID == device.id {
                                model.renamingDeviceID = nil
                            }
                            removeDevice(device.id)
                        } label: {
                            Text("移除设备")
                        }
                    } label: {
                        Image(systemName: "ellipsis")
                            .font(.system(size: 12, weight: .medium))
                            .foregroundStyle(StudioPalette.inkSecondary)
                            .frame(width: 28, height: 28)
                            .background(
                                RoundedRectangle(cornerRadius: 8, style: .continuous)
                                    .fill(StudioPalette.subtleSurface)
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: 8, style: .continuous)
                                    .stroke(StudioPalette.hairline, lineWidth: 1.0)
                            )
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)

            if isRenaming {
                Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                HStack(spacing: 8) {
                    TextField("输入自定义设备名称（留空恢复默认）", text: $model.renameDraft)
                        .textFieldStyle(.plain)
                        .font(.system(size: 12))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6.5)
                        .background(
                            RoundedRectangle(cornerRadius: 9, style: .continuous)
                                .fill(StudioPalette.cardSurface)
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: 9, style: .continuous)
                                .stroke(StudioPalette.hairline, lineWidth: 1.0)
                        )

                    Button("保存") {
                        renameDevice(device.id, model.renameDraft)
                        model.renamingDeviceID = nil
                    }
                    .buttonStyle(StudioPrimaryButtonStyle())

                    Button("取消") {
                        model.renamingDeviceID = nil
                    }
                    .buttonStyle(StudioSecondaryButtonStyle())
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .background(StudioPalette.subtleSurface)
            }
        }
        .background(StudioPalette.cardSurface)
        .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                .stroke(isConnected ? StudioPalette.accentBorder : StudioPalette.hairline, lineWidth: isConnected ? 1.4 : 1.0)
        )
    }

    // MARK: - Settings Page

    private var settingsContent: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("显示与启动设置")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(StudioPalette.ink)

            // Startup Behavior
            VStack(alignment: .leading, spacing: 8) {
                StudioSectionHeader(title: "启动行为")

                VStack(spacing: 0) {
                    controlToggleRow(
                        title: "登录时打开 DeskMate",
                        subtitle: state.loginItemSummary.isEmpty ? "配置 macOS 登录项，登录系统时自动启动应用。" : state.loginItemSummary,
                        isOn: state.launchAtLogin,
                        action: { setLaunchAtLogin(!state.launchAtLogin) }
                    )

                    if state.loginItemNeedsApproval || (state.loginItemError?.isEmpty == false) {
                        Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                        HStack(spacing: 10) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(state.loginItemNeedsApproval ? "需要在系统登录项中批准" : "登录项注册错误")
                                    .font(.system(size: 12, weight: .semibold))
                                    .foregroundStyle(StudioPalette.ink)

                                if let err = state.loginItemError, !err.isEmpty {
                                    Text(err)
                                        .font(.system(size: 11, design: .monospaced))
                                        .foregroundStyle(StudioPalette.inkSecondary)
                                        .fixedSize(horizontal: false, vertical: true)
                                }
                            }

                            Spacer(minLength: 8)

                            Button("打开登录项设置") {
                                openLoginItemsSettings()
                            }
                            .buttonStyle(StudioSecondaryButtonStyle())
                        }
                        .padding(.horizontal, 16)
                        .padding(.vertical, 10)
                        .background(StudioPalette.warningWash)
                    }

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    controlToggleRow(
                        title: "打开应用后自动启动服务",
                        subtitle: "与上方登录项同时开启后，登录 Mac 即可自动提供 USB 扩展串流服务。",
                        isOn: state.autoStartService,
                        action: { setAutoStartService(!state.autoStartService) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 2) {
                            HStack(spacing: 8) {
                                Text("当前串流服务")
                                    .font(.system(size: 13, weight: .medium))
                                    .foregroundStyle(StudioPalette.ink)

                                StudioStatusBadge(
                                    text: state.serviceRunning ? "运行中" : "已停止",
                                    tone: state.serviceRunning ? .positive : .neutral
                                )
                            }

                            Text(state.serviceRunning ? "正在监听 USB 平板连接并管理虚拟显示器" : "服务已停止，不会自动接管 USB 设备")
                                .font(.system(size: 11.5))
                                .foregroundStyle(StudioPalette.inkMuted)
                        }

                        Spacer(minLength: 10)

                        if state.serviceRunning {
                            Button("停止服务") { setServiceRunning(false) }
                                .buttonStyle(StudioSecondaryButtonStyle())
                        } else {
                            Button("启动服务") { setServiceRunning(true) }
                                .buttonStyle(StudioPrimaryButtonStyle())
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 11)
                }
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 1.0)
                )
            }

            touchInputSection

            // Virtual Display & Source
            VStack(alignment: .leading, spacing: 8) {
                StudioSectionHeader(title: "虚拟显示器与输入源")

                VStack(spacing: 0) {
                    controlToggleRow(
                        title: "自动匹配设备生成虚拟屏",
                        subtitle: "连接时读取平板比例创建 HiDPI 扩展屏，断开后自动销毁",
                        isOn: state.autoVirtualDisplay,
                        action: { setAutoVirtual(!state.autoVirtualDisplay) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    VStack(alignment: .leading, spacing: 8) {
                        HStack(spacing: 8) {
                            Text("默认扩展位置")
                                .font(.system(size: 13, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                            Spacer(minLength: 8)
                            Text("相对 Mac 主屏的对齐方向")
                                .font(.system(size: 11))
                                .foregroundStyle(StudioPalette.inkMuted)
                        }

                        HStack(spacing: 8) {
                            ForEach(DisplayPosition.allCases, id: \.self) { pos in
                                StudioSegmentOption(
                                    title: pos.shortLabel,
                                    active: state.position == pos,
                                    expand: true,
                                    action: { setDefaultPosition(pos) }
                                )
                            }
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 11)

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    VStack(alignment: .leading, spacing: 8) {
                        HStack(spacing: 8) {
                            Text("画面帧率")
                                .font(.system(size: 13, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                            Spacer(minLength: 8)
                            Text(frameRateSubtitleText)
                                .font(.system(size: 11))
                                .foregroundStyle(state.preferredFrameRateHz > 0 ? StudioPalette.accent : StudioPalette.inkMuted)
                        }

                        HStack(spacing: 8) {
                            ForEach(FrameRateOption.allCases) { opt in
                                StudioSegmentOption(
                                    title: opt.shortLabel,
                                    active: state.preferredFrameRateHz == opt.rawValue,
                                    expand: true,
                                    action: { setPreferredFrameRate(opt.rawValue) }
                                )
                            }
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 11)

                    if !state.autoVirtualDisplay {
                        Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Text(state.fixedSource ? "固定输出显示源" : "选择现有物理 / 虚拟显示器")
                                    .font(.system(size: 12.5, weight: .medium))
                                    .foregroundStyle(StudioPalette.ink)
                                Spacer()
                                Text(state.source)
                                    .font(.system(size: 11.5, design: .monospaced))
                                    .foregroundStyle(StudioPalette.inkSecondary)
                            }

                            if !state.fixedSource && !state.choices.isEmpty {
                                VStack(spacing: 6) {
                                    ForEach(state.choices) { choice in
                                        let selected = (state.selected == choice.id)
                                        Button {
                                            chooseSource(choice.id)
                                        } label: {
                                            HStack(spacing: 8) {
                                                Circle()
                                                    .fill(selected ? StudioPalette.accent : Color.clear)
                                                    .frame(width: 7, height: 7)
                                                    .overlay(
                                                        Circle()
                                                            .stroke(selected ? StudioPalette.accent : StudioPalette.inkMuted, lineWidth: 1.0)
                                                    )

                                                Text(choice.title)
                                                    .font(.system(size: 12, weight: selected ? .medium : .regular))
                                                    .foregroundStyle(selected ? StudioPalette.accent : StudioPalette.ink)

                                                Spacer()

                                                if selected {
                                                    Text("当前使用")
                                                        .font(.system(size: 11, weight: .medium))
                                                        .foregroundStyle(StudioPalette.accent)
                                                }
                                            }
                                            .padding(.horizontal, 12)
                                            .padding(.vertical, 8)
                                            .background(
                                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                                    .fill(selected ? StudioPalette.accentSoft : StudioPalette.subtleSurface)
                                            )
                                            .overlay(
                                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                                    .stroke(selected ? StudioPalette.accentBorder : StudioPalette.softHairline, lineWidth: 1.0)
                                            )
                                            .contentShape(Rectangle())
                                        }
                                        .buttonStyle(.plain)
                                    }
                                }
                            }
                        }
                        .padding(.horizontal, 16)
                        .padding(.vertical, 12)
                    }
                }
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 1.0)
                )
            }

            // Permissions
            VStack(alignment: .leading, spacing: 8) {
                StudioSectionHeader(title: "系统权限")

                VStack(spacing: 0) {
                    permissionRow(
                        title: "屏幕录制",
                        granted: state.screenPermission,
                        detail: state.screenPermission
                            ? "已授予系统屏幕捕捉权限"
                            : (state.needsRestart ? "请重启 DeskMate 以应用授权" : "需要授权以采集副屏画面"),
                        actionTitle: "前往系统设置",
                        action: { openPermission(true) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 2) {
                            HStack(spacing: 8) {
                                Text("辅助功能")
                                    .font(.system(size: 13, weight: .medium))
                                    .foregroundStyle(StudioPalette.ink)

                                StudioStatusBadge(
                                    text: state.inputPermission ? "已授权" : "未授权",
                                    tone: state.inputPermission ? .positive : .warning
                                )
                            }

                            Text(state.inputPermission
                                ? "已授予输入事件注入权限"
                                : "一键弹窗注册权限并直达系统设置勾选 DeskMate；重编覆盖后若失效可点重置。")
                                .font(.system(size: 11.5))
                                .foregroundStyle(StudioPalette.inkMuted)
                        }

                        Spacer(minLength: 10)

                        if state.inputPermission {
                            Button("系统设置") {
                                openPermission(false)
                            }
                            .buttonStyle(StudioSecondaryButtonStyle())
                        } else {
                            HStack(spacing: 8) {
                                Button("重置旧授权") {
                                    requestAccessibility(true)
                                }
                                .buttonStyle(StudioSecondaryButtonStyle())

                                Button("一键获取") {
                                    requestAccessibility(false)
                                }
                                .buttonStyle(StudioPrimaryButtonStyle())
                            }
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 11)
                }
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 1.0)
                )
            }

            updateSettingsSection
        }
    }

    @ViewBuilder
    private var updateCalloutIfNeeded: some View {
        let u = state.update
        let active = (u.phase == .available || u.phase == .downloading ||
                      u.phase == .verifying || u.phase == .installing)
        if active {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 8) {
                        Text(u.availableVersion.map { "发现新版本 v\($0)" } ?? "软件更新")
                            .font(.system(size: 12.5, weight: .semibold))
                            .foregroundStyle(StudioPalette.ink)
                        StudioStatusBadge(text: "v\(u.currentVersion) → v\(u.availableVersion ?? "")", tone: .accent)
                    }
                    Text(u.statusText)
                        .font(.system(size: 11.5))
                        .foregroundStyle(StudioPalette.inkSecondary)
                        .lineLimit(1)
                }

                Spacer(minLength: 10)

                if u.phase == .available {
                    Button("前往更新") {
                        model.page = .settings
                    }
                    .buttonStyle(StudioSecondaryButtonStyle())

                    Button("立即更新") {
                        installUpdate()
                    }
                    .buttonStyle(StudioPrimaryButtonStyle())
                } else {
                    Text("\(Int((u.progress * 100.0).rounded()))%")
                        .font(.system(size: 12, weight: .semibold, design: .monospaced))
                        .monospacedDigit()
                        .foregroundStyle(StudioPalette.accent)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 11)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .fill(StudioPalette.accentSoft)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.accentBorder, lineWidth: 1.0)
            )
        }
    }

    private var updateSettingsSection: some View {
        let u = state.update
        let busy = (u.phase == .checking || u.phase == .downloading ||
                    u.phase == .verifying || u.phase == .installing)
        let badgeInfo: (String, StudioTone) = {
            switch u.phase {
            case .idle: return ("v\(u.currentVersion)", .neutral)
            case .checking: return ("检查中", .neutral)
            case .upToDate: return ("已是最新", .positive)
            case .available: return ("v\(u.availableVersion ?? "") 可用", .accent)
            case .downloading: return ("下载中 \(Int((u.progress * 100.0).rounded()))%", .accent)
            case .verifying: return ("校验签名中", .accent)
            case .installing: return ("正在安装", .accent)
            case .failed: return ("更新失败", .warning)
            }
        }()

        return VStack(alignment: .leading, spacing: 8) {
            StudioSectionHeader(title: "软件更新")

            VStack(spacing: 0) {
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 8) {
                            Text("当前版本 v\(u.currentVersion)")
                                .font(.system(size: 13, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                            StudioStatusBadge(text: badgeInfo.0, tone: badgeInfo.1)
                        }

                        Text(u.errorMessage ?? u.statusText)
                            .font(.system(size: 11.5))
                            .foregroundStyle(u.phase == .failed ? StudioPalette.warning : StudioPalette.inkMuted)
                            .fixedSize(horizontal: false, vertical: true)
                    }

                    Spacer(minLength: 10)

                    HStack(spacing: 8) {
                        Button("Releases") {
                            openReleasesPage()
                        }
                        .buttonStyle(StudioSecondaryButtonStyle())

                        if u.phase == .available {
                            Button("重新检查") {
                                checkForUpdates()
                            }
                            .buttonStyle(StudioSecondaryButtonStyle())

                            Button("立即更新并重启") {
                                installUpdate()
                            }
                            .buttonStyle(StudioPrimaryButtonStyle())
                        } else if !busy {
                            Button("检查更新") {
                                checkForUpdates()
                            }
                            .buttonStyle(StudioPrimaryButtonStyle())
                        }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)

                if u.phase == .downloading || u.phase == .verifying || u.phase == .installing {
                    GeometryReader { geo in
                        ZStack(alignment: .leading) {
                            Capsule()
                                .fill(StudioPalette.softHairline)
                                .frame(height: 5)
                            Capsule()
                                .fill(StudioPalette.accent)
                                .frame(width: max(6, geo.size.width * CGFloat(min(1.0, max(0.0, u.progress)))), height: 5)
                        }
                    }
                    .frame(height: 5)
                    .padding(.horizontal, 16)
                    .padding(.bottom, 12)
                }

                if u.phase == .available && !u.releaseNotes.isEmpty {
                    Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                    Text(u.releaseNotes)
                        .font(.system(size: 11.5))
                        .foregroundStyle(StudioPalette.inkSecondary)
                        .lineLimit(3)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 16)
                        .padding(.vertical, 9)
                        .background(StudioPalette.subtleSurface)
                }

                Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                controlToggleRow(
                    title: "启动时自动检查更新",
                    subtitle: "从 GitHub Releases 检查带 Ed25519 签名的新版本（免手动重新解除隔离）",
                    isOn: u.autoCheckEnabled,
                    action: { setAutoCheckUpdates(!u.autoCheckEnabled) }
                )

                Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                VStack(alignment: .leading, spacing: 6) {
                    HStack(spacing: 8) {
                        Text("GitHub 加速前缀（可选）")
                            .font(.system(size: 12.5, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                        Spacer()
                        Text("通过 Ed25519 校验完整性，留空即直连")
                            .font(.system(size: 11))
                            .foregroundStyle(StudioPalette.inkMuted)
                    }

                    HStack(spacing: 8) {
                        TextField("例如 https://ghfast.top/", text: $model.mirrorDraft)
                            .textFieldStyle(.plain)
                            .font(.system(size: 12, design: .monospaced))
                            .padding(.horizontal, 10)
                            .padding(.vertical, 6.5)
                            .background(
                                RoundedRectangle(cornerRadius: 9, style: .continuous)
                                    .fill(StudioPalette.subtleSurface)
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: 9, style: .continuous)
                                    .stroke(StudioPalette.hairline, lineWidth: 1.0)
                            )
                            .onAppear {
                                model.mirrorDraft = state.update.mirrorPrefix
                            }
                            .onChange(of: state.update.mirrorPrefix) { _, newValue in
                                if model.mirrorDraft != newValue {
                                    model.mirrorDraft = newValue
                                }
                            }

                        Button("保存") {
                            setUpdateMirrorPrefix(model.mirrorDraft)
                        }
                        .buttonStyle(StudioSecondaryButtonStyle())

                        if !state.update.mirrorPrefix.isEmpty {
                            Button("清除") {
                                model.mirrorDraft = ""
                                setUpdateMirrorPrefix("")
                            }
                            .buttonStyle(StudioSecondaryButtonStyle())
                        }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
            }
            .background(StudioPalette.cardSurface)
            .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 1.0)
            )
        }
    }

    private var touchInputSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            StudioSectionHeader(title: "触控输入")

            VStack(spacing: 0) {
                controlToggleRow(
                    title: "平板触控回传",
                    subtitle: "选择 Mac 如何接收客户端触控；支持按场景切换触控模式并实时生效。",
                    isOn: state.inputEnabled,
                    action: { setInput(!state.inputEnabled) }
                )

                Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("输入模式")
                            .font(.system(size: 13, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                        Text("选择单指、多指手势或原生/妙控板映射方式")
                            .font(.system(size: 11.5))
                            .foregroundStyle(StudioPalette.inkMuted)
                    }

                    Spacer(minLength: 10)

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
                            .font(.system(size: 12, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                            .lineLimit(1)
                            .padding(.horizontal, 12)
                            .padding(.vertical, 6.5)
                            .background(
                                RoundedRectangle(cornerRadius: 9, style: .continuous)
                                    .fill(StudioPalette.subtleSurface)
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: 9, style: .continuous)
                                    .stroke(StudioPalette.hairline, lineWidth: 1.0)
                            )
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 11)

                Rectangle().fill(StudioPalette.softHairline).frame(height: 1.0)

                touchModeDetailPanel
            }
            .background(StudioPalette.cardSurface)
            .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 1.0)
            )
        }
    }

    @ViewBuilder
    private var touchModeDetailPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            if !state.inputPermission {
                HStack(spacing: 10) {
                    StudioStatusBadge(text: "需要辅助功能", tone: .warning)
                    Spacer(minLength: 6)
                    Button("一键获取") {
                        requestAccessibility(false)
                    }
                    .buttonStyle(StudioPrimaryButtonStyle())
                    .fixedSize()
                    Button("重置旧授权") {
                        requestAccessibility(true)
                    }
                    .buttonStyle(StudioSecondaryButtonStyle())
                    .fixedSize()
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 9)
                .background(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .fill(StudioPalette.warningWash)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .stroke(StudioPalette.warningBorder, lineWidth: 1.0)
                )
            }

            if state.touchInputMode.usesVirtualHIDWhenAvailable {
                HStack(spacing: 10) {
                    StudioStatusBadge(
                        text: state.touchCapability.virtualHIDAvailable
                            ? "虚拟 HID 可用"
                            : (state.touchInputMode == .virtualTrackpad ? "妙控板手势模拟" : "兼容手势映射回退"),
                        tone: state.touchCapability.virtualHIDAvailable ? .positive : .neutral
                    )

                    Text(state.touchCapability.activeBackendLabel)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(StudioPalette.inkMuted)
                        .lineLimit(1)

                    Spacer(minLength: 8)

                    if state.touchInputMode == .virtualTrackpad {
                        Button("触控板设置") {
                            openTrackpadSettings()
                        }
                        .buttonStyle(StudioSecondaryButtonStyle())
                    }

                    Button("管理授权") {
                        openPermission(false)
                    }
                    .buttonStyle(StudioSecondaryButtonStyle())
                }
            }

            Text(state.touchInputMode.summaryText)
                .font(.system(size: 12))
                .foregroundStyle(StudioPalette.inkSecondary)
                .lineSpacing(2)
                .fixedSize(horizontal: false, vertical: true)

            if state.touchInputMode == .compatibleGestures {
                VStack(spacing: 6) {
                    ForEach(state.touchInputMode.compatibleGestureRows, id: \.fingers) { row in
                        HStack(alignment: .firstTextBaseline, spacing: 12) {
                            Text(row.fingers)
                                .font(.system(size: 12, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                                .frame(width: 82, alignment: .leading)

                            Text(row.description)
                                .font(.system(size: 12))
                                .foregroundStyle(StudioPalette.inkSecondary)
                                .fixedSize(horizontal: false, vertical: true)

                            Spacer(minLength: 0)
                        }
                    }
                }
                .padding(.top, 2)
            } else if state.touchInputMode == .virtualTrackpad {
                VStack(alignment: .leading, spacing: 5) {
                    ForEach(state.touchInputMode.trackpadGuidanceLines, id: \.self) { line in
                        Text(line)
                            .font(.system(size: 11.5))
                            .foregroundStyle(StudioPalette.inkMuted)
                            .lineSpacing(2)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .fill(StudioPalette.subtleSurface)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .stroke(StudioPalette.softHairline, lineWidth: 1.0)
                )
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    private func permissionRow(title: String, granted: Bool, detail: String, actionTitle: String, action: @escaping () -> Void) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 8) {
                    Text(title)
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(StudioPalette.ink)

                    StudioStatusBadge(
                        text: granted ? "已授权" : "未授权",
                        tone: granted ? .positive : .warning
                    )
                }

                Text(detail)
                    .font(.system(size: 11.5))
                    .foregroundStyle(StudioPalette.inkMuted)
            }

            Spacer(minLength: 10)

            Button(actionTitle, action: action)
                .buttonStyle(StudioSecondaryButtonStyle())
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 11)
    }

    // MARK: - Diagnostics Page

    private var diagnosticsContent: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .center, spacing: 10) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("运行诊断日志")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)
                    Text("实时查看 USB AOA 链路、编码器协商与触控注入快照。")
                        .font(.system(size: 12))
                        .foregroundStyle(StudioPalette.inkSecondary)
                }

                Spacer()

                if state.canRetry {
                    Button("重新建立连接") { retry() }
                        .buttonStyle(StudioSecondaryButtonStyle())
                }

                Button(model.copiedDiagnostics ? "已复制到剪贴板" : "复制诊断信息") {
                    copy()
                    model.copiedDiagnostics = true
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.8) {
                        model.copiedDiagnostics = false
                    }
                }
                .buttonStyle(StudioPrimaryButtonStyle())
            }

            ScrollView(.vertical, showsIndicators: true) {
                Text(diagnosticText)
                    .font(.system(size: 11.5, design: .monospaced))
                    .foregroundStyle(StudioPalette.inkSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(16)
                    .textSelection(.enabled)
            }
            .frame(minHeight: 280, maxHeight: 380)
            .background(StudioPalette.cardSurface)
            .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 1.0)
            )
        }
    }

    private var diagnosticText: String {
        if !state.diagnostic.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return state.diagnostic
        }
        return """
        state: \(state.badge)
        service_running: \(state.serviceRunning) (auto_start=\(state.autoStartService), launch_at_login=\(state.launchAtLogin))
        linked: \(state.linked)
        device: \(state.connectedDeviceName ?? "none")
        paired_devices: \(state.pairedDevices.count)
        source: \(state.source)
        auto_virtual_display: \(state.autoVirtualDisplay)
        position: \(state.position.shortLabel)
        video: \(state.video) (enabled=\(state.videoEnabled))
        input: \(state.input) (enabled=\(state.inputEnabled), mode=\(state.touchInputMode.displayName), backend=\(state.touchCapability.activeBackendLabel), events=\(state.inputEventCount))
        permissions: screen=\(state.screenPermission), accessibility=\(state.inputPermission)
        stream: \(state.streamSummary.isEmpty ? "none" : state.streamSummary)
        """
    }

    // MARK: - Bottom Whisper Bar

    private var whisperBar: some View {
        HStack(spacing: 12) {
            Text("USB-C AOA 硬件直连 · \(state.autoVirtualDisplay ? "自动 HiDPI 虚拟屏" : state.source)")
                .font(.system(size: 11))
                .foregroundStyle(StudioPalette.inkMuted)
                .lineLimit(1)

            Spacer(minLength: 10)

            if !state.streamSummary.isEmpty {
                Text(state.streamSummary)
                    .font(.system(size: 11, design: .monospaced))
                    .monospacedDigit()
                    .foregroundStyle(StudioPalette.inkSecondary)
                    .lineLimit(1)
            }

            Button {
                copy()
                model.copiedDiagnostics = true
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                    model.copiedDiagnostics = false
                }
            } label: {
                Text(model.copiedDiagnostics ? "已复制" : "复制摘要")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(model.copiedDiagnostics ? StudioPalette.accent : StudioPalette.inkSecondary)
            }
            .buttonStyle(StudioFlatActionStyle())
        }
        .padding(.horizontal, 20)
        .frame(height: 32)
        .background(StudioPalette.cardSurface)
    }
}
