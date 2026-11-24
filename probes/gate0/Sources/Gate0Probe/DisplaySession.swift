import Foundation
import CoreGraphics

enum DisplaySessionError: Error, CustomStringConvertible {
    case failed(DisplayRecord)

    var description: String {
        switch self {
        case .failed(let record):
            return record.failure ?? "virtual display attempt failed"
        }
    }
}

final class ActiveVirtualDisplay {
    let session: G0VirtualDisplaySession?
    let displayID: UInt32
    let initialModeSummaries: [String]
    let attachedExisting: Bool
    private(set) var record: DisplayRecord

    private init(
        session: G0VirtualDisplaySession?,
        displayID: UInt32,
        initialModeSummaries: [String],
        attachedExisting: Bool,
        record: DisplayRecord
    ) {
        self.session = session
        self.displayID = displayID
        self.initialModeSummaries = initialModeSummaries
        self.attachedExisting = attachedExisting
        self.record = record
    }

    static func create(options: ProbeOptions) throws -> ActiveVirtualDisplay {
        let creation = G0VirtualDisplaySession.create(
            withLogicalWidth: UInt32(options.logicalWidth),
            logicalHeight: UInt32(options.logicalHeight),
            scale: UInt32(options.scale),
            refreshRate: options.refreshRate
        )
        guard (creation["ok"] as? Bool) == true,
              let session = creation["session"] as? G0VirtualDisplaySession,
              let displayNumber = creation["displayID"] as? NSNumber else {
            let message = creation["error"] as? String ?? "unknown creation error"
            throw DisplaySessionError.failed(failedRecord(
                options: options,
                created: false,
                displayID: nil,
                destroyed: nil,
                failure: "virtual display creation failed: \(message)"
            ))
        }

        let displayID = displayNumber.uint32Value
        let summaries = creation["modeSummaries"] as? [String] ?? []
        let initialRecord = DisplayRecord(
            created: true,
            provider: "CGVirtualDisplay",
            attachedExisting: false,
            displayID: displayID,
            requestedLogicalWidth: options.logicalWidth,
            requestedLogicalHeight: options.logicalHeight,
            scale: options.scale,
            requestedPixelWidth: options.logicalWidth * options.scale,
            requestedPixelHeight: options.logicalHeight * options.scale,
            refreshRate: options.refreshRate,
            actualMode: nil,
            isMainDisplay: nil,
            isMirroring: nil,
            destroyed: nil,
            stillOnlineAfterRun: nil,
            failure: nil
        )

        let active = ActiveVirtualDisplay(
            session: session,
            displayID: displayID,
            initialModeSummaries: summaries,
            attachedExisting: false,
            record: initialRecord
        )

        let application = session.applyAndSelectHiDPIMode()
        guard (application["ok"] as? Bool) == true else {
            let message = application["error"] as? String ?? "unknown mode selection error"
            let cleanup = active.destroy()
            throw DisplaySessionError.failed(failedRecord(
                options: options,
                created: true,
                displayID: displayID,
                destroyed: cleanup.destroyed,
                failure: "virtual display verification failed: \(message); cleanup: \(cleanup.detail)"
            ))
        }

        active.record = DisplayRecord(
            created: true,
            provider: "CGVirtualDisplay",
            attachedExisting: false,
            displayID: displayID,
            requestedLogicalWidth: options.logicalWidth,
            requestedLogicalHeight: options.logicalHeight,
            scale: options.scale,
            requestedPixelWidth: options.logicalWidth * options.scale,
            requestedPixelHeight: options.logicalHeight * options.scale,
            refreshRate: options.refreshRate,
            actualMode: application["modeSummary"] as? String,
            isMainDisplay: (application["isMain"] as? NSNumber)?.boolValue,
            isMirroring: (application["isMirroring"] as? NSNumber)?.boolValue,
            destroyed: nil,
            stillOnlineAfterRun: nil,
            failure: nil
        )
        return active
    }

    // Attach mode is for hosts where CGVirtualDisplay no longer publishes a 2x
    // mode: an external tool (for example BetterDisplay) owns the display and
    // this probe only borrows it. The probe never destroys a display it did not
    // create.
    static func attach(options: ProbeOptions, displayID: UInt32) throws -> ActiveVirtualDisplay {
        guard let mode = CGDisplayCopyDisplayMode(displayID) else {
            throw DisplaySessionError.failed(attachedFailureRecord(
                options: options,
                displayID: displayID,
                failure: "display \(displayID) is not online; create the virtual screen before running the probe"
            ))
        }
        let pointWidth = mode.width
        let pointHeight = mode.height
        let pixelWidth = mode.pixelWidth
        let pixelHeight = mode.pixelHeight
        let refreshRate = mode.refreshRate
        let summary = String(
            format: "%zux%zu pt / %zux%zu px @ %.2f Hz",
            pointWidth, pointHeight, pixelWidth, pixelHeight, refreshRate
        )
        let isMain = CGDisplayIsMain(displayID) != 0
        let isMirroring = CGDisplayIsInMirrorSet(displayID) != 0 ||
            CGDisplayMirrorsDisplay(displayID) != kCGNullDirectDisplay

        let record = DisplayRecord(
            created: false,
            provider: "external-attached",
            attachedExisting: true,
            displayID: displayID,
            requestedLogicalWidth: options.logicalWidth,
            requestedLogicalHeight: options.logicalHeight,
            scale: options.scale,
            requestedPixelWidth: options.logicalWidth * options.scale,
            requestedPixelHeight: options.logicalHeight * options.scale,
            refreshRate: refreshRate,
            actualMode: summary,
            isMainDisplay: isMain,
            isMirroring: isMirroring,
            destroyed: nil,
            stillOnlineAfterRun: nil,
            failure: nil
        )

        if isMain {
            throw DisplaySessionError.failed(recordWithFailure(
                record, "attached display \(displayID) is the main display; Extended Display is required"
            ))
        }
        if isMirroring {
            throw DisplaySessionError.failed(recordWithFailure(
                record, "attached display \(displayID) is mirroring another display; Extended Display is required"
            ))
        }
        guard pointWidth > 0, pixelWidth == pointWidth * 2, pixelHeight == pointHeight * 2 else {
            throw DisplaySessionError.failed(recordWithFailure(
                record,
                "attached display \(displayID) is not 2x HiDPI (\(summary)); enable a HiDPI virtual screen in the external tool"
            ))
        }

        return ActiveVirtualDisplay(
            session: nil,
            displayID: displayID,
            initialModeSummaries: [summary],
            attachedExisting: true,
            record: record
        )
    }

