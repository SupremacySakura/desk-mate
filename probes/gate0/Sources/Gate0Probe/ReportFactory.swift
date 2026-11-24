import Foundation

enum ReportFactory {
    static func dryRun(options: ProbeOptions) -> Gate0Report {
        let environment = SystemProbe.environment()
        let privateAPI = PrivateDisplayAPI.probe()
        let videoToolbox = VideoToolboxProbe.probe()
        let signing = SystemProbe.signingSummary()
        let screenRecording = SystemProbe.screenRecordingPreflight()
        let reference = referenceEnvironment(options: options, environment: environment)

        let prerequisites = [
            PrerequisiteRecord(
                id: "reference-mac",
                state: reference.confirmed ? .passed : .pending,
                detail: reference.detail
            ),
            PrerequisiteRecord(
                id: "screen-recording-permission",
                state: screenRecording ? .passed : .pending,
                detail: screenRecording
                    ? "Screen Recording preflight passed; capture was still not started."
                    : "Screen Recording permission is not currently granted."
            ),
            PrerequisiteRecord(
                id: "developer-id",
                state: .pending,
                detail: signing.detail
            ),
            PrerequisiteRecord(
                id: "notarization-and-gatekeeper",
                state: .pending,
                detail: "No signed, notarized, stapled artifact was submitted to Gatekeeper."
            ),
            PrerequisiteRecord(
                id: "live-display-and-capture",
                state: .pending,
                detail: "Requires --live --allow-create-display --allow-screen-capture."
            ),
            PrerequisiteRecord(
                id: "lock-sleep-recovery",
                state: .pending,
                detail: "Requires an observed lock and sleep cycle on the Reference Mac."
            ),
        ]

        var notExecuted: [String] = []
        if !reference.confirmed {
            notExecuted.append("Confirm that this Mac model and macOS build are the Reference Environment.")
        }
        notExecuted.append("Create and verify a non-mirrored HiDPI Extended Display.")
        notExecuted.append(screenRecording
            ? "Start ScreenCaptureKit on the created Extended Display."
            : "Grant Screen Recording permission and start ScreenCaptureKit from that display."
        )
        notExecuted.append("Encode real-time HEVC and H.264 Annex-B samples with parameter sets and color metadata.")
        notExecuted.append("Exercise lock and sleep to prove capture teardown and display cleanup leave no ghost display.")
        notExecuted.append("Run Developer ID signing, notarization, stapling, and Gatekeeper assessment.")

        let reasonPrefix = reference.confirmed
            ? "Non-live probe on a matching Reference Host."
            : "Non-live probe; Reference Host identity is unconfirmed."

        return Gate0Report(
            schemaVersion: 1,
            probe: "deskmate-gate0",
            issue: ".scratch/deskmate/issues/05-gate0-host-display-capture-encode-distribution.md",
            generatedAt: isoTimestamp(),
            mode: .dryRun,
            decision: .pending,
            decisionReason: "\(reasonPrefix) Gate 0 requires Screen Recording permission, live display/capture evidence, lifecycle evidence, and Developer ID notarization evidence.",
            safety: SafetyRecord(
                liveActionsExecuted: false,
                displayCreationAuthorized: false,
                screenCaptureAuthorized: false,
                mainDisplayFallbackUsed: false,
                audioCaptureDisabledByDesign: true
            ),
            environment: environment,
            referenceEnvironment: reference,
            privateAPI: privateAPI,
            display: DisplayRecord(
                created: false,
                displayID: nil,
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
            ),
            capture: CaptureRecord(
                started: false,
                target: "created-extended-display-only",
                displayID: nil,
                width: nil,
                height: nil,
                frameRate: options.refreshRate,
                framesReceived: 0,
                audioEnabled: false,
                audioRequested: false,
                audioOutputAdded: false,
                failure: nil
            ),
            videoToolbox: videoToolbox,
            artifacts: [],
            artifactPlan: artifactPlans(for: options.codecs),
            lifecycle: lifecycleRecord(),
            signing: signing,
            prerequisites: prerequisites,
            notExecuted: notExecuted
        )
    }

    static func artifactPlans(for selection: CodecSelection) -> [ArtifactPlan] {
        let all = [
            ArtifactPlan(
                codec: "hevc",
                container: "annex-b",
                annexB: true,
                realTime: true,
                noBFrames: true,
                parameterSets: ["VPS", "SPS", "PPS"],
                colorMetadata: ["colorPrimaries", "transferFunction", "yCbCrMatrix"]
            ),
            ArtifactPlan(
                codec: "h264",
                container: "annex-b",
                annexB: true,
                realTime: true,
                noBFrames: true,
                parameterSets: ["SPS", "PPS"],
                colorMetadata: ["colorPrimaries", "transferFunction", "yCbCrMatrix"]
            ),
        ]

        switch selection {
        case .both:
            return all
        case .hevc:
            return all.filter { $0.codec == "hevc" }
        case .h264:
            return all.filter { $0.codec == "h264" }
        }
    }

    static func lifecycleRecord() -> LifecycleRecord {
        LifecycleRecord(
            hooksImplemented: true,
            hooksRegistered: false,
            eventsObserved: [],
            behavior: "Live mode will register distributed lock/unlock and NSWorkspace sleep/wake observers, stop capture, invalidate encoders, and release the virtual display. The probe exits after a lock or sleep event. Relaunch manually after unlock/wake and verify no ghost display remains.",
            manualSteps: [
                "Start a live probe with a duration longer than the manual test.",
                "Lock the Mac and confirm the report contains screenIsLocked and cleanup completed.",
                "Unlock the Mac, rerun the live probe, and confirm exactly one new Extended Display appears.",
                "Put the Mac to sleep and wake it, then confirm the previous Extended Display disappeared.",
                "Repeat with System Settings > Displays open and verify no stale DeskMate display remains.",
            ]
        )
    }

    static func referenceEnvironment(options: ProbeOptions, environment: EnvironmentRecord) -> ReferenceEnvironmentRecord {
        guard let expectedModel = options.referenceModel, !expectedModel.isEmpty,
              let expectedBuild = options.referenceBuild, !expectedBuild.isEmpty else {
            return ReferenceEnvironmentRecord(
                expectedModel: options.referenceModel,
                expectedBuild: options.referenceBuild,
                observedModel: environment.modelIdentifier,
                observedBuild: environment.buildVersion,
                confirmed: false,
                detail: "Reference Mac model/build was not supplied. The observed host is not assumed to be the Reference Mac."
            )
        }

        let confirmed = expectedModel == environment.modelIdentifier && expectedBuild == environment.buildVersion
        return ReferenceEnvironmentRecord(
            expectedModel: expectedModel,
            expectedBuild: expectedBuild,
            observedModel: environment.modelIdentifier,
            observedBuild: environment.buildVersion,
            confirmed: confirmed,
            detail: confirmed
                ? "Observed model/build match the supplied Reference Mac values."
                : "Observed model/build do not match the supplied Reference Mac values."
        )
    }

    static func isoTimestamp() -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter.string(from: Date())
    }
}
