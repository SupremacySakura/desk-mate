import CoreMedia
import CoreVideo
import Foundation
@preconcurrency import ScreenCaptureKit

enum CapturePipelineError: Error, CustomStringConvertible {
    case targetDisplayMissing(UInt32)
    case outputRegistration(String)
    case start(String)
    case encoderCreation(String)

    var description: String {
        switch self {
        case .targetDisplayMissing(let displayID):
            return "ScreenCaptureKit did not expose virtual display \(displayID); refusing to fall back to the main display"
        case .outputRegistration(let message):
            return "screen output registration failed: \(message)"
        case .start(let message):
            return "ScreenCaptureKit start failed: \(message)"
        case .encoderCreation(let message):
            return "encoder creation failed: \(message)"
        }
    }
}

struct CapturePipelineResult {
    let record: CaptureRecord
    let artifacts: [ArtifactRecord]
    let failure: String?
}

final class CaptureEncodePipeline: NSObject, SCStreamOutput, SCStreamDelegate {
    private let displayID: UInt32
    private let width: Int
    private let height: Int
    private let frameRate: Double
    private let outputDirectory: URL
    private let captureQueue = DispatchQueue(label: "dev.deskmate.gate0.capture")
    private let stateLock = NSLock()
    private let encoders: [Gate0Encoder]
    private var stream: SCStream?
    private var started = false
    private var outputRegistered = false
    private var framesReceived = 0
    private var runtimeFailure: String?

    init(options: ProbeOptions, displayID: UInt32, width: Int, height: Int) throws {
        self.displayID = displayID
        self.width = width
        self.height = height
        let configuredFrameRate = options.refreshRate
        let configuredOutputDirectory = URL(fileURLWithPath: options.outputDirectory).standardizedFileURL
        frameRate = configuredFrameRate
        outputDirectory = configuredOutputDirectory
        let timestamp = Self.timestamp()
        let codecNames: [String]
        switch options.codecs {
        case .both:
            codecNames = ["hevc", "h264"]
        case .hevc:
            codecNames = ["hevc"]
        case .h264:
            codecNames = ["h264"]
        }

        do {
            let builtEncoders = try codecNames.map { codec in
                try Gate0Encoder(
                    codec: codec,
                    width: width,
                    height: height,
                    pixelFormat: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                    frameRate: configuredFrameRate,
                    outputDirectory: configuredOutputDirectory,
                    timestamp: timestamp
                )
            }
            encoders = builtEncoders
        } catch {
            throw CapturePipelineError.encoderCreation("\(error)")
        }
        super.init()
    }

