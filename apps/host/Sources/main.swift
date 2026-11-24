import AppKit
import CoreMedia
import CoreGraphics
import Foundation

struct HostOptions {
    var displayID: UInt32 = 41
    var createDisplay = false
    var width = 2800
    var height = 1840
    var frameRate = 60.0
    var codec = "hevc"
    var listDisplays = false
    var dryRun = false
}

enum CLIError: Error, CustomStringConvertible {
    case invalid(String)

    var description: String {
        switch self {
        case .invalid(let message): return message
        }
    }
}

enum CLI {
    static func parse(_ arguments: [String]) throws -> HostOptions {
        var options = HostOptions()
        var index = 1
        while index < arguments.count {
            let argument = arguments[index]
            func value() throws -> String {
                guard index + 1 < arguments.count else {
                    throw CLIError.invalid("missing value for \(argument)")
                }
                index += 1
                return arguments[index]
            }
            switch argument {
            case "--display-id":
                guard let parsed = UInt32(try value()) else {
                    throw CLIError.invalid("--display-id requires a number")
                }
                options.displayID = parsed
            case "--create":
                options.createDisplay = true
            case "--width":
                guard let parsed = Int(try value()), parsed > 0 else {
                    throw CLIError.invalid("--width requires a positive number")
                }
                options.width = parsed
            case "--height":
                guard let parsed = Int(try value()), parsed > 0 else {
                    throw CLIError.invalid("--height requires a positive number")
                }
                options.height = parsed
            case "--fps":
                guard let parsed = Double(try value()), parsed > 0 else {
                    throw CLIError.invalid("--fps requires a positive number")
                }
                options.frameRate = parsed
            case "--codec":
                let parsed = try value()
                guard parsed == "hevc" || parsed == "h264" else {
                    throw CLIError.invalid("--codec must be hevc or h264")
                }
                options.codec = parsed
            case "--list-displays":
                options.listDisplays = true
            case "--dry-run":
                options.dryRun = true
            case "--help", "-h":
                print(usage)
                exit(0)
            default:
                throw CLIError.invalid("unknown argument: \(argument)")
            }
            index += 1
        }
        return options
    }

    static let usage = """
    DeskMate Host

    Usage: DeskMate [options]

      --display-id N   borrow an existing Extended Display (default 41)
      --create         create a CGVirtualDisplay instead of borrowing one
      --width N        capture width in pixels (default 2800)
      --height N       capture height in pixels (default 1840)
      --fps N          capture frame rate (default 60)
      --codec NAME     hevc (default) or h264
      --list-displays  print the online display inventory and exit
      --dry-run        attach the display and capture locally, no USB session

    The app never destroys a display it did not create.
    """
}

final class AppController: NSObject, NSApplicationDelegate {
    private let options: HostOptions
    private var statusItem: NSStatusItem?
    private var stateItem: NSMenuItem?
    private var profileItem: NSMenuItem?
    private var framesItem: NSMenuItem?
    private var inputItem: NSMenuItem?
    private var errorItem: NSMenuItem?
    private var displaySession: DisplaySession?
    private var capture: CapturePipeline?
    private var bridge: OpaquePointer?
    private var timer: Timer?
    private var lastSnapshot = "starting"
    private var fatalError: String?
    private var captureWanted = false
    private var inputWanted = false
    private var lastReportedEncoded = -1
    private var lastReportTick = Date()
    private var captureTask: Task<Void, Never>?

    init(options: HostOptions) {
        self.options = options
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        installStatusItem()

        guard ensurePermissions() else {
            return
        }
        do {
            try attachDisplay()
        } catch {
            fail("\(error)")
            return
        }

        if options.dryRun {
            captureWanted = true
            lastSnapshot = "{\"state\":\"dry-run\",\"running\":true,\"negotiated\":\"local only\",\"inputEvents\":0,\"error\":\"\"}"
            Task { await startCaptureIfNeeded() }
            startTimer()
            return
        }

        createBridge()
        observeSystemEvents()
        startTimer()
        Task { await startCaptureIfNeeded() }
    }

    private func observeSystemEvents() {
        let workspace = NSWorkspace.shared.notificationCenter
        workspace.addObserver(
            forName: NSWorkspace.screensDidSleepNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.handleSystemEvent(DM_HOST_EVENT_SCREEN_LOCKED)
        }
        workspace.addObserver(
            forName: NSWorkspace.screensDidWakeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.handleSystemEvent(DM_HOST_EVENT_SCREEN_UNLOCKED)
        }
        workspace.addObserver(
            forName: NSWorkspace.willSleepNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.handleSystemEvent(DM_HOST_EVENT_WILL_SLEEP)
        }

        let distributed = DistributedNotificationCenter.default()
        distributed.addObserver(
            forName: NSNotification.Name("com.apple.screenIsLocked"),
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.handleSystemEvent(DM_HOST_EVENT_SCREEN_LOCKED)
        }
        distributed.addObserver(
            forName: NSNotification.Name("com.apple.screenIsUnlocked"),
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.handleSystemEvent(DM_HOST_EVENT_SCREEN_UNLOCKED)
        }
    }

