import Foundation

struct SelfTestCase: Codable {
    let name: String
    let passed: Bool
    let detail: String
}

struct SelfTestReport: Codable {
    let status: String
    let liveActionsExecuted: Bool
    let cases: [SelfTestCase]
}

enum SelfTestRunner {
    static func run() -> SelfTestReport {
        var cases: [SelfTestCase] = []

        do {
            let firstNAL: [UInt8] = [0x67, 0x64, 0x00, 0x1F]
            let secondNAL: [UInt8] = [0x68, 0xEE, 0x3C, 0x80]
            var input = Data([0x00, 0x00, 0x00, 0x04])
            input.append(contentsOf: firstNAL)
            input.append(contentsOf: [0x00, 0x00, 0x00, 0x04])
            input.append(contentsOf: secondNAL)

            let expected = Data([0x00, 0x00, 0x00, 0x01])
                + Data(firstNAL)
                + Data([0x00, 0x00, 0x00, 0x01])
                + Data(secondNAL)
            let actual = try AnnexB.convertLengthPrefixed(input, nalUnitHeaderLength: 4)
            cases.append(SelfTestCase(
                name: "length-prefixed-to-annex-b",
                passed: actual == expected,
                detail: actual == expected ? "converted two AVCC NAL units" : "unexpected Annex-B bytes"
            ))
        } catch {
            cases.append(SelfTestCase(
                name: "length-prefixed-to-annex-b",
                passed: false,
                detail: "threw: \(error)"
            ))
        }

        do {
            var invalid = Data([0x00, 0x00, 0x00, 0x08])
            invalid.append(contentsOf: [0x65, 0x01, 0x02])
            _ = try AnnexB.convertLengthPrefixed(invalid, nalUnitHeaderLength: 4)
            cases.append(SelfTestCase(
                name: "reject-invalid-nal-length",
                passed: false,
                detail: "accepted a NAL length larger than the remaining payload"
            ))
        } catch {
            cases.append(SelfTestCase(
                name: "reject-invalid-nal-length",
                passed: true,
                detail: "rejected: \(error)"
            ))
        }

        let passed = cases.allSatisfy(\.passed)
        return SelfTestReport(
            status: passed ? "passed" : "failed",
            liveActionsExecuted: false,
            cases: cases
        )
    }
}
