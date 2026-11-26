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
    var sizeExplicit = false
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
                options.sizeExplicit = true
            case "--height":
                guard let parsed = Int(try value()), parsed > 0 else {
                    throw CLIError.invalid("--height requires a positive number")
                }
                options.height = parsed
                options.sizeExplicit = true
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
    private var popover: NSPopover?
    private var lastIconLinked = false
    private var lastIconStreaming = false
    private var window: NSWindow?
    private let windowModel = HostWindowModel()
    private var selectedSource: UInt32?
    private var autoVirtualDisplay = true
    private var displayPosition: DisplayPosition = .right
    private var preferredFrameRateHz = 0
    private var videoEnabled = true
    private var allowInput = true
    private var touchInputMode: TouchInputMode = .compatibleGestures
    private var touchCapability = InputInjector.capabilityStatus()
    private let startupManager = StartupServiceManager()
    private var startupStatus = StartupStatus()
    private let pairedStore = PairedDeviceStore()
    private let updateManager = UpdateManager()
    private var autoStartService = true
    private var serviceRunning = false
    private var serviceTransitioning = false
    private var onlineDeviceIDs: Set<String> = []
    private static let displayPreference = "preferredExtendedDisplayID"
    private static let autoVirtualPreference = "autoVirtualDisplay"
    private static let positionPreference = "virtualDisplayPosition"
    private static let frameRatePreference = "preferredFrameRateHz"
    private static let inputPreference = "allowReverseInput"
    private static let touchModePreference = "touchInputMode"
    private static let autoStartPreference = "autoStartService"

    private let inputDrainLock = NSLock()
    private var pendingInputDrainLinks: Set<UInt64> = []
    private var inputDrainScheduled = false

    private var channels: [UInt64: DeviceChannel] = [:]
    private var selectedLinkID: UInt64?
    private var dryRunChannel: DeviceChannel?
    private var manualPreviewSession: DisplaySession?

    private var bridge: OpaquePointer?
    private var timer: Timer?
    private var keyframeTimer: Timer?
    private var activityToken: NSObjectProtocol?
    private var lastRawSnapshot = "{\"activeLinkCount\":0,\"links\":[],\"state\":\"starting\"}"
    private var latestSnapshot: BridgeSnapshot?
    private var fatalError: String?
    private var lastReportTick = Date()
    private var cachedScreenPermission = CGPreflightScreenCaptureAccess()
    private var cachedAXPermission = AXIsProcessTrusted()
    private var cachedPostEventPermission = CGPreflightPostEventAccess()
    private var cachedInputPermission = AXIsProcessTrusted() && CGPreflightPostEventAccess()
    private var lastPermissionCheckTime = 0.0
    private var accessibilityWatchDeadline = 0.0

    init(options: HostOptions) {
        self.options = options
        self.selectedSource = options.displayID ??
            (UserDefaults.standard.object(forKey: Self.displayPreference) as? NSNumber)?.uint32Value
        if options.displayID != nil {
            self.autoVirtualDisplay = false
        } else if options.createDisplay {
            self.autoVirtualDisplay = true
        } else if UserDefaults.standard.object(forKey: Self.autoVirtualPreference) != nil {
            self.autoVirtualDisplay = UserDefaults.standard.bool(forKey: Self.autoVirtualPreference)
        } else {
            self.autoVirtualDisplay = true
        }
        if let rawPos = UserDefaults.standard.string(forKey: Self.positionPreference),
           let parsedPos = DisplayPosition(rawValue: rawPos) {
            self.displayPosition = parsedPos
        }
        if options.frameRateExplicit {
            self.preferredFrameRateHz = Self.normalizePreferredFrameRate(Int(options.frameRate.rounded()))
        } else if UserDefaults.standard.object(forKey: Self.frameRatePreference) != nil {
            self.preferredFrameRateHz = Self.normalizePreferredFrameRate(UserDefaults.standard.integer(forKey: Self.frameRatePreference))
        } else {
            self.preferredFrameRateHz = 0
        }
        if UserDefaults.standard.object(forKey: Self.inputPreference) != nil {
            self.allowInput = UserDefaults.standard.bool(forKey: Self.inputPreference)
        } else {
            self.allowInput = true
        }
        if let rawMode = UserDefaults.standard.string(forKey: Self.touchModePreference),
           let parsedMode = TouchInputMode(rawValue: rawMode) {
            self.touchInputMode = parsedMode
        }
        InputInjector.mode = self.touchInputMode
        self.touchCapability = InputInjector.refreshCapability()
        if UserDefaults.standard.object(forKey: Self.autoStartPreference) != nil {
            self.autoStartService = UserDefaults.standard.bool(forKey: Self.autoStartPreference)
        } else {
            self.autoStartService = true
        }
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
        startupStatus = startupManager.currentStatus()
        installStatusItem()
        installWindow()
        NSApp.activate(ignoringOtherApps: true)

        NotificationCenter.default.addObserver(
            forName: NSApplication.didBecomeActiveNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            self.startupStatus = self.startupManager.currentStatus()
            self.refreshPermissions(force: true)
            self.syncWindow()
        }

        NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.syncWindow()
        }

        startTimer()
        if !options.dryRun { observeSystemEvents() }
        updateManager.onChange = { [weak self] _ in
            self?.syncWindow()
        }
        if !options.dryRun && updateManager.autoCheckEnabled {
            DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) { [weak self] in
                self?.updateManager.checkForUpdates(manual: false)
            }
        }
        if autoStartService || options.dryRun {
            serviceRunning = true
            prepare()
        } else {
            serviceRunning = false
            lastRawSnapshot = "{\"activeLinkCount\":0,\"links\":[],\"state\":\"stopped\"}"
            syncWindow()
        }
        refreshPairedDevices()
    }

    private func refreshPermissions(force: Bool = false) {
        let now = ProcessInfo.processInfo.systemUptime
        let watchingAccessibility = now < accessibilityWatchDeadline
        if force || watchingAccessibility || !cachedScreenPermission || !cachedInputPermission || (now - lastPermissionCheckTime) >= 2.0 {
            cachedScreenPermission = CGPreflightScreenCaptureAccess()
            let wasInputOK = cachedInputPermission
            cachedAXPermission = AXIsProcessTrusted()
            cachedPostEventPermission = CGPreflightPostEventAccess()
            cachedInputPermission = cachedAXPermission && cachedPostEventPermission
            if cachedInputPermission && !wasInputOK {
                accessibilityWatchDeadline = 0.0
                if UserDefaults.standard.object(forKey: Self.inputPreference) == nil || allowInput {
                    allowInput = true
                }
            }
            lastPermissionCheckTime = now
        }
    }

    private func prepare() {
        refreshPermissions(force: true)
        guard cachedScreenPermission else { syncWindow(); return }
        fatalError = nil
        if options.dryRun {
            let channel = dryRunChannel ?? DeviceChannel(
                linkID: 0,
                position: displayPosition,
                inputEngine: InputInjector.makeEngine(mode: touchInputMode)
            )
            channel.deviceName = "本机采集测试"
            channel.captureWanted = true
            if !autoVirtualDisplay && channel.displaySession == nil {
                do {
                    channel.displaySession = try attachManualDisplay(excluding: [])
                } catch {
                    fail("\(error)")
                    syncWindow()
                    return
                }
            }
            dryRunChannel = channel
            lastRawSnapshot = "{\"activeLinkCount\":0,\"links\":[],\"state\":\"dry-run\",\"negotiated\":\"local only\"}"
            Task { await startCaptureIfNeeded(for: channel) }
            syncWindow()
            return
        }

        if !autoVirtualDisplay && manualPreviewSession == nil {
            do {
                manualPreviewSession = try attachManualDisplay(excluding: [])
            } catch {
                fail("\(error)")
                syncWindow()
                return
            }
        }
        if bridge == nil {
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
            for channel in channels.values {
                channel.captureWanted = false
                stopCapture(for: channel, destroyAutoDisplay: true, immediate: true)
            }
            if let dryRunChannel {
                dryRunChannel.captureWanted = false
                stopCapture(for: dryRunChannel, destroyAutoDisplay: true, immediate: true)
            }
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
        let allChannels = Array(channels.values) + (dryRunChannel.map { [$0] } ?? [])
        let semaphore = DispatchSemaphore(value: 0)
        Task {
            for channel in allChannels {
                await channel.capture?.stop()
                channel.capture = nil
            }
            semaphore.signal()
        }
        _ = semaphore.wait(timeout: .now() + 5)
        for channel in allChannels {
            channel.displaySession?.destroy()
            channel.displaySession = nil
        }
        manualPreviewSession?.destroy()
        manualPreviewSession = nil
        channels.removeAll()
        dryRunChannel = nil
        if let bridge {
            dm_bridge_destroy(bridge)
            self.bridge = nil
        }
    }

    private func installStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = item.button {
            button.image = MenuBarIcon.makeImage(connected: false, streaming: false)
            button.imagePosition = .imageOnly
            button.title = ""
            button.toolTip = "DeskMate"
            button.target = self
            button.action = #selector(togglePopover(_:))
            button.sendAction(on: [.leftMouseUp, .rightMouseUp])
        }
        let popover = NSPopover()
        popover.behavior = .transient
        popover.animates = true
        popover.contentViewController = NSHostingController(rootView: MenuBarPopoverView(
            model: windowModel,
            setVideo: { [weak self] enabled in self?.setVideo(enabled) },
            setInput: { [weak self] enabled in self?.setInput(enabled) },
            setTouchInputMode: { [weak self] mode in self?.setTouchInputMode(mode) },
            setPosition: { [weak self] pos in self?.setPosition(pos, for: self?.activeTargetLinkID()) },
            selectLink: { [weak self] linkID in self?.selectLink(linkID) },
            disconnectLink: { [weak self] linkID in self?.disconnectLink(linkID) },
            setServiceRunning: { [weak self] enabled in self?.setServiceRunning(enabled) },
            openPermission: { [weak self] screen in
                self?.popover?.performClose(nil)
                self?.openPermission(screen: screen)
            },
            requestAccessibility: { [weak self] resetStale in
                self?.popover?.performClose(nil)
                self?.requestAccessibilityPermission(resetStale: resetStale)
            },
            showWindow: { [weak self] in
                self?.popover?.performClose(nil)
                self?.showWindow()
            },
            copyStatus: { [weak self] in
                self?.copyStatus()
                self?.popover?.performClose(nil)
            },
            quit: { [weak self] in
                self?.popover?.performClose(nil)
                self?.quit()
            }
        ))
        self.popover = popover
        self.statusItem = item
    }

    @objc private func togglePopover(_ sender: AnyObject?) {
        guard let popover, let button = statusItem?.button else { return }
        if popover.isShown {
            popover.performClose(sender)
        } else {
            refreshPermissions(force: true)
            syncWindow()
            NSApp.activate(ignoringOtherApps: true)
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
        }
    }

    @objc private func copyStatus() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(windowModel.state.diagnostic, forType: .string)
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    @objc private func showWindow() {
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func activeTargetLinkID() -> UInt64? {
        if let selectedLinkID, channels[selectedLinkID] != nil {
            return selectedLinkID
        }
        return channels.keys.sorted().first
    }

    private func selectLink(_ linkID: UInt64) {
        guard channels[linkID] != nil else { return }
        selectedLinkID = linkID
        syncWindow()
    }

    private func setVideo(_ enabled: Bool) {
        videoEnabled = enabled
        if enabled {
            for channel in allActiveChannelsSorted() where channel.captureWanted {
                Task { await self.startCaptureIfNeeded(for: channel) }
            }
        } else {
            for channel in allActiveChannelsSorted() {
                stopCapture(for: channel, destroyAutoDisplay: false)
            }
        }
        syncWindow()
    }

    private func setInput(_ enabled: Bool) {
        allowInput = enabled
        UserDefaults.standard.set(enabled, forKey: Self.inputPreference)
        if !enabled {
            for channel in allActiveChannelsSorted() {
                channel.inputEngine.reset(displayBounds: channel.displaySession?.bounds())
            }
        } else if !cachedInputPermission {
            openPermission(screen: false)
        }
        syncWindow()
    }

    private func setTouchInputMode(_ mode: TouchInputMode) {
        touchInputMode = mode
        UserDefaults.standard.set(mode.rawValue, forKey: Self.touchModePreference)
        InputInjector.mode = mode
        for channel in allActiveChannelsSorted() {
            channel.inputEngine.mode = mode
        }
        touchCapability = InputInjector.capabilityStatus()
        syncWindow()
    }

    private func openTrackpadSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.Trackpad-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }

    private func setAutoVirtual(_ enabled: Bool) {
        guard !options.createDisplay, options.displayID == nil else { return }
        autoVirtualDisplay = enabled
        UserDefaults.standard.set(enabled, forKey: Self.autoVirtualPreference)
        if let bridge {
            dm_bridge_set_auto_match_resolution(bridge, (enabled && !options.sizeExplicit) ? 1 : 0)
        }

        let allChannels = allActiveChannelsSorted()
        for channel in allChannels {
            channel.destroyDisplayTask?.cancel()
            channel.destroyDisplayTask = nil
            channel.liveFps = 0
        }

        let preview = manualPreviewSession
        manualPreviewSession = nil
        preview?.destroy()

        Task { @MainActor in
            for channel in allChannels {
                if let pipeline = channel.capture {
                    channel.capture = nil
                    await pipeline.stop()
                }
            }
            var sessionsToDestroy: [DisplaySession] = []
            for channel in allChannels {
                if enabled {
                    if let existing = channel.displaySession, !existing.info.createdByUs {
                        channel.displaySession = nil
                        channel.manualDisplayID = nil
                    }
                } else {
                    if let existing = channel.displaySession, existing.info.createdByUs {
                        channel.displaySession = nil
                        sessionsToDestroy.append(existing)
                    }
                }
            }
            if !sessionsToDestroy.isEmpty {
                await DisplayConfigCoordinator.shared.run {
                    for session in sessionsToDestroy {
                        session.destroy()
                    }
                }
            }
            if !enabled && allChannels.isEmpty {
                self.manualPreviewSession = try? self.attachManualDisplay(excluding: [])
            }
            for channel in allChannels where channel.captureWanted {
                await self.startCaptureIfNeeded(for: channel)
            }
            await self.rearrangeAllVirtualDisplays()
            self.syncWindow()
            self.updateStatusIcon()
        }
    }

    private func setDefaultPosition(_ position: DisplayPosition) {
        displayPosition = position
        UserDefaults.standard.set(position.rawValue, forKey: Self.positionPreference)
        if channels.isEmpty, let dryRunChannel {
            dryRunChannel.position = position
            Task { @MainActor in await self.rearrangeAllVirtualDisplays() }
        }
        syncWindow()
    }

    private static func normalizePreferredFrameRate(_ hz: Int) -> Int {
        switch hz {
        case 30, 60, 90, 120:
            return hz
        default:
            return 0
        }
    }

    private static func effectiveFrameRate(
        preferredHz: Int,
        negotiatedHz: Int,
        explicitFrameRate: Double?,
        fallbackHz: Double
    ) -> Double {
        let rawTarget: Double
        if preferredHz > 0 {
            rawTarget = Double(preferredHz)
        } else if let explicitFrameRate {
            rawTarget = explicitFrameRate
        } else if negotiatedHz > 0 {
            rawTarget = Double(negotiatedHz)
        } else {
            rawTarget = fallbackHz
        }
        if negotiatedHz > 0 {
            return min(rawTarget, Double(negotiatedHz))
        }
        return rawTarget
    }

    @MainActor
    private func restartCapture(for channel: DeviceChannel) async {
        guard !channel.captureStarting else { return }
        channel.captureStarting = true
        let oldPipeline = channel.capture
        channel.capture = nil
        channel.activeCaptureFrameRate = 0
        if let oldPipeline {
            await oldPipeline.stop()
        }
        channel.captureStarting = false
        await startCaptureIfNeeded(for: channel)
    }

    private func setPreferredFrameRate(_ hz: Int) {
        let normalized = Self.normalizePreferredFrameRate(hz)
        preferredFrameRateHz = normalized
        UserDefaults.standard.set(normalized, forKey: Self.frameRatePreference)
        if let bridge {
            dm_bridge_set_preferred_refresh_hz(bridge, Int32(normalized))
        }
        let allChannels = allActiveChannelsSorted()
        var channelsToRestart: [DeviceChannel] = []
        for channel in allChannels where channel.captureWanted {
            let negotiatedHz = (channel.linkID > 0 ? bridge.map { Int(dm_bridge_negotiated_refresh_hz_for_link($0, channel.linkID)) } : nil) ?? 0
            let maxSupportedHz = (channel.linkID > 0 ? bridge.map { Int(dm_bridge_max_refresh_hz_for_link($0, channel.linkID)) } : nil) ?? 0
            let desiredCap = normalized > 0 ? normalized : 120
            var reconfInitiated = false
            if let bridge, channel.linkID > 0, negotiatedHz > 0,
               ((normalized > 0 && normalized > negotiatedHz) || (normalized == 0 && maxSupportedHz > negotiatedHz)),
               dm_bridge_request_refresh_hz_for_link(bridge, channel.linkID, Int32(desiredCap)) > 0 {
                reconfInitiated = true
            }
            if !reconfInitiated {
                let targetRate = Int(Self.effectiveFrameRate(
                    preferredHz: normalized,
                    negotiatedHz: negotiatedHz,
                    explicitFrameRate: options.frameRateExplicit ? options.frameRate : nil,
                    fallbackHz: targetFrameRate(for: channel)
                ).rounded())
                if targetRate != channel.activeCaptureFrameRate {
                    channelsToRestart.append(channel)
                }
            }
        }
        if !channelsToRestart.isEmpty {
            Task { @MainActor in
                for channel in channelsToRestart {
                    await self.restartCapture(for: channel)
                }
                self.syncWindow()
                self.updateStatusIcon()
            }
        }
        syncWindow()
    }

    private func setPosition(_ position: DisplayPosition, for linkID: UInt64?) {
        if let linkID, let channel = channels[linkID] {
            channel.position = position
            if !channel.deviceID.isEmpty {
                pairedStore.setPreferredPosition(id: channel.deviceID, position: position)
            }
            selectedLinkID = linkID
            Task { @MainActor in
                await self.rearrangeAllVirtualDisplays()
                self.syncWindow()
            }
            syncWindow()
            return
        }
        setDefaultPosition(position)
    }

    private func setDevicePreferredPosition(id: String, position: DisplayPosition?) {
        pairedStore.setPreferredPosition(id: id, position: position)
        if let position {
            var moved = false
            for channel in channels.values where channel.deviceID == id {
                channel.position = position
                moved = true
            }
            if moved {
                Task { @MainActor in
                    await self.rearrangeAllVirtualDisplays()
                    self.syncWindow()
                }
            }
        }
        syncWindow()
    }

    private func disconnectLink(_ linkID: UInt64) {
        guard let bridge else { return }
        dm_bridge_disconnect_link(bridge, linkID)
    }

    private func openPermission(screen: Bool) {
        if screen {
            _ = CGRequestScreenCaptureAccess()
        } else {
            _ = CGRequestPostEventAccess()
            let promptKey = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
            _ = AXIsProcessTrustedWithOptions([promptKey: true] as CFDictionary)
            accessibilityWatchDeadline = ProcessInfo.processInfo.systemUptime + 30.0
        }
        refreshPermissions(force: true)
        syncWindow()
        let privacy = screen ? "Privacy_ScreenCapture" : "Privacy_Accessibility"
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(privacy)") {
            NSWorkspace.shared.open(url)
        }
    }

    private func requestAccessibilityPermission(resetStale: Bool) {
        allowInput = true
        UserDefaults.standard.set(true, forKey: Self.inputPreference)
        accessibilityWatchDeadline = ProcessInfo.processInfo.systemUptime + 30.0

        if resetStale {
            let bundleID = Bundle.main.bundleIdentifier ?? "dev.deskmate.host"
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                let process = Process()
                process.executableURL = URL(fileURLWithPath: "/usr/bin/tccutil")
                process.arguments = ["reset", "Accessibility", bundleID]
                try? process.run()
                process.waitUntilExit()
                DispatchQueue.main.async { [weak self] in
                    self?.openPermission(screen: false)
                }
            }
            return
        }

        openPermission(screen: false)
    }

    private func installWindow() {
        let frame = NSRect(x: 0, y: 0, width: 960, height: 660)
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
        window.backgroundColor = NSColor(name: nil) { appearance in
            let isDark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            return isDark
                ? NSColor(srgbRed: 0.067, green: 0.071, blue: 0.078, alpha: 1.0) // #111214
                : NSColor(srgbRed: 0.961, green: 0.961, blue: 0.953, alpha: 1.0) // #F5F5F3
        }
        window.contentMinSize = NSSize(width: 820, height: 540)
        window.isReleasedWhenClosed = false
        window.center()

        window.contentView = NSHostingView(rootView: HostWindow(
            model: windowModel,
            chooseSource: { [weak self] id in self?.chooseSource(id) },
            setAutoVirtual: { [weak self] enabled in self?.setAutoVirtual(enabled) },
            setPosition: { [weak self] pos in self?.setPosition(pos, for: self?.activeTargetLinkID()) },
            setDefaultPosition: { [weak self] pos in self?.setDefaultPosition(pos) },
            setPreferredFrameRate: { [weak self] hz in self?.setPreferredFrameRate(hz) },
            selectLink: { [weak self] linkID in self?.selectLink(linkID) },
            setLinkPosition: { [weak self] linkID, pos in self?.setPosition(pos, for: linkID) },
            disconnectLink: { [weak self] linkID in self?.disconnectLink(linkID) },
            applyDisplayLayout: { [weak self] origins in self?.applyDisplayLayout(origins) },
            refreshDisplays: { [weak self] in self?.syncWindow() },
            setVideo: { [weak self] enabled in self?.setVideo(enabled) },
            setInput: { [weak self] enabled in self?.setInput(enabled) },
            setTouchInputMode: { [weak self] mode in self?.setTouchInputMode(mode) },
            openTrackpadSettings: { [weak self] in self?.openTrackpadSettings() },
            openPermission: { [weak self] screen in self?.openPermission(screen: screen) },
            requestAccessibility: { [weak self] resetStale in self?.requestAccessibilityPermission(resetStale: resetStale) },
            setLaunchAtLogin: { [weak self] enabled in self?.setLaunchAtLogin(enabled) },
            openLoginItemsSettings: { [weak self] in self?.startupManager.openSystemLoginItemsSettings() },
            setAutoStartService: { [weak self] enabled in self?.setAutoStartService(enabled) },
            setServiceRunning: { [weak self] enabled in self?.setServiceRunning(enabled) },
            refreshDevices: { [weak self] in self?.refreshPairedDevices() },
            setDeviceAllowed: { [weak self] id, allowed in self?.setDeviceAllowed(id: id, allowed: allowed) },
            setDevicePreferredPosition: { [weak self] id, pos in self?.setDevicePreferredPosition(id: id, position: pos) },
            renameDevice: { [weak self] id, name in self?.renameDevice(id: id, name: name) },
            removeDevice: { [weak self] id in self?.removeDevice(id: id) },
            copyDeviceID: { [weak self] id in self?.copyDeviceID(id) },
            checkForUpdates: { [weak self] in self?.updateManager.checkForUpdates(manual: true) },
            installUpdate: { [weak self] in self?.installAvailableUpdate() },
            setAutoCheckUpdates: { [weak self] enabled in self?.updateManager.setAutoCheckEnabled(enabled) },
            setUpdateMirrorPrefix: { [weak self] prefix in self?.updateManager.setMirrorPrefix(prefix) },
            openReleasesPage: { [weak self] in self?.updateManager.openReleasesPage() },
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
        if !autoVirtualDisplay {
            manualPreviewSession = try? attachManualDisplay(excluding: [])
        }
        syncWindow()
    }

    private func retry() {
        refreshPermissions(force: true)
        fatalError = nil
        for channel in channels.values {
            channel.captureError = nil
        }
        if !serviceRunning {
            setServiceRunning(true)
            return
        }
        if bridge == nil && (!options.dryRun || dryRunChannel == nil) {
            prepare()
        } else {
            for channel in allActiveChannelsSorted() where channel.captureWanted {
                Task { await self.startCaptureIfNeeded(for: channel) }
            }
        }
        syncWindow()
    }

    private func setLaunchAtLogin(_ enabled: Bool) {
        startupStatus = startupManager.setLaunchAtLogin(enabled)
        syncWindow()
    }

    private func setAutoStartService(_ enabled: Bool) {
        autoStartService = enabled
        UserDefaults.standard.set(enabled, forKey: Self.autoStartPreference)
        syncWindow()
    }

    private func setServiceRunning(_ enabled: Bool) {
        guard !serviceTransitioning else { return }
        if enabled {
            guard !serviceRunning else { return }
            serviceRunning = true
            fatalError = nil
            prepare()
            syncBlockedDevicesToBridge()
            refreshPairedDevices()
            syncWindow()
            updateStatusIcon()
        } else {
            guard serviceRunning else { return }
            serviceTransitioning = true
            serviceRunning = false
            let activeChannels = allActiveChannelsSorted()
            channels.removeAll()
            selectedLinkID = nil
            dryRunChannel = nil
            manualPreviewSession?.destroy()
            manualPreviewSession = nil

            for channel in activeChannels {
                channel.removedFromBridge = true
                channel.captureWanted = false
                channel.inputWanted = false
                channel.liveFps = 0
                channel.liveBitrateMbps = 0.0
                channel.inputEngine.reset(displayBounds: channel.displaySession?.bounds())
                channel.destroyDisplayTask?.cancel()
                channel.destroyDisplayTask = nil
            }

            let bridgeRaw = bridge.map { UInt(bitPattern: $0) }
            bridge = nil
            lastRawSnapshot = "{\"activeLinkCount\":0,\"links\":[],\"state\":\"stopped\"}"
            latestSnapshot = nil
            syncWindow()
            updateStatusIcon()

            Task.detached(priority: .userInitiated) { [weak self, bridgeRaw, activeChannels] in
                for channel in activeChannels {
                    await channel.capture?.stop()
                    channel.capture = nil
                }
                let createdSessions = activeChannels.compactMap { $0.displaySession }.filter { $0.info.createdByUs }
                if !createdSessions.isEmpty {
                    await DisplayConfigCoordinator.shared.run {
                        for session in createdSessions {
                            session.destroy()
                        }
                    }
                }
                for channel in activeChannels {
                    channel.displaySession = nil
                }
                if let bridgeRaw, let ptr = OpaquePointer(bitPattern: bridgeRaw) {
                    dm_bridge_destroy(ptr)
                }
                await MainActor.run {
                    guard let self else { return }
                    self.serviceTransitioning = false
                    self.syncWindow()
                    self.updateStatusIcon()
                }
            }
        }
    }

    private func syncBlockedDevicesToBridge() {
        guard let bridge else { return }
        let blocked = pairedStore.blockedDeviceIDs()
        if blocked.isEmpty {
            dm_bridge_set_blocked_devices(bridge, nil, 0)
            return
        }
        let cStrings: [UnsafeMutablePointer<CChar>?] = blocked.map { strdup($0) }
        defer {
            for ptr in cStrings {
                free(ptr)
            }
        }
        let constPointers: [UnsafePointer<CChar>?] = cStrings.map { UnsafePointer($0) }
        constPointers.withUnsafeBufferPointer { buffer in
            dm_bridge_set_blocked_devices(bridge, buffer.baseAddress, Int32(buffer.count))
        }
    }

    private func refreshPairedDevices() {
        guard let raw = dm_bridge_scan_devices(bridge) else { return }
        defer { free(raw) }
        let jsonString = String(cString: raw)
        guard let data = jsonString.data(using: .utf8),
              let scanned = try? JSONDecoder().decode([ScannedUsbDevice].self, from: data) else {
            return
        }
        onlineDeviceIDs = Set(scanned.map(\.id))
        for item in scanned {
            pairedStore.recordSeenDevice(
                id: item.id,
                name: item.name,
                manufacturer: item.manufacturer,
                serial: item.serial,
                markConnected: false
            )
        }
        syncBlockedDevicesToBridge()
        syncWindow()
    }

    private func setDeviceAllowed(id: String, allowed: Bool) {
        pairedStore.setAllowed(id: id, allowed: allowed)
        syncBlockedDevicesToBridge()
        syncWindow()
    }

    private func renameDevice(id: String, name: String) {
        pairedStore.rename(id: id, customName: name)
        syncWindow()
    }

    private func removeDevice(id: String) {
        if let bridge {
            id.withCString { cStr in
                dm_bridge_disconnect_device(bridge, cStr)
            }
        }
        pairedStore.remove(id: id)
        syncBlockedDevicesToBridge()
        syncWindow()
    }

    private func copyDeviceID(_ id: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(id, forType: .string)
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

    private func allActiveChannelsSorted() -> [DeviceChannel] {
        var list = channels.values.sorted { $0.linkID < $1.linkID }
        if let dryRunChannel {
            list.append(dryRunChannel)
        }
        return list
    }

    private func defaultPositionForNewChannel(deviceID: String?) -> DisplayPosition {
        if let deviceID, !deviceID.isEmpty,
           let preferred = pairedStore.preferredPosition(for: deviceID) {
            return preferred
        }
        let occupied = Set(channels.values.map(\.position))
        let candidates: [DisplayPosition] = [displayPosition, .right, .left, .bottom, .top]
        for candidate in candidates where !occupied.contains(candidate) {
            return candidate
        }
        return displayPosition
    }

    private func getOrCreateChannel(linkID: UInt64, hint: BridgeLinkSnapshot? = nil) -> DeviceChannel {
        if let existing = channels[linkID] {
            return existing
        }
        let pos = defaultPositionForNewChannel(deviceID: hint?.deviceId)
        let channel = DeviceChannel(
            linkID: linkID,
            position: pos,
            inputEngine: InputInjector.makeEngine(mode: touchInputMode)
        )
        if let hint {
            applySnapshot(hint, to: channel)
        }
        channels[linkID] = channel
        if selectedLinkID == nil {
            selectedLinkID = linkID
        }
        return channel
    }

    private func applySnapshot(_ snap: BridgeLinkSnapshot, to channel: DeviceChannel) {
        channel.bridgeState = snap.state
        channel.negotiated = snap.negotiated
        channel.bridgeError = snap.error
        channel.inputEvents = snap.inputEvents
        channel.txBytes = snap.txBytes

        if !snap.deviceId.isEmpty {
            let now = ProcessInfo.processInfo.systemUptime
            let isNewDevice = (channel.deviceID != snap.deviceId)
            channel.deviceID = snap.deviceId
            channel.deviceManufacturer = snap.deviceManufacturer
            channel.deviceSerial = snap.deviceSerial

            let activeResolution = snap.negotiated != "none" ? snap.negotiated.uppercased() : nil
            if snap.state != "idle" && snap.state != "stopped" {
                if isNewDevice || (now - channel.lastDeviceTouchTime) >= 15.0 || activeResolution != nil {
                    pairedStore.recordSeenDevice(
                        id: snap.deviceId,
                        name: snap.deviceName,
                        manufacturer: snap.deviceManufacturer,
                        serial: snap.deviceSerial,
                        resolution: activeResolution,
                        markConnected: isNewDevice || (now - channel.lastDeviceTouchTime) >= 30.0
                    )
                    channel.lastDeviceTouchTime = now
                }
                onlineDeviceIDs.insert(snap.deviceId)
            }
            channel.deviceName = pairedStore.displayName(for: snap.deviceId) ??
                (snap.deviceName.isEmpty ? "HarmonyOS 设备" : snap.deviceName)
            if isNewDevice, let preferred = pairedStore.preferredPosition(for: snap.deviceId) {
                if channel.position != preferred {
                    channel.position = preferred
                    if channel.displaySession?.info.createdByUs == true {
                        Task { @MainActor in await self.rearrangeAllVirtualDisplays() }
                    }
                }
            }
        }
    }

    private func refreshSnapshotFromBridge() {
        guard let bridge, let raw = dm_bridge_snapshot(bridge) else { return }
        lastRawSnapshot = String(cString: raw)
        free(raw)
        if let data = lastRawSnapshot.data(using: .utf8),
           let decoded = try? JSONDecoder().decode(BridgeSnapshot.self, from: data) {
            latestSnapshot = decoded
            reconcileChannels(with: decoded)
        }
    }

    private func reconcileChannels(with snapshot: BridgeSnapshot) {
        let activeIDs = Set(snapshot.links.map(\.linkId))

        // Clean up any channel whose USB link terminated without a final intent callback.
        let removedIDs = channels.keys.filter { !activeIDs.contains($0) }
        for linkID in removedIDs {
            if let removed = channels.removeValue(forKey: linkID) {
                removed.removedFromBridge = true
                removed.captureWanted = false
                removed.inputWanted = false
                removed.inputEngine.reset(displayBounds: removed.displaySession?.bounds())
                stopCapture(for: removed, destroyAutoDisplay: true, immediate: false)
            }
        }

        for linkSnap in snapshot.links {
            let channel = getOrCreateChannel(linkID: linkSnap.linkId, hint: linkSnap)
            applySnapshot(linkSnap, to: channel)
        }

        if let selectedLinkID, channels[selectedLinkID] == nil {
            self.selectedLinkID = channels.keys.sorted().first
        } else if selectedLinkID == nil {
            self.selectedLinkID = channels.keys.sorted().first
        }
    }

    private func channelStatusSummary(for channel: DeviceChannel) -> (title: String, guidance: String, badge: String, linked: Bool, streaming: Bool) {
        let streaming = (channel.bridgeState == "streaming" && videoEnabled && channel.capture != nil)
        let name = channel.deviceName.isEmpty ? "设备 #\(channel.linkID)" : channel.deviceName
        if let captureError = channel.captureError {
            return ("暂时无法发送画面", captureError, "出错", channel.bridgeState != "idle", false)
        }
        if channel.bridgeState == "awaiting_client_hello" {
            return ("等待「\(name)」确认", "请在设备上打开 DeskMate，并在系统提示中允许 USB 配件连接。", "USB 已连接", true, false)
        }
        if streaming {
            return ("正在向「\(name)」发送画面", "正在向「\(name)」传输扩展桌面画面与反向触控。", "会话已建立", true, true)
        }
        if channel.bridgeState.hasPrefix("awaiting_") || channel.bridgeState == "streaming" || channel.bridgeState == "paused" {
            let title = (channel.bridgeState == "paused" || !videoEnabled) ? "画面发送已停止" : "正在准备画面"
            let guidance = !videoEnabled ? "需要继续时，开启画面发送。" : "保持 USB 数据线连接，并确认 Mac 未锁屏。"
            return (title, guidance, "USB 已连接", true, false)
        }
        return ("正在握手 USB 通道", channel.bridgeError.isEmpty ? "等待会话建立。" : channel.bridgeError, "USB 已连接", true, false)
    }

    private func syncWindow() {
        let snapshot = latestSnapshot
        let sortedChannels = channels.values.sorted { $0.linkID < $1.linkID }
        let targetChannel = (selectedLinkID.flatMap { channels[$0] }) ?? sortedChannels.first ?? dryRunChannel

        let activeSessions: [ActiveDisplaySessionState] = sortedChannels.map { channel in
            let summary = channelStatusSummary(for: channel)
            let streamText = channel.negotiated != "none" ? channel.negotiated.uppercased() : ""
            return ActiveDisplaySessionState(
                linkID: channel.linkID,
                deviceID: channel.deviceID,
                deviceName: channel.deviceName.isEmpty ? "HarmonyOS 设备 #\(channel.linkID)" : channel.deviceName,
                state: channel.bridgeState,
                badge: summary.badge,
                linked: summary.linked,
                streaming: summary.streaming,
                streamSummary: streamText,
                position: channel.position,
                liveFps: channel.liveFps,
                liveBitrateMbps: channel.liveBitrateMbps,
                inputEventCount: Int(channel.inputEvents),
                displayID: channel.displaySession?.info.displayID,
                error: channel.captureError ?? (channel.bridgeError.isEmpty ? nil : channel.bridgeError)
            )
        }

        let totalFps = activeSessions.reduce(0) { $0 + $1.liveFps } + (dryRunChannel?.liveFps ?? 0)
        let totalBitrate = activeSessions.reduce(0.0) { $0 + $1.liveBitrateMbps } + (dryRunChannel?.liveBitrateMbps ?? 0.0)
        let totalInputs = activeSessions.reduce(0) { $0 + $1.inputEventCount }

        var view = HostWindowState()
        view.screenPermission = cachedScreenPermission
        view.inputPermission = cachedInputPermission
        view.videoEnabled = videoEnabled
        view.inputEnabled = allowInput && view.inputPermission
        view.touchInputMode = touchInputMode
        view.touchCapability = touchCapability
        view.autoVirtualDisplay = autoVirtualDisplay
        view.position = targetChannel?.position ?? displayPosition
        view.preferredFrameRateHz = preferredFrameRateHz
        if let targetChannel, targetChannel.linkID > 0, let bridge {
            view.negotiatedRefreshHz = Int(dm_bridge_negotiated_refresh_hz_for_link(bridge, targetChannel.linkID))
            view.maxSupportedRefreshHz = Int(dm_bridge_max_refresh_hz_for_link(bridge, targetChannel.linkID))
        } else {
            view.negotiatedRefreshHz = 0
            view.maxSupportedRefreshHz = 0
        }
        view.liveFps = totalFps
        view.liveBitrateMbps = totalBitrate
        view.inputEventCount = totalInputs
        view.serviceRunning = serviceRunning
        view.autoStartService = autoStartService
        view.launchAtLogin = startupStatus.launchAtLoginEnabled
        view.loginItemNeedsApproval = startupStatus.needsApproval
        view.loginItemSummary = startupStatus.statusDescription
        view.loginItemError = startupStatus.errorMessage
        view.update = updateManager.snapshot()
        view.pairedDevices = pairedStore.devices
        view.activeSessions = activeSessions
        view.topologyDisplays = DisplayInventory.snapshotTopologyDisplays(sessions: activeSessions)
        view.selectedLinkID = selectedLinkID ?? activeSessions.first?.linkID
        view.connectedDeviceIDs = Set(activeSessions.map(\.deviceID).filter { !$0.isEmpty })
        view.connectedDeviceID = targetChannel?.deviceID.isEmpty == false ? targetChannel?.deviceID : nil
        view.onlineDeviceIDs = onlineDeviceIDs
        view.connectedDeviceName = targetChannel?.deviceName.isEmpty == false ? targetChannel?.deviceName : nil
        view.blockedDeviceNames = snapshot?.blockedDevices ?? []
        view.blockedDeviceName = view.blockedDeviceNames.first ?? (snapshot?.deviceBlocked == true ? (snapshot?.blockedDeviceName.isEmpty == false ? snapshot?.blockedDeviceName : "未允许的 USB 设备") : nil)

        let activeStreamTexts = activeSessions.map(\.streamSummary).filter { !$0.isEmpty }
        if activeStreamTexts.count == 1 {
            view.streamSummary = activeStreamTexts[0]
        } else if activeStreamTexts.count > 1 {
            view.streamSummary = "\(activeStreamTexts.count) 路并发 · \(activeStreamTexts[0])"
        } else if let targetChannel, targetChannel.negotiated != "none" {
            view.streamSummary = targetChannel.negotiated.uppercased()
        }

        let anyInputWanted = sortedChannels.contains(where: \.inputWanted)
        if !view.inputPermission {
            view.input = "需授权辅助功能"
        } else {
            view.input = view.inputEnabled ? (anyInputWanted ? "\(touchInputMode.shortLabel) · 已启用" : "\(touchInputMode.shortLabel) · 待会话") : "未开启"
        }

        view.choices = displayChoices()
        view.fixedSource = options.createDisplay || options.displayID != nil
        view.selected = view.choices.contains(where: { $0.id == selectedSource }) ? selectedSource : nil

        let displayReference = targetChannel?.displaySession ?? manualPreviewSession
        if let info = displayReference?.info {
            if info.createdByUs {
                if activeSessions.count > 1 {
                    let createdCount = sortedChannels.filter { $0.displaySession != nil }.count
                    view.source = "\(createdCount) 块独立虚拟显示器"
                } else {
                    view.source = "虚拟显示器 · \(info.pixelWidth)×\(info.pixelHeight)（\(view.position.shortLabel)）"
                }
            } else {
                view.source = view.choices.first(where: { $0.id == info.displayID })?.title ?? "扩展显示器（\(info.displayID)）"
                view.needsRestart = !view.fixedSource && !autoVirtualDisplay && selectedSource != nil && selectedSource != info.displayID
            }
        } else if autoVirtualDisplay {
            view.source = "连接时按设备分辨率自动创建（默认\(displayPosition.shortLabel)）"
        }

        let anyStreaming = activeSessions.contains(where: \.streaming) || (dryRunChannel?.capture != nil)
        let streamingCount = activeSessions.filter(\.streaming).count
        let anyError = fatalError ?? sortedChannels.compactMap(\.captureError).first
        let bridgeErr = snapshot?.error ?? ""
        let manualReady = autoVirtualDisplay || displayReference != nil || !view.choices.isEmpty

        if !serviceRunning {
            view.title = "串流服务已停止"
            view.guidance = "当前未开启自动服务或已手动暂停；点击「启动服务」即可恢复监听 USB 设备连接。"
            view.badge = "已停止"
            view.canRetry = true
        } else if !view.screenPermission {
            (view.title, view.guidance, view.badge) = ("需要屏幕录制权限", "在系统设置中允许 DeskMate 录制屏幕，再返回重试。", "待授权")
            view.canRetry = true
        } else if !manualReady {
            view.title = "需要扩展显示器"
            view.guidance = view.choices.isEmpty ? "先在 Mac 上准备一块扩展显示器，或开启「自动创建虚拟显示器」。" : "请在设置中选择要发送的扩展显示器。"
            view.badge = "未就绪"
            view.canRetry = true
        } else if anyError != nil {
            (view.title, view.guidance, view.badge) = ("暂时无法发送画面", anyError ?? "重试或打开诊断查看原因。", "出错")
            view.canRetry = true
        } else if options.dryRun {
            (view.title, view.guidance, view.badge) = ("本机采集测试", "仅在本机采集，不建立 USB 会话。", "测试模式")
        } else if activeSessions.isEmpty && !view.blockedDeviceNames.isEmpty {
            let blockedLabel = view.blockedDeviceName ?? "此设备"
            view.title = "已拦截未允许的设备"
            view.guidance = "检测到 \(blockedLabel) 接入，但该设备在「已配对设备」列表中已禁止连接。"
            view.badge = "已拦截"
        } else if activeSessions.isEmpty && !bridgeErr.isEmpty {
            view.title = "USB 连接重试中"
            view.guidance = "USB 通信暂时异常（\(bridgeErr)），正在自动重连。"
            view.badge = "未连接"
            view.canRetry = true
        } else if activeSessions.count >= 2 {
            view.linked = true
            let names = activeSessions.map(\.deviceName).joined(separator: "、")
            if streamingCount == activeSessions.count {
                view.title = "正在向 \(activeSessions.count) 台设备输出副屏"
                view.guidance = "当前同时连接：\(names)，每台设备拥有独立虚拟显示器与触控通道。"
                view.badge = "\(streamingCount) 台副屏运行中"
                view.video = "并发发送 (\(streamingCount) 路)"
            } else if streamingCount > 0 {
                view.title = "\(streamingCount)/\(activeSessions.count) 台副屏推流中"
                view.guidance = "当前连接：\(names)，其余设备正在等待画面或确认授权。"
                view.badge = "\(activeSessions.count) 台已连接"
                view.video = "并发发送 (\(streamingCount)/\(activeSessions.count))"
            } else {
                view.title = "已接入 \(activeSessions.count) 台设备"
                view.guidance = "当前连接：\(names)，请在设备上打开 DeskMate 并允许 USB 配件连接。"
                view.badge = "\(activeSessions.count) 台已连接"
            }
        } else if !activeSessions.isEmpty, let firstChannel = sortedChannels.first {
            let summary = channelStatusSummary(for: firstChannel)
            view.title = summary.title
            view.guidance = summary.guidance
            view.badge = summary.badge
            view.linked = summary.linked
            if summary.streaming {
                view.video = "正在发送"
            }
        }

        if !videoEnabled { view.video = "已关闭" }
        if anyStreaming && view.video == "等待连接" { view.video = "正在发送" }

        var channelDiagnostics = ""
        for channel in sortedChannels {
            let boundsText = channel.displaySession.map { NSStringFromRect($0.bounds()) } ?? "none"
            let displayIDText = channel.displaySession.map { String($0.info.displayID) } ?? "none"
            channelDiagnostics += "\n- link=\(channel.linkID) device=\(channel.deviceName) (\(channel.deviceID)) state=\(channel.bridgeState) pos=\(channel.position.shortLabel) display=\(displayIDText) bounds=\(boundsText) stream=\(channel.negotiated) fps=\(channel.liveFps) encoded=\(channel.capture?.encodedFrameCount ?? 0) inputs=\(channel.inputEvents)"
            if let capErr = channel.captureError {
                channelDiagnostics += " captureError=\(capErr)"
            }
        }
        let fpsModeText = preferredFrameRateHz > 0 ? "\(preferredFrameRateHz)Hz" : "auto"
        view.diagnostic = "服务：\(serviceRunning ? "运行中" : "已停止")（登录启动=\(view.launchAtLogin)，自动启动=\(autoStartService)，帧率=\(fpsModeText)）\n在线连接数：\(activeSessions.count)（已记录 \(view.pairedDevices.count) 台）\n显示来源：\(view.source)\(channelDiagnostics)\n触控：mode=\(touchInputMode.rawValue) backend=\(touchCapability.activeBackendLabel) allow=\(allowInput) ax=\(cachedAXPermission) postEvent=\(cachedPostEventPermission) totalEvents=\(totalInputs)\n\(fatalError ?? "")\n\(lastRawSnapshot)"

        if windowModel.state != view {
            windowModel.state = view
        }
    }

    private func attachManualDisplay(excluding excludedIDs: Set<UInt32>) throws -> DisplaySession {
        if options.createDisplay {
            let scale = 2
            return try DisplaySession.create(
                logicalWidth: options.width / scale,
                logicalHeight: options.height / scale,
                scale: scale,
                refreshRate: options.frameRate,
                position: displayPosition
            )
        }
        let choices = displayChoices()
        let unusedChoices = choices.filter { !excludedIDs.contains($0.id) }
        if let explicitID = options.displayID {
            return try DisplaySession.attach(displayID: explicitID)
        }
        if let saved = selectedSource,
           choices.contains(where: { $0.id == saved }),
           (!excludedIDs.contains(saved) || unusedChoices.isEmpty) {
            return try DisplaySession.attach(displayID: saved)
        }
        if let firstUnused = unusedChoices.first {
            if selectedSource == nil {
                selectedSource = firstUnused.id
            }
            return try DisplaySession.attach(displayID: firstUnused.id)
        }
        if let sole = choices.first {
            selectedSource = sole.id
            return try DisplaySession.attach(displayID: sole.id)
        }
        throw DisplaySessionError.unavailable("no extended display selected")
    }

    private func targetFrameRate(for channel: DeviceChannel) -> Double {
        if options.frameRateExplicit {
            return options.frameRate
        }
        guard let refresh = channel.displaySession?.info.refreshRate, refresh >= 60.0 else {
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
        if options.frameRateExplicit {
            config.refresh_hz = Int32(options.frameRate.rounded())
        } else {
            config.refresh_hz = 120
        }
        config.prefer_hevc = options.codec == "hevc" ? 1 : 0
        config.auto_match_device_resolution = (autoVirtualDisplay && !options.sizeExplicit) ? 1 : 0

        let bridge = dm_bridge_create(&config)
        guard let bridge else {
            fail("failed to create USB bridge")
            return
        }
        dm_bridge_set_preferred_refresh_hz(bridge, Int32(preferredFrameRateHz))
        self.bridge = bridge
        dm_bridge_set_log(bridge, { message, _ in
            guard let message else { return }
            NSLog("DeskMate bridge: %@", String(cString: message))
        }, nil)
        dm_bridge_set_link_intent_callback(bridge, { linkID, capture, input, context in
            guard let context else { return }
            let controller = Unmanaged<AppController>.fromOpaque(context).takeUnretainedValue()
            controller.applyIntent(linkID: linkID, capture: capture, input: input)
        }, Unmanaged.passUnretained(self).toOpaque())
        dm_bridge_set_link_input_callback(bridge, { linkID, context in
            guard let context else { return }
            let controller = Unmanaged<AppController>.fromOpaque(context).takeUnretainedValue()
            controller.scheduleInputDrain(linkID: linkID)
        }, Unmanaged.passUnretained(self).toOpaque())

        syncBlockedDevicesToBridge()

        if dm_bridge_start(bridge) != 0 {
            fail("failed to start USB bridge")
            dm_bridge_destroy(bridge)
            self.bridge = nil
        }
    }

    private func applyIntent(linkID: UInt64, capture: Int32, input: Int32) {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.refreshSnapshotFromBridge()
            let linkSnap = self.latestSnapshot?.links.first(where: { $0.linkId == linkID })
            let channel = self.getOrCreateChannel(linkID: linkID, hint: linkSnap)
            if capture >= 0 {
                channel.captureWanted = capture != 0
                if channel.captureWanted {
                    channel.captureError = nil
                    self.fatalError = nil
                    Task { await self.startCaptureIfNeeded(for: channel) }
                } else {
                    self.stopCapture(for: channel, destroyAutoDisplay: true)
                }
            }
            if input >= 0 {
                channel.inputWanted = input != 0
                if !channel.inputWanted {
                    channel.inputEngine.reset(displayBounds: channel.displaySession?.bounds())
                }
            }
            self.syncWindow()
            self.updateStatusIcon()
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
        let now = ProcessInfo.processInfo.systemUptime
        for channel in channels.values {
            if channel.inputWanted && allowInput && cachedInputPermission,
               let displaySession = channel.displaySession {
                channel.inputEngine.tick(displayBounds: displaySession.bounds())
            }
            guard let bridge,
                  let capture = channel.capture,
                  dm_bridge_needs_keyframe_for_link(bridge, channel.linkID) != 0 else {
                channel.nextKeyframeRetry = 0
                continue
            }
            guard now >= channel.nextKeyframeRetry else { continue }
            channel.nextKeyframeRetry = now + 0.1
            capture.requestKeyframe()
        }
    }

    private func tick() {
        refreshPermissions()
        if bridge != nil {
            refreshSnapshotFromBridge()
            for channel in allActiveChannelsSorted() where channel.linkID > 0 {
                if let bridge {
                    drainInput(bridge, for: channel)
                }
            }
        }
        for channel in allActiveChannelsSorted() {
            if channel.captureWanted && videoEnabled && channel.capture == nil &&
               !channel.captureStarting && channel.captureError == nil &&
               fatalError == nil && cachedScreenPermission &&
               (autoVirtualDisplay || channel.displaySession != nil || !displayChoices().isEmpty) {
                Task { await self.startCaptureIfNeeded(for: channel) }
            } else if channel.capture != nil && !channel.captureStarting &&
                      channel.captureWanted && videoEnabled && channel.linkID > 0,
                      let bridge {
                let negotiatedHz = Int(dm_bridge_negotiated_refresh_hz_for_link(bridge, channel.linkID))
                let desiredRate = Int(Self.effectiveFrameRate(
                    preferredHz: preferredFrameRateHz,
                    negotiatedHz: negotiatedHz,
                    explicitFrameRate: options.frameRateExplicit ? options.frameRate : nil,
                    fallbackHz: targetFrameRate(for: channel)
                ).rounded())
                if desiredRate > 0 && channel.activeCaptureFrameRate > 0 &&
                   desiredRate != channel.activeCaptureFrameRate {
                    Task { @MainActor in
                        await self.restartCapture(for: channel)
                    }
                }
            }
            updateLiveMetrics(for: channel)
        }
        syncWindow()
        updateStatusIcon()
        reportProgress()
    }

    private func updateLiveMetrics(for channel: DeviceChannel) {
        let now = ProcessInfo.processInfo.systemUptime
        let dt = now - channel.lastMetricTime
        guard dt >= 0.45 else { return }
        let encoded = channel.capture?.encodedFrameCount ?? 0
        if channel.capture != nil && encoded >= channel.lastMetricFrames {
            channel.liveFps = Int((Double(encoded - channel.lastMetricFrames) / dt).rounded())
        } else {
            channel.liveFps = 0
        }
        if channel.txBytes >= channel.lastMetricTxBytes {
            channel.liveBitrateMbps = Double(channel.txBytes - channel.lastMetricTxBytes) * 8.0 / (dt * 1_000_000.0)
        } else {
            channel.liveBitrateMbps = 0.0
        }
        channel.lastMetricTime = now
        channel.lastMetricFrames = encoded
        channel.lastMetricTxBytes = channel.txBytes
    }

    private func reportProgress() {
        let now = Date()
        guard now.timeIntervalSince(lastReportTick) >= 2 else { return }
        lastReportTick = now
        for channel in allActiveChannelsSorted() {
            guard let capture = channel.capture else { continue }
            let encoded = capture.encodedFrameCount
            guard encoded != channel.lastReportedEncoded else {
                NSLog("DeskMate: [link %llu] capture stalled at %d encoded frames (received %d)",
                      channel.linkID, encoded, capture.receivedFrameCount)
                continue
            }
            channel.lastReportedEncoded = encoded
            NSLog(
                "DeskMate: [link %llu] captured %d / encoded %d frames (%dfps)",
                channel.linkID,
                capture.receivedFrameCount,
                encoded,
                channel.liveFps
            )
        }
    }

    private func scheduleInputDrain(linkID: UInt64) {
        inputDrainLock.lock()
        pendingInputDrainLinks.insert(linkID)
        if inputDrainScheduled {
            inputDrainLock.unlock()
            return
        }
        inputDrainScheduled = true
        inputDrainLock.unlock()
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.inputDrainLock.lock()
            let linkIDs = self.pendingInputDrainLinks
            self.pendingInputDrainLinks.removeAll()
            self.inputDrainScheduled = false
            self.inputDrainLock.unlock()
            guard let bridge = self.bridge else { return }
            for id in linkIDs {
                if let channel = self.channels[id] {
                    self.drainInput(bridge, for: channel)
                }
            }
        }
    }

    private func drainInput(_ bridge: OpaquePointer, for channel: DeviceChannel) {
        let canInject = channel.inputWanted && allowInput && cachedInputPermission
        var buffer = [DMInputEvent](repeating: DMInputEvent(), count: 32)
        while true {
            let count = buffer.withUnsafeMutableBufferPointer { pointer -> Int32 in
                guard let base = pointer.baseAddress else { return 0 }
                return dm_bridge_take_input_for_link(bridge, channel.linkID, base, Int32(pointer.count))
            }
            if count <= 0 { break }
            if canInject, let displaySession = channel.displaySession {
                let bounds = displaySession.bounds()
                if !channel.loggedFirstInputBatch {
                    channel.loggedFirstInputBatch = true
                    NSLog("DeskMate: [link %llu] injecting touch batch count=%d mode=%@ bounds=(%.0f,%.0f %.0fx%.0f)",
                          channel.linkID, count, touchInputMode.rawValue,
                          bounds.origin.x, bounds.origin.y, bounds.width, bounds.height)
                }
                for index in 0..<Int(count) {
                    _ = channel.inputEngine.process(DMInputEventValue(buffer[index]), displayBounds: bounds)
                }
            } else {
                let now = ProcessInfo.processInfo.systemUptime
                if (now - channel.lastBlockedInputLogTime) >= 2.0 {
                    channel.lastBlockedInputLogTime = now
                    NSLog("DeskMate: [link %llu] dropped %d touch events (inputWanted=%d allowInput=%d ax=%d postEvent=%d hasDisplay=%d)",
                          channel.linkID, count, channel.inputWanted ? 1 : 0, allowInput ? 1 : 0,
                          cachedAXPermission ? 1 : 0, cachedPostEventPermission ? 1 : 0,
                          channel.displaySession != nil ? 1 : 0)
                }
            }
            if count < Int32(buffer.count) { break }
        }
    }

    @MainActor
    private func rearrangeAllVirtualDisplays() async {
        let entries: [(session: DisplaySession, position: DisplayPosition)] = allActiveChannelsSorted().compactMap { channel in
            guard let session = channel.displaySession, session.info.createdByUs else { return nil }
            return (session: session, position: channel.position)
        }
        guard !entries.isEmpty else { return }
        await DisplayConfigCoordinator.shared.run {
            DisplaySession.arrangeDisplays(entries)
        }
    }

    private func applyDisplayLayout(_ origins: [(displayID: UInt32, origin: CGPoint)]) {
        guard !origins.isEmpty else { return }
        Task { @MainActor in
            let ok = (try? await DisplayConfigCoordinator.shared.run {
                DisplayInventory.applyDisplayLayout(origins)
            }) ?? false
            guard ok else {
                self.syncWindow()
                return
            }
            let updated = DisplayInventory.snapshotTopologyDisplays(sessions: self.windowModel.state.activeSessions)
            if let primary = updated.first(where: \.isPrimary) ?? updated.first {
                for channel in self.channels.values {
                    if let did = channel.displaySession?.info.displayID,
                       let match = updated.first(where: { $0.id == did }) {
                        let inferred = DisplayTopologySolver.inferCardinalPosition(primary: primary, target: match)
                        channel.position = inferred
                        if !channel.deviceID.isEmpty {
                            self.pairedStore.setPreferredPosition(id: channel.deviceID, position: inferred)
                        }
                    }
                }
            }
            self.syncWindow()
        }
    }

    private func virtualDisplayName(for channel: DeviceChannel) -> String {
        let base = pairedStore.displayName(for: channel.deviceID) ?? channel.deviceName
        let trimmed = base.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            return "DeskMate 副屏 \(channel.linkID)"
        }
        return "DeskMate · \(trimmed)"
    }

    @MainActor
    private func startCaptureIfNeeded(for channel: DeviceChannel) async {
        channel.destroyDisplayTask?.cancel()
        channel.destroyDisplayTask = nil
        refreshPermissions()
        guard channel.captureWanted, videoEnabled, channel.capture == nil,
              !channel.captureStarting, !channel.removedFromBridge,
              cachedScreenPermission else { return }

        channel.captureStarting = true
        defer {
            channel.captureStarting = false
            if !channel.removedFromBridge && channel.captureWanted && videoEnabled &&
               channel.capture == nil && channel.captureError == nil && fatalError == nil &&
               (autoVirtualDisplay || channel.displaySession != nil || !displayChoices().isEmpty) {
                Task { await self.startCaptureIfNeeded(for: channel) }
            }
        }

        let isDryRun = (channel.linkID == 0)
        let negotiatedW = (!isDryRun ? bridge.map { Int(dm_bridge_negotiated_width_for_link($0, channel.linkID)) } : nil) ?? 0
        let negotiatedH = (!isDryRun ? bridge.map { Int(dm_bridge_negotiated_height_for_link($0, channel.linkID)) } : nil) ?? 0
        let negotiatedHz = (!isDryRun ? bridge.map { Int(dm_bridge_negotiated_refresh_hz_for_link($0, channel.linkID)) } : nil) ?? 0
        let negotiatedCodecRaw = (!isDryRun ? bridge.map { Int(dm_bridge_negotiated_codec_for_link($0, channel.linkID)) } : nil) ?? 0

        let captureWidth = negotiatedW > 0 ? negotiatedW : options.width
        let captureHeight = negotiatedH > 0 ? negotiatedH : options.height
        let captureFrameRate = Self.effectiveFrameRate(
            preferredHz: preferredFrameRateHz,
            negotiatedHz: negotiatedHz,
            explicitFrameRate: options.frameRateExplicit ? options.frameRate : nil,
            fallbackHz: targetFrameRate(for: channel)
        )
        let captureCodec: String
        switch negotiatedCodecRaw {
        case 1: captureCodec = "h264"
        case 2: captureCodec = "hevc"
        default: captureCodec = options.codec
        }

        if autoVirtualDisplay {
            let needsCreate: Bool
            if let existing = channel.displaySession {
                needsCreate = !existing.info.createdByUs ||
                    existing.info.pixelWidth != captureWidth ||
                    existing.info.pixelHeight != captureHeight ||
                    abs(existing.info.refreshRate - captureFrameRate) > 1.0
            } else {
                needsCreate = true
            }
            if needsCreate {
                if let old = channel.displaySession {
                    channel.displaySession = nil
                    if old.info.createdByUs {
                        await DisplayConfigCoordinator.shared.run {
                            old.destroy()
                        }
                        await rearrangeAllVirtualDisplays()
                    }
                }
                guard channel.captureWanted, videoEnabled, autoVirtualDisplay, !channel.removedFromBridge else { return }

                if let preview = manualPreviewSession, preview.info.createdByUs {
                    manualPreviewSession = nil
                    await DisplayConfigCoordinator.shared.run {
                        preview.destroy()
                    }
                }

                let usedSerials = Set(
                    allActiveChannelsSorted()
                        .filter { $0.linkID != channel.linkID && $0.virtualSerial != 0 }
                        .map(\.virtualSerial)
                )
                let seed = channel.deviceID.isEmpty ? "link:\(channel.linkID)" : channel.deviceID
                let serialNum = DisplaySession.stableSerialNumber(for: seed, avoiding: usedSerials)
                let displayName = virtualDisplayName(for: channel)
                let positionSnapshot = channel.position

                do {
                    let created = try await DisplayConfigCoordinator.shared.run {
                        try DisplaySession.createMatchingDevice(
                            pixelWidth: captureWidth,
                            pixelHeight: captureHeight,
                            refreshRate: captureFrameRate,
                            position: positionSnapshot,
                            displayName: displayName,
                            serialNum: serialNum
                        )
                    }
                    guard channel.captureWanted, videoEnabled, autoVirtualDisplay, !channel.removedFromBridge else {
                        await DisplayConfigCoordinator.shared.run {
                            created.destroy()
                        }
                        return
                    }
                    channel.virtualSerial = serialNum
                    channel.displaySession = created
                    await rearrangeAllVirtualDisplays()
                    NSLog("DeskMate: [link %llu] created virtual display %u (%@ serial=0x%08X) at %@",
                          channel.linkID, created.info.displayID, created.info.summary, serialNum, positionSnapshot.rawValue)
                } catch {
                    channel.captureError = "虚拟显示器创建失败：\(error)"
                    syncWindow()
                    return
                }
            }
        } else if channel.displaySession == nil {
            let usedDisplayIDs = Set(
                allActiveChannelsSorted()
                    .filter { $0.linkID != channel.linkID }
                    .compactMap { $0.displaySession?.info.displayID }
            )
            do {
                let attached = try attachManualDisplay(excluding: usedDisplayIDs)
                channel.displaySession = attached
                channel.manualDisplayID = attached.info.displayID
            } catch {
                channel.captureError = "\(error)"
                syncWindow()
                return
            }
        }

        guard let displaySession = channel.displaySession else { return }
        let pipeline = CapturePipeline(
            displayID: displaySession.info.displayID,
            width: captureWidth,
            height: captureHeight,
            frameRate: captureFrameRate,
            codec: captureCodec,
            keyframeRequestIDProvider: isDryRun ? nil : { [weak self] in
                guard let bridge = self?.bridge else { return 0 }
                return dm_bridge_keyframe_request_id_for_link(bridge, channel.linkID)
            },
            onFrame: { [weak self] frame in
                guard !isDryRun else { return }
                self?.submit(frame, for: channel)
            },
            onFailure: { [weak self, weak channel] message in
                DispatchQueue.main.async {
                    channel?.captureError = message
                    self?.fail(message)
                }
            }
        )
        do {
            try await pipeline.start()
            guard channel.captureWanted, videoEnabled, !channel.removedFromBridge else {
                await pipeline.stop()
                if !channel.captureWanted, let existing = channel.displaySession, existing.info.createdByUs {
                    scheduleDestroyDisplay(for: channel)
                }
                return
            }
            channel.capture = pipeline
            channel.activeCaptureFrameRate = Int(captureFrameRate.rounded())
            channel.captureError = nil
            NSLog("DeskMate: [link %llu] capture started on display %u at %dx%d@%d %@",
                  channel.linkID, displaySession.info.displayID, captureWidth, captureHeight,
                  Int(captureFrameRate.rounded()), captureCodec)
            syncWindow()
            updateStatusIcon()
        } catch {
            channel.captureError = "capture start failed: \(error)"
            fail("capture start failed: \(error)")
        }
    }

    @MainActor
    private func scheduleDestroyDisplay(for channel: DeviceChannel) {
        channel.destroyDisplayTask?.cancel()
        channel.destroyDisplayTask = Task { @MainActor [weak self, channel] in
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            guard !Task.isCancelled, !channel.captureWanted,
                  let existing = channel.displaySession, existing.info.createdByUs else { return }
            channel.displaySession = nil
            NSLog("DeskMate: [link %llu] destroying auto-created virtual display %u",
                  channel.linkID, existing.info.displayID)
            await DisplayConfigCoordinator.shared.run {
                existing.destroy()
            }
            await self?.rearrangeAllVirtualDisplays()
            self?.syncWindow()
            self?.updateStatusIcon()
        }
    }

    private func stopCapture(for channel: DeviceChannel, destroyAutoDisplay: Bool, immediate: Bool = false) {
        let pipeline = channel.capture
        channel.capture = nil
        channel.activeCaptureFrameRate = 0
        channel.liveFps = 0
        channel.destroyDisplayTask?.cancel()
        channel.destroyDisplayTask = nil
        let destroyNow = destroyAutoDisplay && immediate
        Task { @MainActor in
            if let pipeline {
                await pipeline.stop()
            }
            if destroyNow, let existing = channel.displaySession, existing.info.createdByUs {
                channel.displaySession = nil
                NSLog("DeskMate: [link %llu] immediately destroying auto-created virtual display %u",
                      channel.linkID, existing.info.displayID)
                await DisplayConfigCoordinator.shared.run {
                    existing.destroy()
                }
                await self.rearrangeAllVirtualDisplays()
            } else if destroyAutoDisplay && !immediate && !channel.captureWanted,
                      let existing = channel.displaySession, existing.info.createdByUs {
                self.scheduleDestroyDisplay(for: channel)
            }
            self.syncWindow()
            self.updateStatusIcon()
        }
    }

    private func submit(_ frame: EncodedFrame, for channel: DeviceChannel) {
        guard let bridge, channel.capture != nil else { return }
        let seconds = CMTimeGetSeconds(frame.presentationTime)
        let ptsUs = seconds.isFinite && seconds >= 0
            ? UInt64(seconds * 1_000_000)
            : DispatchTime.now().uptimeNanoseconds / 1_000
        _ = frame.annexB.withUnsafeBytes { raw -> Int32 in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return -1 }
            return dm_bridge_submit_video_for_link(
                bridge,
                channel.linkID,
                base,
                frame.annexB.count,
                ptsUs,
                frame.keyframe ? 1 : 0
            )
        }
    }

    private func installAvailableUpdate() {
        updateManager.downloadAndInstall { [weak self] in
            await self?.prepareForAppUpdateExit()
        }
    }

    private func prepareForAppUpdateExit() async {
        let state = await MainActor.run { () -> ([DeviceChannel], UInt?) in
            self.timer?.invalidate()
            self.timer = nil
            self.keyframeTimer?.invalidate()
            self.keyframeTimer = nil
            self.serviceRunning = false
            let activeChannels = self.allActiveChannelsSorted()
            self.channels.removeAll()
            self.selectedLinkID = nil
            self.dryRunChannel = nil
            self.manualPreviewSession?.destroy()
            self.manualPreviewSession = nil
            for channel in activeChannels {
                channel.removedFromBridge = true
                channel.captureWanted = false
                channel.inputWanted = false
                channel.destroyDisplayTask?.cancel()
                channel.destroyDisplayTask = nil
            }
            let bridgeRaw = self.bridge.map { UInt(bitPattern: $0) }
            self.bridge = nil
            if let token = self.activityToken {
                ProcessInfo.processInfo.endActivity(token)
                self.activityToken = nil
            }
            return (activeChannels, bridgeRaw)
        }
        let activeChannels = state.0
        for channel in activeChannels {
            await channel.capture?.stop()
            channel.capture = nil
        }
        let createdSessions = activeChannels.compactMap { $0.displaySession }.filter { $0.info.createdByUs }
        if !createdSessions.isEmpty {
            await DisplayConfigCoordinator.shared.run {
                for session in createdSessions {
                    session.destroy()
                }
            }
        }
        for channel in activeChannels {
            channel.displaySession = nil
        }
        if let bridgeRaw = state.1, let ptr = OpaquePointer(bitPattern: bridgeRaw) {
            dm_bridge_destroy(ptr)
        }
    }

    private func updateStatusIcon() {
        guard let button = statusItem?.button else { return }
        let linked = windowModel.state.linked
        let streaming = allActiveChannelsSorted().contains { $0.capture != nil && $0.captureWanted && videoEnabled }
        if linked != lastIconLinked || streaming != lastIconStreaming {
            lastIconLinked = linked
            lastIconStreaming = streaming
            button.image = MenuBarIcon.makeImage(connected: linked, streaming: streaming)
        }
        button.toolTip = fatalError ?? (windowModel.state.streamSummary.isEmpty ? "DeskMate" : "DeskMate · \(windowModel.state.streamSummary)")
    }

    private func fail(_ message: String) {
        if fatalError == nil {
            fatalError = message
        }
        NSLog("DeskMate error: %@", message)
        DispatchQueue.main.async { [weak self] in
            self?.syncWindow()
            self?.updateStatusIcon()
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
