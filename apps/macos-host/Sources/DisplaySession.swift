import AppKit
import CoreGraphics
import Foundation

enum DisplayPosition: String, CaseIterable, Identifiable, Codable {
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

    static func snapshotTopologyDisplays(
        sessions: [ActiveDisplaySessionState]
    ) -> [TopologyDisplayItem] {
        let mainID = CGMainDisplayID()
        let activeIDs = onlineDisplayIDs().filter { id in
            CGDisplayIsActive(id) != 0 &&
            CGDisplayIsInMirrorSet(id) == 0 &&
            CGDisplayMirrorsDisplay(id) == kCGNullDirectDisplay
        }
        let sessionsByDisplayID: [UInt32: ActiveDisplaySessionState] = Dictionary(
            uniqueKeysWithValues: sessions.compactMap { s in
                guard let did = s.displayID else { return nil }
                return (did, s)
            }
        )
        let screenNamesByID: [UInt32: String] = Dictionary(
            uniqueKeysWithValues: NSScreen.screens.compactMap { screen in
                guard let num = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else {
                    return nil
                }
                return (num.uint32Value, screen.localizedName)
            }
        )

        var rawItems: [TopologyDisplayItem] = []
        for id in activeIDs {
            let bounds = CGDisplayBounds(id)
            guard bounds.width > 0, bounds.height > 0 else { continue }
            let mode = CGDisplayCopyDisplayMode(id)
            let isPrimary = (id == mainID) || (CGDisplayIsMain(id) != 0)
            let matched = sessionsByDisplayID[id]
            let isDeskMate = (matched != nil)
            let fallbackName = isPrimary ? "Mac 主屏" : (screenNamesByID[id] ?? "显示器 \(id)")
            let title = matched?.deviceName ?? fallbackName
            let isVirtualFlag = isDeskMate || (CGDisplayVendorNumber(id) == 0x0D53)

            rawItems.append(TopologyDisplayItem(
                id: id,
                indexNumber: 1,
                name: title,
                x: Double(bounds.origin.x),
                y: Double(bounds.origin.y),
                width: Double(bounds.width),
                height: Double(bounds.height),
                pixelWidth: mode?.pixelWidth ?? Int(bounds.width),
                pixelHeight: mode?.pixelHeight ?? Int(bounds.height),
                refreshHz: mode?.refreshRate ?? 60.0,
                isPrimary: isPrimary,
                isVirtual: isVirtualFlag,
                isDeskMateVirtual: isDeskMate,
                isGhostPreview: false,
                linkID: matched?.linkID,
                streaming: matched?.streaming ?? false,
                liveFps: matched?.liveFps ?? 0
            ))
        }

        // Deterministic numbering: primary = 1, others sorted left-to-right then top-to-bottom.
        let sorted = rawItems.sorted { lhs, rhs in
            if lhs.isPrimary != rhs.isPrimary { return lhs.isPrimary }
            if abs(lhs.x - rhs.x) > 8.0 { return lhs.x < rhs.x }
            if abs(lhs.y - rhs.y) > 8.0 { return lhs.y < rhs.y }
            return lhs.id < rhs.id
        }
        return sorted.enumerated().map { idx, item in
            var copy = item
            copy.indexNumber = idx + 1
            return copy
        }
    }

