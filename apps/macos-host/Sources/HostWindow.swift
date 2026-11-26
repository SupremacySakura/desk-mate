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

        var englishTag: String {
            switch self {
            case .display: return "DISPLAY"
            case .devices: return "DEVICES"
            case .settings: return "SETTINGS"
            case .diagnostics: return "LOGS"
            }
        }
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
            return .crimson
        }
        switch state.badge {
        case "出错", "待授权", "未就绪", "已拦截":
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
                    .frame(height: 0.75)

                HStack(alignment: .top, spacing: 24) {
                    leftTopologyColumn
                        .frame(width: 368)

                    rightContentColumn
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                .padding(.horizontal, 24)
                .padding(.top, 14)
                .padding(.bottom, 12)
                .frame(maxWidth: .infinity, maxHeight: .infinity)

                Rectangle()
                    .fill(StudioPalette.hairline)
                    .frame(height: 0.75)

                whisperBar
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(minWidth: 820, minHeight: 520)
    }

    // MARK: - Top Navigation Bar

    private var topBar: some View {
        HStack(spacing: 16) {
            HStack(spacing: 12) {
                DualDisplayGlyph(linked: state.linked, streaming: streaming, size: 34)

                VStack(alignment: .leading, spacing: 2) {
                    Text("DESKMATE // USB HOST")
                        .font(.system(size: 9.5, weight: .semibold, design: .monospaced))
                        .tracking(1.4)
                        .foregroundStyle(StudioPalette.inkMuted)

                    HStack(spacing: 10) {
                        Text("桌面副屏工作台")
                            .font(.system(size: 14.5, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)

                        StudioStatusBadge(text: state.badge, tone: badgeTone)
                    }
                }
            }

            Spacer(minLength: 16)

            HStack(spacing: 18) {
                ForEach(Page.allCases, id: \.self) { tab in
                    topTabButton(tab)
                }
            }
        }
        .padding(.horizontal, 24)
        .padding(.vertical, 10)
        .background(StudioPalette.cardSurface.opacity(0.75))
    }

    private func topTabButton(_ tab: Page) -> some View {
        let active = (model.page == tab)
        return Button {
            model.page = tab
        } label: {
            VStack(spacing: 5) {
                HStack(spacing: 5) {
                    Rectangle()
                        .fill(active ? StudioPalette.crimson : Color.clear)
                        .frame(width: 4, height: 4)

                    Text("\(tab.englishTag) / \(tab.rawValue)")
                        .font(.system(size: 11, weight: active ? .semibold : .regular))
                        .foregroundStyle(active ? StudioPalette.crimson : StudioPalette.inkMuted)
                        .lineLimit(1)
                        .fixedSize(horizontal: true, vertical: false)
                }

                Rectangle()
                    .fill(active ? StudioPalette.crimson : Color.clear)
                    .frame(height: 1.5)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    // MARK: - Left Topology Column

    private var leftTopologyColumn: some View {
        VStack(spacing: 9) {
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

            // Compact runtime summary strip below topology schematic
            VStack(spacing: 0) {
                metaRow(label: "USB 通道", value: usbChannelText, highlight: state.linked)
                Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)
                metaRow(label: "画面管线", value: state.video, highlight: streaming)
                Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)
                metaRow(label: "触控通道", value: state.input, highlight: state.inputEnabled && state.inputPermission)
            }
            .background(StudioPalette.cardSurface)
            .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 0.75)
            )

            Spacer(minLength: 0)
        }
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
                return "\(name) · AOA 直连"
            }
            return "AOA 直连已就绪"
        }
        return "等待设备接入"
    }

    private func metaRow(label: String, value: String, highlight: Bool) -> some View {
        HStack(spacing: 8) {
            Text(label)
                .font(.system(size: 11))
                .foregroundStyle(StudioPalette.inkMuted)
            Spacer(minLength: 8)
            Text(value)
                .font(.system(size: 11.5, weight: highlight ? .medium : .regular))
                .foregroundStyle(highlight ? StudioPalette.ink : StudioPalette.inkSecondary)
                .lineLimit(1)
                .truncationMode(.tail)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6.5)
    }

    // MARK: - Right Content Column

    @ViewBuilder
    private var rightContentColumn: some View {
        ScrollView(.vertical, showsIndicators: false) {
            VStack(alignment: .leading, spacing: 18) {
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
            .padding(.bottom, 8)
        }
    }

    // MARK: - Overview Page

    private var displayOverviewContent: some View {
        VStack(alignment: .leading, spacing: 13) {
            // Editorial state headline
            VStack(alignment: .leading, spacing: 4) {
                Text("SESSION // 运行状态")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .tracking(1.4)
                    .foregroundStyle(StudioPalette.crimson)

                Text(state.title)
                    .font(.system(size: 21, weight: .light))
                    .foregroundStyle(StudioPalette.ink)
                    .fixedSize(horizontal: false, vertical: true)

                Text(state.guidance)
                    .font(.system(size: 12))
                    .foregroundStyle(StudioPalette.inkSecondary)
                    .lineSpacing(2.5)
                    .fixedSize(horizontal: false, vertical: true)
            }

            // Contextual action / authorization bar when needed
            actionCalloutIfNeeded

            updateCalloutIfNeeded

            if !state.activeSessions.isEmpty {
                activeSessionsSection
            }

            // Telemetry bar
            VStack(alignment: .leading, spacing: 8) {
                StudioSectionHeader(english: "TELEMETRY", chinese: "实时性能")

                HStack(spacing: 0) {
                    metricCell(
                        title: "帧率 FPS",
                        value: state.liveFps > 0 ? "\(state.liveFps)" : "—",
                        unit: state.liveFps > 0 ? "fps" : "待机",
                        accent: streaming && state.liveFps > 0
                    )

                    Rectangle()
                        .fill(StudioPalette.softHairline)
                        .frame(width: 0.75, height: 36)

                    metricCell(
                        title: "码率 BITRATE",
                        value: state.liveBitrateMbps > 0 ? String(format: "%.1f", state.liveBitrateMbps) : "—",
                        unit: state.liveBitrateMbps > 0 ? "Mbps" : "待机",
                        accent: false
                    )

                    Rectangle()
                        .fill(StudioPalette.softHairline)
                        .frame(width: 0.75, height: 36)

                    metricCell(
                        title: "反向触控",
                        value: state.inputEnabled ? "\(state.inputEventCount)" : "关闭",
                        unit: state.inputEnabled ? "次事件" : "已禁用",
                        accent: false
                    )
                }
                .padding(.vertical, 9.5)
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 0.75)
                )
            }

            // Quick controls
            VStack(alignment: .leading, spacing: 8) {
                StudioSectionHeader(english: "CONTROLS", chinese: "快速控制")

                VStack(spacing: 0) {
                    controlToggleRow(
                        title: "视频画面推流",
                        subtitle: "将虚拟显示器或指定画面编码发送至平板",
                        isOn: state.videoEnabled,
                        action: { setVideo(!state.videoEnabled) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    controlToggleRow(
                        title: "平板触控回传",
                        subtitle: "当前模式：\(state.touchInputMode.displayName) · 可在「设置」中切换",
                        isOn: state.inputEnabled,
                        action: { setInput(!state.inputEnabled) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    VStack(alignment: .leading, spacing: 7) {
                        HStack(spacing: 8) {
                            Text(state.activeSessions.count > 1 ? "当前选中副屏方位" : "虚拟副屏方位")
                                .font(.system(size: 12.5, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                            Spacer(minLength: 8)
                            Text("支持点选左侧拓扑图或下方快速切换")
                                .font(.system(size: 10.5))
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
                    .padding(.horizontal, 14)
                    .padding(.vertical, 9)

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    frameRateControlRow
                }
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 0.75)
                )
            }
        }
    }

    private var activeSessionsSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            StudioSectionHeader(
                english: "ACTIVE DISPLAYS",
                chinese: "在线副屏（\(state.activeSessions.count)）"
            )

            VStack(spacing: 8) {
                ForEach(state.activeSessions) { session in
                    activeSessionCard(session)
                }
            }
        }
    }

    private func activeSessionCard(_ session: ActiveDisplaySessionState) -> some View {
        let isSelected = (state.selectedLinkID == session.linkID)
        let sessionTone: StudioTone = session.streaming ? .crimson : (session.error != nil ? .warning : .positive)

        return VStack(spacing: 0) {
            HStack(spacing: 10) {
                StudioDeviceGlyph(active: session.streaming || isSelected, allowed: true, size: 28)

                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 7) {
                        Text(session.deviceName)
                            .font(.system(size: 12.5, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                            .lineLimit(1)

                        StudioStatusBadge(text: session.badge, tone: sessionTone)

                        if isSelected && state.activeSessions.count > 1 {
                            Text("已选中")
                                .font(.system(size: 9.5, weight: .medium))
                                .foregroundStyle(StudioPalette.crimson)
                                .padding(.horizontal, 5)
                                .padding(.vertical, 1.5)
                                .background(StudioPalette.roseWash)
                                .clipShape(RoundedRectangle(cornerRadius: 2, style: .continuous))
                        }
                    }

                    HStack(spacing: 8) {
                        if !session.streamSummary.isEmpty {
                            Text(session.streamSummary)
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundStyle(StudioPalette.inkSecondary)
                        }
                        if session.streaming && session.liveFps > 0 {
                            Text(session.liveBitrateMbps > 0
                                 ? String(format: "%d FPS · %.1f Mbps", session.liveFps, session.liveBitrateMbps)
                                 : "\(session.liveFps) FPS")
                                .font(.system(size: 10, weight: .semibold, design: .monospaced))
                                .foregroundStyle(StudioPalette.crimson)
                        }
                        if let displayID = session.displayID {
                            Text("#\(displayID)")
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundStyle(StudioPalette.inkMuted)
                        }
                    }
                }

                Spacer(minLength: 6)

                HStack(spacing: 4) {
                    ForEach(DisplayPosition.allCases, id: \.self) { pos in
                        let active = (session.position == pos)
                        Button {
                            setLinkPosition(session.linkID, pos)
                        } label: {
                            Text(pos.shortLabel)
                                .font(.system(size: 10, weight: active ? .semibold : .regular))
                                .foregroundStyle(active ? StudioPalette.crimson : StudioPalette.inkMuted)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 3.5)
                                .background(
                                    RoundedRectangle(cornerRadius: 2.5, style: .continuous)
                                        .fill(active ? StudioPalette.roseWash : StudioPalette.subtleSurface)
                                )
                                .overlay(
                                    RoundedRectangle(cornerRadius: 2.5, style: .continuous)
                                        .stroke(active ? StudioPalette.roseBorder : StudioPalette.hairline, lineWidth: 0.6)
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
            .padding(.horizontal, 12)
            .padding(.vertical, 9)
        }
        .background(StudioPalette.cardSurface)
        .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                .stroke(isSelected ? StudioPalette.roseBorder : StudioPalette.hairline, lineWidth: isSelected ? 1.0 : 0.75)
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
                VStack(alignment: .leading, spacing: 3) {
                    Text("串流服务当前处于停止状态")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)
                    Text("启动服务后将监听 USB 接入并按偏好创建虚拟显示器。")
                        .font(.system(size: 11))
                        .foregroundStyle(StudioPalette.inkSecondary)
                }
                Spacer(minLength: 10)
                Button("启动串流服务") { setServiceRunning(true) }
                    .buttonStyle(StudioPrimaryButtonStyle())
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .fill(StudioPalette.subtleSurface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 0.75)
            )
        } else if hasBlocked, let blocked = state.blockedDeviceName {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 3) {
                    Text("已阻止连接：\(blocked)")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)
                    Text("该设备已在「已配对设备」中设为禁止连接。")
                        .font(.system(size: 11))
                        .foregroundStyle(StudioPalette.inkSecondary)
                }
                Spacer(minLength: 10)
                Button("前往设备列表") { model.page = .devices }
                    .buttonStyle(StudioSecondaryButtonStyle())
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .fill(StudioPalette.amberWash)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioTone.warning.border, lineWidth: 0.75)
            )
        } else if missingScreen || missingInput || state.canRetry {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 3) {
                    Text(missingScreen ? "需要屏幕录制权限" : (missingInput ? "需要辅助功能权限" : "连接维护"))
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(StudioPalette.ink)

                    Text(
                        missingScreen
                        ? (state.needsRestart ? "如已在系统设置中勾选，请重新打开 DeskMate 使其生效。" : "DeskMate 需要截取扩展屏幕并编码传输至平板。")
                        : (missingInput ? "开启触控回传需要辅助功能权限以注入鼠标与滚轮事件。" : "可手动重新握手 USB 通道并同步虚拟显示器。")
                    )
                    .font(.system(size: 11))
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
            .padding(.horizontal, 14)
            .padding(.vertical, 11)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .fill((missingScreen || missingInput) ? StudioPalette.amberWash : StudioPalette.subtleSurface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke((missingScreen || missingInput) ? StudioTone.warning.border : StudioPalette.hairline, lineWidth: 0.75)
            )
        }
    }

    private func metricCell(title: String, value: String, unit: String, accent: Bool) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title)
                .font(.system(size: 9.5, weight: .medium, design: .monospaced))
                .tracking(0.8)
                .foregroundStyle(StudioPalette.inkMuted)

            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Text(value)
                    .font(.system(size: 20, weight: .medium, design: .monospaced))
                    .monospacedDigit()
                    .foregroundStyle(accent ? StudioPalette.crimson : StudioPalette.ink)
                Text(unit)
                    .font(.system(size: 10.5))
                    .foregroundStyle(StudioPalette.inkMuted)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 14)
    }

    private func controlToggleRow(title: String, subtitle: String, isOn: Bool, action: @escaping () -> Void) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 12.5, weight: .medium))
                    .foregroundStyle(StudioPalette.ink)
                Text(subtitle)
                    .font(.system(size: 11))
                    .foregroundStyle(StudioPalette.inkMuted)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Spacer(minLength: 10)

            StudioHairlineToggle(isOn: isOn, action: action)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 9)
    }

    private var frameRateSubtitleText: String {
        if state.preferredFrameRateHz == 0 {
            if state.negotiatedRefreshHz > 0 {
                return "跟随设备协商（当前 \(state.negotiatedRefreshHz) Hz）"
            }
            return "默认跟随设备协商帧率"
        }
        if state.negotiatedRefreshHz > 0 {
            if state.preferredFrameRateHz > state.negotiatedRefreshHz {
                return "已按协商上限 \(state.negotiatedRefreshHz) Hz 输出（请求 \(state.preferredFrameRateHz) Hz）"
            }
            return "手动锁定 \(state.preferredFrameRateHz) Hz（协商 \(state.negotiatedRefreshHz) Hz）"
        }
        return "手动锁定 \(state.preferredFrameRateHz) Hz"
    }

    private var frameRateControlRow: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text("画面帧率")
                    .font(.system(size: 12.5, weight: .medium))
                    .foregroundStyle(StudioPalette.ink)
                Spacer(minLength: 8)
                Text(frameRateSubtitleText)
                    .font(.system(size: 10.5))
                    .foregroundStyle(state.preferredFrameRateHz > 0 ? StudioPalette.crimson : StudioPalette.inkMuted)
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
        .padding(.horizontal, 14)
        .padding(.vertical, 9)
    }

    // MARK: - Paired Devices Page

    private var devicesContent: some View {
        VStack(alignment: .leading, spacing: 15) {
            VStack(alignment: .leading, spacing: 4) {
                Text("DEVICES // 客户端设备管理")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .tracking(1.4)
                    .foregroundStyle(StudioPalette.crimson)

                Text("已配对设备")
                    .font(.system(size: 20, weight: .light))
                    .foregroundStyle(StudioPalette.ink)

                Text("管理允许通过 USB 连接此 Mac 的平板设备；关闭授权将禁止自动接管并立即断开当前会话。")
                    .font(.system(size: 11.5))
                    .foregroundStyle(StudioPalette.inkSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack(spacing: 10) {
                StudioStatusBadge(
                    text: "\(state.pairedDevices.count) 台设备",
                    tone: state.pairedDevices.isEmpty ? .neutral : .positive
                )

                Spacer()

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
                        .font(.system(size: 12.5, weight: .medium))
                        .foregroundStyle(StudioPalette.ink)
                    Text("使用 USB 数据线连接平板并打开 DeskMate 后，设备会自动记录在这里；也可点击右上角「刷新」手动扫描已接入设备。")
                        .font(.system(size: 11.5))
                        .foregroundStyle(StudioPalette.inkMuted)
                        .lineSpacing(2.5)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(16)
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 0.75)
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
                StudioDeviceGlyph(active: isConnected, allowed: device.allowed, size: 32)

                VStack(alignment: .leading, spacing: 4) {
                    Text(device.displayName)
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(StudioPalette.ink)
                        .lineLimit(1)

                    HStack(spacing: 8) {
                        if isConnected {
                            StudioStatusBadge(text: "当前连接", tone: .crimson)
                        } else if !device.allowed {
                            StudioStatusBadge(text: "已禁止连接", tone: .warning)
                        } else if isOnline {
                            StudioStatusBadge(text: "USB 已接入", tone: .positive)
                        } else {
                            StudioStatusBadge(text: "允许连接", tone: .positive)
                        }

                        if let prefPos = device.preferredPosition {
                            Text("默认\(prefPos.shortLabel)")
                                .font(.system(size: 10, weight: .medium))
                                .foregroundStyle(StudioPalette.inkSecondary)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(StudioPalette.subtleSurface)
                                .clipShape(RoundedRectangle(cornerRadius: 2, style: .continuous))
                                .overlay(
                                    RoundedRectangle(cornerRadius: 2, style: .continuous)
                                        .stroke(StudioPalette.hairline, lineWidth: 0.6)
                                )
                        }

                        Text("最近连接 \(PairedDeviceStore.relativeTimeText(from: device.lastConnectedAt))")
                            .font(.system(size: 11))
                            .foregroundStyle(StudioPalette.inkMuted)
                            .lineLimit(1)
                            .fixedSize(horizontal: true, vertical: false)
                    }

                    if let resolution = device.lastResolution, !resolution.isEmpty {
                        Text(resolution)
                            .font(.system(size: 10, design: .monospaced))
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
                            .font(.system(size: 11.5, weight: .medium))
                            .foregroundStyle(StudioPalette.inkSecondary)
                            .frame(width: 24, height: 24)
                            .background(
                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                    .fill(StudioPalette.subtleSurface)
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                    .stroke(StudioPalette.hairline, lineWidth: 0.75)
                            )
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 11)

            if isRenaming {
                Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                HStack(spacing: 8) {
                    TextField("输入自定义设备名称（留空恢复默认）", text: $model.renameDraft)
                        .textFieldStyle(.plain)
                        .font(.system(size: 11.5))
                        .padding(.horizontal, 9)
                        .padding(.vertical, 5.5)
                        .background(
                            RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                .fill(StudioPalette.cardSurface)
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                .stroke(StudioPalette.hairline, lineWidth: 0.75)
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
                .padding(.horizontal, 14)
                .padding(.vertical, 9)
                .background(StudioPalette.subtleSurface)
            }
        }
        .background(StudioPalette.cardSurface)
        .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                .stroke(isConnected ? StudioPalette.roseBorder : StudioPalette.hairline, lineWidth: isConnected ? 1.0 : 0.75)
        )
    }

    // MARK: - Settings Page

    private var settingsContent: some View {
        VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 4) {
                Text("SETTINGS // 显示与控制配置")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .tracking(1.4)
                    .foregroundStyle(StudioPalette.crimson)

                Text("显示与启动设置")
                    .font(.system(size: 20, weight: .light))
                    .foregroundStyle(StudioPalette.ink)
            }

            // Startup Behavior section
            VStack(alignment: .leading, spacing: 10) {
                StudioSectionHeader(english: "STARTUP", chinese: "启动行为")

                VStack(spacing: 0) {
                    controlToggleRow(
                        title: "登录时打开 DeskMate",
                        subtitle: state.loginItemSummary.isEmpty ? "配置 macOS 登录项，登录系统时自动启动应用。" : state.loginItemSummary,
                        isOn: state.launchAtLogin,
                        action: { setLaunchAtLogin(!state.launchAtLogin) }
                    )

                    if state.loginItemNeedsApproval || (state.loginItemError?.isEmpty == false) {
                        Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                        HStack(spacing: 10) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(state.loginItemNeedsApproval ? "需要在系统登录项中批准" : "登录项注册错误")
                                    .font(.system(size: 11.5, weight: .semibold))
                                    .foregroundStyle(StudioPalette.ink)

                                if let err = state.loginItemError, !err.isEmpty {
                                    Text(err)
                                        .font(.system(size: 10.5, design: .monospaced))
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
                        .padding(.horizontal, 14)
                        .padding(.vertical, 9)
                        .background(StudioPalette.amberWash)
                    }

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    controlToggleRow(
                        title: "打开应用后自动启动服务",
                        subtitle: "与上方登录项同时开启后，登录 Mac 即可自动提供 USB 扩展串流服务。",
                        isOn: state.autoStartService,
                        action: { setAutoStartService(!state.autoStartService) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 2) {
                            HStack(spacing: 8) {
                                Text("当前串流服务")
                                    .font(.system(size: 12.5, weight: .medium))
                                    .foregroundStyle(StudioPalette.ink)

                                StudioStatusBadge(
                                    text: state.serviceRunning ? "运行中" : "已停止",
                                    tone: state.serviceRunning ? .positive : .neutral
                                )
                            }

                            Text(state.serviceRunning ? "正在监听 USB 平板连接并管理虚拟显示器" : "服务已停止，不会自动接管 USB 设备")
                                .font(.system(size: 11))
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
                    .padding(.horizontal, 14)
                    .padding(.vertical, 9)
                }
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 0.75)
                )
            }

            // Touch Input Mode section
            touchInputSection

            // Virtual Display & Source section
            VStack(alignment: .leading, spacing: 10) {
                StudioSectionHeader(english: "VIRTUAL DISPLAY", chinese: "虚拟显示器与输入源")

                VStack(spacing: 0) {
                    controlToggleRow(
                        title: "自动匹配设备生成虚拟屏",
                        subtitle: "连接时读取平板比例创建 HiDPI 扩展屏，断开后自动销毁",
                        isOn: state.autoVirtualDisplay,
                        action: { setAutoVirtual(!state.autoVirtualDisplay) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    VStack(alignment: .leading, spacing: 8) {
                        HStack(spacing: 8) {
                            Text("默认扩展位置")
                                .font(.system(size: 12.5, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                            Spacer(minLength: 8)
                            Text("相对 Mac 主屏的对齐方向")
                                .font(.system(size: 10.5))
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
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    frameRateControlRow

                    if !state.autoVirtualDisplay {
                        Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Text(state.fixedSource ? "固定输出显示源" : "选择现有物理 / 虚拟显示器")
                                    .font(.system(size: 12, weight: .medium))
                                    .foregroundStyle(StudioPalette.ink)
                                Spacer()
                                Text(state.source)
                                    .font(.system(size: 11, design: .monospaced))
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
                                                Rectangle()
                                                    .fill(selected ? StudioPalette.crimson : Color.clear)
                                                    .frame(width: 5, height: 5)
                                                    .overlay(
                                                        Rectangle()
                                                            .stroke(selected ? StudioPalette.crimson : StudioPalette.inkMuted, lineWidth: 0.75)
                                                    )

                                                Text(choice.title)
                                                    .font(.system(size: 11.5, weight: selected ? .medium : .regular))
                                                    .foregroundStyle(selected ? StudioPalette.crimson : StudioPalette.ink)

                                                Spacer()

                                                if selected {
                                                    Text("当前使用")
                                                        .font(.system(size: 10))
                                                        .foregroundStyle(StudioPalette.crimson)
                                                }
                                            }
                                            .padding(.horizontal, 10)
                                            .padding(.vertical, 7)
                                            .background(
                                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                                    .fill(selected ? StudioPalette.roseWash : StudioPalette.subtleSurface)
                                            )
                                            .overlay(
                                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                                    .stroke(selected ? StudioPalette.roseBorder : StudioPalette.hairline, lineWidth: 0.75)
                                            )
                                            .contentShape(Rectangle())
                                        }
                                        .buttonStyle(.plain)
                                    }
                                }
                            }
                        }
                        .padding(.horizontal, 14)
                        .padding(.vertical, 11)
                    }
                }
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 0.75)
                )
            }

            // Permissions Section
            VStack(alignment: .leading, spacing: 10) {
                StudioSectionHeader(english: "PERMISSIONS", chinese: "系统授权状态")

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

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 2) {
                            HStack(spacing: 8) {
                                Text("辅助功能")
                                    .font(.system(size: 12.5, weight: .medium))
                                    .foregroundStyle(StudioPalette.ink)

                                StudioStatusBadge(
                                    text: state.inputPermission ? "已授权" : "未授权",
                                    tone: state.inputPermission ? .positive : .warning
                                )
                            }

                            Text(state.inputPermission
                                ? "已授予输入事件注入权限"
                                : "一键弹窗注册权限并直达系统设置勾选 DeskMate；重编覆盖后若失效可点重置。")
                                .font(.system(size: 11))
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
                    .padding(.horizontal, 14)
                    .padding(.vertical, 9)
                }
                .background(StudioPalette.cardSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 0.75)
                )
            }

            // Software Update Section
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
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(StudioPalette.ink)
                        StudioStatusBadge(text: "v\(u.currentVersion) → v\(u.availableVersion ?? "")", tone: .crimson)
                    }
                    Text(u.statusText)
                        .font(.system(size: 11))
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
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundStyle(StudioPalette.crimson)
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .fill(StudioPalette.roseWash)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.roseBorder, lineWidth: 0.75)
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
            case .available: return ("v\(u.availableVersion ?? "") 可用", .crimson)
            case .downloading: return ("下载中 \(Int((u.progress * 100.0).rounded()))%", .crimson)
            case .verifying: return ("校验签名中", .crimson)
            case .installing: return ("正在安装", .crimson)
            case .failed: return ("更新失败", .warning)
            }
        }()

        return VStack(alignment: .leading, spacing: 10) {
            StudioSectionHeader(english: "UPDATES", chinese: "软件更新")

            VStack(spacing: 0) {
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 8) {
                            Text("当前版本 v\(u.currentVersion)")
                                .font(.system(size: 12.5, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                            StudioStatusBadge(text: badgeInfo.0, tone: badgeInfo.1)
                        }

                        Text(u.errorMessage ?? u.statusText)
                            .font(.system(size: 11))
                            .foregroundStyle(u.phase == .failed ? StudioPalette.amber : StudioPalette.inkMuted)
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
                .padding(.horizontal, 14)
                .padding(.vertical, 10)

                if u.phase == .downloading || u.phase == .verifying || u.phase == .installing {
                    VStack(alignment: .leading, spacing: 5) {
                        GeometryReader { geo in
                            ZStack(alignment: .leading) {
                                RoundedRectangle(cornerRadius: 2, style: .continuous)
                                    .fill(StudioPalette.softHairline)
                                    .frame(height: 4)
                                RoundedRectangle(cornerRadius: 2, style: .continuous)
                                    .fill(StudioPalette.crimson)
                                    .frame(width: max(4, geo.size.width * CGFloat(min(1.0, max(0.0, u.progress)))), height: 4)
                            }
                        }
                        .frame(height: 4)
                    }
                    .padding(.horizontal, 14)
                    .padding(.bottom, 10)
                }

                if u.phase == .available && !u.releaseNotes.isEmpty {
                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    Text(u.releaseNotes)
                        .font(.system(size: 11))
                        .foregroundStyle(StudioPalette.inkSecondary)
                        .lineLimit(3)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 8)
                        .background(StudioPalette.subtleSurface)
                }

                Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                controlToggleRow(
                    title: "启动时自动检查更新",
                    subtitle: "从 GitHub Releases 检查带 Ed25519 签名的新版本（免手动重新解除隔离）",
                    isOn: u.autoCheckEnabled,
                    action: { setAutoCheckUpdates(!u.autoCheckEnabled) }
                )

                Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                VStack(alignment: .leading, spacing: 6) {
                    HStack(spacing: 8) {
                        Text("GitHub 加速前缀（可选）")
                            .font(.system(size: 12, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                        Spacer()
                        Text("通过 Ed25519 校验完整性，留空即直连")
                            .font(.system(size: 10.5))
                            .foregroundStyle(StudioPalette.inkMuted)
                    }

                    HStack(spacing: 8) {
                        TextField("例如 https://ghfast.top/", text: $model.mirrorDraft)
                            .textFieldStyle(.plain)
                            .font(.system(size: 11.5, design: .monospaced))
                            .padding(.horizontal, 9)
                            .padding(.vertical, 6)
                            .background(
                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                    .fill(StudioPalette.subtleSurface)
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                    .stroke(StudioPalette.hairline, lineWidth: 0.75)
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
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
            }
            .background(StudioPalette.cardSurface)
            .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 0.75)
            )
        }
    }

    private var touchInputSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            StudioSectionHeader(english: "TOUCH INPUT", chinese: "触控输入")

            VStack(spacing: 0) {
                controlToggleRow(
                    title: "平板触控回传",
                    subtitle: "选择 Mac 如何接收客户端触控；支持按场景切换触控模式并实时生效。",
                    isOn: state.inputEnabled,
                    action: { setInput(!state.inputEnabled) }
                )

                Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("输入模式")
                            .font(.system(size: 12.5, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                        Text("选择单指、多指手势或原生/妙控板映射方式")
                            .font(.system(size: 11))
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
                            .font(.system(size: 11.5, weight: .medium))
                            .foregroundStyle(StudioPalette.ink)
                            .lineLimit(1)
                            .padding(.horizontal, 11)
                            .padding(.vertical, 6)
                            .background(
                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                    .fill(StudioPalette.subtleSurface)
                            )
                            .overlay(
                                RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                                    .stroke(StudioPalette.hairline, lineWidth: 0.75)
                            )
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 9)

                Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                touchModeDetailPanel
            }
            .background(StudioPalette.cardSurface)
            .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                    .stroke(StudioPalette.hairline, lineWidth: 0.75)
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
                        .fill(StudioPalette.amberWash)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .stroke(StudioTone.warning.border, lineWidth: 0.75)
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
                        .font(.system(size: 10.5, design: .monospaced))
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
                .font(.system(size: 11.5))
                .foregroundStyle(StudioPalette.inkSecondary)
                .lineSpacing(2)
                .fixedSize(horizontal: false, vertical: true)

            if state.touchInputMode == .compatibleGestures {
                VStack(spacing: 6) {
                    ForEach(state.touchInputMode.compatibleGestureRows, id: \.fingers) { row in
                        HStack(alignment: .firstTextBaseline, spacing: 12) {
                            Text(row.fingers)
                                .font(.system(size: 11.5, weight: .medium))
                                .foregroundStyle(StudioPalette.ink)
                                .frame(width: 82, alignment: .leading)

                            Text(row.description)
                                .font(.system(size: 11.5))
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
                            .font(.system(size: 11))
                            .foregroundStyle(StudioPalette.inkMuted)
                            .lineSpacing(2)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .padding(10)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .fill(StudioPalette.subtleSurface)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cornerRadius, style: .continuous)
                        .stroke(StudioPalette.softHairline, lineWidth: 0.75)
                )
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 14)
        .padding(.vertical, 11)
    }

    private func permissionRow(title: String, granted: Bool, detail: String, actionTitle: String, action: @escaping () -> Void) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 8) {
                    Text(title)
                        .font(.system(size: 12.5, weight: .medium))
                        .foregroundStyle(StudioPalette.ink)

                    StudioStatusBadge(
                        text: granted ? "已授权" : "未授权",
                        tone: granted ? .positive : .warning
                    )
                }

                Text(detail)
                    .font(.system(size: 11))
                    .foregroundStyle(StudioPalette.inkMuted)
            }

            Spacer(minLength: 10)

            Button(actionTitle, action: action)
                .buttonStyle(StudioSecondaryButtonStyle())
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 9)
    }

    // MARK: - Diagnostics Page

    private var diagnosticsContent: some View {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 4) {
                Text("DIAGNOSTICS // 连接与编码摘要")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .tracking(1.4)
                    .foregroundStyle(StudioPalette.crimson)

                Text("运行诊断日志")
                    .font(.system(size: 20, weight: .light))
                    .foregroundStyle(StudioPalette.ink)
            }

            HStack(spacing: 10) {
                Button(model.copiedDiagnostics ? "✓ 已复制到剪贴板" : "复制诊断信息") {
                    copy()
                    model.copiedDiagnostics = true
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.8) {
                        model.copiedDiagnostics = false
                    }
                }
                .buttonStyle(StudioPrimaryButtonStyle())

                if state.canRetry {
                    Button("重新建立连接") { retry() }
                        .buttonStyle(StudioSecondaryButtonStyle())
                }

                Spacer()
            }

            VStack(alignment: .leading, spacing: 8) {
                StudioSectionHeader(english: "LOG OUTPUT", chinese: "状态快照")

                ScrollView(.vertical, showsIndicators: true) {
                    Text(diagnosticText)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(StudioPalette.inkSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(14)
                        .textSelection(.enabled)
                }
                .frame(minHeight: 230, maxHeight: 310)
                .background(StudioPalette.subtleSurface)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.cardRadius, style: .continuous)
                        .stroke(StudioPalette.hairline, lineWidth: 0.75)
                )
            }
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
            Text("USB-C AOA 直连  //  \(state.autoVirtualDisplay ? "自动 HiDPI 虚拟屏" : state.source)")
                .font(.system(size: 10.5, design: .monospaced))
                .foregroundStyle(StudioPalette.inkMuted)
                .lineLimit(1)

            Spacer(minLength: 10)

            if !state.streamSummary.isEmpty {
                Text(state.streamSummary)
                    .font(.system(size: 10.5, design: .monospaced))
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
                Text(model.copiedDiagnostics ? "✓ 已复制" : "复制摘要")
                    .font(.system(size: 10.5, weight: .medium))
                    .foregroundStyle(model.copiedDiagnostics ? StudioPalette.crimson : StudioPalette.inkSecondary)
            }
            .buttonStyle(StudioFlatActionStyle())
        }
        .padding(.horizontal, 24)
        .frame(height: 30)
        .background(StudioPalette.cardSurface.opacity(0.85))
    }
}
