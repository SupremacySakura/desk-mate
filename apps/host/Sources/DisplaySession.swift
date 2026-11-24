import CoreGraphics
import Foundation

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
        refreshRate: Double
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
                "\(message). This macOS build publishes only 1x modes for CGVirtualDisplay; use --display-id with an external HiDPI virtual screen."
            )
        }
        let displayID = displayNumber.uint32Value
        guard let info = DisplayInventory.info(for: displayID) else {
            _ = session.destroyAndWait()
            throw DisplaySessionError.unavailable("created display \(displayID) is not online")
        }
        return DisplaySession(
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
