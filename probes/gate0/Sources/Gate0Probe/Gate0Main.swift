import Darwin
import Foundation

@main
struct Gate0Main {
    static func main() async {
        do {
            let options = try parseOptions(Array(CommandLine.arguments.dropFirst()))
            if options.listDisplays {
                let inventory = DisplayInventory.probe()
                let data = try JSONEncoder.pretty.encode(inventory)
                if options.json {
                    FileHandle.standardOutput.write(data)
                    FileHandle.standardOutput.write(Data("\n".utf8))
                } else {
                    DisplayInventory.printHuman(inventory)
                }
                exit(inventory.status == "failed" ? 2 : 0)
            }
            if options.selfTest {
                let result = SelfTestRunner.run()
                let data = try JSONEncoder.pretty.encode(result)
                if options.json {
                    FileHandle.standardOutput.write(data)
                    FileHandle.standardOutput.write(Data("\n".utf8))
                } else {
                    print("Gate 0 Annex-B self-test: \(result.status)")
                    for testCase in result.cases {
                        print("- \(testCase.name): \(testCase.passed ? "passed" : "failed") — \(testCase.detail)")
                    }
                }
                exit(result.status == "passed" ? 0 : 1)
            }
            if options.mode == .dryRun {
                let report = ReportFactory.dryRun(options: options)
                try emit(report, options: options, exitCode: 0)
            } else {
                let report = await LiveGate0.run(options: options)
                let code: Int32 = report.decision == .passed ? 0 : 2
                try emit(report, options: options, exitCode: code)
            }
        } catch let error as CLIError {
            FileHandle.standardError.write(Data("error: \(error.description)\n".utf8))
            exit(64)
        } catch {
            FileHandle.standardError.write(Data("error: \(error)\n".utf8))
            exit(1)
        }
    }

    static func emit(_ report: Gate0Report, options: ProbeOptions, exitCode: Int32) throws {
        if let path = options.reportPath {
            let url = URL(fileURLWithPath: path)
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            try encode(report).write(to: url)
        }

        if options.json {
            FileHandle.standardOutput.write(try encode(report))
            FileHandle.standardOutput.write(Data("\n".utf8))
        } else {
            printHuman(report, reportPath: options.reportPath)
        }
        exit(exitCode)
    }

    static func encode(_ report: Gate0Report) throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        return try encoder.encode(report)
    }

    static func printHuman(_ report: Gate0Report, reportPath: String?) {
        let env = report.environment
        let api = report.privateAPI
        print("DeskMate Gate 0 probe: \(report.decision.rawValue.uppercased()) (\(report.mode.rawValue))")
        print("Host: \(env.modelIdentifier), macOS \(env.productVersion) (\(env.buildVersion)), \(env.architecture)")
        print("Private API: \(api.staticAvailability) static availability; live creation \(api.liveCreation)")
        if report.capture.started {
            print("Screen capture: started; \(report.capture.framesReceived) frame(s); audio explicitly disabled")
        } else {
            print("Screen capture: not started; audio explicitly disabled")
        }
        print("Signing: \(report.signing.detail)")
        print("Decision: \(report.decisionReason)")
        print("Not executed:")
        for item in report.notExecuted {
            print("- \(item)")
        }
        if let reportPath {
            print("Report: \(reportPath)")
        }
        if report.decision == .passed {
            print("Gate 0 prerequisites are recorded as passed for this run.")
        } else {
            print("This report is not Gate 0 evidence.")
        }
    }
}