    private func handleSystemEvent(_ event: Int) {
        guard let bridge else { return }
        let status = dm_bridge_host_event(bridge, Int32(event))
        if status != 0 {
            NSLog("DeskMate: host event %d not applied (status %d)", Int32(event), status)
        }
        if event == DM_HOST_EVENT_WILL_SLEEP {
            captureWanted = false
            stopCapture()
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        timer?.invalidate()
        timer = nil
        if let bridge {
            dm_bridge_stop(bridge)
        }
        let semaphore = DispatchSemaphore(value: 0)
        Task {
            await capture?.stop()
            semaphore.signal()
        }
        _ = semaphore.wait(timeout: .now() + 5)
        capture = nil
        displaySession?.destroy()
        displaySession = nil
        if let bridge {
            dm_bridge_destroy(bridge)
            self.bridge = nil
        }
    }

    private func installStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.title = "DM"
        let menu = NSMenu()
        menu.addItem(withTitle: "DeskMate", action: nil, keyEquivalent: "")

        func statusLine(_ title: String) -> NSMenuItem {
            let line = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            line.isEnabled = false
            menu.addItem(line)
            return line
        }

        stateItem = statusLine("State: starting")
        profileItem = statusLine("Stream: \(options.width)x\(options.height)@\(Int(options.frameRate)) \(options.codec)")
        framesItem = statusLine("Encoded frames: 0")
        inputItem = statusLine("Input events: 0")
        errorItem = statusLine("Error: none")
        errorItem?.isHidden = true

        menu.addItem(.separator())
        let copy = NSMenuItem(title: "Copy Status", action: #selector(copyStatus), keyEquivalent: "c")
        copy.target = self
        menu.addItem(copy)
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit DeskMate", action: #selector(quit), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
        item.menu = menu
        statusItem = item
    }

    @objc private func copyStatus() {
        let text = "\(stateItem?.title ?? "")\n\(profileItem?.title ?? "")\n\(framesItem?.title ?? "")\n\(inputItem?.title ?? "")\n\(errorItem?.isHidden == false ? (errorItem?.title ?? "") : "")\n\(lastSnapshot)"
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    private func ensurePermissions() -> Bool {
        if !CGPreflightScreenCaptureAccess() {
            let granted = CGRequestScreenCaptureAccess()
            if !granted {
                fail("Screen Recording permission is required; enable it in System Settings and relaunch")
                return false
            }
        }
        if !options.dryRun {
            let promptKey = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
            let trusted = AXIsProcessTrustedWithOptions([promptKey: true] as CFDictionary)
            if !trusted {
                NSLog("DeskMate: Accessibility permission missing; input injection stays disabled until granted")
            }
        }
        return true
    }

    private func attachDisplay() throws {
        if options.createDisplay {
            let scale = 2
            let session = try DisplaySession.create(
                logicalWidth: options.width / scale,
                logicalHeight: options.height / scale,
                scale: scale,
                refreshRate: options.frameRate
            )
            displaySession = session
        } else {
            displaySession = try DisplaySession.attach(displayID: options.displayID)
        }
        if let info = displaySession?.info {
            NSLog("DeskMate: attached display \(info.displayID) \(info.summary)")
        }
    }

    private func createBridge() {
        var config = DMBridgeConfig()
        config.device_selector = nil
        config.width = Int32(options.width)
        config.height = Int32(options.height)
        config.refresh_hz = Int32(options.frameRate.rounded())
        config.prefer_hevc = options.codec == "hevc" ? 1 : 0

        let bridge = dm_bridge_create(&config)
        guard let bridge else {
            fail("failed to create USB bridge")
            return
        }
        self.bridge = bridge
        dm_bridge_set_log(bridge, { message, _ in
            guard let message else { return }
            NSLog("DeskMate bridge: %@", String(cString: message))
        }, nil)
        dm_bridge_set_intent_callback(bridge, { capture, input, context in
            guard let context else { return }
            let controller = Unmanaged<AppController>.fromOpaque(context).takeUnretainedValue()
            controller.applyIntent(capture: capture, input: input)
        }, Unmanaged.passUnretained(self).toOpaque())

        if dm_bridge_start(bridge) != 0 {
            fail("failed to start USB bridge")
        }
    }

    private func applyIntent(capture: Int32, input: Int32) {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            if capture >= 0 {
                self.captureWanted = capture != 0
                if self.captureWanted {
                    Task { await self.startCaptureIfNeeded() }
                } else {
                    self.stopCapture()
                }
            }
            if input >= 0 {
                self.inputWanted = input != 0
            }
        }
    }

    private func startTimer() {
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            self?.tick()
        }
    }

    private func tick() {
        if let bridge {
            if let snapshot = dm_bridge_snapshot(bridge) {
                lastSnapshot = String(cString: snapshot)
                free(snapshot)
            }
            if dm_bridge_needs_keyframe(bridge) != 0 {
                capture?.requestKeyframe()
            }
            drainInput(bridge)
        }
        updateTitle()
        reportProgress()
    }

