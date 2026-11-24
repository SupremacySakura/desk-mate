import AppKit
import SwiftUI

struct DisplayChoice: Identifiable, Equatable {
    let id: UInt32
    let title: String
}

struct HostWindowState: Equatable {
    var title = "等待平板连接"
    var guidance = "打开平板上的 DeskMate，再接上 USB 数据线。"
    var badge = "未连接"
    var linked = false
    var source = "自动创建虚拟显示器"
    var streamSummary = ""
    var video = "等待连接"
    var input = "未开启"
    var screenPermission = false
    var inputPermission = false
    var videoEnabled = true
    var inputEnabled = false
    var autoVirtualDisplay = true
    var position: DisplayPosition = .right
    var liveFps = 0
    var liveBitrateMbps = 0.0
    var inputEventCount = 0
    var choices: [DisplayChoice] = []
    var selected: UInt32?
    var fixedSource = false
    var needsRestart = false
    var canRetry = false
    var diagnostic = ""
}

final class HostWindowModel: ObservableObject {
    @Published var state = HostWindowState()
    @Published var page: HostWindow.Page = .display
    @Published var showDiagnostics = false
    @Published var copiedDiagnostics = false
}

struct HostWindow: View {
    enum Page: String, CaseIterable {
        case display = "扩展概览"
        case settings = "显示设置"
        case diagnostics = "连接诊断"

        var englishTag: String {
            switch self {
            case .display: return "DISPLAY"
            case .settings: return "SETTINGS"
            case .diagnostics: return "DIAGNOSTICS"
            }
        }
    }

    @ObservedObject var model: HostWindowModel
    let chooseSource: (UInt32) -> Void
    let setAutoVirtual: (Bool) -> Void
    let setPosition: (DisplayPosition) -> Void
    let setVideo: (Bool) -> Void
    let setInput: (Bool) -> Void
    let openPermission: (Bool) -> Void
    let retry: () -> Void
    let copy: () -> Void

    private var state: HostWindowState { model.state }

    private var streaming: Bool {
        state.badge == "会话已建立" || state.liveFps > 0
    }

    private var badgeTone: StudioTone {
        switch state.badge {
        case "会话已建立":
            return .crimson
        case "出错":
            return .warning
        case "待授权", "未就绪":
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

            VStack(spacing: 0) {
                topBar

                Rectangle()
                    .fill(StudioPalette.hairline)
                    .frame(height: 0.75)

                HStack(alignment: .top, spacing: 24) {
                    leftTopologyColumn
                        .frame(width: 336)

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
        .frame(minWidth: 760, minHeight: 500)
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

            HStack(spacing: 20) {
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
                linked: state.linked,
                streaming: streaming,
                position: state.position,
                autoVirtualDisplay: state.autoVirtualDisplay,
                sourceLabel: state.source,
                streamSummary: state.streamSummary,
                liveFps: state.liveFps,
                liveBitrateMbps: state.liveBitrateMbps,
                onSelectPosition: setPosition
            )

            // Compact runtime summary strip below topology schematic
            VStack(spacing: 0) {
                metaRow(label: "USB 通道", value: state.linked ? "AOA 直连已就绪" : "等待平板接入", highlight: state.linked)
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
            VStack(alignment: .leading, spacing: 20) {
                switch model.page {
                case .display:
                    displayOverviewContent
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
                        subtitle: "允许通过平板触摸、双指滚动操作 Mac 副屏",
                        isOn: state.inputEnabled,
                        action: { setInput(!state.inputEnabled) }
                    )

                    Rectangle().fill(StudioPalette.softHairline).frame(height: 0.75)

                    VStack(alignment: .leading, spacing: 7) {
                        HStack(spacing: 8) {
                            Text("虚拟副屏方位")
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
                                    action: { setPosition(pos) }
                                )
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
        }
    }

    @ViewBuilder
    private var actionCalloutIfNeeded: some View {
        let missingScreen = !state.screenPermission
        let missingInput = state.inputEnabled && !state.inputPermission

        if missingScreen || missingInput || state.canRetry {
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
                    } else if missingInput {
                        Button("打开辅助功能") { openPermission(false) }
                            .buttonStyle(StudioPrimaryButtonStyle())
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
            }

            Spacer(minLength: 10)

            StudioHairlineToggle(isOn: isOn, action: action)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 9)
    }

    // MARK: - Settings Page

    private var settingsContent: some View {
        VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 4) {
                Text("SETTINGS // 显示与控制配置")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .tracking(1.4)
                    .foregroundStyle(StudioPalette.crimson)

                Text("显示与权限设置")
                    .font(.system(size: 20, weight: .light))
                    .foregroundStyle(StudioPalette.ink)
            }

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
                                    action: { setPosition(pos) }
                                )
                            }
                        }
                    }
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)

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

                    permissionRow(
                        title: "辅助功能",
                        granted: state.inputPermission,
                        detail: state.inputPermission
                            ? "已授予输入事件注入权限"
                            : "开启平板触控回传时需授予该权限",
                        actionTitle: "前往系统设置",
                        action: { openPermission(false) }
                    )
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
        linked: \(state.linked)
        source: \(state.source)
        auto_virtual_display: \(state.autoVirtualDisplay)
        position: \(state.position.shortLabel)
        video: \(state.video) (enabled=\(state.videoEnabled))
        input: \(state.input) (enabled=\(state.inputEnabled), events=\(state.inputEventCount))
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
