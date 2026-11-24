import CoreMedia
import CoreVideo
import Foundation
@preconcurrency import ScreenCaptureKit

enum CaptureError: Error, CustomStringConvertible {
    case targetDisplayMissing(UInt32)
    case outputRegistration(String)
    case start(String)
    case encoderCreation(String)

    var description: String {
        switch self {
        case .targetDisplayMissing(let displayID):
            return "ScreenCaptureKit did not expose display \(displayID); refusing to fall back to the main display"
        case .outputRegistration(let message):
            return "screen output registration failed: \(message)"
        case .start(let message):
            return "ScreenCaptureKit start failed: \(message)"
        case .encoderCreation(let message):
            return "encoder creation failed: \(message)"
        }
    }
}

final class CapturePipeline: NSObject, SCStreamOutput, SCStreamDelegate {
    private let displayID: UInt32
    private let width: Int
    private let height: Int
    private let frameRate: Double
    private let codec: String
    private let captureQueue = DispatchQueue(label: "dev.deskmate.host.capture")
    private let stateLock = NSLock()
    private var encoder: VideoEncoderCore?
    private var stream: SCStream?
    private var started = false
    private var outputRegistered = false
    private var framesReceived = 0
    private var runtimeFailure: String?

    private let onFrame: (EncodedFrame) -> Void
    private let onFailure: (String) -> Void

    init(
        displayID: UInt32,
        width: Int,
        height: Int,
        frameRate: Double,
        codec: String,
        onFrame: @escaping (EncodedFrame) -> Void,
        onFailure: @escaping (String) -> Void
    ) {
        self.displayID = displayID
        self.width = width
        self.height = height
        self.frameRate = frameRate
        self.codec = codec
        self.onFrame = onFrame
        self.onFailure = onFailure
        super.init()
    }

    var receivedFrameCount: Int {
        locked { framesReceived }
    }

    var encodedFrameCount: Int {
        encoder?.encodedFrameCount ?? 0
    }

    var isRunning: Bool {
        locked { started }
    }

    func requestKeyframe() {
        encoder?.requestKeyframe()
    }

    func start() async throws {
        do {
            encoder = try VideoEncoderCore(
                codec: codec,
                width: width,
                height: height,
                frameRate: frameRate,
                onFrame: onFrame,
                onFailure: { [weak self] message in
                    self?.recordFailure(message)
                }
            )
        } catch {
            throw CaptureError.encoderCreation("\(error)")
        }

        let content: SCShareableContent
        do {
            content = try await SCShareableContent.current
        } catch {
            throw CaptureError.start("SCShareableContent.current failed: \(error.localizedDescription)")
        }
        guard let target = content.displays.first(where: { $0.displayID == displayID }) else {
            throw CaptureError.targetDisplayMissing(displayID)
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
        if let colorSpace = CGColorSpace(name: CGColorSpace.itur_709)
            ?? CGColorSpace(name: CGColorSpace.sRGB),
           let colorSpaceName = colorSpace.name {
            configuration.colorSpaceName = colorSpaceName
        }

        let filter = SCContentFilter(display: target, excludingWindows: [])
        let stream = SCStream(filter: filter, configuration: configuration, delegate: self)
        self.stream = stream
        do {
            try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: captureQueue)
            locked { outputRegistered = true }
        } catch {
            throw CaptureError.outputRegistration(error.localizedDescription)
        }
        do {
            try await stream.startCapture()
        } catch {
            throw CaptureError.start(error.localizedDescription)
        }
        locked { started = true }
    }

    func stop() async {
        let (wasStarted, wasRegistered) = locked { () -> (Bool, Bool) in
            let snapshot = (started, outputRegistered)
            started = false
            outputRegistered = false
            return snapshot
        }

        if let stream {
            if wasStarted {
                do {
                    try await stream.stopCapture()
                } catch {
                    recordFailure("stopCapture failed: \(error.localizedDescription)")
                }
            }
            if wasRegistered {
                do {
                    try stream.removeStreamOutput(self, type: .screen)
                } catch {
                    recordFailure("removeStreamOutput failed: \(error.localizedDescription)")
                }
            }
        }
        stream = nil
        captureQueue.sync {}
        encoder?.stop()
        encoder = nil
    }

    func currentFailure() -> String? {
        locked { runtimeFailure }
    }

    func stream(
        _ stream: SCStream,
        didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
        of outputType: SCStreamOutputType
    ) {
        guard outputType == .screen else { return }
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

        let activeEncoder = locked { () -> VideoEncoderCore? in
            framesReceived += 1
            return encoder
        }

        guard let activeEncoder else { return }
        do {
            try activeEncoder.encode(
                pixelBuffer: pixelBuffer,
                presentationTime: CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
                duration: CMSampleBufferGetDuration(sampleBuffer)
            )
        } catch {
            recordFailure("encode failed: \(error)")
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
        let isFirst = locked { () -> Bool in
            if runtimeFailure == nil {
                runtimeFailure = message
                return true
            }
            return false
        }
        if isFirst {
            onFailure(message)
        }
    }

    private func locked<T>(_ body: () -> T) -> T {
        stateLock.lock()
        defer { stateLock.unlock() }
        return body()
    }
}
