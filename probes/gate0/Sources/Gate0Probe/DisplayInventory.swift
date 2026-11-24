import CoreGraphics
import Foundation

struct DisplayInventoryEntry: Codable {
    let id: UInt32
    let isMain: Bool
    let isInMirrorSet: Bool
    let mirrorsDisplayID: UInt32?
    let bounds: String
    let mode: String
    let pointWidth: Int
    let pointHeight: Int
    let pixelWidth: Int
    let pixelHeight: Int
    let refreshRate: Double
}

struct DisplayInventoryReport: Codable {
    let status: String
    let liveActionsExecuted: Bool
    let mainDisplayID: UInt32
    let displays: [DisplayInventoryEntry]
    let note: String
}

enum DisplayInventory {
    static func probe() -> DisplayInventoryReport {
        var count: UInt32 = 0
        CGGetOnlineDisplayList(0, nil, &count)
        var identifiers = [CGDirectDisplayID](repeating: 0, count: Int(count))
        var actualCount: UInt32 = 0
        let result = identifiers.withUnsafeMutableBufferPointer { buffer in
            CGGetOnlineDisplayList(count, buffer.baseAddress, &actualCount)
        }
        guard result == .success else {
            return DisplayInventoryReport(
                status: "failed",
                liveActionsExecuted: false,
                mainDisplayID: CGMainDisplayID(),
                displays: [],
                note: "CGGetOnlineDisplayList failed with CGError \(result.rawValue)"
            )
        }
        identifiers = Array(identifiers.prefix(Int(actualCount)))

        let displays = identifiers.map { displayID -> DisplayInventoryEntry in
            let mode = CGDisplayCopyDisplayMode(displayID)
            let mirrored = CGDisplayMirrorsDisplay(displayID)
            let refreshRate = mode?.refreshRate ?? 0
            let bounds = CGDisplayBounds(displayID)
            let entry = DisplayInventoryEntry(
                id: displayID,
                isMain: CGDisplayIsMain(displayID) != 0,
                isInMirrorSet: CGDisplayIsInMirrorSet(displayID) != 0,
                mirrorsDisplayID: mirrored == 0 ? nil : mirrored,
                bounds: String(format: "%.0f,%.0f %.0fx%.0f",
                               bounds.origin.x, bounds.origin.y, bounds.width, bounds.height),
                mode: mode.map(modeSummary) ?? "unavailable",
                pointWidth: mode?.width ?? 0,
                pointHeight: mode?.height ?? 0,
                pixelWidth: mode?.pixelWidth ?? 0,
                pixelHeight: mode?.pixelHeight ?? 0,
                refreshRate: refreshRate
            )
            return entry
        }

        let mainDisplayID = CGMainDisplayID()
        let unavailable = displays.isEmpty || mainDisplayID == 0
        return DisplayInventoryReport(
            status: unavailable ? "unavailable" : "ok",
            liveActionsExecuted: false,
            mainDisplayID: mainDisplayID,
            displays: displays,
            note: unavailable
                ? "CoreGraphics returned no online display or a null main display. This process likely lacks WindowServer access; run the inventory from the Reference Host's logged-in desktop session."
                : "Read-only CoreGraphics display inventory. Use it after lock/sleep to check that a probe display did not remain online."
        )
    }

    static func printHuman(_ report: DisplayInventoryReport) {
        print("Online displays: \(report.displays.count) (main \(report.mainDisplayID))")
        for display in report.displays {
            print("- id \(display.id): \(display.mode), bounds \(display.bounds), main \(display.isMain), mirror \(display.isInMirrorSet)")
        }
        print(report.note)
    }

    private static func modeSummary(_ mode: CGDisplayMode) -> String {
        String(
            format: "%zux%zu pt / %zux%zu px @ %.2f Hz",
            mode.width,
            mode.height,
            mode.pixelWidth,
            mode.pixelHeight,
            mode.refreshRate
        )
    }
}