    func start() async throws {
        let content = try await SCShareableContent.current
        guard let target = content.displays.first(where: { $0.displayID == displayID }) else {
            throw CapturePipelineError.targetDisplayMissing(displayID)
        }

        let configuration = SCStreamConfiguration()
        configuration.width = width
        configuration.height = height
        configuration.minimumFrameInterval = CMTime(
            value: 1,
            timescale: CMTimeScale(frameRate.rounded())
        )
        configuration.queueDepth = 3
        configuration.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        configuration.showsCursor = true
        configuration.capturesAudio = false
        configuration.excludesCurrentProcessAudio = true
        let colorSpace = CGColorSpace(name: CGColorSpace.itur_709)
            ?? CGColorSpace(name: CGColorSpace.sRGB)
        if let colorSpaceName = colorSpace?.name {
            configuration.colorSpaceName = colorSpaceName
        }

        let filter = SCContentFilter(display: target, excludingWindows: [])
        let stream = SCStream(filter: filter, configuration: configuration, delegate: self)
        self.stream = stream
        do {
            try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: captureQueue)
            markOutputRegistered()
        } catch {
            throw CapturePipelineError.outputRegistration(error.localizedDescription)
        }
        do {
            try await stream.startCapture()
        } catch {
            throw CapturePipelineError.start(error.localizedDescription)
        }
        markStarted()
    }

    func stop() async -> CapturePipelineResult {
        var stopFailure: String?
        if let stream {
            if isStarted() {
                do {
                    try await stream.stopCapture()
                } catch {
                    stopFailure = "stopCapture failed: \(error.localizedDescription)"
                }
            }
            if isOutputRegistered() {
                do {
                    try stream.removeStreamOutput(self, type: .screen)
                } catch {
                    if stopFailure == nil {
                        stopFailure = "removeStreamOutput failed: \(error.localizedDescription)"
                    }
                }
            }
        }
        self.stream = nil
        captureQueue.sync {}

        let artifacts = encoders.map { $0.finish() }
        let (totalFrames, activeFailure, wasStarted) = snapshotAfterStop()

        var failures: [String] = []
        if let activeFailure {
            failures.append(activeFailure)
        }
        if let stopFailure {
            failures.append(stopFailure)
        }
        if !wasStarted {
            failures.append("capture pipeline was never started")
        }
        if totalFrames == 0 {
            failures.append("ScreenCaptureKit delivered no complete screen frames")
        }
        for artifact in artifacts where artifact.failure != nil {
            failures.append("\(artifact.codec): \(artifact.failure!)")
        }

        let record = CaptureRecord(
            started: wasStarted,
            target: "created-extended-display-only",
            displayID: displayID,
            width: width,
            height: height,
            frameRate: frameRate,
            framesReceived: totalFrames,
            audioEnabled: false,
            audioRequested: false,
            audioOutputAdded: false,
            failure: failures.isEmpty ? nil : failures.joined(separator: "; ")
        )
        return CapturePipelineResult(
            record: record,
            artifacts: artifacts,
            failure: record.failure
        )
    }

    func currentFailure() -> String? {
        stateLock.lock()
        defer { stateLock.unlock() }
        return runtimeFailure
    }

    private func markStarted() {
        stateLock.lock()
        started = true
        stateLock.unlock()
    }

    private func markOutputRegistered() {
        stateLock.lock()
        outputRegistered = true
        stateLock.unlock()
    }

    private func isStarted() -> Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        return started
    }

    private func isOutputRegistered() -> Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        return outputRegistered
    }

    private func snapshotAfterStop() -> (frames: Int, failure: String?, wasStarted: Bool) {
        stateLock.lock()
        defer { stateLock.unlock() }
        let totalFrames = framesReceived
        let activeFailure = runtimeFailure
        let wasStarted = started
        started = false
        outputRegistered = false
        return (totalFrames, activeFailure, wasStarted)
    }

    func stream(
        _ stream: SCStream,
        didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
        of outputType: SCStreamOutputType
    ) {
        guard outputType == .screen else {
            recordFailure("unexpected non-screen output type delivered to video-only pipeline")
            return
        }
        guard CMSampleBufferIsValid(sampleBuffer) else {
            recordFailure("ScreenCaptureKit delivered an invalid sample buffer")
            return
        }
        if let status = frameStatus(sampleBuffer), status != .complete && status != .started {
            return
        }
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            recordFailure("screen sample has no image buffer")
            return
        }

        stateLock.lock()
        let frameIndex = framesReceived
        framesReceived += 1
        stateLock.unlock()

        let presentationTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let duration = CMSampleBufferGetDuration(sampleBuffer)
        for encoder in encoders {
            do {
                try encoder.encode(
                    pixelBuffer: pixelBuffer,
                    presentationTime: presentationTime,
                    duration: duration,
                    forceKeyFrame: frameIndex == 0
                )
            } catch {
                recordFailure("\(encoder.codec) encode failed: \(error)")
            }
        }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        recordFailure("ScreenCaptureKit stopped stream: \(error.localizedDescription)")
    }

    private func frameStatus(_ sampleBuffer: CMSampleBuffer) -> SCFrameStatus? {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(
            sampleBuffer,
            createIfNecessary: false
        ) as? [NSDictionary], let first = attachments.first,
        let rawValue = first[SCStreamFrameInfo.status.rawValue] as? NSNumber else {
            return nil
        }
        return SCFrameStatus(rawValue: rawValue.intValue)
    }

    private func recordFailure(_ message: String) {
        stateLock.lock()
        if runtimeFailure == nil {
            runtimeFailure = message
        }
        stateLock.unlock()
    }

    private static func timestamp() -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(secondsFromGMT: 0)
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return formatter.string(from: Date())
    }
}
