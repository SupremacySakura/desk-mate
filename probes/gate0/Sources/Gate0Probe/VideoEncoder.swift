import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

enum Gate0EncoderError: Error, CustomStringConvertible {
    case unsupportedCodec(String)
    case fileCreation(String)
    case sessionCreation(OSStatus)
    case sessionProperty(key: String, status: OSStatus)
    case sessionPreparation(OSStatus)
    case missingSession
    case missingPixelBuffer
    case dimensionMismatch(expected: String, actual: String)
    case encode(OSStatus)
    case invalidSample(String)
    case conversion(String)
    case write(String)
    case completion(OSStatus)

    var description: String {
        switch self {
        case .unsupportedCodec(let codec):
            return "unsupported codec: \(codec)"
        case .fileCreation(let message):
            return "artifact file creation failed: \(message)"
        case .sessionCreation(let status):
            return "VTCompressionSessionCreate failed with OSStatus \(status)"
        case .sessionProperty(let key, let status):
            return "VTSessionSetProperty \(key) failed with OSStatus \(status)"
        case .sessionPreparation(let status):
            return "VTCompressionSessionPrepareToEncodeFrames failed with OSStatus \(status)"
        case .missingSession:
            return "compression session is not active"
        case .missingPixelBuffer:
            return "screen sample has no CVPixelBuffer"
        case .dimensionMismatch(let expected, let actual):
            return "pixel buffer dimensions changed: expected \(expected), got \(actual)"
        case .encode(let status):
            return "VTCompressionSessionEncodeFrame failed with OSStatus \(status)"
        case .invalidSample(let message):
            return "invalid encoded sample: \(message)"
        case .conversion(let message):
            return "Annex-B conversion failed: \(message)"
        case .write(let message):
            return "artifact write failed: \(message)"
        case .completion(let status):
            return "VTCompressionSessionCompleteFrames failed with OSStatus \(status)"
        }
    }
}

struct ParameterSetMetadata: Codable {
    let type: String
    let byteCount: Int
    let hex: String
}

struct EncodedFrameMetadata: Codable {
    let index: Int
    let presentationTimeValue: Int64
    let presentationTimeScale: Int32
    // VideoToolbox can hand back an indefinite duration or an invalid
    // presentation time. JSONEncoder refuses non-finite doubles, which used to
    // fail the whole metadata write after a successful encode.
    let presentationSeconds: Double?
    let durationSeconds: Double?
    let byteCount: Int
    let keyFrame: Bool
}

struct EncodedArtifactMetadata: Codable {
    let codec: String
    let width: Int
    let height: Int
    let pixelFormat: String
    let realTimeRequested: Bool
    let allowFrameReorderingRequested: Bool
    let allowFrameReorderingReadback: Bool?
    let hardwareAccelerationRequired: Bool
    let hardwareAccelerationReadback: Bool?
    let profileLevelReadback: String?
    let requestedColorMetadata: [String: String]
    let formatDescriptionExtensions: [String: String]
    let parameterSets: [ParameterSetMetadata]
    let frames: [EncodedFrameMetadata]
    let audioRequested: Bool
}

private final class Gate0CallbackContext {
    weak var encoder: Gate0Encoder?
    private let condition = NSCondition()
    private var callbacksInFlight = 0
    private var stopping = false

