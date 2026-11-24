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
    var source = "未选择"
    var video = "等待连接"
    var input = "未开启"
    var screenPermission = false
    var inputPermission = false
    var videoEnabled = true
    var inputEnabled = false
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
}

struct HostWindow: View {
    enum Page: String, CaseIterable {
        case display = "扩展显示"
        case settings = "设置"

        var symbol: String { self == .display ? "display" : "slider.horizontal.3" }
    }

    @ObservedObject var model: HostWindowModel
    let chooseSource: (UInt32?) -> Void
    let setVideo: (Bool) -> Void
    let setInput: (Bool) -> Void
    let openPermission: (Bool) -> Void
    let retry: () -> Void
    let copy: () -> Void
    @Environment(\.colorScheme) private var scheme

    private var state: HostWindowState { model.state }
    private var page: Page { model.page }
    private var accent: Color { scheme == .dark ? Color(red: 0.54, green: 0.70, blue: 1) : Color(red: 0.13, green: 0.32, blue: 0.75) }
    private var rail: Color { scheme == .dark ? Color(red: 0.15, green: 0.18, blue: 0.23) : Color(red: 0.93, green: 0.95, blue: 0.98) }

    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 6) {
                Label("DeskMate", systemImage: "display.2")
                    .font(.headline)
                    .padding(.horizontal, 10)
                    .padding(.bottom, 12)
                ForEach(Page.allCases, id: \.self) { item in
                    Button { model.page = item } label: {
                        Label(item.rawValue, systemImage: item.symbol)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.horizontal, 10)
                            .frame(height: 34)
                            .foregroundStyle(page == item ? accent : Color.secondary)
                            .background(page == item ? accent.opacity(0.12) : .clear, in: RoundedRectangle(cornerRadius: 7))
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(page == item ? .isSelected : [])
                }
                Spacer()
                Text("USB 有线连接").font(.caption).foregroundStyle(.secondary).padding(.horizontal, 10)
            }
            .padding(12)
            .padding(.top, 26)
            .frame(width: 158)
            .background(rail)
            Divider()
            VStack(alignment: .leading, spacing: 0) {
                HStack {
                    Text(page.rawValue).font(.headline)
                    Spacer()
                    if page == .display {
                        Label(state.badge, systemImage: "circle.fill")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(state.linked ? Color.green : Color.secondary)
                    }
                }
                .frame(height: 52)
                Divider()
                ScrollView {
                    Group {
                        if page == .display { overview } else { settings }
                    }
                    .padding(.vertical, 18)
                }
            }
            .padding(.horizontal, 25)
            .background(Color(nsColor: .textBackgroundColor))
        }
        .frame(minWidth: 610, minHeight: 480)
        .tint(accent)
        .sheet(isPresented: $model.showDiagnostics) {
            VStack(alignment: .leading, spacing: 14) {
                Text("连接诊断").font(.title2.weight(.semibold))
                ScrollView {
                    Text(state.diagnostic)
                        .font(.system(.callout, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                HStack {
                    Button("复制状态", action: copy)
                    Spacer()
                    Button("完成") { model.showDiagnostics = false }.keyboardShortcut(.defaultAction)
                }
            }
            .padding(22)
            .frame(width: 530, height: 320)
        }
    }

    private var overview: some View {
        VStack(alignment: .leading, spacing: 13) {
            VStack(alignment: .leading, spacing: 5) {
                Text(state.title).font(.system(size: 26, weight: .semibold))
                Text(state.guidance).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            HStack(spacing: 16) {
                endpoint("Mac", symbol: "laptopcomputer")
                VStack(spacing: 6) {
                    Text("USB").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                    Path { path in
                        path.move(to: .init(x: 0, y: 1))
                        path.addLine(to: .init(x: 100, y: 1))
                    }
                    .stroke(state.linked ? accent : .secondary, style: StrokeStyle(lineWidth: 1.5, dash: state.linked ? [] : [4, 4]))
                    .frame(width: 100, height: 2)
                }
                endpoint("鸿蒙平板", symbol: "ipad.landscape")
            }
            .accessibilityHidden(true)
            .frame(maxWidth: .infinity, minHeight: 139)
            .background(rail, in: RoundedRectangle(cornerRadius: 12))
            VStack(spacing: 0) {
                row("显示来源") { Text(state.source).foregroundStyle(.secondary) }
                Divider()
                row("画面发送") {
                    Text(state.video).foregroundStyle(.secondary)
                    Toggle("发送画面", isOn: Binding(get: { state.videoEnabled }, set: setVideo))
                        .labelsHidden().toggleStyle(.switch).controlSize(.small)
                }
                Divider()
                row("反向输入") { Text(state.input).foregroundStyle(.secondary) }
            }
            HStack {
                Button("查看诊断") { model.showDiagnostics = true }.buttonStyle(.link)
                Spacer()
                if state.canRetry {
                    Button("重试", action: retry)
                }
                if !state.screenPermission {
                    Button("去授权") { openPermission(true) }
                } else if state.source == "未选择" {
                    Button("选择显示来源") { model.page = .settings }
                }
            }
        }
    }

    private var settings: some View {
        VStack(alignment: .leading, spacing: 15) {
            group("扩展显示") {
                row("显示来源") {
                    if state.fixedSource {
                        Text(state.source).foregroundStyle(.secondary)
                    } else {
                        Picker("显示来源", selection: Binding(get: { state.selected }, set: chooseSource)) {
                            Text("请选择扩展显示器").tag(nil as UInt32?).disabled(true)
                            ForEach(state.choices) { item in Text(item.title).tag(Optional(item.id)) }
                        }
                        .labelsHidden()
                        .fixedSize()
                    }
                }
                if state.needsRestart {
                    Text("已保存新来源，重新打开 DeskMate 后生效。").font(.caption).foregroundStyle(.secondary)
                } else if state.choices.isEmpty && !state.fixedSource {
                    Text("未找到可借用的扩展显示器。主屏与镜像屏不会出现在列表中。")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            group("系统权限") {
                row("屏幕录制") { permission(state.screenPermission, screen: true) }
                Divider()
                row("辅助功能") { permission(state.inputPermission, screen: false) }
            }
            group("反向输入") {
                row("允许平板操作 Mac") {
                    Toggle("允许平板操作 Mac", isOn: Binding(get: { state.inputEnabled }, set: setInput))
                        .labelsHidden().toggleStyle(.switch).controlSize(.small)
                        .disabled(!state.inputPermission)
                }
                Text("启用前需授权辅助功能。不授权也能发送画面。").font(.caption).foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func endpoint(_ title: String, symbol: String) -> some View {
        VStack(spacing: 8) {
            Image(systemName: symbol).font(.system(size: 52, weight: .ultraLight)).foregroundStyle(accent)
                .frame(height: 60)
            Text(title).font(.caption).foregroundStyle(.secondary)
        }
    }

    private func row<Content: View>(_ caption: String, @ViewBuilder content: () -> Content) -> some View {
        HStack(spacing: 9) {
            Text(caption)
            Spacer()
            content()
        }
        .frame(minHeight: 35)
    }

    private func group<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            content()
        }
    }

    @ViewBuilder private func permission(_ granted: Bool, screen: Bool) -> some View {
        if granted {
            Text("已允许").foregroundStyle(.secondary)
        } else {
            Button("打开系统设置") { openPermission(screen) }.buttonStyle(.link)
        }
    }
}