func parseOptions(_ arguments: [String]) throws -> ProbeOptions {
    var options = ProbeOptions()
    var index = 0
    var dryRunRequested = false
    var liveRequested = false

    func value(for flag: String) throws -> String {
        index += 1
        guard index < arguments.count else {
            throw CLIError(description: "\(flag) requires a value")
        }
        return arguments[index]
    }

    while index < arguments.count {
        let argument = arguments[index]
        switch argument {
        case "--dry-run":
            if liveRequested {
                throw CLIError(description: "--dry-run and --live cannot be combined")
            }
            dryRunRequested = true
            options.mode = .dryRun
        case "--self-test":
            options.selfTest = true
        case "--list-displays":
            options.listDisplays = true
        case "--live":
            if dryRunRequested {
                throw CLIError(description: "--dry-run and --live cannot be combined")
            }
            liveRequested = true
            options.mode = .live
        case "--allow-create-display":
            options.allowCreateDisplay = true
        case "--allow-screen-capture":
            options.allowScreenCapture = true
        case "--request-screen-recording-permission":
            options.requestScreenRecordingPermission = true
        case "--json":
            options.json = true
        case "--report":
            options.reportPath = try value(for: "--report")
        case "--output":
            options.outputDirectory = try value(for: "--output")
        case "--duration":
            let raw = try value(for: "--duration")
            guard let duration = Double(raw), duration > 0 else {
                throw CLIError(description: "--duration must be a positive number")
            }
            options.durationSeconds = duration
        case "--logical-width":
            let raw = try value(for: "--logical-width")
            guard let width = Int(raw), width > 0 else {
                throw CLIError(description: "--logical-width must be a positive integer")
            }
            options.logicalWidth = width
        case "--logical-height":
            let raw = try value(for: "--logical-height")
            guard let height = Int(raw), height > 0 else {
                throw CLIError(description: "--logical-height must be a positive integer")
            }
            options.logicalHeight = height
        case "--scale":
            let raw = try value(for: "--scale")
            guard let scale = Int(raw), scale == 1 || scale == 2 else {
                throw CLIError(description: "--scale must be 1 or 2")
            }
            options.scale = scale
        case "--fps":
            let raw = try value(for: "--fps")
            guard let fps = Double(raw), fps > 0, fps <= 120 else {
                throw CLIError(description: "--fps must be in (0, 120]")
            }
            options.refreshRate = fps
        case "--codec":
            let raw = try value(for: "--codec")
            guard let codec = CodecSelection(rawValue: raw) else {
                throw CLIError(description: "--codec must be both, hevc, or h264")
            }
            options.codecs = codec
        case "--reference-model":
            options.referenceModel = try value(for: "--reference-model")
        case "--reference-build":
            options.referenceBuild = try value(for: "--reference-build")
        case "--help", "-h":
            printHelp()
            exit(0)
        case "--use-main-display":
            throw CLIError(description: "unknown option: --use-main-display (main-display fallback is forbidden)")
        default:
            throw CLIError(description: "unknown option: \(argument)")
        }
        index += 1
    }

    if options.mode == .live {
        if options.selfTest || options.listDisplays {
            throw CLIError(description: "--self-test and --list-displays cannot be combined with --live")
        }
        if options.scale != 2 {
            throw CLIError(description: "Gate 0 live mode requires --scale 2 (HiDPI)")
        }
        var missing: [String] = []
        if !options.allowCreateDisplay {
            missing.append("--allow-create-display")
        }
        if !options.allowScreenCapture {
            missing.append("--allow-screen-capture")
        }
        if !missing.isEmpty {
            throw CLIError(description: "live mode requires explicit opt-in: \(missing.joined(separator: ", "))")
        }
    } else if options.requestScreenRecordingPermission {
        throw CLIError(description: "--request-screen-recording-permission requires --live")
    }

    return options
}

func printHelp() {
    print(
        """
        DeskMate Gate 0 macOS probe

        Default behavior is a read-only dry run. No display is created, no ScreenCaptureKit
        stream starts, and no audio is requested.

        Usage:
          gate0-probe [--dry-run] [--json] [--report PATH]
          gate0-probe --self-test [--json]
          gate0-probe --list-displays [--json]
          gate0-probe --live --allow-create-display --allow-screen-capture [options]

        Live options:
          --duration SECONDS         Capture duration, default 10
          --logical-width PX         HiDPI logical width, default 1400
          --logical-height PX        HiDPI logical height, default 920
          --scale 2                  HiDPI scale; Gate 0 requires 2
          --fps HZ                   Refresh/frame rate, default 60
          --codec both|hevc|h264     Sample output, default both
          --output DIRECTORY         Annex-B output directory, default artifacts
          --reference-model ID       Expected Reference Mac model identifier
          --reference-build BUILD    Expected macOS build
          --request-screen-recording-permission
                                    Explicitly prompt for Screen Recording access

        Safety:
          Live display/capture never runs without both --allow-create-display and
          --allow-screen-capture. Main-display fallback is not supported.
        """
    )
}