    func beginCallback() -> Gate0Encoder? {
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

private let gate0VTCompressionOutputCallback: VTCompressionOutputCallback = {
    refcon, _, status, infoFlags, sampleBuffer in
    guard let refcon else { return }
    let context = Unmanaged<Gate0CallbackContext>.fromOpaque(refcon)
        .takeUnretainedValue()
    guard let encoder = context.beginCallback() else { return }
    defer { context.endCallback() }
    encoder.handleOutput(status: status, infoFlags: infoFlags, sampleBuffer: sampleBuffer)
}

final class Gate0Encoder {
    let codec: String
    let width: Int
    let height: Int
    let pixelFormat: OSType
    let frameRate: Double
    let bitstreamURL: URL
    let metadataURL: URL

    private let lock = NSLock()
    private let fileHandle: FileHandle
    private let callbackContext = Gate0CallbackContext()
    private var retainedCallbackContext: Unmanaged<Gate0CallbackContext>?
    private var session: VTCompressionSession?
    private var fileClosed = false
    private var failure: String?
    private var parameterSetsWritten = false
    private var parameterSetMetadata: [ParameterSetMetadata] = []
    private var formatExtensions: [String: String] = [:]
    private var frames: [EncodedFrameMetadata] = []
    private var lastInputPTS = CMTime.invalid
    private var lastOutputPTS = CMTime.invalid
    private var outputPTSMonotonic = true
    private var allowFrameReorderingReadback: Bool?
    private var hardwareAccelerationReadback: Bool?
    private var profileLevelReadback: String?
    private var droppedFrames = 0
    private let requestedColorMetadata: [String: String] = [
        "colorPrimaries": kCVImageBufferColorPrimaries_ITU_R_709_2 as String,
        "transferFunction": kCVImageBufferTransferFunction_ITU_R_709_2 as String,
        "yCbCrMatrix": kCVImageBufferYCbCrMatrix_ITU_R_709_2 as String,
    ]

    init(
        codec: String,
        width: Int,
        height: Int,
        pixelFormat: OSType,
        frameRate: Double,
        outputDirectory: URL,
        timestamp: String
    ) throws {
        guard codec == "hevc" || codec == "h264" else {
            throw Gate0EncoderError.unsupportedCodec(codec)
        }
        self.codec = codec
        self.width = width
        self.height = height
        self.pixelFormat = pixelFormat
        self.frameRate = frameRate

        let fileExtension = codec == "hevc" ? "h265" : "h264"
        bitstreamURL = outputDirectory.appendingPathComponent("gate0-\(codec)-\(timestamp).\(fileExtension)")
        metadataURL = outputDirectory.appendingPathComponent("gate0-\(codec)-\(timestamp).json")

        do {
            try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
            guard FileManager.default.createFile(atPath: bitstreamURL.path, contents: nil) else {
                throw Gate0EncoderError.fileCreation("createFile returned false for \(bitstreamURL.path)")
            }
            fileHandle = try FileHandle(forWritingTo: bitstreamURL)
        } catch let error as Gate0EncoderError {
            throw error
        } catch {
            throw Gate0EncoderError.fileCreation(error.localizedDescription)
        }

        try createSession()
    }

    deinit {
        if let session {
            VTCompressionSessionCompleteFrames(
                session,
                untilPresentationTimeStamp: .invalid
            )
            callbackContext.stopAndWait()
            VTCompressionSessionInvalidate(session)
        } else {
            callbackContext.stopAndWait()
        }
        callbackContext.encoder = nil
        releaseCallbackContext()
        closeFileIfOpen()
    }

    func encode(pixelBuffer: CVPixelBuffer, presentationTime: CMTime, duration: CMTime, forceKeyFrame: Bool) throws {
        let actualWidth = CVPixelBufferGetWidth(pixelBuffer)
        let actualHeight = CVPixelBufferGetHeight(pixelBuffer)
        guard actualWidth == width, actualHeight == height else {
            throw Gate0EncoderError.dimensionMismatch(
                expected: "\(width)x\(height)",
                actual: "\(actualWidth)x\(actualHeight)"
            )
        }

        lock.lock()
        guard failure == nil else {
            lock.unlock()
            throw Gate0EncoderError.encode(-1)
        }
        guard let session else {
            lock.unlock()
            throw Gate0EncoderError.missingSession
        }
        if lastInputPTS.isValid && CMTimeCompare(presentationTime, lastInputPTS) <= 0 {
            failure = "input presentation timestamps are not strictly increasing"
            lock.unlock()
            throw Gate0EncoderError.invalidSample("non-monotonic input PTS")
        }
        lastInputPTS = presentationTime
        lock.unlock()

        let frameProperties: CFDictionary? = forceKeyFrame
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
            recordFailure(Gate0EncoderError.encode(status).description)
            throw Gate0EncoderError.encode(status)
        }
    }

    func finish() -> ArtifactRecord {
        lock.lock()
        let activeSession = session
        lock.unlock()

        if let activeSession {
            let status = VTCompressionSessionCompleteFrames(activeSession, untilPresentationTimeStamp: .invalid)
            if status != noErr {
                recordFailure(Gate0EncoderError.completion(status).description)
            }
            allowFrameReorderingReadback = copyBoolProperty(
                activeSession,
                key: kVTCompressionPropertyKey_AllowFrameReordering
            )
            hardwareAccelerationReadback = copyBoolProperty(
                activeSession,
                key: kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder
            )
            profileLevelReadback = copyStringProperty(
                activeSession,
                key: kVTCompressionPropertyKey_ProfileLevel
            )
            callbackContext.stopAndWait()
            VTCompressionSessionInvalidate(activeSession)
        } else {
            callbackContext.stopAndWait()
        }

        lock.lock()
        session = nil
        lock.unlock()
        callbackContext.encoder = nil
        releaseCallbackContext()
        closeFileIfOpen()

        lock.lock()
        let capturedFrames = frames
        let capturedParameterSets = parameterSetMetadata
        let capturedExtensions = formatExtensions
        let capturedFailure = failure
        let capturedPTSMonotonic = outputPTSMonotonic
        let capturedDroppedFrames = droppedFrames
        lock.unlock()

        var finalFailure = capturedFailure
        if finalFailure == nil && capturedFrames.isEmpty {
            finalFailure = "encoder produced no frames"
        }
        if finalFailure == nil && capturedParameterSets.isEmpty {
            finalFailure = "format description exposed no parameter sets"
        }
        let requiredParameterSets = codec == "hevc" ? ["VPS", "SPS", "PPS"] : ["SPS", "PPS"]
        let presentParameterSets = Set(capturedParameterSets.map(\.type))
        let missingParameterSets = requiredParameterSets.filter { !presentParameterSets.contains($0) }
        if finalFailure == nil && !missingParameterSets.isEmpty {
            finalFailure = "missing required parameter sets: \(missingParameterSets.joined(separator: ", "))"
        }
        if finalFailure == nil && !capturedFrames.contains(where: \.keyFrame) {
            finalFailure = "encoder produced no keyframe"
        }
        if finalFailure == nil && allowFrameReorderingReadback != false {
            finalFailure = "encoder did not verify AllowFrameReordering=false"
        }
        if finalFailure == nil && hardwareAccelerationReadback != true {
            finalFailure = "encoder did not verify hardware acceleration"
        }
        if finalFailure == nil && !capturedPTSMonotonic {
            finalFailure = "encoded presentation timestamps were not monotonic"
        }
        if finalFailure == nil && capturedDroppedFrames > 0 {
            finalFailure = "encoder reported \(capturedDroppedFrames) dropped frame(s)"
        }
        let colorKeys = [
            kCMFormatDescriptionExtension_ColorPrimaries as String,
            kCMFormatDescriptionExtension_TransferFunction as String,
            kCMFormatDescriptionExtension_YCbCrMatrix as String,
        ]
        let missingColorKeys = colorKeys.filter { capturedExtensions[$0] == nil }
        if finalFailure == nil && !missingColorKeys.isEmpty {
            finalFailure = "format description lacked color metadata keys: \(missingColorKeys.joined(separator: ", "))"
        }

        let metadata = EncodedArtifactMetadata(
            codec: codec,
            width: width,
            height: height,
            pixelFormat: fourCC(pixelFormat),
            realTimeRequested: true,
            allowFrameReorderingRequested: false,
            allowFrameReorderingReadback: allowFrameReorderingReadback,
            hardwareAccelerationRequired: true,
            hardwareAccelerationReadback: hardwareAccelerationReadback,
            profileLevelReadback: profileLevelReadback,
            requestedColorMetadata: requestedColorMetadata,
            formatDescriptionExtensions: capturedExtensions,
            parameterSets: capturedParameterSets,
            frames: capturedFrames,
            audioRequested: false
        )

        do {
            let data = try JSONEncoder.pretty.encode(metadata)
            try data.write(to: metadataURL, options: .atomic)
        } catch {
            if finalFailure == nil {
                finalFailure = "metadata write failed: \(error.localizedDescription)"
            }
        }

        let keyFrameCount = capturedFrames.filter(\.keyFrame).count
        var colorMetadata = requestedColorMetadata
        for key in colorKeys {
            if let value = capturedExtensions[key] {
                colorMetadata["format.\(key)"] = value
            }
        }

        return ArtifactRecord(
            codec: codec,
            path: bitstreamURL.path,
            metadataPath: metadataURL.path,
            frameCount: capturedFrames.count,
            keyFrameCount: keyFrameCount,
            parameterSetCount: capturedParameterSets.count,
            noBFramePropertyVerified: allowFrameReorderingReadback == false,
            outputPTSMonotonic: capturedPTSMonotonic,
            colorMetadata: colorMetadata,
            failure: finalFailure
        )
    }

    fileprivate func handleOutput(
        status: OSStatus,
        infoFlags: VTEncodeInfoFlags,
        sampleBuffer: CMSampleBuffer?
    ) {
        if status != noErr {
            recordFailure("compression callback failed with OSStatus \(status)")
            return
        }
        if infoFlags.contains(.frameDropped) {
            lock.lock()
            droppedFrames += 1
            lock.unlock()
        }
        guard let sampleBuffer else {
            recordFailure("compression callback returned no sample buffer")
            return
        }

        do {
            try append(sampleBuffer: sampleBuffer)
        } catch {
            recordFailure("\(error)")
        }
    }

    private func createSession() throws {
        let codecType: CMVideoCodecType
        let profileLevel: String
        switch codec {
        case "hevc":
            codecType = kCMVideoCodecType_HEVC
            profileLevel = kVTProfileLevel_HEVC_Main_AutoLevel as String
        case "h264":
            codecType = kCMVideoCodecType_H264
            profileLevel = kVTProfileLevel_H264_High_AutoLevel as String
        default:
            throw Gate0EncoderError.unsupportedCodec(codec)
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
            kCVPixelBufferPixelFormatTypeKey as String: NSNumber(value: pixelFormat),
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
        ]

        var createdSession: VTCompressionSession?
        callbackContext.encoder = self
        let retainedContext = Unmanaged.passRetained(callbackContext)
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: Int32(width),
            height: Int32(height),
            codecType: codecType,
            encoderSpecification: encoderSpecification as CFDictionary,
            imageBufferAttributes: sourceAttributes as CFDictionary,
            compressedDataAllocator: kCFAllocatorDefault,
            outputCallback: gate0VTCompressionOutputCallback,
            refcon: retainedContext.toOpaque(),
            compressionSessionOut: &createdSession
        )
        guard status == noErr, let createdSession else {
            retainedContext.release()
            throw Gate0EncoderError.sessionCreation(status)
        }
        retainedCallbackContext = retainedContext

        for (key, value) in properties {
            let propertyStatus = VTSessionSetProperty(
                createdSession,
                key: key as CFString,
                value: value as AnyObject
            )
            guard propertyStatus == noErr else {
                VTCompressionSessionInvalidate(createdSession)
                callbackContext.stopAndWait()
                callbackContext.encoder = nil
                releaseCallbackContext()
                throw Gate0EncoderError.sessionProperty(key: key, status: propertyStatus)
            }
        }

        let prepareStatus = VTCompressionSessionPrepareToEncodeFrames(createdSession)
        guard prepareStatus == noErr else {
            VTCompressionSessionInvalidate(createdSession)
            callbackContext.stopAndWait()
            callbackContext.encoder = nil
            releaseCallbackContext()
            throw Gate0EncoderError.sessionPreparation(prepareStatus)
        }

        allowFrameReorderingReadback = copyBoolProperty(
            createdSession,
            key: kVTCompressionPropertyKey_AllowFrameReordering
        )
        profileLevelReadback = copyStringProperty(
            createdSession,
            key: kVTCompressionPropertyKey_ProfileLevel
        )
        session = createdSession
    }