    private static func recordWithFailure(_ record: DisplayRecord, _ failure: String) -> DisplayRecord {
        DisplayRecord(
            created: record.created,
            provider: record.provider,
            attachedExisting: record.attachedExisting,
            displayID: record.displayID,
            requestedLogicalWidth: record.requestedLogicalWidth,
            requestedLogicalHeight: record.requestedLogicalHeight,
            scale: record.scale,
            requestedPixelWidth: record.requestedPixelWidth,
            requestedPixelHeight: record.requestedPixelHeight,
            refreshRate: record.refreshRate,
            actualMode: record.actualMode,
            isMainDisplay: record.isMainDisplay,
            isMirroring: record.isMirroring,
            destroyed: record.destroyed,
            stillOnlineAfterRun: record.stillOnlineAfterRun,
            failure: failure
        )
    }

    private static func attachedFailureRecord(
        options: ProbeOptions,
        displayID: UInt32,
        failure: String
    ) -> DisplayRecord {
        DisplayRecord(
            created: false,
            provider: "external-attached",
            attachedExisting: true,
            displayID: displayID,
            requestedLogicalWidth: options.logicalWidth,
            requestedLogicalHeight: options.logicalHeight,
            scale: options.scale,
            requestedPixelWidth: options.logicalWidth * options.scale,
            requestedPixelHeight: options.logicalHeight * options.scale,
            refreshRate: options.refreshRate,
            actualMode: nil,
            isMainDisplay: nil,
            isMirroring: nil,
            destroyed: nil,
            stillOnlineAfterRun: nil,
            failure: failure
        )
    }

    private static func failedRecord(
        options: ProbeOptions,
        created: Bool,
        displayID: UInt32?,
        destroyed: Bool?,
        failure: String
    ) -> DisplayRecord {
        DisplayRecord(
            created: created,
            provider: "CGVirtualDisplay",
            attachedExisting: false,
            displayID: displayID,
            requestedLogicalWidth: options.logicalWidth,
            requestedLogicalHeight: options.logicalHeight,
            scale: options.scale,
            requestedPixelWidth: options.logicalWidth * options.scale,
            requestedPixelHeight: options.logicalHeight * options.scale,
            refreshRate: options.refreshRate,
            actualMode: nil,
            isMainDisplay: nil,
            isMirroring: nil,
            destroyed: destroyed,
            stillOnlineAfterRun: nil,
            failure: failure
        )
    }

    @discardableResult
    func destroy() -> (destroyed: Bool, detail: String) {
        if attachedExisting {
            let online = DisplayInventory.isOnline(displayID)
            record = DisplayRecord(
                created: record.created,
                provider: record.provider,
                attachedExisting: true,
                displayID: record.displayID,
                requestedLogicalWidth: record.requestedLogicalWidth,
                requestedLogicalHeight: record.requestedLogicalHeight,
                scale: record.scale,
                requestedPixelWidth: record.requestedPixelWidth,
                requestedPixelHeight: record.requestedPixelHeight,
                refreshRate: record.refreshRate,
                actualMode: record.actualMode,
                isMainDisplay: record.isMainDisplay,
                isMirroring: record.isMirroring,
                destroyed: nil,
                stillOnlineAfterRun: online,
                failure: online ? record.failure : "attached display went offline during the run"
            )
            return (
                true,
                online
                    ? "left the externally owned display online; the probe did not create or destroy it"
                    : "externally owned display went offline during the run"
            )
        }

        guard let session else {
            return (false, "no CGVirtualDisplay session to release")
        }
        let result = session.destroyAndWait()
        let destroyed = (result["destroyed"] as? Bool) == true
        let onlineAfter = (result["onlineAfter"] as? NSNumber)?.boolValue ?? false
        let error = result["error"] as? String
        let detail: String
        if destroyed {
            detail = "released CGVirtualDisplay and display ID is offline"
        } else if let error {
            detail = error
        } else {
            detail = "display remained online: \(onlineAfter)"
        }
        record = DisplayRecord(
            created: record.created,
            provider: record.provider,
            attachedExisting: record.attachedExisting,
            displayID: record.displayID,
            requestedLogicalWidth: record.requestedLogicalWidth,
            requestedLogicalHeight: record.requestedLogicalHeight,
            scale: record.scale,
            requestedPixelWidth: record.requestedPixelWidth,
            requestedPixelHeight: record.requestedPixelHeight,
            refreshRate: record.refreshRate,
            actualMode: record.actualMode,
            isMainDisplay: record.isMainDisplay,
            isMirroring: record.isMirroring,
            destroyed: destroyed,
            stillOnlineAfterRun: nil,
            failure: destroyed ? nil : detail
        )
        return (destroyed, detail)
    }
}
