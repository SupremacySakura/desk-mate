import Foundation
import CoreGraphics

private enum LiveGate0Error: Error, CustomStringConvertible {
    case safety(String)

    var description: String {
        switch self {
        case .safety(let message):
            return message
        }
    }
}

enum LiveGate0 {
    @MainActor
    static func run(options: ProbeOptions) async -> Gate0Report {
        let baseline = ReportFactory.dryRun(options: options)
        let lifecycleMonitor = LifecycleMonitor()
        var prerequisites = baseline.prerequisites
        var displayRecord = baseline.display
        var captureRecord = baseline.capture
        var privateAPI = baseline.privateAPI
        var artifacts: [ArtifactRecord] = []
        var runtimeFailures: [String] = []
        var safety = SafetyRecord(
            liveActionsExecuted: false,
            displayCreationAuthorized: true,
            screenCaptureAuthorized: true,
            mainDisplayFallbackUsed: false,
            audioCaptureDisabledByDesign: true
        )

        var permission = SystemProbe.screenRecordingPreflight()
        if !permission && options.requestScreenRecordingPermission {
            permission = SystemProbe.requestScreenRecordingPermission()
        }
        guard permission else {
            replacePrerequisite(
                &prerequisites,
                id: "screen-recording-permission",
                state: .pending,
                detail: "Screen Recording permission is unavailable. No display or capture action was executed."
            )
            return buildReport(
                baseline: baseline,
                signing: baseline.signing,
                safety: safety,
                privateAPI: privateAPI,
                display: displayRecord,
                capture: captureRecord,
                artifacts: artifacts,
                lifecycle: lifecycleRecord(
                    registered: false,
                    events: [],
                    behavior: "No lifecycle observer was registered because Screen Recording permission was unavailable."
                ),
                prerequisites: prerequisites,
                decision: .pending,
                decisionReason: "Live execution was not attempted because Screen Recording permission is unavailable."
            )
        }

        replacePrerequisite(
            &prerequisites,
            id: "screen-recording-permission",
            state: .passed,
            detail: "Screen Recording preflight passed for this invocation."
        )

        let attachDisplayID = options.attachDisplayID
        if attachDisplayID == nil, baseline.privateAPI.staticAvailability != "available" {
            let detail = "The required CGVirtualDisplay classes or selectors are unavailable on this macOS build. Live display creation was not attempted."
            replacePrerequisite(
                &prerequisites,
                id: "live-display-and-capture",
                state: .failed,
                detail: detail
            )
            return buildReport(
                baseline: baseline,
                signing: baseline.signing,
                safety: safety,
                privateAPI: privateAPI,
                display: displayRecord,
                capture: captureRecord,
                artifacts: artifacts,
                lifecycle: lifecycleRecord(
                    registered: false,
                    events: [],
                    behavior: "No lifecycle observer was registered because the private display API was unavailable."
                ),
                prerequisites: prerequisites,
                decision: .failed,
                decisionReason: detail
            )
        }
        lifecycleMonitor.start()

        var activeDisplay: ActiveVirtualDisplay?
        var pipeline: CaptureEncodePipeline?

        do {
            let active: ActiveVirtualDisplay
            if let attachDisplayID {
                active = try ActiveVirtualDisplay.attach(options: options, displayID: attachDisplayID)
            } else {
                active = try ActiveVirtualDisplay.create(options: options)
            }
            activeDisplay = active
            displayRecord = active.record
            safety.liveActionsExecuted = true
            if attachDisplayID == nil {
                privateAPI = PrivateAPIRecord(
                    frameworkPath: baseline.privateAPI.frameworkPath,
                    classes: baseline.privateAPI.classes,
                    requiredSelectors: baseline.privateAPI.requiredSelectors,
                    staticAvailability: baseline.privateAPI.staticAvailability,
                    liveCreation: "passed",
                    notes: baseline.privateAPI.notes
                )
            } else {
                privateAPI = PrivateAPIRecord(
                    frameworkPath: baseline.privateAPI.frameworkPath,
                    classes: baseline.privateAPI.classes,
                    requiredSelectors: baseline.privateAPI.requiredSelectors,
                    staticAvailability: baseline.privateAPI.staticAvailability,
                    liveCreation: "not-used",
                    notes: baseline.privateAPI.notes + [
                        "Attach mode: display \(attachDisplayID!) was created by an external tool, not by CGVirtualDisplay."
                    ]
                )
            }

            guard active.record.isMainDisplay == false else {
                throw LiveGate0Error.safety("created display is the main display")
            }
            guard active.record.isMirroring == false else {
                throw LiveGate0Error.safety("created display is mirroring another display")
            }

            let pixelWidth: Int
            let pixelHeight: Int
            if attachDisplayID == nil {
                pixelWidth = options.logicalWidth * options.scale
                pixelHeight = options.logicalHeight * options.scale
            } else {
                guard let mode = CGDisplayCopyDisplayMode(active.displayID) else {
                    throw LiveGate0Error.safety("attached display \(active.displayID) has no readable mode")
                }
                pixelWidth = Int(mode.pixelWidth)
                pixelHeight = Int(mode.pixelHeight)
            }
            let capturePipeline = try CaptureEncodePipeline(
                options: options,
                displayID: active.displayID,
                width: pixelWidth,
                height: pixelHeight
            )
            pipeline = capturePipeline
            try await capturePipeline.start()
            safety.liveActionsExecuted = true

            let deadline = Date().addingTimeInterval(options.durationSeconds)
            while Date() < deadline {
                if lifecycleMonitor.terminationEvent != nil {
                    break
                }
                if capturePipeline.currentFailure() != nil {
                    break
                }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
        } catch {
            runtimeFailures.append("\(error)")
            if let displayError = error as? DisplaySessionError,
               case .failed(let failedDisplay) = displayError {
                displayRecord = failedDisplay
                safety.liveActionsExecuted = failedDisplay.created
            }
            if privateAPI.liveCreation != "passed" {
                privateAPI = PrivateAPIRecord(
                    frameworkPath: baseline.privateAPI.frameworkPath,
                    classes: baseline.privateAPI.classes,
                    requiredSelectors: baseline.privateAPI.requiredSelectors,
                    staticAvailability: baseline.privateAPI.staticAvailability,
                    liveCreation: "failed",
                    notes: baseline.privateAPI.notes
                )
            }
        }

        if let pipeline {
            let result = await pipeline.stop()
            captureRecord = result.record
            artifacts = result.artifacts
            if let failure = result.failure {
                runtimeFailures.append(failure)
            }
        }

        if let activeDisplay {
            let cleanup = activeDisplay.destroy()
            displayRecord = activeDisplay.record
            if !cleanup.destroyed {
                runtimeFailures.append("display cleanup failed: \(cleanup.detail)")
            }
        }

        let lifecycleEvents = lifecycleMonitor.events
        let hooksWereRegistered = lifecycleMonitor.registered
        let observedTermination = lifecycleMonitor.terminationEvent
        lifecycleMonitor.stop()

        let displayAndCapturePassed =
            (displayRecord.created || displayRecord.attachedExisting) &&
            (displayRecord.destroyed == true || displayRecord.stillOnlineAfterRun == true) &&
            captureRecord.started &&
            captureRecord.framesReceived > 0 &&
            artifacts.count > 0 &&
            artifacts.allSatisfy { $0.failure == nil } &&
            displayRecord.failure == nil &&
            runtimeFailures.isEmpty

        replacePrerequisite(
            &prerequisites,
            id: "live-display-and-capture",
            state: displayAndCapturePassed ? .passed : .failed,
            detail: displayAndCapturePassed
                ? (displayRecord.attachedExisting
                    ? "Attached to the externally created non-mirrored HiDPI display \(displayRecord.displayID ?? 0), captured \(captureRecord.framesReceived) frame(s), encoded selected codecs, and left the display online."
                    : "Created a non-mirrored HiDPI display, captured \(captureRecord.framesReceived) frame(s), encoded selected codecs, and destroyed the display.")
                : (runtimeFailures.isEmpty
                    ? "Live display/capture was not completed."
                    : runtimeFailures.joined(separator: "; "))
        )

        if let observedTermination {
            replacePrerequisite(
                &prerequisites,
                id: "lock-sleep-recovery",
                state: .pending,
                detail: "Observed \(observedTermination) and stopped the probe. Recreate after unlock/wake still requires a second live run and ghost-display inspection."
            )
        } else {
            replacePrerequisite(
                &prerequisites,
                id: "lock-sleep-recovery",
                state: .pending,
                detail: "No lock or sleep event was exercised in this run. Use the manual lifecycle steps."
            )
        }

        if !baseline.referenceEnvironment.confirmed {
            replacePrerequisite(
                &prerequisites,
                id: "reference-mac",
                state: .pending,
                detail: baseline.referenceEnvironment.detail
            )
        }

        let failed = prerequisites.contains { $0.state == .failed }
        let allPassed = prerequisites.allSatisfy { $0.state == .passed || $0.state == .notApplicable }
        let decision: GateDecision = failed ? .failed : (allPassed ? .passed : .pending)
        let decisionReason: String
        if failed {
            let failedChecks = prerequisites
                .filter { $0.state == .failed }
                .map { "\($0.id): \($0.detail)" }
            decisionReason = "One or more live Gate 0 checks failed: \(failedChecks.joined(separator: "; "))"
        } else if !allPassed {
            let pending = prerequisites.filter { $0.state == .pending }.map(\.id)
            decisionReason = "Live checks that ran did not fail, but Gate 0 remains pending for: \(pending.joined(separator: ", "))."
        } else {
            decisionReason = "All Gate 0 prerequisites recorded evidence on the Reference Host."
        }

        return buildReport(
            baseline: baseline,
            signing: baseline.signing,
            safety: safety,
            privateAPI: privateAPI,
            display: displayRecord,
            capture: captureRecord,
            artifacts: artifacts,
            lifecycle: lifecycleRecord(
                registered: hooksWereRegistered,
                events: lifecycleEvents,
                behavior: lifecycleBehavior(events: lifecycleEvents)
            ),
            prerequisites: prerequisites,
            decision: decision,
            decisionReason: decisionReason
        )
    }

    private static func lifecycleBehavior(events: [String]) -> String {
        if let terminating = events.first(where: {
            $0 == "screenIsLocked" || $0 == "systemWillSleep" || $0 == "screensDidSleep"
        }) {
            return "Observed \(terminating), stopped capture, invalidated encoders, and released the virtual display. The probe does not recreate automatically; rerun after unlock/wake and verify no ghost display remains."
        }
        return "Registered distributed lock/unlock and NSWorkspace sleep/wake observers. No lock or sleep event was observed during this run."
    }

    private static func lifecycleRecord(
        registered: Bool,
        events: [String],
        behavior: String
    ) -> LifecycleRecord {
        LifecycleRecord(
            hooksImplemented: true,
            hooksRegistered: registered,
            eventsObserved: events,
            behavior: behavior,
            manualSteps: ReportFactory.lifecycleRecord().manualSteps
        )
    }

    private static func replacePrerequisite(
        _ prerequisites: inout [PrerequisiteRecord],
        id: String,
        state: CheckState,
        detail: String
    ) {
        guard let index = prerequisites.firstIndex(where: { $0.id == id }) else {
            prerequisites.append(PrerequisiteRecord(id: id, state: state, detail: detail))
            return
        }
        prerequisites[index] = PrerequisiteRecord(id: id, state: state, detail: detail)
    }

    private static func buildReport(
        baseline: Gate0Report,
        signing: SigningRecord,
        safety: SafetyRecord,
        privateAPI: PrivateAPIRecord,
        display: DisplayRecord,
        capture: CaptureRecord,
        artifacts: [ArtifactRecord],
        lifecycle: LifecycleRecord,
        prerequisites: [PrerequisiteRecord],
        decision: GateDecision,
        decisionReason: String
    ) -> Gate0Report {
        var notExecuted: [String] = []
        if !baseline.referenceEnvironment.confirmed {
            notExecuted.append("Confirm that this Mac model and macOS build are the Reference Environment.")
        }
        if !display.created {
            notExecuted.append("Create and verify a non-mirrored HiDPI Extended Display.")
        }
        if !capture.started {
            notExecuted.append("Start ScreenCaptureKit on the created display.")
        }
        if artifacts.isEmpty {
            notExecuted.append("Encode real-time HEVC/H.264 Annex-B samples with parameter sets and color metadata.")
        }
        if lifecycle.eventsObserved.isEmpty {
            notExecuted.append("Exercise lock and sleep to prove teardown behavior.")
        }
        notExecuted.append("Recreate after unlock/wake and verify no ghost display remains.")
        if signing.signingStatus != "passed" {
            notExecuted.append("Sign with Developer ID.")
        }
        notExecuted.append("Notarize, staple, and launch through Gatekeeper.")

        return Gate0Report(
            schemaVersion: baseline.schemaVersion,
            probe: baseline.probe,
            issue: baseline.issue,
            generatedAt: ReportFactory.isoTimestamp(),
            mode: .live,
            decision: decision,
            decisionReason: decisionReason,
            safety: safety,
            environment: baseline.environment,
            referenceEnvironment: baseline.referenceEnvironment,
            privateAPI: privateAPI,
            display: display,
            capture: capture,
            videoToolbox: baseline.videoToolbox,
            artifacts: artifacts,
            artifactPlan: baseline.artifactPlan,
            lifecycle: lifecycle,
            signing: signing,
            prerequisites: prerequisites,
            notExecuted: notExecuted
        )
    }
}