    @discardableResult
    static func applyDisplayLayout(_ origins: [(displayID: UInt32, origin: CGPoint)]) -> Bool {
        guard !origins.isEmpty else { return false }
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success, let config else {
            return false
        }
        for item in origins {
            let err = CGConfigureDisplayOrigin(
                config,
                item.displayID,
                Int32(item.origin.x.rounded()),
                Int32(item.origin.y.rounded())
            )
            if err != .success {
                CGCancelDisplayConfiguration(config)
                return false
            }
        }
        return CGCompleteDisplayConfiguration(config, .forSession) == .success
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
        position: DisplayPosition = .right,
        displayName: String = "DeskMate Extended Display",
        serialNum: UInt32 = 0x0001
    ) throws -> DisplaySession {
        let result = DMVirtualDisplaySession.create(
            withLogicalWidth: UInt32(logicalWidth),
            logicalHeight: UInt32(logicalHeight),
            scale: UInt32(scale),
            refreshRate: refreshRate,
            name: displayName,
            serialNum: serialNum
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
        position: DisplayPosition,
        displayName: String = "DeskMate Extended Display",
        serialNum: UInt32 = 0x0001
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
                position: position,
                displayName: displayName,
                serialNum: serialNum
            )
        } catch {
            if scale == 2 {
                // Graceful fallback to 1x if this macOS build cannot publish 2x for an unusual size.
                return try create(
                    logicalWidth: pixelWidth,
                    logicalHeight: pixelHeight,
                    scale: 1,
                    refreshRate: refreshRate,
                    position: position,
                    displayName: displayName,
                    serialNum: serialNum
                )
            }
            throw error
        }
    }

    static func stableSerialNumber(for deviceID: String, avoiding existing: Set<UInt32>) -> UInt32 {
        var hash: UInt32 = 2166136261
        for byte in deviceID.utf8 {
            hash ^= UInt32(byte)
            hash &*= 16777619
        }
        var candidate = hash == 0 ? 1 : hash
        while existing.contains(candidate) || candidate == 0 {
            candidate &+= 1
        }
        return candidate
    }

    static func arrangeDisplays(_ entries: [(session: DisplaySession, position: DisplayPosition)]) {
        let mainID = CGMainDisplayID()
        let mainBounds = CGDisplayBounds(mainID)
        guard !entries.isEmpty, mainBounds.width > 0, mainBounds.height > 0 else { return }

        let managedIDs = Set(entries.map { $0.session.info.displayID })
        var fixedBounds = mainBounds
        for id in DisplayInventory.onlineDisplayIDs() where !managedIDs.contains(id) {
            guard CGDisplayIsActive(id) != 0,
                  CGDisplayIsInMirrorSet(id) == 0,
                  CGDisplayMirrorsDisplay(id) == kCGNullDirectDisplay else { continue }
            let b = CGDisplayBounds(id)
            if b.width > 0, b.height > 0 {
                fixedBounds = fixedBounds.union(b)
            }
        }

        var origins: [(displayID: UInt32, origin: CGPoint)] = []

        let rightEntries = entries.filter { $0.position == .right }
        var cursorX = fixedBounds.maxX
        for entry in rightEntries {
            let bounds = CGDisplayBounds(entry.session.info.displayID)
            guard bounds.width > 0, bounds.height > 0 else { continue }
            let originY = mainBounds.origin.y + (mainBounds.height - bounds.height) / 2.0
            origins.append((entry.session.info.displayID, CGPoint(x: cursorX, y: originY)))
            cursorX += bounds.width
        }

        let leftEntries = entries.filter { $0.position == .left }
        var leftEdgeX = fixedBounds.minX
        for entry in leftEntries {
            let bounds = CGDisplayBounds(entry.session.info.displayID)
            guard bounds.width > 0, bounds.height > 0 else { continue }
            let originX = leftEdgeX - bounds.width
            let originY = mainBounds.origin.y + (mainBounds.height - bounds.height) / 2.0
            origins.append((entry.session.info.displayID, CGPoint(x: originX, y: originY)))
            leftEdgeX = originX
        }

        let topEntries = entries.filter { $0.position == .top }
        if !topEntries.isEmpty {
            let boundsList = topEntries.map { CGDisplayBounds($0.session.info.displayID) }
            let totalWidth = boundsList.reduce(CGFloat(0)) { $0 + $1.width }
            var currentX = fixedBounds.midX - totalWidth / 2.0
            for (entry, bounds) in zip(topEntries, boundsList) where bounds.width > 0 && bounds.height > 0 {
                let originY = fixedBounds.minY - bounds.height
                origins.append((entry.session.info.displayID, CGPoint(x: currentX, y: originY)))
                currentX += bounds.width
            }
        }

        let bottomEntries = entries.filter { $0.position == .bottom }
        if !bottomEntries.isEmpty {
            let boundsList = bottomEntries.map { CGDisplayBounds($0.session.info.displayID) }
            let totalWidth = boundsList.reduce(CGFloat(0)) { $0 + $1.width }
            var currentX = fixedBounds.midX - totalWidth / 2.0
            for (entry, bounds) in zip(bottomEntries, boundsList) where bounds.width > 0 && bounds.height > 0 {
                let originY = fixedBounds.maxY
                origins.append((entry.session.info.displayID, CGPoint(x: currentX, y: originY)))
                currentX += bounds.width
            }
        }

        guard !origins.isEmpty else { return }
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success, let config else { return }
        for item in origins where item.displayID != mainID {
            let err = CGConfigureDisplayOrigin(
                config,
                item.displayID,
                Int32(item.origin.x.rounded()),
                Int32(item.origin.y.rounded())
            )
            if err != .success {
                CGCancelDisplayConfiguration(config)
                return
            }
        }
        _ = CGCompleteDisplayConfiguration(config, .forSession)
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

/// Serializes virtual display creation, destruction, and CoreGraphics origin arrangement
/// so concurrent device connections never race SkyLight display transactions.
final class DisplayConfigCoordinator {
    static let shared = DisplayConfigCoordinator()
    private let queue = DispatchQueue(label: "dev.deskmate.host.display-config")

    func run<T>(_ operation: @escaping () throws -> T) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            queue.async {
                do {
                    continuation.resume(returning: try operation())
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    func run(_ operation: @escaping () -> Void) async {
        await withCheckedContinuation { continuation in
            queue.async {
                operation()
                continuation.resume()
            }
        }
    }
}
