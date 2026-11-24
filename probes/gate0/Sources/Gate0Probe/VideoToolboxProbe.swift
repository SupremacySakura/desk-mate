import CoreMedia
import Foundation
import VideoToolbox

enum VideoToolboxProbe {
    static func probe() -> VideoToolboxRecord {
        var encoderList: CFArray?
        let status = VTCopyVideoEncoderList(nil, &encoderList)
        let rawCandidates = encoderList as? [[String: Any]] ?? []

        let candidates: [VideoEncoderCandidate] = rawCandidates.compactMap { entry in
            guard let codecNumber = entry[kVTVideoEncoderList_CodecType as String] as? NSNumber else {
                return nil
            }
            let codecType = codecNumber.uint32Value
            let codec: String
            switch codecType {
            case kCMVideoCodecType_HEVC:
                codec = "hevc"
            case kCMVideoCodecType_H264:
                codec = "h264"
            default:
                return nil
            }

            return VideoEncoderCandidate(
                codec: codec,
                name: entry[kVTVideoEncoderList_EncoderName as String] as? String
                    ?? entry[kVTVideoEncoderList_CodecName as String] as? String
                    ?? "unknown",
                encoderID: entry[kVTVideoEncoderList_EncoderID as String] as? String ?? "unknown",
                hardwareAccelerated: (entry[kVTVideoEncoderList_IsHardwareAccelerated as String] as? NSNumber)?.boolValue,
                supportsFrameReordering: (entry[kVTVideoEncoderList_SupportsFrameReordering as String] as? NSNumber)?.boolValue
            )
        }

        let hardwareHEVC = candidates.contains { $0.codec == "hevc" && $0.hardwareAccelerated == true }
        let hardwareH264 = candidates.contains { $0.codec == "h264" && $0.hardwareAccelerated == true }

        return VideoToolboxRecord(
            encoderListStatus: status,
            reportedCandidateCount: rawCandidates.count,
            hevcCandidateFound: candidates.contains { $0.codec == "hevc" },
            h264CandidateFound: candidates.contains { $0.codec == "h264" },
            hardwareHEVCCandidateFound: hardwareHEVC,
            hardwareH264CandidateFound: hardwareH264,
            candidates: candidates,
            note: "The encoder list is advisory. A live VTCompressionSession is required to prove hardware HEVC/H.264 availability and accepted properties; an empty H.264/HEVC list is not by itself a gate failure."
        )
    }
}
