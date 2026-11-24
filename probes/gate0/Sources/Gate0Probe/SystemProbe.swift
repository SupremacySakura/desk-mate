import CoreGraphics
import Foundation

enum SystemProbe {
    static func environment() -> EnvironmentRecord {
        let productName = command("/usr/bin/sw_vers", ["-productName"])
        let product = command("/usr/bin/sw_vers", ["-productVersion"])
        let build = command("/usr/bin/sw_vers", ["-buildVersion"])
        let kernel = command("/usr/bin/uname", ["-r"])
        let xcode = command("/usr/bin/xcodebuild", ["-version"])
        let hardware = hardwareSummary()

        let xcodeLines = xcode.output.split(separator: "\n").map(String.init)
        let xcodeVersion = xcodeLines.first?.replacingOccurrences(of: "Xcode ", with: "") ?? "unavailable"
        let xcodeBuild = xcodeLines.first(where: { $0.hasPrefix("Build version ") })?
            .replacingOccurrences(of: "Build version ", with: "") ?? "unavailable"

        return EnvironmentRecord(
            productName: productName.output.isEmpty ? "macOS" : productName.output,
            productVersion: product.output.isEmpty ? ProcessInfo.processInfo.operatingSystemVersionString : product.output,
            buildVersion: build.output.isEmpty ? "unavailable" : build.output,
            architecture: unameMachine(),
            modelIdentifier: hardware.model,
            modelName: hardware.name,
            chip: hardware.chip,
            kernelRelease: kernel.output.isEmpty ? "unavailable" : kernel.output,
            xcodeVersion: xcodeVersion,
            xcodeBuild: xcodeBuild
        )
    }

    static func screenRecordingPreflight() -> Bool {
        CGPreflightScreenCaptureAccess()
    }

    static func requestScreenRecordingPermission() -> Bool {
        CGRequestScreenCaptureAccess()
    }

    static func signingSummary() -> SigningRecord {
        let identities = command("/usr/bin/security", ["find-identity", "-v", "-p", "codesigning"])
        let developerIDLines = identities.output
            .split(separator: "\n")
            .filter { $0.contains("Developer ID Application:") }
        let notary = command("/usr/bin/xcrun", ["notarytool", "--version"])
        let gatekeeper = command("/usr/sbin/spctl", ["--status"])

        let signing = developerIDLines.isEmpty ? "pending" : "not-executed"
        let notarization = developerIDLines.isEmpty ? "pending" : "not-executed"
        let gatekeeperState = developerIDLines.isEmpty ? "not-executed" : "not-executed"
        let detail: String
        if developerIDLines.isEmpty {
            detail = "No Developer ID Application identity is installed; cannot sign or submit this probe."
        } else {
            detail = "Developer ID identity detected, but this invocation did not sign, submit, staple, or assess an artifact."
        }

        return SigningRecord(
            developerIDAvailable: !developerIDLines.isEmpty,
            developerIDIdentityCount: developerIDLines.count,
            notarytoolAvailable: notary.status == 0,
            gatekeeperAssessmentEnabled: gatekeeper.output.contains("enabled"),
            signingStatus: signing,
            notarizationStatus: notarization,
            gatekeeperStatus: gatekeeperState,
            detail: detail
        )
    }

    static func command(_ executable: String, _ arguments: [String]) -> (status: Int32, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        let stdout = Pipe()
        let stderr = Pipe()
        process.standardOutput = stdout
        process.standardError = stderr
        do {
            try process.run()
            process.waitUntilExit()
            let out = stdout.fileHandleForReading.readDataToEndOfFile()
            _ = stderr.fileHandleForReading.readDataToEndOfFile()
            let text = String(decoding: out, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            return (process.terminationStatus, text)
        } catch {
            return (-1, "")
        }
    }

    private static func unameMachine() -> String {
        var systemInfo = utsname()
        uname(&systemInfo)
        return withUnsafePointer(to: &systemInfo.machine) {
            $0.withMemoryRebound(to: CChar.self, capacity: 1) {
                String(cString: $0)
            }
        }
    }

    private static func hardwareSummary() -> (model: String, name: String, chip: String) {
        let result = command("/usr/sbin/system_profiler", ["SPHardwareDataType", "-json"])
        if let data = result.output.data(using: .utf8),
           let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let entries = root["SPHardwareDataType"] as? [[String: Any]],
           let hardware = entries.first {
            let model = hardware["machine_model"] as? String ?? "unavailable"
            let name = hardware["machine_name"] as? String ?? "unavailable"
            let chip = hardware["chip_type"] as? String ?? "unavailable"
            if model != "unavailable" {
                return (model, name, chip)
            }
        }

        let ioreg = command("/usr/sbin/ioreg", ["-c", "IOPlatformExpertDevice", "-d", "2"])
        if let range = ioreg.output.range(of: #""model" = <"([^"]+)">"#, options: .regularExpression) {
            let match = String(ioreg.output[range])
            let model = match
                .replacingOccurrences(of: #""model" = <""#, with: "")
                .replacingOccurrences(of: #"">"#, with: "")
            return (model, "unavailable", "unavailable")
        }
        return ("unavailable", "unavailable", "unavailable")
    }
}
