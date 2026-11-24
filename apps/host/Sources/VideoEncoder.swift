import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

enum EncoderError: Error, CustomStringConvertible {
    case unsupportedCodec(String)
    case sessionCreation(OSStatus)
    case sessionProperty(key: String, status: OSStatus)
    case sessionPreparation(OSStatus)
    case missingSession
    case dimensionMismatch(expected: String, actual: String)
    case encode(OSStatus)
    case invalidSample(String)
    case conversion(String)
    case completion(OSStatus)

    var description: String {
        switch self {
        case .unsupportedCodec(let codec):
            return "unsupported codec: \(codec)"
        case .sessionCreation(let status):
            return "VTCompressionSessionCreate failed with OSStatus \(status)"
        case .sessionProperty(let key, let status):
            return "VTSessionSetProperty \(key) failed with OSStatus \(status)"
        case .sessionPreparation(let status):
            return "VTCompressionSessionPrepareToEncodeFrames failed with OSStatus \(status)"
        case .missingSession:
            return "compression session is not active"
        case .dimensionMismatch(let expected, let actual):
            return "pixel buffer dimensions changed: expected \(expected), got \(actual)"
        case .encode(let status):
            return "VTCompressionSessionEncodeFrame failed with OSStatus \(status)"
        case .invalidSample(let message):
            return "invalid encoded sample: \(message)"
        case .conversion(let message):
            return "Annex-B conversion failed: \(message)"
        case .completion(let status):
            return "VTCompressionSessionCompleteFrames failed with OSStatus \(status)"
        }
    }
}

struct EncodedFrame {
    let annexB: Data
    let presentationTime: CMTime
    let keyframe: Bool
}

private final class EncoderCallbackContext {
    weak var encoder: VideoEncoderCore?
    private let condition = NSCondition()
    private var callbacksInFlight = 0
    private var stopping = false

    func beginCallback() -> VideoEncoderCore? {
        condition.lock()
        defer { condition.unlock() }
        guard !stopping else { return nil }
        callbacksInFlight += 1
        return encoder
    }

    func endCallback() {
        condition.lock()
        callbacksInFlight -= 1
        if callbacksInFlight == 0 {
            condition.broadcast()
        }
        condition.unlock()
    }

    func stopAndWait() {
        condition.lock()
        stopping = true
        while callbacksInFlight > 0 {
            condition.wait()
        }
        condition.unlock()
    }
}

private let deskMateCompressionCallback: VTCompressionOutputCallback = {
    refcon, _, status, infoFlags, sampleBuffer in
    guard let refcon else { return }
    let context = Unmanaged<EncoderCallbackContext>.fromOpaque(refcon)
        .takeUnretainedValue()
    guard let encoder = context.beginCallback() else { return }
    defer { context.endCallback() }
    encoder.handleOutput(status: status, infoFlags: infoFlags, sampleBuffer: sampleBuffer)
}

final class VideoEncoderCore {
    let codec: String
    let width: Int
    let height: Int
    let frameRate: Double
    let onFrame: (EncodedFrame) -> Void
    let onFailure: (String) -> Void

    private let lock = NSLock()
    private let callbackContext = EncoderCallbackContext()
    private var retainedCallbackContext: Unmanaged<EncoderCallbackContext>?
    private var session: VTCompressionSession?
    private var parameterSets = Data()
    private var forceKeyframeNext = true
    private var framesEncoded = 0
    private var keyframesEncoded = 0
    private var lastInputPTS = CMTime.invalid
    private var nalUnitHeaderLength = 4

    init(
        codec: String,
        width: Int,
        height: Int,
        frameRate: Double,
        onFrame: @escaping (EncodedFrame) -> Void,
        onFailure: @escaping (String) -> Void
    ) throws {
        guard codec == "hevc" || codec == "h264" else {
            throw EncoderError.unsupportedCodec(codec)
        }
        self.codec = codec
        self.width = width
        self.height = height
        self.frameRate = frameRate
        self.onFrame = onFrame
        self.onFailure = onFailure
        try createSession()
    }

    deinit {
        stop()
    }

