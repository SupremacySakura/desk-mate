import Foundation

enum GateDecision: String, Codable {
    case passed
    case failed
    case pending
}

enum ProbeMode: String, Codable {
    case dryRun = "dry-run"
    case live
}

enum CheckState: String, Codable {
    case passed
    case failed
    case pending
    case notApplicable = "not-applicable"
}

enum CodecSelection: String {
    case both
    case hevc
    case h264
}

struct SafetyRecord: Codable {
    var liveActionsExecuted: Bool
    let displayCreationAuthorized: Bool
    let screenCaptureAuthorized: Bool
    let mainDisplayFallbackUsed: Bool
    let audioCaptureDisabledByDesign: Bool
}

struct EnvironmentRecord: Codable {
    let productName: String
    let productVersion: String
    let buildVersion: String
    let architecture: String
    let modelIdentifier: String
    let modelName: String
    let chip: String
    let kernelRelease: String
    let xcodeVersion: String
    let xcodeBuild: String
}

struct ReferenceEnvironmentRecord: Codable {
    let expectedModel: String?
    let expectedBuild: String?
    let observedModel: String
    let observedBuild: String
    let confirmed: Bool
    let detail: String
}

struct PrivateAPIRecord: Codable {
    let frameworkPath: String
    let classes: [String: Bool]
    let requiredSelectors: [String: [String: Bool]]
    let staticAvailability: String
    let liveCreation: String
    let notes: [String]
}

struct DisplayRecord: Codable {
    let created: Bool
    let provider: String
    let attachedExisting: Bool
    let displayID: UInt32?
    let requestedLogicalWidth: Int
    let requestedLogicalHeight: Int
    let scale: Int
    let requestedPixelWidth: Int
    let requestedPixelHeight: Int
    let refreshRate: Double
    let actualMode: String?
    let isMainDisplay: Bool?
    let isMirroring: Bool?
    let destroyed: Bool?
    let stillOnlineAfterRun: Bool?
    let failure: String?
}

struct CaptureRecord: Codable {
    let started: Bool
    let target: String
    let displayID: UInt32?
    let width: Int?
    let height: Int?
    let frameRate: Double
    let framesReceived: Int
    let audioEnabled: Bool
    let audioRequested: Bool
    let audioOutputAdded: Bool
    let failure: String?
}

struct ArtifactPlan: Codable {
    let codec: String
    let container: String
    let annexB: Bool
    let realTime: Bool
    let noBFrames: Bool
    let parameterSets: [String]
    let colorMetadata: [String]
}

struct VideoEncoderCandidate: Codable {
    let codec: String
    let name: String
    let encoderID: String
    let hardwareAccelerated: Bool?
    let supportsFrameReordering: Bool?
}

struct VideoToolboxRecord: Codable {
    let encoderListStatus: Int32
    let reportedCandidateCount: Int
    let hevcCandidateFound: Bool
    let h264CandidateFound: Bool
    let hardwareHEVCCandidateFound: Bool
    let hardwareH264CandidateFound: Bool
    let candidates: [VideoEncoderCandidate]
    let note: String
}

struct ArtifactRecord: Codable {
    let codec: String
    let path: String
    let metadataPath: String
    let frameCount: Int
    let keyFrameCount: Int
    let parameterSetCount: Int
    let noBFramePropertyVerified: Bool
    let outputPTSMonotonic: Bool
    let colorMetadata: [String: String]
    let failure: String?
}

struct LifecycleRecord: Codable {
    let hooksImplemented: Bool
    let hooksRegistered: Bool
    let eventsObserved: [String]
    let behavior: String
    let manualSteps: [String]
}

struct SigningRecord: Codable {
    let developerIDAvailable: Bool
    let developerIDIdentityCount: Int
    let notarytoolAvailable: Bool
    let gatekeeperAssessmentEnabled: Bool
    let signingStatus: String
    let notarizationStatus: String
    let gatekeeperStatus: String
    let detail: String
}

struct PrerequisiteRecord: Codable {
    let id: String
    let state: CheckState
    let detail: String
}

struct Gate0Report: Codable {
    let schemaVersion: Int
    let probe: String
    let issue: String
    let generatedAt: String
    let mode: ProbeMode
    let decision: GateDecision
    let decisionReason: String
    let safety: SafetyRecord
    let environment: EnvironmentRecord
    let referenceEnvironment: ReferenceEnvironmentRecord
    let privateAPI: PrivateAPIRecord
    let display: DisplayRecord
    let capture: CaptureRecord
    let videoToolbox: VideoToolboxRecord
    let artifacts: [ArtifactRecord]
    let artifactPlan: [ArtifactPlan]
    let lifecycle: LifecycleRecord
    let signing: SigningRecord
    let prerequisites: [PrerequisiteRecord]
    let notExecuted: [String]
}

struct ProbeOptions {
    var mode: ProbeMode = .dryRun
    var selfTest = false
    var listDisplays = false
    var allowCreateDisplay = false
    var allowScreenCapture = false
    var requestScreenRecordingPermission = false
    var json = false
    var reportPath: String?
    var outputDirectory = "artifacts"
    var durationSeconds = 10.0
    var logicalWidth = 1400
    var logicalHeight = 920
    var scale = 2
    var refreshRate = 60.0
    var codecs: CodecSelection = .both
    var referenceModel: String?
    var referenceBuild: String?
    var attachDisplayID: UInt32?
}

struct CLIError: Error, CustomStringConvertible {
    let description: String
}

extension JSONEncoder {
    static var pretty: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        return encoder
    }
}
