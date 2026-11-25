import AppKit
import Foundation
import ServiceManagement

struct StartupStatus: Equatable {
    var launchAtLoginEnabled = false
    var needsApproval = false
    var statusDescription = "未开启。开启后登录 macOS 时将自动运行 DeskMate。"
    var errorMessage: String? = nil
}

final class StartupServiceManager {
    private var lastErrorMessage: String?

    func currentStatus() -> StartupStatus {
        let status = SMAppService.mainApp.status
        switch status {
        case .enabled:
            return StartupStatus(
                launchAtLoginEnabled: true,
                needsApproval: false,
                statusDescription: "已添加到 macOS 登录项，登录系统时将自动打开 DeskMate。",
                errorMessage: lastErrorMessage
            )
        case .requiresApproval:
            return StartupStatus(
                launchAtLoginEnabled: false,
                needsApproval: true,
                statusDescription: "已在系统注册，但需要在「系统设置 → 通用 → 登录项与扩展」中批准后生效。",
                errorMessage: lastErrorMessage
            )
        case .notFound:
            return StartupStatus(
                launchAtLoginEnabled: false,
                needsApproval: false,
                statusDescription: "macOS 尚未找到此登录项。可尝试开启；若失败，请查看下方的系统错误。",
                errorMessage: lastErrorMessage
            )
        case .notRegistered:
            return StartupStatus(
                launchAtLoginEnabled: false,
                needsApproval: false,
                statusDescription: "未开启。开启后登录 macOS 时将自动运行 DeskMate。",
                errorMessage: lastErrorMessage
            )
        @unknown default:
            return StartupStatus(
                launchAtLoginEnabled: false,
                needsApproval: false,
                statusDescription: "未知的登录项状态（\(status.rawValue)）。",
                errorMessage: lastErrorMessage
            )
        }
    }

    @discardableResult
    func setLaunchAtLogin(_ enabled: Bool) -> StartupStatus {
        lastErrorMessage = nil
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
        } catch {
            lastErrorMessage = error.localizedDescription
            NSLog("DeskMate: SMAppService %@ failed: %@", enabled ? "register" : "unregister", error.localizedDescription)
        }
        return currentStatus()
    }

    func openSystemLoginItemsSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }
}