    private func reportProgress() {
        let now = Date()
        guard now.timeIntervalSince(lastReportTick) >= 2 else { return }
        lastReportTick = now
        guard let capture else { return }
        let encoded = capture.encodedFrameCount
        guard encoded != lastReportedEncoded else {
            NSLog("DeskMate: capture stalled at \(encoded) encoded frames (received \(capture.receivedFrameCount))")
            return
        }
        lastReportedEncoded = encoded
        NSLog(
            "DeskMate: captured %d / encoded %d frames; %@",
            capture.receivedFrameCount,
            encoded,
            lastSnapshot
        )
    }

    private func drainInput(_ bridge: OpaquePointer) {
        guard inputWanted, let displaySession else { return }
        var buffer = [DMInputEvent](repeating: DMInputEvent(), count: 32)
        while true {
            let count = buffer.withUnsafeMutableBufferPointer { pointer -> Int32 in
                guard let base = pointer.baseAddress else { return 0 }
                return dm_bridge_take_input(bridge, base, Int32(pointer.count))
            }
            if count <= 0 { break }
            let bounds = displaySession.bounds()
            for index in 0..<Int(count) {
                _ = InputInjector.inject(DMInputEventValue(buffer[index]), displayBounds: bounds)
            }
            if count < Int32(buffer.count) { break }
        }
    }

    private func startCaptureIfNeeded() async {
        guard captureWanted, capture == nil, let displaySession else { return }
        let pipeline = CapturePipeline(
            displayID: displaySession.info.displayID,
            width: options.width,
            height: options.height,
            frameRate: options.frameRate,
            codec: options.codec,
            onFrame: { [weak self] frame in
                self?.submit(frame)
            },
            onFailure: { [weak self] message in
                self?.fail(message)
            }
        )
        do {
            try await pipeline.start()
            capture = pipeline
            NSLog("DeskMate: capture started at \(options.width)x\(options.height)@\(Int(options.frameRate)) \(options.codec)")
        } catch {
            fail("capture start failed: \(error)")
        }
    }

    private func stopCapture() {
        guard let pipeline = capture else { return }
        capture = nil
        Task { await pipeline.stop() }
    }

    private func submit(_ frame: EncodedFrame) {
        guard let bridge, let capture else { return }
        let seconds = CMTimeGetSeconds(frame.presentationTime)
        let ptsUs = seconds.isFinite && seconds >= 0
            ? UInt64(seconds * 1_000_000)
            : DispatchTime.now().uptimeNanoseconds / 1_000
        _ = frame.annexB.withUnsafeBytes { raw -> Int32 in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return -1 }
            return dm_bridge_submit_video(
                bridge,
                base,
                frame.annexB.count,
                ptsUs,
                frame.keyframe ? 1 : 0
            )
        }
        _ = capture
    }

    private func updateTitle() {
        guard let button = statusItem?.button else { return }
        let snapshot = parseSnapshot(lastSnapshot)
        let state = snapshot?["state"] as? String ?? (options.dryRun ? "dry-run" : "starting")
        let encoded = capture?.encodedFrameCount ?? 0
        let inputs = snapshot?["inputEvents"] as? Int ?? 0
        let negotiated = snapshot?["negotiated"] as? String ?? "none"

        stateItem?.title = "State: \(state)"
        profileItem?.title = "Stream: \(negotiated == "none" ? "\(options.width)x\(options.height)@\(Int(options.frameRate)) \(options.codec)" : negotiated)"
        framesItem?.title = "Encoded frames: \(encoded)"
        inputItem?.title = "Input events: \(inputs)"
        let message = fatalError ?? (snapshot?["error"] as? String ?? "")
        errorItem?.title = "Error: \(message)"
        errorItem?.isHidden = message.isEmpty

        if let fatalError {
            button.title = "DM !"
            button.toolTip = fatalError
            return
        }
        button.title = captureWanted ? "DM ●" : "DM ○"
        button.toolTip = lastSnapshot
    }

    private func parseSnapshot(_ text: String) -> [String: Any]? {
        guard let data = text.data(using: .utf8) else { return nil }
        return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
    }

    private func fail(_ message: String) {
        if fatalError == nil {
            fatalError = message
        }
        NSLog("DeskMate error: %@", message)
        DispatchQueue.main.async { [weak self] in
            self?.updateTitle()
        }
    }
}

let arguments = CommandLine.arguments
let options: HostOptions
do {
    options = try CLI.parse(arguments)
} catch {
    FileHandle.standardError.write(Data("\(error)\n\n\(CLI.usage)\n".utf8))
    exit(2)
}

if options.listDisplays {
    print("Online displays:")
    for line in DisplayInventory.describeAll() {
        print(line)
    }
    exit(0)
}

let application = NSApplication.shared
let controller = AppController(options: options)
application.delegate = controller
application.run()
