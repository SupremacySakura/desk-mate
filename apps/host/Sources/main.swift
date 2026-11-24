import AppKit
import CoreMedia
import CoreGraphics
import Foundation
import SwiftUI

struct HostOptions {
    var displayID: UInt32?
    var createDisplay = false
    var width = 2800
    var height = 1840
    var frameRate = 60.0
    var frameRateExplicit = false
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
                options.frameRateExplicit = true
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

      --display-id N   borrow this Extended Display (default: saved or sole available display)
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
    private var window: NSWindow?
    private let windowModel = HostWindowModel()
    private var selectedSource: UInt32?
    private var videoEnabled = true
    private var allowInput = false
    private var captureStarting = false
    private static let displayPreference = "preferredExtendedDisplayID"
    private var displaySession: DisplaySession?
    private var capture: CapturePipeline?
    private var bridge: OpaquePointer?
    private var timer: Timer?
    private var keyframeTimer: Timer?
    private var nextKeyframeRetry = 0.0
    private var activityToken: NSObjectProtocol?
    private var lastSnapshot = "starting"
    private var fatalError: String?
    private var captureWanted = false
    private var inputWanted = false
    private var lastReportedEncoded = -1
    private var lastReportTick = Date()
    private var captureTask: Task<Void, Never>?

    init(options: HostOptions) {
        self.options = options
        self.selectedSource = options.displayID ??
            (UserDefaults.standard.object(forKey: Self.displayPreference) as? NSNumber)?.uint32Value
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Prevent macOS App Nap and timer coalescing from throttling capture
        // and USB streaming threads after DeskMate loses foreground focus.
        activityToken = ProcessInfo.processInfo.beginActivity(
            options: [.userInitiatedAllowingIdleSystemSleep, .latencyCritical],
            reason: "DeskMate real-time extended display streaming"
        )
        NSApp.setActivationPolicy(.regular)
        installStatusItem()
        installWindow()
        NSApp.activate(ignoringOtherApps: true)

        startTimer()
        if !options.dryRun { observeSystemEvents() }
        prepare()
    }

