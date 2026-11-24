import CoreGraphics
import Foundation

enum DisplayPosition: String, CaseIterable, Identifiable {
    case right
    case left
    case top
    case bottom

    var id: String { rawValue }

    var label: String {
        switch self {
        case .right: return "主屏右侧"
        case .left: return "主屏左侧"
        case .top: return "主屏上方"
        case .bottom: return "主屏下方"
        }
    }

    var shortLabel: String {
        switch self {
        case .right: return "右侧"
        case .left: return "左侧"
        case .top: return "上方"
        case .bottom: return "下方"
        }
    }
}

struct DisplayInfo {
    let displayID: UInt32
    let pointWidth: Int
    let pointHeight: Int
    let pixelWidth: Int
    let pixelHeight: Int
    let refreshRate: Double
    let createdByUs: Bool

    var summary: String {
        String(
            format: "%zux%zu pt / %zux%zu px @ %.2f Hz",
            pointWidth, pointHeight, pixelWidth, pixelHeight, refreshRate
        )
    }
}

enum DisplaySessionError: Error, CustomStringConvertible {
    case unavailable(String)
    case notExtended(String)

    var description: String {
        switch self {
        case .unavailable(let message): return message
        case .notExtended(let message): return message
        }
    }
}

enum DisplayInventory {
    static func onlineDisplayIDs() -> [UInt32] {
        var count: UInt32 = 0
        guard CGGetOnlineDisplayList(0, nil, &count) == .success, count > 0 else {
            return []
        }
        var ids = [UInt32](repeating: 0, count: Int(count))
        guard CGGetOnlineDisplayList(count, &ids, &count) == .success else {
            return []
        }
        return Array(ids.prefix(Int(count)))
    }

    static func isOnline(_ displayID: UInt32) -> Bool {
        onlineDisplayIDs().contains(displayID)
    }

    static func info(for displayID: UInt32) -> DisplayInfo? {
        guard isOnline(displayID), let mode = CGDisplayCopyDisplayMode(displayID) else {
            return nil
        }
        return DisplayInfo(
            displayID: displayID,
            pointWidth: mode.width,
            pointHeight: mode.height,
            pixelWidth: mode.pixelWidth,
            pixelHeight: mode.pixelHeight,
            refreshRate: mode.refreshRate,
            createdByUs: false
        )
    }

    static func describeAll() -> [String] {
        onlineDisplayIDs().map { id in
            guard let mode = CGDisplayCopyDisplayMode(id) else {
                return "- id \(id): mode unavailable"
            }
            return String(
                format: "- id %u: %zux%zu pt / %zux%zu px @ %.2f Hz, main %@, mirror %@",
                id,
                mode.width,
                mode.height,
                mode.pixelWidth,
                mode.pixelHeight,
                mode.refreshRate,
                CGDisplayIsMain(id) != 0 ? "true" : "false",
                (CGDisplayIsInMirrorSet(id) != 0 ||
                    CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay) ? "true" : "false"
            )
        }
    }
}

final class DisplaySession {
    let info: DisplayInfo
    private let virtualSession: DMVirtualDisplaySession?

    private init(info: DisplayInfo, virtualSession: DMVirtualDisplaySession?) {
        self.info = info
        self.virtualSession = virtualSession
    }

    deinit {
        destroy()
    }

    static func attach(displayID: UInt32) throws -> DisplaySession {
        guard let info = DisplayInventory.info(for: displayID) else {
            throw DisplaySessionError.unavailable(
                "display \(displayID) is not online; create a virtual screen first (BetterDisplay id 41 by default)"
            )
        }
        guard CGDisplayIsMain(displayID) == 0 else {
            throw DisplaySessionError.notExtended(
                "display \(displayID) is the main display; an Extended Display is required"
            )
        }
        guard CGDisplayIsInMirrorSet(displayID) == 0,
              CGDisplayMirrorsDisplay(displayID) == kCGNullDirectDisplay else {
            throw DisplaySessionError.notExtended(
                "display \(displayID) is mirroring another display; an Extended Display is required"
            )
        }
        return DisplaySession(info: info, virtualSession: nil)
    }

