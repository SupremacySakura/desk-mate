import Foundation

enum AnnexBError: Error, CustomStringConvertible {
    case invalidNALUnitHeaderLength(Int)
    case truncatedNALLength(offset: Int, remaining: Int)
    case zeroLengthNALUnit(offset: Int)
    case invalidNALLength(offset: Int, declared: Int, remaining: Int)

    var description: String {
        switch self {
        case .invalidNALUnitHeaderLength(let length):
            return "NAL unit header length must be 1...4, got \(length)"
        case .truncatedNALLength(let offset, let remaining):
            return "truncated NAL length prefix at offset \(offset), \(remaining) byte(s) remain"
        case .zeroLengthNALUnit(let offset):
            return "zero-length NAL unit at offset \(offset)"
        case .invalidNALLength(let offset, let declared, let remaining):
            return "NAL length \(declared) at offset \(offset) exceeds \(remaining) remaining byte(s)"
        }
    }
}

enum AnnexB {
    static let startCode = Data([0x00, 0x00, 0x00, 0x01])

    static func containsStartCode(_ data: Data) -> Bool {
        data.starts(with: [0x00, 0x00, 0x01]) || data.starts(with: [0x00, 0x00, 0x00, 0x01])
    }

    static func convertLengthPrefixed(_ data: Data, nalUnitHeaderLength: Int) throws -> Data {
        guard (1...4).contains(nalUnitHeaderLength) else {
            throw AnnexBError.invalidNALUnitHeaderLength(nalUnitHeaderLength)
        }
        if containsStartCode(data) {
            return data
        }

        let bytes = [UInt8](data)
        var output = Data()
        output.reserveCapacity(bytes.count + 16)
        var offset = 0

        while offset < bytes.count {
            let remaining = bytes.count - offset
            guard remaining >= nalUnitHeaderLength else {
                throw AnnexBError.truncatedNALLength(offset: offset, remaining: remaining)
            }

            var length = 0
            for index in 0..<nalUnitHeaderLength {
                length = (length << 8) | Int(bytes[offset + index])
            }
            offset += nalUnitHeaderLength

            guard length > 0 else {
                throw AnnexBError.zeroLengthNALUnit(offset: offset - nalUnitHeaderLength)
            }
            let remainingPayload = bytes.count - offset
            guard length <= remainingPayload else {
                throw AnnexBError.invalidNALLength(
                    offset: offset - nalUnitHeaderLength,
                    declared: length,
                    remaining: remainingPayload
                )
            }

            output.append(startCode)
            output.append(contentsOf: bytes[offset..<(offset + length)])
            offset += length
        }

        return output
    }

    static func parameterSetType(codec: String, nalUnit: Data) -> String? {
        guard let first = nalUnit.first else { return nil }
        switch codec {
        case "h264":
            switch first & 0x1F {
            case 7: return "SPS"
            case 8: return "PPS"
            default: return nil
            }
        case "hevc":
            switch (first >> 1) & 0x3F {
            case 32: return "VPS"
            case 33: return "SPS"
            case 34: return "PPS"
            default: return nil
            }
        default:
            return nil
        }
    }
}