    private func prepare() {
        guard displaySession == nil || (!options.dryRun && bridge == nil) else { return }
        guard CGPreflightScreenCaptureAccess() else { syncWindow(); return }
        fatalError = nil
        do {
            if displaySession == nil { try attachDisplay() }
        } catch {
            fail("\(error)")
            syncWindow()
            return
        }
        if options.dryRun {
            captureWanted = true
            lastSnapshot = "{\"state\":\"dry-run\",\"negotiated\":\"local only\"}"
            Task { await startCaptureIfNeeded() }
        } else {
            createBridge()
        }
        syncWindow()
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
        if let activityToken {
            ProcessInfo.processInfo.endActivity(activityToken)
            self.activityToken = nil
        }
        timer?.invalidate()
        timer = nil
        keyframeTimer?.invalidate()
        keyframeTimer = nil
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
        let show = NSMenuItem(title: "显示窗口", action: #selector(showWindow), keyEquivalent: "o")
        show.target = self
        menu.addItem(show)
        menu.addItem(.separator())

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

    @objc private func showWindow() {
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func setVideo(_ enabled: Bool) {
        videoEnabled = enabled
        if enabled {
            Task { await startCaptureIfNeeded() }
        } else {
            stopCapture()
        }
        syncWindow()
    }

    private func openPermission(screen: Bool) {
        if screen { _ = CGRequestScreenCaptureAccess() }
        let privacy = screen ? "Privacy_ScreenCapture" : "Privacy_Accessibility"
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(privacy)") {
            NSWorkspace.shared.open(url)
        }
    }

    private func installWindow() {
        let frame = NSRect(x: 0, y: 0, width: 700, height: 500)
        let window = NSWindow(
            contentRect: frame,
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )
        window.title = "DeskMate"
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.isMovableByWindowBackground = true
        window.contentMinSize = NSSize(width: 610, height: 480)
        window.isReleasedWhenClosed = false
        window.center()

        window.contentView = NSHostingView(rootView: HostWindow(
            model: windowModel,
            chooseSource: { [weak self] id in self?.chooseSource(id) },
            setVideo: { [weak self] enabled in self?.setVideo(enabled) },
            setInput: { [weak self] enabled in
                self?.allowInput = enabled && AXIsProcessTrusted()
                self?.syncWindow()
            },
            openPermission: { [weak self] screen in self?.openPermission(screen: screen) },
            retry: { [weak self] in self?.retry() },
            copy: { [weak self] in self?.copyStatus() }
        ))
        self.window = window
        window.makeKeyAndOrderFront(nil)
    }

    private func chooseSource(_ id: UInt32?) {
        guard let id, displayChoices().contains(where: { $0.id == id }) else { return }
        selectedSource = id
        UserDefaults.standard.set(Int(id), forKey: Self.displayPreference)
        if displaySession == nil { prepare() }
        syncWindow()
    }

    private func retry() {
        if bridge == nil && (!options.dryRun || displaySession == nil) {
            prepare()
        } else {
            fatalError = nil
            Task { await startCaptureIfNeeded() }
        }
        syncWindow()
    }

    private func displayChoices() -> [DisplayChoice] {
        DisplayInventory.onlineDisplayIDs().compactMap { id in
            guard CGDisplayIsMain(id) == 0, CGDisplayIsActive(id) != 0,
                  CGDisplayIsInMirrorSet(id) == 0,
                  CGDisplayMirrorsDisplay(id) == kCGNullDirectDisplay,
                  let mode = CGDisplayCopyDisplayMode(id) else { return nil }
            let screen = NSScreen.screens.first {
                ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == id
            }
            return DisplayChoice(id: id, title: "\(screen?.localizedName ?? "扩展显示器") · \(mode.pixelWidth)×\(mode.pixelHeight)（\(id)）")
        }
    }

    private func syncWindow() {
        let snapshot = parseSnapshot(lastSnapshot)
        let state = snapshot?["state"] as? String ?? (options.dryRun ? "dry-run" : "starting")
        var view = HostWindowState()
        view.screenPermission = CGPreflightScreenCaptureAccess()
        view.inputPermission = AXIsProcessTrusted()
        view.videoEnabled = videoEnabled
        view.inputEnabled = allowInput && view.inputPermission
        view.input = view.inputEnabled ? (inputWanted ? "已启用" : "等待会话") : "未开启"
        view.choices = displayChoices()
        view.fixedSource = options.createDisplay || options.displayID != nil
        view.selected = view.choices.contains(where: { $0.id == selectedSource }) ? selectedSource : nil
        if let id = displaySession?.info.displayID {
            view.source = view.choices.first(where: { $0.id == id })?.title ?? "扩展显示器（\(id)）"
            view.needsRestart = !view.fixedSource && selectedSource != nil && selectedSource != id
        }
        if !view.screenPermission {
            (view.title, view.guidance, view.badge) = ("需要屏幕录制权限", "在系统设置中允许 DeskMate 录制屏幕，再返回重试。", "待授权")
            view.canRetry = true
        } else if displaySession == nil {
            view.title = "需要扩展显示器"
            view.guidance = view.choices.isEmpty ? "先在 Mac 上准备一块扩展显示器，再重试。DeskMate 不会采集主屏。" : "请在设置中选择要发送到鸿蒙平板的扩展显示器。"
            view.badge = "未就绪"
            view.canRetry = true
        } else if fatalError != nil {
            (view.title, view.guidance, view.badge) = ("暂时无法发送画面", "重试或打开诊断查看原因。", "出错")
            view.canRetry = true
        } else if options.dryRun {
            (view.title, view.guidance, view.badge) = ("本机采集测试", "仅在本机采集，不建立 USB 会话。", "测试模式")
        } else if state == "awaiting_client_hello" {
            (view.title, view.guidance, view.badge) = ("等待平板确认", "请在平板上打开 DeskMate，并在系统提示中允许 USB 配件连接。", "USB 已连接")
            view.linked = true
        } else if state == "streaming" && videoEnabled && capture != nil {
            (view.title, view.guidance, view.badge) = ("正在发送画面", "在平板上查看 Mac 的扩展桌面。", "会话已建立")
            view.linked = true
            view.video = "正在发送"
        } else if state.hasPrefix("awaiting_") || state == "streaming" || state == "paused" {
            view.linked = true
            view.badge = "USB 已连接"
            view.title = state == "paused" || !videoEnabled ? "画面发送已停止" : "正在准备画面"
            view.guidance = !videoEnabled ? "需要继续时，开启画面发送。" : "保持 USB 数据线连接，并确认 Mac 未锁屏。"
        }
        if !videoEnabled { view.video = "已关闭" }
        view.diagnostic = "连接：\(state)\n显示来源：\(view.source)\n编码帧：\(capture?.encodedFrameCount ?? 0)\n\(fatalError ?? "")\n\(lastSnapshot)"
        if windowModel.state != view { windowModel.state = view }
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
            let choices = displayChoices()
            let saved = choices.first { $0.id == selectedSource }?.id
            guard let id = options.displayID ?? saved ?? (choices.count == 1 ? choices.first?.id : nil) else {
                throw DisplaySessionError.unavailable("no extended display selected")
            }
            displaySession = try DisplaySession.attach(displayID: id)
            selectedSource = id
        }
        if let info = displaySession?.info {
            NSLog("DeskMate: attached display \(info.displayID) \(info.summary)")
        }
    }

    private func targetFrameRate() -> Double {
        if options.frameRateExplicit {
            return options.frameRate
        }
        guard let refresh = displaySession?.info.refreshRate, refresh >= 60.0 else {
            return options.frameRate
        }
        if refresh >= 115.0 { return 120.0 }
        if refresh >= 85.0 { return 90.0 }
        return 60.0
    }

    private func createBridge() {
        var config = DMBridgeConfig()
        config.device_selector = nil
        config.width = Int32(options.width)
        config.height = Int32(options.height)
        config.refresh_hz = Int32(targetFrameRate().rounded())
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
            dm_bridge_destroy(bridge)
            self.bridge = nil
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
        let keyframeTimer = Timer(timeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            self?.pollKeyframeRequest()
        }
        RunLoop.main.add(keyframeTimer, forMode: .common)
        self.keyframeTimer = keyframeTimer
    }

    private func pollKeyframeRequest() {
        guard let bridge, let capture, dm_bridge_needs_keyframe(bridge) != 0 else {
            nextKeyframeRetry = 0
            return
        }
        let now = ProcessInfo.processInfo.systemUptime
        guard now >= nextKeyframeRetry else { return }
        nextKeyframeRetry = now + 0.1
        capture.requestKeyframe()
    }

    private func tick() {
        if let bridge {
            if let snapshot = dm_bridge_snapshot(bridge) {
                lastSnapshot = String(cString: snapshot)
                free(snapshot)
            }
            drainInput(bridge)
        }
        updateTitle()
        syncWindow()
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
        let canInject = inputWanted && allowInput && AXIsProcessTrusted()
        var buffer = [DMInputEvent](repeating: DMInputEvent(), count: 32)
        while true {
            let count = buffer.withUnsafeMutableBufferPointer { pointer -> Int32 in
                guard let base = pointer.baseAddress else { return 0 }
                return dm_bridge_take_input(bridge, base, Int32(pointer.count))
            }
            if count <= 0 { break }
            if canInject, let displaySession {
                let bounds = displaySession.bounds()
                for index in 0..<Int(count) {
                    _ = InputInjector.inject(DMInputEventValue(buffer[index]), displayBounds: bounds)
                }
            }
            if count < Int32(buffer.count) { break }
        }
    }

    private func startCaptureIfNeeded() async {
        guard captureWanted, videoEnabled, capture == nil, !captureStarting,
              CGPreflightScreenCaptureAccess(), let displaySession else { return }
        captureStarting = true
        defer { captureStarting = false }
        let negotiatedHz = bridge.map { Int(dm_bridge_negotiated_refresh_hz($0)) } ?? 0
        let captureFrameRate = negotiatedHz > 0 ? Double(negotiatedHz) : targetFrameRate()
        let pipeline = CapturePipeline(
            displayID: displaySession.info.displayID,
            width: options.width,
            height: options.height,
            frameRate: captureFrameRate,
            codec: options.codec,
            keyframeRequestIDProvider: { [weak self] in
                guard let bridge = self?.bridge else { return 0 }
                return dm_bridge_keyframe_request_id(bridge)
            },
            onFrame: { [weak self] frame in
                self?.submit(frame)
            },
            onFailure: { [weak self] message in
                self?.fail(message)
            }
        )
        do {
            try await pipeline.start()
            guard captureWanted, videoEnabled else {
                await pipeline.stop()
                return
            }
            capture = pipeline
            NSLog("DeskMate: capture started at \(options.width)x\(options.height)@\(Int(captureFrameRate.rounded())) \(options.codec)")
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