    private func releaseCallbackContext() {
        retainedCallbackContext?.release()
        retainedCallbackContext = nil
    }

    private func closeFileIfOpen() {
        guard !fileClosed else { return }
        fileClosed = true
        do {
            try fileHandle.synchronize()
        } catch {
            recordFailure("bitstream synchronize failed: \(error.localizedDescription)")
        }
        do {
            try fileHandle.close()
        } catch {
            recordFailure("bitstream close failed: \(error.localizedDescription)")
        }
    }

    private func append(sampleBuffer: CMSampleBuffer) throws {
        guard let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer) else {
            throw Gate0EncoderError.invalidSample("missing format description")
        }
        guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else {
            throw Gate0EncoderError.invalidSample("missing block buffer")
        }

        let totalLength = CMBlockBufferGetDataLength(dataBuffer)
        guard totalLength > 0 else {
            throw Gate0EncoderError.invalidSample("empty block buffer")
        }
        var payload = Data(count: totalLength)
        let copyStatus = payload.withUnsafeMutableBytes { rawBuffer -> OSStatus in
            guard let baseAddress = rawBuffer.baseAddress else { return -1 }
            return CMBlockBufferCopyDataBytes(dataBuffer, atOffset: 0, dataLength: totalLength, destination: baseAddress)
        }
        guard copyStatus == noErr else {
            throw Gate0EncoderError.invalidSample("CMBlockBufferCopyDataBytes returned \(copyStatus)")
        }

