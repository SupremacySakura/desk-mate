import Foundation

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
    let session: G0VirtualDisplaySession
    let displayID: UInt32
    let initialModeSummaries: [String]
    private(set) var record: DisplayRecord

    private init(
        session: G0VirtualDisplaySession,
        displayID: UInt32,
        initialModeSummaries: [String],
        record: DisplayRecord
    ) {
        self.session = session
        self.displayID = displayID
        self.initialModeSummaries = initialModeSummaries
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
            failure: nil
        )

        let active = ActiveVirtualDisplay(
            session: session,
            displayID: displayID,
            initialModeSummaries: summaries,
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
            failure: nil
        )
        return active
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
            failure: failure
        )
    }

    @discardableResult
    func destroy() -> (destroyed: Bool, detail: String) {
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
            failure: destroyed ? nil : detail
        )
        return (destroyed, detail)
    }
}