    static func create(
        logicalWidth: Int,
        logicalHeight: Int,
        scale: Int,
        refreshRate: Double,
        position: DisplayPosition = .right
    ) throws -> DisplaySession {
        let result = DMVirtualDisplaySession.create(
            withLogicalWidth: UInt32(logicalWidth),
            logicalHeight: UInt32(logicalHeight),
            scale: UInt32(scale),
            refreshRate: refreshRate
        )
        guard (result["ok"] as? Bool) == true,
              let session = result["session"] as? DMVirtualDisplaySession,
              let displayNumber = result["displayID"] as? NSNumber else {
            throw DisplaySessionError.unavailable(
                result["error"] as? String ?? "CGVirtualDisplay creation failed"
            )
        }
        let applied = session.applyAndSelectHiDPIMode()
        guard (applied["ok"] as? Bool) == true else {
            let message = applied["error"] as? String ?? "mode selection failed"
            _ = session.destroyAndWait()
            throw DisplaySessionError.unavailable(
                "\(message). Failed to apply HiDPI mode on CGVirtualDisplay."
            )
        }
        let displayID = displayNumber.uint32Value
        guard let info = DisplayInventory.info(for: displayID) else {
            _ = session.destroyAndWait()
            throw DisplaySessionError.unavailable("created display \(displayID) is not online")
        }
        let created = DisplaySession(
            info: DisplayInfo(
                displayID: info.displayID,
                pointWidth: info.pointWidth,
                pointHeight: info.pointHeight,
                pixelWidth: info.pixelWidth,
                pixelHeight: info.pixelHeight,
                refreshRate: info.refreshRate,
                createdByUs: true
            ),
            virtualSession: session
        )
        created.applyPosition(position)
        return created
    }

    static func createMatchingDevice(
        pixelWidth: Int,
        pixelHeight: Int,
        refreshRate: Double,
        position: DisplayPosition
    ) throws -> DisplaySession {
        let useHiDPI = (pixelWidth % 2 == 0) && (pixelHeight % 2 == 0) && pixelWidth >= 1600
        let scale = useHiDPI ? 2 : 1
        let logicalWidth = max(640, pixelWidth / scale)
        let logicalHeight = max(480, pixelHeight / scale)
        do {
            return try create(
                logicalWidth: logicalWidth,
                logicalHeight: logicalHeight,
                scale: scale,
                refreshRate: refreshRate,
                position: position
            )
        } catch {
            if scale == 2 {
                // Graceful fallback to 1x if this macOS build cannot publish 2x for an unusual size.
                return try create(
                    logicalWidth: pixelWidth,
                    logicalHeight: pixelHeight,
                    scale: 1,
                    refreshRate: refreshRate,
                    position: position
                )
            }
            throw error
        }
    }

    func applyPosition(_ position: DisplayPosition) {
        let mainID = CGMainDisplayID()
        guard info.displayID != mainID else { return }
        let mainBounds = CGDisplayBounds(mainID)
        let extBounds = CGDisplayBounds(info.displayID)
        guard extBounds.width > 0, extBounds.height > 0 else { return }

        let originX: CGFloat
        let originY: CGFloat
        switch position {
        case .right:
            originX = mainBounds.maxX
            originY = mainBounds.origin.y + (mainBounds.height - extBounds.height) / 2.0
        case .left:
            originX = mainBounds.minX - extBounds.width
            originY = mainBounds.origin.y + (mainBounds.height - extBounds.height) / 2.0
        case .top:
            originX = mainBounds.origin.x + (mainBounds.width - extBounds.width) / 2.0
            originY = mainBounds.minY - extBounds.height
        case .bottom:
            originX = mainBounds.origin.x + (mainBounds.width - extBounds.width) / 2.0
            originY = mainBounds.maxY
        }

        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success, let config else { return }
        let err = CGConfigureDisplayOrigin(
            config,
            info.displayID,
            Int32(originX.rounded()),
            Int32(originY.rounded())
        )
        if err == .success {
            _ = CGCompleteDisplayConfiguration(config, .forSession)
        } else {
            CGCancelDisplayConfiguration(config)
        }
    }

    /// Returns the display bounds in global CoreGraphics coordinates, used to map
    /// normalized input back onto the correct screen.
    func bounds() -> CGRect {
        CGDisplayBounds(info.displayID)
    }

    func destroy() {
        guard let virtualSession else { return }
        _ = virtualSession.destroyAndWait()
    }
}