        lock.lock()
        if !parameterSetsWritten {
            do {
                let extracted = try extractParameterSets(from: formatDescription)
                var header = Data()
                for parameterSet in extracted.sets {
                    header.append(AnnexB.startCode)
                    header.append(parameterSet)
                }
                try fileHandle.write(contentsOf: header)
                parameterSetMetadata = extracted.metadata
                formatExtensions = extractFormatExtensions(formatDescription)
                parameterSetsWritten = true
            } catch {
                lock.unlock()
                throw error
            }
        }
        let nalUnitHeaderLength = currentNALUnitHeaderLength
        lock.unlock()

        let annexB: Data
        do {
            annexB = try AnnexB.convertLengthPrefixed(payload, nalUnitHeaderLength: nalUnitHeaderLength)
        } catch {
            throw Gate0EncoderError.conversion("\(error)")
        }

        let presentationTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let duration = CMSampleBufferGetDuration(sampleBuffer)
        let keyFrame = isKeyFrame(sampleBuffer)
        let frameIndex: Int

        lock.lock()
        defer { lock.unlock() }
        if lastOutputPTS.isValid && CMTimeCompare(presentationTime, lastOutputPTS) <= 0 {
            outputPTSMonotonic = false
        }
        lastOutputPTS = presentationTime
        frameIndex = frames.count
        do {
            try fileHandle.write(contentsOf: annexB)
        } catch {
            throw Gate0EncoderError.write(error.localizedDescription)
        }
        let presentationSeconds = CMTimeGetSeconds(presentationTime)
        let durationSeconds = CMTimeGetSeconds(duration)
        frames.append(EncodedFrameMetadata(
            index: frameIndex,
            presentationTimeValue: presentationTime.value,
            presentationTimeScale: presentationTime.timescale,
            presentationSeconds: presentationSeconds.isFinite ? presentationSeconds : nil,
            durationSeconds: durationSeconds.isFinite ? durationSeconds : nil,
            byteCount: annexB.count,
            keyFrame: keyFrame
        ))
    }

    private var currentNALUnitHeaderLength = 4

    private func extractParameterSets(
        from formatDescription: CMFormatDescription
    ) throws -> (sets: [Data], metadata: [ParameterSetMetadata]) {
        var count: Int = 0
        var nalUnitHeaderLength: Int32 = 4

        switch codec {
        case "hevc":
            var status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                formatDescription,
                parameterSetIndex: 0,
                parameterSetPointerOut: nil,
                parameterSetSizeOut: nil,
                parameterSetCountOut: &count,
                nalUnitHeaderLengthOut: &nalUnitHeaderLength
            )
            guard status == noErr, count > 0 else {
                throw Gate0EncoderError.invalidSample("HEVC parameter set enumeration failed: \(status), count \(count)")
            }
            var sets: [Data] = []
            var metadata: [ParameterSetMetadata] = []
            for index in 0..<count {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    formatDescription,
                    parameterSetIndex: index,
                    parameterSetPointerOut: &pointer,
                    parameterSetSizeOut: &size,
                    parameterSetCountOut: &count,
                    nalUnitHeaderLengthOut: &nalUnitHeaderLength
                )
                guard status == noErr, let pointer, size > 0 else {
                    throw Gate0EncoderError.invalidSample("HEVC parameter set \(index) is unavailable")
                }
                let data = Data(bytes: pointer, count: size)
                sets.append(data)
                metadata.append(ParameterSetMetadata(
                    type: AnnexB.parameterSetType(codec: codec, nalUnit: data) ?? "UNKNOWN",
                    byteCount: size,
                    hex: data.hexString
                ))
            }
            currentNALUnitHeaderLength = Int(nalUnitHeaderLength)
            return (sets, metadata)
        case "h264":
            var status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                formatDescription,
                parameterSetIndex: 0,
                parameterSetPointerOut: nil,
                parameterSetSizeOut: nil,
                parameterSetCountOut: &count,
                nalUnitHeaderLengthOut: &nalUnitHeaderLength
            )
            guard status == noErr, count > 0 else {
                throw Gate0EncoderError.invalidSample("H.264 parameter set enumeration failed: \(status), count \(count)")
            }
            var sets: [Data] = []
            var metadata: [ParameterSetMetadata] = []
            for index in 0..<count {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    formatDescription,
                    parameterSetIndex: index,
                    parameterSetPointerOut: &pointer,
                    parameterSetSizeOut: &size,
                    parameterSetCountOut: &count,
                    nalUnitHeaderLengthOut: &nalUnitHeaderLength
                )
                guard status == noErr, let pointer, size > 0 else {
                    throw Gate0EncoderError.invalidSample("H.264 parameter set \(index) is unavailable")
                }
                let data = Data(bytes: pointer, count: size)
                sets.append(data)
                metadata.append(ParameterSetMetadata(
                    type: AnnexB.parameterSetType(codec: codec, nalUnit: data) ?? "UNKNOWN",
                    byteCount: size,
                    hex: data.hexString
                ))
            }
            currentNALUnitHeaderLength = Int(nalUnitHeaderLength)
            return (sets, metadata)
        default:
            throw Gate0EncoderError.unsupportedCodec(codec)
        }
    }

    private func extractFormatExtensions(_ formatDescription: CMFormatDescription) -> [String: String] {
        guard let extensions = CMFormatDescriptionGetExtensions(formatDescription) as NSDictionary? else {
            return [:]
        }
        var result: [String: String] = [:]
        for (key, value) in extensions {
            result[String(describing: key)] = stringify(value)
        }
        return result
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

    private func recordFailure(_ message: String) {
        lock.lock()
        if failure == nil {
            failure = message
        }
        lock.unlock()
    }

    private func copyBoolProperty(_ session: VTCompressionSession, key: CFString) -> Bool? {
        var value: CFTypeRef?
        let status = withUnsafeMutablePointer(to: &value) { pointer in
            VTSessionCopyProperty(
                session,
                key: key,
                allocator: kCFAllocatorDefault,
                valueOut: UnsafeMutableRawPointer(pointer)
            )
        }
        guard status == noErr, let number = value as? NSNumber else { return nil }
        return number.boolValue
    }

    private func copyStringProperty(_ session: VTCompressionSession, key: CFString) -> String? {
        var value: CFTypeRef?
        let status = withUnsafeMutablePointer(to: &value) { pointer in
            VTSessionCopyProperty(
                session,
                key: key,
                allocator: kCFAllocatorDefault,
                valueOut: UnsafeMutableRawPointer(pointer)
            )
        }
        guard status == noErr, let value else { return nil }
        return String(describing: value)
    }

    private func stringify(_ value: Any) -> String {
        if let string = value as? String {
            return string
        }
        if let number = value as? NSNumber {
            return number.stringValue
        }
        if let data = value as? Data {
            return data.hexString
        }
        return String(describing: value)
    }

    private func fourCC(_ value: OSType) -> String {
        let bytes: [UInt8] = [
            UInt8((value >> 24) & 0xFF),
            UInt8((value >> 16) & 0xFF),
            UInt8((value >> 8) & 0xFF),
            UInt8(value & 0xFF),
        ]
        let printable = bytes.map { byte -> String in
            (32...126).contains(byte) ? String(UnicodeScalar(byte)) : String(format: "\\x%02X", byte)
        }
        return printable.joined()
    }
}

private extension Data {
    var hexString: String {
        map { String(format: "%02x", $0) }.joined()
    }
}