    var encodedFrameCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return framesEncoded
    }

    var keyframeCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return keyframesEncoded
    }

    func requestKeyframe() {
        lock.lock()
        forceKeyframeNext = true
        lock.unlock()
    }

    func encode(pixelBuffer: CVPixelBuffer, presentationTime: CMTime, duration: CMTime) throws {
        let actualWidth = CVPixelBufferGetWidth(pixelBuffer)
        let actualHeight = CVPixelBufferGetHeight(pixelBuffer)
        guard actualWidth == width, actualHeight == height else {
            throw EncoderError.dimensionMismatch(
                expected: "\(width)x\(height)",
                actual: "\(actualWidth)x\(actualHeight)"
            )
        }

        lock.lock()
        guard let session else {
            lock.unlock()
            throw EncoderError.missingSession
        }
        if lastInputPTS.isValid && CMTimeCompare(presentationTime, lastInputPTS) <= 0 {
            lock.unlock()
            throw EncoderError.invalidSample("input presentation timestamps are not increasing")
        }
        lastInputPTS = presentationTime
        let forceKeyframe = forceKeyframeNext
        forceKeyframeNext = false
        lock.unlock()

        let frameProperties: CFDictionary? = forceKeyframe
            ? ([kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary)
            : nil
        let status = VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: pixelBuffer,
            presentationTimeStamp: presentationTime,
            duration: duration,
            frameProperties: frameProperties,
            sourceFrameRefcon: nil,
            infoFlagsOut: nil
        )
        guard status == noErr else {
            throw EncoderError.encode(status)
        }
    }

    func stop() {
        lock.lock()
        let activeSession = session
        session = nil
        lock.unlock()

        guard let activeSession else {
            callbackContext.stopAndWait()
            callbackContext.encoder = nil
            releaseCallbackContext()
            return
        }
        _ = VTCompressionSessionCompleteFrames(activeSession, untilPresentationTimeStamp: .invalid)
        callbackContext.stopAndWait()
        VTCompressionSessionInvalidate(activeSession)
        callbackContext.encoder = nil
        releaseCallbackContext()
    }

    fileprivate func handleOutput(
        status: OSStatus,
        infoFlags: VTEncodeInfoFlags,
        sampleBuffer: CMSampleBuffer?
    ) {
        if status != noErr {
            onFailure("compression callback failed with OSStatus \(status)")
            return
        }
        if infoFlags.contains(.frameDropped) {
            return
        }
        guard let sampleBuffer else {
            onFailure("compression callback returned no sample buffer")
            return
        }
        do {
            try deliver(sampleBuffer: sampleBuffer)
        } catch {
            onFailure("\(error)")
        }
    }

    private func deliver(sampleBuffer: CMSampleBuffer) throws {
        guard let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer) else {
            throw EncoderError.invalidSample("missing format description")
        }
        guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else {
            throw EncoderError.invalidSample("missing block buffer")
        }
        let totalLength = CMBlockBufferGetDataLength(dataBuffer)
        guard totalLength > 0 else {
            throw EncoderError.invalidSample("empty block buffer")
        }

        var payload = Data(count: totalLength)
        let copyStatus = payload.withUnsafeMutableBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return -1 }
            return CMBlockBufferCopyDataBytes(
                dataBuffer,
                atOffset: 0,
                dataLength: totalLength,
                destination: base
            )
        }
        guard copyStatus == noErr else {
            throw EncoderError.invalidSample("CMBlockBufferCopyDataBytes returned \(copyStatus)")
        }

        lock.lock()
        if parameterSets.isEmpty {
            do {
                let extracted = try extractParameterSets(from: formatDescription)
                var header = Data()
                for set in extracted {
                    header.append(AnnexB.startCode)
                    header.append(set)
                }
                parameterSets = header
            } catch {
                lock.unlock()
                throw error
            }
        }
        let header = parameterSets
        let currentNALLength = nalUnitHeaderLength
        let keyframe = isKeyFrame(sampleBuffer)
        lock.unlock()

        let annexB = try AnnexB.convertLengthPrefixed(payload, nalUnitHeaderLength: currentNALLength)
        var out = Data()
        out.reserveCapacity(header.count + annexB.count)
        if keyframe {
            out.append(header)
        }
        out.append(annexB)

        lock.lock()
        framesEncoded += 1
        if keyframe { keyframesEncoded += 1 }
        lock.unlock()

        onFrame(EncodedFrame(
            annexB: out,
            presentationTime: CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
            keyframe: keyframe
        ))
    }

    private func createSession() throws {
        let codecType: CMVideoCodecType
        let profileLevel: String
        switch codec {
        case "hevc":
            codecType = kCMVideoCodecType_HEVC
            profileLevel = kVTProfileLevel_HEVC_Main_AutoLevel as String
        default:
            codecType = kCMVideoCodecType_H264
            profileLevel = kVTProfileLevel_H264_High_AutoLevel as String
        }

        let bitRate = max(8_000_000, Int(Double(width * height) * frameRate * 0.10))
        let properties: [String: Any] = [
            kVTCompressionPropertyKey_RealTime as String: true,
            kVTCompressionPropertyKey_AllowFrameReordering as String: false,
            kVTCompressionPropertyKey_ProfileLevel as String: profileLevel,
            kVTCompressionPropertyKey_ExpectedFrameRate as String: NSNumber(value: frameRate),
            kVTCompressionPropertyKey_MaximumRealTimeFrameRate as String: NSNumber(value: frameRate),
            kVTCompressionPropertyKey_AverageBitRate as String: NSNumber(value: bitRate),
            kVTCompressionPropertyKey_MaxKeyFrameInterval as String: NSNumber(value: Int(frameRate)),
            kVTCompressionPropertyKey_AllowTemporalCompression as String: true,
            kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality as String: true,
            kVTCompressionPropertyKey_ColorPrimaries as String:
                kCVImageBufferColorPrimaries_ITU_R_709_2 as String,
            kVTCompressionPropertyKey_TransferFunction as String:
                kCVImageBufferTransferFunction_ITU_R_709_2 as String,
            kVTCompressionPropertyKey_YCbCrMatrix as String:
                kCVImageBufferYCbCrMatrix_ITU_R_709_2 as String,
        ]
        let encoderSpecification: [String: Any] = [
            kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder as String: true,
        ]
        let sourceAttributes: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String:
                NSNumber(value: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
        ]

        var created: VTCompressionSession?
        callbackContext.encoder = self
        let retained = Unmanaged.passRetained(callbackContext)
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: Int32(width),
            height: Int32(height),
            codecType: codecType,
            encoderSpecification: encoderSpecification as CFDictionary,
            imageBufferAttributes: sourceAttributes as CFDictionary,
            compressedDataAllocator: kCFAllocatorDefault,
            outputCallback: deskMateCompressionCallback,
            refcon: retained.toOpaque(),
            compressionSessionOut: &created
        )
        guard status == noErr, let created else {
            retained.release()
            callbackContext.encoder = nil
            throw EncoderError.sessionCreation(status)
        }
        retainedCallbackContext = retained

        for (key, value) in properties {
            let propertyStatus = VTSessionSetProperty(
                created,
                key: key as CFString,
                value: value as AnyObject
            )
            guard propertyStatus == noErr else {
                VTCompressionSessionInvalidate(created)
                callbackContext.stopAndWait()
                callbackContext.encoder = nil
                releaseCallbackContext()
                throw EncoderError.sessionProperty(key: key, status: propertyStatus)
            }
        }

        let prepareStatus = VTCompressionSessionPrepareToEncodeFrames(created)
        guard prepareStatus == noErr else {
            VTCompressionSessionInvalidate(created)
            callbackContext.stopAndWait()
            callbackContext.encoder = nil
            releaseCallbackContext()
            throw EncoderError.sessionPreparation(prepareStatus)
        }
        session = created
    }

    private func releaseCallbackContext() {
        retainedCallbackContext?.release()
        retainedCallbackContext = nil
    }

    private func extractParameterSets(from formatDescription: CMFormatDescription) throws -> [Data] {
        var count = 0
        var headerLength: Int32 = 4
        var sets: [Data] = []
        if codec == "hevc" {
            var status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                formatDescription,
                parameterSetIndex: 0,
                parameterSetPointerOut: nil,
                parameterSetSizeOut: nil,
                parameterSetCountOut: &count,
                nalUnitHeaderLengthOut: &headerLength
            )
            guard status == noErr, count > 0 else {
                throw EncoderError.invalidSample("HEVC parameter set enumeration failed")
            }
            for index in 0..<count {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    formatDescription,
                    parameterSetIndex: index,
                    parameterSetPointerOut: &pointer,
                    parameterSetSizeOut: &size,
                    parameterSetCountOut: &count,
                    nalUnitHeaderLengthOut: &headerLength
                )
                guard status == noErr, let pointer, size > 0 else {
                    throw EncoderError.invalidSample("HEVC parameter set \(index) unavailable")
                }
                sets.append(Data(bytes: pointer, count: size))
            }
        } else {
            var status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                formatDescription,
                parameterSetIndex: 0,
                parameterSetPointerOut: nil,
                parameterSetSizeOut: nil,
                parameterSetCountOut: &count,
                nalUnitHeaderLengthOut: &headerLength
            )
            guard status == noErr, count > 0 else {
                throw EncoderError.invalidSample("H.264 parameter set enumeration failed")
            }
            for index in 0..<count {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    formatDescription,
                    parameterSetIndex: index,
                    parameterSetPointerOut: &pointer,
                    parameterSetSizeOut: &size,
                    parameterSetCountOut: &count,
                    nalUnitHeaderLengthOut: &headerLength
                )
                guard status == noErr, let pointer, size > 0 else {
                    throw EncoderError.invalidSample("H.264 parameter set \(index) unavailable")
                }
                sets.append(Data(bytes: pointer, count: size))
            }
        }
        nalUnitHeaderLength = Int(headerLength)
        return sets
    }

    private func isKeyFrame(_ sampleBuffer: CMSampleBuffer) -> Bool {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(
            sampleBuffer,
            createIfNecessary: false
        ) as? [NSDictionary], let first = attachments.first else {
            return true
        }
        let notSync = (first[kCMSampleAttachmentKey_NotSync as String] as? NSNumber)?.boolValue ?? false
        return !notSync
    }
}
