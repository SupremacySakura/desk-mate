import AppKit
import CoreGraphics
import Foundation

struct TouchContactSample: Equatable {
    let id: Int
    let x: Float
    let y: Float
}

enum TouchScrollPhase: Equatable {
    case began
    case changed
    case ended
}

protocol TouchInputSink: AnyObject {
    func warpCursor(to point: CGPoint)
    func currentCursorLocation() -> CGPoint?
    func postMouse(
        type: CGEventType,
        at point: CGPoint,
        button: CGMouseButton,
        deltaX: Int64,
        deltaY: Int64,
        clickState: Int64
    )
    func postScroll(at point: CGPoint, deltaX: Int32, deltaY: Int32, phase: TouchScrollPhase)
    func postShortcut(keyCode: CGKeyCode, modifiers: CGEventFlags)
    func desktopUnionBounds(fallback: CGRect) -> CGRect
    func virtualHIDAvailable(for mode: TouchInputMode) -> Bool
    func sendVirtualHID(mode: TouchInputMode, contacts: [TouchContactSample], primaryButton: Bool) -> Bool
    func resetVirtualHID()
}

final class LiveTouchInputSink: TouchInputSink {
    private let eventSource = CGEventSource(stateID: .hidSystemState)
    private var hidSession: DMVirtualHIDSession?
    private var hidSessionMode: DMVirtualHIDMode?
    private var mouseEventNumber: Int64 = 1

    init() {
        eventSource?.localEventsSuppressionInterval = 0.0
    }

    func warpCursor(to point: CGPoint) {
        CGWarpMouseCursorPosition(point)
    }

    func currentCursorLocation() -> CGPoint? {
        CGEvent(source: nil)?.location
    }

    func postMouse(
        type: CGEventType,
        at point: CGPoint,
        button: CGMouseButton,
        deltaX: Int64,
        deltaY: Int64,
        clickState: Int64
    ) {
        guard let event = CGEvent(
            mouseEventSource: eventSource,
            mouseType: type,
            mouseCursorPosition: point,
            mouseButton: button
        ) else {
            return
        }
        if type == .leftMouseDown || type == .rightMouseDown {
            mouseEventNumber &+= 1
        }
        if type != .mouseMoved {
            event.setIntegerValueField(.mouseEventNumber, value: mouseEventNumber)
        }
        if type == .leftMouseDown || type == .leftMouseDragged {
            event.setDoubleValueField(.mouseEventPressure, value: 1.0)
        } else if type == .leftMouseUp {
            event.setDoubleValueField(.mouseEventPressure, value: 0.0)
        }
        if deltaX != 0 || deltaY != 0 {
            event.setIntegerValueField(.mouseEventDeltaX, value: deltaX)
            event.setIntegerValueField(.mouseEventDeltaY, value: deltaY)
        }
        if clickState > 0 {
            event.setIntegerValueField(.mouseEventClickState, value: clickState)
        }
        event.post(tap: .cghidEventTap)
    }

    func postScroll(at point: CGPoint, deltaX: Int32, deltaY: Int32, phase: TouchScrollPhase) {
        guard let scroll = CGEvent(
            scrollWheelEvent2Source: eventSource,
            units: .pixel,
            wheelCount: 2,
            wheel1: deltaY,
            wheel2: deltaX,
            wheel3: 0
        ) else {
            return
        }
        scroll.location = point
        scroll.setIntegerValueField(.scrollWheelEventIsContinuous, value: 1)
        let cgPhase: Int64
        switch phase {
        case .began: cgPhase = Int64(CGScrollPhase.began.rawValue)
        case .changed: cgPhase = Int64(CGScrollPhase.changed.rawValue)
        case .ended: cgPhase = Int64(CGScrollPhase.ended.rawValue)
        }
        scroll.setIntegerValueField(.scrollWheelEventScrollPhase, value: cgPhase)
        scroll.post(tap: .cghidEventTap)
    }

    func postShortcut(keyCode: CGKeyCode, modifiers: CGEventFlags) {
        guard let keyDown = CGEvent(keyboardEventSource: eventSource, virtualKey: keyCode, keyDown: true),
              let keyUp = CGEvent(keyboardEventSource: eventSource, virtualKey: keyCode, keyDown: false) else {
            return
        }
        keyDown.flags = modifiers
        keyUp.flags = modifiers
        keyDown.post(tap: .cghidEventTap)
        keyUp.post(tap: .cghidEventTap)
    }

    func desktopUnionBounds(fallback: CGRect) -> CGRect {
        let ids = DisplayInventory.onlineDisplayIDs()
        guard !ids.isEmpty else { return fallback }
        var union = CGRect.null
        for id in ids {
            let bounds = CGDisplayBounds(id)
            if !bounds.isEmpty {
                union = union.isNull ? bounds : union.union(bounds)
            }
        }
        return union.isNull || union.isEmpty ? fallback : union
    }

    func virtualHIDAvailable(for mode: TouchInputMode) -> Bool {
        guard mode.usesVirtualHIDWhenAvailable else { return false }
        let info = DMVirtualHIDSession.probeCapability()
        return (info["available"] as? Bool) ?? false
    }

    func sendVirtualHID(mode: TouchInputMode, contacts: [TouchContactSample], primaryButton: Bool) -> Bool {
        let targetMode: DMVirtualHIDMode
        switch mode {
        case .nativeTouch:
            targetMode = .touchScreen
        case .virtualTrackpad:
            targetMode = .trackpad
        default:
            return false
        }

        if hidSessionMode != targetMode {
            hidSession?.invalidate()
            hidSession = nil
            hidSessionMode = nil
        }
        if hidSession == nil {
            hidSession = try? DMVirtualHIDSession.create(mode: targetMode)
            if hidSession != nil {
                hidSessionMode = targetMode
            }
        }
        guard let hidSession else { return false }

        var rawContacts = contacts.prefix(10).map { contact -> DMVirtualHIDContact in
            let clampedX = UInt16((min(max(contact.x, 0), 1) * 32767.0).rounded())
            let clampedY = UInt16((min(max(contact.y, 0), 1) * 32767.0).rounded())
            return DMVirtualHIDContact(
                contactID: UInt8(clamping: max(0, contact.id)),
                active: 1,
                x: clampedX,
                y: clampedY
            )
        }
        return rawContacts.withUnsafeMutableBufferPointer { buffer in
            hidSession.sendContacts(buffer.baseAddress, count: buffer.count, primaryButton: primaryButton)
        }
    }

    func resetVirtualHID() {
        hidSession?.invalidate()
        hidSession = nil
        hidSessionMode = nil
    }

    static func probeCapabilityDetail() -> (available: Bool, detail: String) {
        let info = DMVirtualHIDSession.probeCapability()
        let available = (info["available"] as? Bool) ?? false
        let detail = (info["detail"] as? String) ?? ""
        return (available, detail)
    }
}

/// Stateful 4-mode multi-touch interpreter running on normalized tablet input.
final class TouchInputEngine {
    private struct ActiveContact {
        let id: Int
        var normX: Float
        var normY: Float
        let downX: Float
        let downY: Float
        let downTime: TimeInterval
        var lastNormX: Float
        var lastNormY: Float
    }

    private struct PendingSinglePress {
        let id: Int
        let startPoint: CGPoint
        let startNormX: Float
        let startNormY: Float
        let startTime: TimeInterval
        var movedExceedsTap: Bool
    }

    private enum KeyCodes {
        static let equal: CGKeyCode = 24     // ⌘+
        static let minus: CGKeyCode = 27     // ⌘-
        static let leftArrow: CGKeyCode = 123
        static let rightArrow: CGKeyCode = 124
        static let downArrow: CGKeyCode = 125
        static let upArrow: CGKeyCode = 126
    }

    private let sink: TouchInputSink
    private var currentMode: TouchInputMode
    private var fallbackNotified = false

    private var contacts: [Int: ActiveContact] = [:]
    private var primaryID: Int?
    private var maxFingersInGesture = 0
    private var gestureCooldownUntilZero = false

    private var lastCursorPoint: CGPoint?
    private var lastClickTime: TimeInterval = 0
    private var lastClickPoint: CGPoint?
    private var clickCount: Int64 = 1

    private var pendingSingle: PendingSinglePress?
    private var singleDragging = false
    private var trackpadLatchDragging = false
    private var trackpadCursor: CGPoint?
    private var trackpadLastNorm: (x: Float, y: Float)?

    private var twoFingerActive = false
    private var twoFingerStartTime: TimeInterval = 0
    private var twoFingerStartMidNorm: (x: Float, y: Float) = (0, 0)
    private var twoFingerPrevMidNorm: (x: Float, y: Float) = (0, 0)
    private var twoFingerStartDistPts: CGFloat = 0
    private var twoFingerPinchRefDistNorm: Float = 0
    private var twoFingerLockedPinch = false
    private var twoFingerDidScroll = false

    private var scrollRemainderX: Double = 0
    private var scrollRemainderY: Double = 0

    private var multiSwipeActive = false
    private var multiSwipeFingerCount = 0
    private var multiSwipeStartCentroid: (x: Float, y: Float) = (0, 0)
    private var multiSwipeTriggered = false

    private var virtualHIDActive = false

    // Legacy single-pointer / pre-multitouch fallback state
    private var legacyLastPoint: CGPoint?
    private var legacyPrimaryDown = false

    init(sink: TouchInputSink, mode: TouchInputMode = .compatibleGestures) {
        self.sink = sink
        self.currentMode = mode
    }

    var mode: TouchInputMode {
        get { currentMode }
        set {
            guard newValue != currentMode else { return }
            reset(displayBounds: nil)
            sink.resetVirtualHID()
            currentMode = newValue
            fallbackNotified = false
        }
    }

    var isUsingFallbackBackend: Bool {
        guard currentMode.usesVirtualHIDWhenAvailable else { return false }
        return !sink.virtualHIDAvailable(for: currentMode)
    }

    func capabilityStatus(probeDetail: String = "") -> TouchCapabilityStatus {
        let hidOK = sink.virtualHIDAvailable(for: currentMode)
        switch currentMode {
        case .nativeTouch:
            if hidOK {
                return TouchCapabilityStatus(
                    virtualHIDAvailable: true,
                    fallbackActive: false,
                    activeBackendLabel: "虚拟触摸屏 HID (最多 10 触点)",
                    statusDetail: "已启用 IOHIDUserDevice 原生多点触控通路。"
                )
            }
            return TouchCapabilityStatus(
                virtualHIDAvailable: false,
                fallbackActive: true,
                activeBackendLabel: "兼容手势映射（回退）",
                statusDetail: probeDetail.isEmpty ? "当前签名未包含 virtual HID 权限，已自动使用兼容手势映射。" : probeDetail
            )
        case .virtualTrackpad:
            if hidOK {
                return TouchCapabilityStatus(
                    virtualHIDAvailable: true,
                    fallbackActive: false,
                    activeBackendLabel: "虚拟妙控板 HID",
                    statusDetail: "已启用 IOHIDUserDevice 妙控板模拟通路。"
                )
            }
            return TouchCapabilityStatus(
                virtualHIDAvailable: false,
                fallbackActive: true,
                activeBackendLabel: "相对触控板手势映射（回退）",
                statusDetail: probeDetail.isEmpty ? "未取得 virtual HID 授权，已自动使用相对移动 + 触控板手势模拟。" : probeDetail
            )
        case .compatibleGestures:
            return TouchCapabilityStatus(
                virtualHIDAvailable: hidOK,
                fallbackActive: false,
                activeBackendLabel: "兼容手势映射",
                statusDetail: "使用 CoreGraphics 事件与系统快捷键执行单指 / 多指操作。"
            )
        case .singleTouchOnly:
            return TouchCapabilityStatus(
                virtualHIDAvailable: hidOK,
                fallbackActive: false,
                activeBackendLabel: "仅单点触控",
                statusDetail: "仅处理主触点点击与拖动，忽略多指手势。"
            )
        }
    }

    @discardableResult
    func process(_ event: DMInputEventValue, displayBounds: CGRect, now: TimeInterval = ProcessInfo.processInfo.systemUptime) -> Bool {
        guard displayBounds.width > 0, displayBounds.height > 0 else { return false }
        if !event.isMultiTouchFrame {
            return processLegacyEvent(event, displayBounds: displayBounds, now: now)
        }

        let x = clamp(event.x)
        let y = clamp(event.y)
        let id = event.pointerID

        switch event.kind {
        case .touchDown:
            contacts[id] = ActiveContact(
                id: id,
                normX: x,
                normY: y,
                downX: x,
                downY: y,
                downTime: now,
                lastNormX: x,
                lastNormY: y
            )
            if primaryID == nil {
                primaryID = id
            }
        case .touchMove:
            if var existing = contacts[id] {
                existing.lastNormX = existing.normX
                existing.lastNormY = existing.normY
                existing.normX = x
                existing.normY = y
                contacts[id] = existing
            } else {
                contacts[id] = ActiveContact(
                    id: id,
                    normX: x,
                    normY: y,
                    downX: x,
                    downY: y,
                    downTime: now,
                    lastNormX: x,
                    lastNormY: y
                )
                if primaryID == nil {
                    primaryID = id
                }
            }
        case .touchUp, .touchCancel:
            contacts.removeValue(forKey: id)
        case .scroll, .key:
            return false
        }

        maxFingersInGesture = max(maxFingersInGesture, contacts.count)

        guard event.isFrameCommit else {
            return true
        }

        return evaluateFrame(displayBounds: displayBounds, now: now)
    }

    func tick(displayBounds: CGRect, now: TimeInterval = ProcessInfo.processInfo.systemUptime) {
        guard displayBounds.width > 0, displayBounds.height > 0 else { return }
        guard !gestureCooldownUntilZero, contacts.count == 1 else { return }

        switch currentMode {
        case .compatibleGestures:
            if let pending = pendingSingle, (now - pending.startTime) >= 0.045 {
                commitSinglePressIfNeeded(now: now)
            }
        case .nativeTouch:
            if !sink.virtualHIDAvailable(for: .nativeTouch),
               let pending = pendingSingle, (now - pending.startTime) >= 0.045 {
                commitSinglePressIfNeeded(now: now)
            }
        case .virtualTrackpad:
            if let pending = pendingSingle, !trackpadLatchDragging, !pending.movedExceedsTap,
               (now - pending.startTime) >= 0.36 {
                trackpadLatchDragging = true
                if virtualHIDActive && sink.virtualHIDAvailable(for: .virtualTrackpad) {
                    _ = dispatchVirtualTrackpadHID(displayBounds: displayBounds, now: now)
                } else {
                    let cursor = currentTrackpadCursor(in: displayBounds)
                    trackpadCursor = cursor
                    clickCount = nextClickCount(at: cursor, now: now)
                    sink.postMouse(type: .leftMouseDown, at: cursor, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                    singleDragging = true
                    lastCursorPoint = cursor
                }
            }
        case .singleTouchOnly:
            break
        }
    }

    func reset(displayBounds: CGRect?) {
        let point = lastCursorPoint ?? legacyLastPoint ?? (displayBounds.map { CGPoint(x: $0.midX, y: $0.midY) } ?? .zero)
        if twoFingerDidScroll {
            sink.postScroll(at: point, deltaX: 0, deltaY: 0, phase: .ended)
            twoFingerDidScroll = false
        }
        if singleDragging || trackpadLatchDragging || legacyPrimaryDown {
            sink.postMouse(type: .leftMouseUp, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
        }
        if virtualHIDActive {
            _ = sink.sendVirtualHID(mode: currentMode, contacts: [], primaryButton: false)
            virtualHIDActive = false
        }
        contacts.removeAll()
        primaryID = nil
        maxFingersInGesture = 0
        gestureCooldownUntilZero = false
        pendingSingle = nil
        singleDragging = false
        trackpadLatchDragging = false
        trackpadCursor = nil
        trackpadLastNorm = nil
        legacyPrimaryDown = false
        twoFingerActive = false
        twoFingerLockedPinch = false
        multiSwipeActive = false
        multiSwipeFingerCount = 0
        multiSwipeTriggered = false
        scrollRemainderX = 0
        scrollRemainderY = 0
    }

    // MARK: - Frame Evaluation Dispatch

    private func evaluateFrame(displayBounds: CGRect, now: TimeInterval) -> Bool {
        switch currentMode {
        case .singleTouchOnly:
            return evaluateSingleTouchOnlyFrame(displayBounds: displayBounds, now: now)
        case .compatibleGestures:
            return evaluateCompatibleGesturesFrame(displayBounds: displayBounds, now: now)
        case .virtualTrackpad:
            if sink.virtualHIDAvailable(for: .virtualTrackpad) {
                return dispatchVirtualTrackpadHID(displayBounds: displayBounds, now: now)
            }
            return evaluateRelativeTrackpadFrame(displayBounds: displayBounds, now: now)
        case .nativeTouch:
            if sink.virtualHIDAvailable(for: .nativeTouch) {
                return dispatchNativeTouchHID(displayBounds: displayBounds)
            }
            return evaluateCompatibleGesturesFrame(displayBounds: displayBounds, now: now)
        }
    }

    // MARK: - Mode 1: Native Touch Screen (IOHIDUserDevice)

    private func dispatchNativeTouchHID(displayBounds: CGRect) -> Bool {
        let unionBounds = sink.desktopUnionBounds(fallback: displayBounds)
        let samples = contacts.values
            .sorted { $0.id < $1.id }
            .prefix(10)
            .map { contact -> TouchContactSample in
                let pt = mapPoint(contact.normX, contact.normY, in: displayBounds)
                let ux = Float((pt.x - unionBounds.origin.x) / max(unionBounds.width, 1))
                let uy = Float((pt.y - unionBounds.origin.y) / max(unionBounds.height, 1))
                return TouchContactSample(id: contact.id, x: clamp(ux), y: clamp(uy))
            }
        virtualHIDActive = !samples.isEmpty
        if contacts.isEmpty {
            maxFingersInGesture = 0
        }
        return sink.sendVirtualHID(mode: .nativeTouch, contacts: samples, primaryButton: false)
    }

    // MARK: - Mode 2: Virtual Trackpad (IOHIDUserDevice + Relative Fallback)

    private func dispatchVirtualTrackpadHID(displayBounds: CGRect, now: TimeInterval) -> Bool {
        let count = contacts.count
        if count == 0 {
            let hadState = virtualHIDActive || trackpadLatchDragging
            pendingSingle = nil
            trackpadLatchDragging = false
            maxFingersInGesture = 0
            gestureCooldownUntilZero = false
            virtualHIDActive = false
            if hadState {
                return sink.sendVirtualHID(mode: .virtualTrackpad, contacts: [], primaryButton: false)
            }
            return true
        }

        if count == 1, let c = contacts.values.first {
            let pt = mapPoint(c.normX, c.normY, in: displayBounds)
            if var pending = pendingSingle, pending.id == c.id {
                let travel = hypot(pt.x - pending.startPoint.x, pt.y - pending.startPoint.y)
                if travel >= 10.0 {
                    pending.movedExceedsTap = true
                    pendingSingle = pending
                } else if !trackpadLatchDragging && !pending.movedExceedsTap && (now - pending.startTime) >= 0.36 {
                    trackpadLatchDragging = true
                }
            } else if maxFingersInGesture == 1 {
                pendingSingle = PendingSinglePress(
                    id: c.id,
                    startPoint: pt,
                    startNormX: c.normX,
                    startNormY: c.normY,
                    startTime: now,
                    movedExceedsTap: false
                )
            }
        } else {
            pendingSingle = nil
            trackpadLatchDragging = false
        }

        let samples = contacts.values
            .sorted { $0.id < $1.id }
            .prefix(10)
            .map { TouchContactSample(id: $0.id, x: $0.normX, y: $0.normY) }
        virtualHIDActive = true
        return sink.sendVirtualHID(
            mode: .virtualTrackpad,
            contacts: samples,
            primaryButton: trackpadLatchDragging
        )
    }

    private func evaluateRelativeTrackpadFrame(displayBounds: CGRect, now: TimeInterval) -> Bool {
        let count = contacts.count

        if count == 0 {
            if twoFingerDidScroll {
                let cursor = currentTrackpadCursor(in: displayBounds)
                sink.postScroll(at: cursor, deltaX: 0, deltaY: 0, phase: .ended)
                twoFingerDidScroll = false
            }
            if trackpadLatchDragging || singleDragging {
                let cursor = currentTrackpadCursor(in: displayBounds)
                sink.postMouse(type: .leftMouseUp, at: cursor, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                trackpadLatchDragging = false
                singleDragging = false
            } else if let pending = pendingSingle, !pending.movedExceedsTap,
                      !gestureCooldownUntilZero, maxFingersInGesture <= 1,
                      (now - pending.startTime) < 0.26 {
                let cursor = currentTrackpadCursor(in: displayBounds)
                performLeftClick(at: cursor, now: now)
            } else if twoFingerActive, maxFingersInGesture == 2, !gestureCooldownUntilZero,
                      !twoFingerDidScroll, !twoFingerLockedPinch,
                      (now - twoFingerStartTime) < 0.30 {
                let cursor = currentTrackpadCursor(in: displayBounds)
                performRightClick(at: cursor)
            }
            resetGestureTrackingOnly()
            return true
        }

        if gestureCooldownUntilZero {
            return true
        }

        if count >= 3 {
            if trackpadLatchDragging || singleDragging {
                let cursor = currentTrackpadCursor(in: displayBounds)
                sink.postMouse(type: .leftMouseUp, at: cursor, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                trackpadLatchDragging = false
                singleDragging = false
            }
            pendingSingle = nil
            endTwoFingerScrollIfNeeded(at: currentTrackpadCursor(in: displayBounds))
            twoFingerActive = false
            twoFingerLockedPinch = false
            return handleMultiFingerSwipe(count: count)
        }

        if count == 2 {
            if maxFingersInGesture > 2 {
                gestureCooldownUntilZero = true
                return true
            }
            pendingSingle = nil
            if trackpadLatchDragging || singleDragging {
                let cursor = currentTrackpadCursor(in: displayBounds)
                sink.postMouse(type: .leftMouseUp, at: cursor, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                trackpadLatchDragging = false
                singleDragging = false
            }
            let cursor = currentTrackpadCursor(in: displayBounds)
            return handleTwoFingerFrame(displayBounds: displayBounds, scrollLocation: cursor, now: now)
        }

        // count == 1
        if maxFingersInGesture > 1 {
            endTwoFingerScrollIfNeeded(at: currentTrackpadCursor(in: displayBounds))
            twoFingerActive = false
            twoFingerLockedPinch = false
            gestureCooldownUntilZero = true
            return true
        }

        guard let c = contacts.values.first else { return false }
        let desktopBounds = sink.desktopUnionBounds(fallback: displayBounds)

        if var pending = pendingSingle, pending.id == c.id {
            let prevCursor = trackpadCursor ?? currentTrackpadCursor(in: displayBounds)
            let prevNorm = trackpadLastNorm ?? (c.lastNormX, c.lastNormY)
            trackpadLastNorm = (c.normX, c.normY)

            let gain = 1.25
            let rawDx = Double(c.normX - prevNorm.x) * Double(displayBounds.width) * gain
            let rawDy = Double(c.normY - prevNorm.y) * Double(displayBounds.height) * gain
            let nextCursor = clampPoint(
                CGPoint(x: prevCursor.x + rawDx, y: prevCursor.y + rawDy),
                to: desktopBounds
            )
            let deltaX = Int64((nextCursor.x.rounded() - prevCursor.x.rounded()))
            let deltaY = Int64((nextCursor.y.rounded() - prevCursor.y.rounded()))
            trackpadCursor = nextCursor
            lastCursorPoint = nextCursor

            let totalMove = hypot(
                CGFloat(c.normX - pending.startNormX) * displayBounds.width,
                CGFloat(c.normY - pending.startNormY) * displayBounds.height
            )
            if totalMove >= 10.0 {
                pending.movedExceedsTap = true
                pendingSingle = pending
            }
            if !trackpadLatchDragging && !pending.movedExceedsTap && (now - pending.startTime) >= 0.36 {
                trackpadLatchDragging = true
                clickCount = nextClickCount(at: nextCursor, now: now)
                sink.postMouse(type: .leftMouseDown, at: nextCursor, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                singleDragging = true
            } else if trackpadLatchDragging || singleDragging {
                if deltaX != 0 || deltaY != 0 || nextCursor != prevCursor {
                    sink.postMouse(type: .leftMouseDragged, at: nextCursor, button: .left, deltaX: deltaX, deltaY: deltaY, clickState: clickCount)
                }
            } else if deltaX != 0 || deltaY != 0 || nextCursor != prevCursor {
                sink.postMouse(type: .mouseMoved, at: nextCursor, button: .left, deltaX: deltaX, deltaY: deltaY, clickState: 0)
            }
        } else {
            let liveCursor = currentTrackpadCursor(in: displayBounds)
            trackpadCursor = liveCursor
            trackpadLastNorm = (c.normX, c.normY)
            lastCursorPoint = liveCursor
            let pt = mapPoint(c.normX, c.normY, in: displayBounds)
            pendingSingle = PendingSinglePress(
                id: c.id,
                startPoint: pt,
                startNormX: c.normX,
                startNormY: c.normY,
                startTime: now,
                movedExceedsTap: false
            )
        }
        return true
    }

    // MARK: - Mode 3: Compatible Gesture Mapping

    private func evaluateCompatibleGesturesFrame(displayBounds: CGRect, now: TimeInterval) -> Bool {
        let count = contacts.count

        if count == 0 {
            if twoFingerDidScroll {
                let point = lastCursorPoint ?? CGPoint(x: displayBounds.midX, y: displayBounds.midY)
                sink.postScroll(at: point, deltaX: 0, deltaY: 0, phase: .ended)
                twoFingerDidScroll = false
            }
            if singleDragging {
                let point = lastCursorPoint ?? CGPoint(x: displayBounds.midX, y: displayBounds.midY)
                sink.postMouse(type: .leftMouseUp, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                singleDragging = false
            } else if let pending = pendingSingle, !gestureCooldownUntilZero, maxFingersInGesture <= 1 {
                performLeftClick(at: pending.startPoint, now: now)
            } else if twoFingerActive, maxFingersInGesture == 2, !gestureCooldownUntilZero,
                      !twoFingerDidScroll, !twoFingerLockedPinch,
                      (now - twoFingerStartTime) < 0.30 {
                let tapPt = mapPoint(twoFingerStartMidNorm.x, twoFingerStartMidNorm.y, in: displayBounds)
                performRightClick(at: tapPt)
            }
            resetGestureTrackingOnly()
            return true
        }

        if gestureCooldownUntilZero {
            return true
        }

        if count >= 3 {
            if singleDragging {
                let point = lastCursorPoint ?? CGPoint(x: displayBounds.midX, y: displayBounds.midY)
                sink.postMouse(type: .leftMouseUp, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                singleDragging = false
            }
            pendingSingle = nil
            endTwoFingerScrollIfNeeded(at: lastCursorPoint ?? CGPoint(x: displayBounds.midX, y: displayBounds.midY))
            twoFingerActive = false
            twoFingerLockedPinch = false
            return handleMultiFingerSwipe(count: count)
        }

        if count == 2 {
            if maxFingersInGesture > 2 {
                gestureCooldownUntilZero = true
                return true
            }
            if singleDragging {
                let point = lastCursorPoint ?? CGPoint(x: displayBounds.midX, y: displayBounds.midY)
                sink.postMouse(type: .leftMouseUp, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                singleDragging = false
            }
            pendingSingle = nil
            return handleTwoFingerFrame(displayBounds: displayBounds, scrollLocation: nil, now: now)
        }

        // count == 1
        if maxFingersInGesture > 1 {
            endTwoFingerScrollIfNeeded(at: lastCursorPoint ?? CGPoint(x: displayBounds.midX, y: displayBounds.midY))
            twoFingerActive = false
            twoFingerLockedPinch = false
            gestureCooldownUntilZero = true
            return true
        }

        guard let c = contacts.values.first else { return false }
        let point = mapPoint(c.normX, c.normY, in: displayBounds)

        if singleDragging {
            let prev = lastCursorPoint ?? point
            let dx = Int64((point.x - prev.x).rounded())
            let dy = Int64((point.y - prev.y).rounded())
            lastCursorPoint = point
            sink.postMouse(type: .leftMouseDragged, at: point, button: .left, deltaX: dx, deltaY: dy, clickState: clickCount)
        } else if let pending = pendingSingle, pending.id == c.id {
            let travel = hypot(point.x - pending.startPoint.x, point.y - pending.startPoint.y)
            if travel >= 8.0 || (now - pending.startTime) >= 0.045 {
                commitSinglePressIfNeeded(now: pending.startTime)
                if singleDragging && point != pending.startPoint {
                    let dx = Int64((point.x - pending.startPoint.x).rounded())
                    let dy = Int64((point.y - pending.startPoint.y).rounded())
                    lastCursorPoint = point
                    sink.postMouse(type: .leftMouseDragged, at: point, button: .left, deltaX: dx, deltaY: dy, clickState: clickCount)
                }
            }
        } else {
            pendingSingle = PendingSinglePress(
                id: c.id,
                startPoint: point,
                startNormX: c.normX,
                startNormY: c.normY,
                startTime: now,
                movedExceedsTap: false
            )
        }
        return true
    }

    private func commitSinglePressIfNeeded(now: TimeInterval) {
        guard let pending = pendingSingle, !singleDragging else { return }
        clickCount = nextClickCount(at: pending.startPoint, now: now)
        lastCursorPoint = pending.startPoint
        sink.warpCursor(to: pending.startPoint)
        sink.postMouse(type: .mouseMoved, at: pending.startPoint, button: .left, deltaX: 0, deltaY: 0, clickState: 0)
        sink.postMouse(type: .leftMouseDown, at: pending.startPoint, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
        singleDragging = true
        pendingSingle = nil
    }

    private func handleTwoFingerFrame(displayBounds: CGRect, scrollLocation: CGPoint?, now: TimeInterval) -> Bool {
        let sorted = contacts.values.sorted { $0.id < $1.id }
        guard sorted.count >= 2 else { return false }
        let c0 = sorted[0]
        let c1 = sorted[1]
        let mx = (c0.normX + c1.normX) * 0.5
        let my = (c0.normY + c1.normY) * 0.5
        let normDist = hypot(c0.normX - c1.normX, c0.normY - c1.normY)
        let distPts = hypot(
            CGFloat(c0.normX - c1.normX) * displayBounds.width,
            CGFloat(c0.normY - c1.normY) * displayBounds.height
        )

        if !twoFingerActive {
            twoFingerActive = true
            twoFingerStartTime = now
            twoFingerStartMidNorm = (mx, my)
            twoFingerPrevMidNorm = (mx, my)
            twoFingerStartDistPts = distPts
            twoFingerPinchRefDistNorm = normDist
            twoFingerLockedPinch = false
            twoFingerDidScroll = false
            scrollRemainderX = 0
            scrollRemainderY = 0
            return true
        }

        let centerPts = hypot(
            CGFloat(mx - twoFingerStartMidNorm.x) * displayBounds.width,
            CGFloat(my - twoFingerStartMidNorm.y) * displayBounds.height
        )
        let distChangePts = abs(distPts - twoFingerStartDistPts)

        if !twoFingerDidScroll && (twoFingerLockedPinch || (distChangePts > 22.0 && distChangePts > centerPts * 1.25)) {
            twoFingerLockedPinch = true
            if twoFingerPinchRefDistNorm > 0.001 {
                let ratio = normDist / twoFingerPinchRefDistNorm
                let delta = normDist - twoFingerPinchRefDistNorm
                if ratio >= 1.11 || delta >= 0.042 {
                    sink.postShortcut(keyCode: KeyCodes.equal, modifiers: .maskCommand)
                    twoFingerPinchRefDistNorm = normDist
                } else if ratio <= 0.90 || delta <= -0.042 {
                    sink.postShortcut(keyCode: KeyCodes.minus, modifiers: .maskCommand)
                    twoFingerPinchRefDistNorm = normDist
                }
            } else {
                twoFingerPinchRefDistNorm = normDist
            }
            return true
        }

        if !twoFingerLockedPinch && (twoFingerDidScroll || centerPts > 10.0) {
            let dxNorm = mx - twoFingerPrevMidNorm.x
            let dyNorm = my - twoFingerPrevMidNorm.y
            twoFingerPrevMidNorm = (mx, my)

            let targetPoint = scrollLocation ?? mapPoint(mx, my, in: displayBounds)
            if scrollLocation == nil && (lastCursorPoint == nil || !displayBounds.contains(lastCursorPoint!)) {
                sink.warpCursor(to: targetPoint)
                sink.postMouse(type: .mouseMoved, at: targetPoint, button: .left, deltaX: 0, deltaY: 0, clickState: 0)
                lastCursorPoint = targetPoint
            }

            scrollRemainderY += Double(dyNorm) * Double(displayBounds.height) * 1.25
            scrollRemainderX += Double(dxNorm) * Double(displayBounds.width) * 1.25
            let vertical = Int32(scrollRemainderY.rounded(.towardZero))
            let horizontal = Int32(scrollRemainderX.rounded(.towardZero))
            scrollRemainderY -= Double(vertical)
            scrollRemainderX -= Double(horizontal)

            if vertical != 0 || horizontal != 0 {
                let phase: TouchScrollPhase = twoFingerDidScroll ? .changed : .began
                sink.postScroll(at: targetPoint, deltaX: horizontal, deltaY: vertical, phase: phase)
                twoFingerDidScroll = true
            }
        }
        return true
    }

    private func handleMultiFingerSwipe(count: Int) -> Bool {
        guard count >= 3 else { return false }
        let active = Array(contacts.values)
        let cx = active.reduce(Float(0)) { $0 + $1.normX } / Float(active.count)
        let cy = active.reduce(Float(0)) { $0 + $1.normY } / Float(active.count)

        if !multiSwipeActive || multiSwipeFingerCount != count {
            multiSwipeActive = true
            multiSwipeFingerCount = count
            multiSwipeStartCentroid = (cx, cy)
            return true
        }

        guard !multiSwipeTriggered else { return true }
        let dx = cx - multiSwipeStartCentroid.x
        let dy = cy - multiSwipeStartCentroid.y
        let absX = abs(dx)
        let absY = abs(dy)
        let threshold: Float = 0.055

        if absX >= threshold && absX >= absY * 1.2 {
            // Left swipe moves to the next Space on the right; right swipe moves left.
            let keyCode = dx < 0 ? KeyCodes.rightArrow : KeyCodes.leftArrow
            sink.postShortcut(keyCode: keyCode, modifiers: .maskControl)
            multiSwipeTriggered = true
            gestureCooldownUntilZero = true
        } else if absY >= threshold && absY >= absX * 1.2 {
            // Up opens Mission Control; down opens App Exposé.
            let keyCode = dy < 0 ? KeyCodes.upArrow : KeyCodes.downArrow
            sink.postShortcut(keyCode: keyCode, modifiers: .maskControl)
            multiSwipeTriggered = true
            gestureCooldownUntilZero = true
        }
        return true
    }

    private func endTwoFingerScrollIfNeeded(at point: CGPoint) {
        if twoFingerDidScroll {
            sink.postScroll(at: point, deltaX: 0, deltaY: 0, phase: .ended)
            twoFingerDidScroll = false
        }
    }

    private func resetGestureTrackingOnly() {
        pendingSingle = nil
        trackpadLastNorm = nil
        twoFingerActive = false
        twoFingerLockedPinch = false
        twoFingerDidScroll = false
        multiSwipeActive = false
        multiSwipeFingerCount = 0
        multiSwipeTriggered = false
        gestureCooldownUntilZero = false
        maxFingersInGesture = 0
        scrollRemainderX = 0
        scrollRemainderY = 0
    }

    // MARK: - Mode 4: Single Touch Only

    private func evaluateSingleTouchOnlyFrame(displayBounds: CGRect, now: TimeInterval) -> Bool {
        if contacts.isEmpty {
            if singleDragging {
                let point = lastCursorPoint ?? CGPoint(x: displayBounds.midX, y: displayBounds.midY)
                sink.postMouse(type: .leftMouseUp, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                singleDragging = false
            }
            primaryID = nil
            gestureCooldownUntilZero = false
            maxFingersInGesture = 0
            return true
        }

        if gestureCooldownUntilZero {
            return true
        }

        guard let targetID = primaryID ?? contacts.keys.sorted().first else { return false }
        primaryID = targetID
        guard let c = contacts[targetID] else {
            // Primary finger lifted while secondary still down: release drag and wait until all release.
            if singleDragging {
                let point = lastCursorPoint ?? CGPoint(x: displayBounds.midX, y: displayBounds.midY)
                sink.postMouse(type: .leftMouseUp, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
                singleDragging = false
            }
            gestureCooldownUntilZero = true
            return true
        }

        let point = mapPoint(c.normX, c.normY, in: displayBounds)
        if !singleDragging {
            clickCount = nextClickCount(at: point, now: now)
            lastCursorPoint = point
            sink.warpCursor(to: point)
            sink.postMouse(type: .mouseMoved, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: 0)
            sink.postMouse(type: .leftMouseDown, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
            singleDragging = true
        } else if lastCursorPoint != point {
            let prev = lastCursorPoint ?? point
            let dx = Int64((point.x - prev.x).rounded())
            let dy = Int64((point.y - prev.y).rounded())
            lastCursorPoint = point
            sink.postMouse(type: .leftMouseDragged, at: point, button: .left, deltaX: dx, deltaY: dy, clickState: clickCount)
        }
        return true
    }

    // MARK: - Legacy Non-MultiTouch Wire Fallback

    private func processLegacyEvent(_ event: DMInputEventValue, displayBounds: CGRect, now: TimeInterval) -> Bool {
        if currentMode == .singleTouchOnly && event.kind == .scroll {
            return false
        }
        switch event.kind {
        case .touchDown:
            let point = mapPoint(event.x, event.y, in: displayBounds)
            clickCount = nextClickCount(at: point, now: now)
            legacyLastPoint = point
            lastCursorPoint = point
            legacyPrimaryDown = true
            sink.warpCursor(to: point)
            sink.postMouse(type: .mouseMoved, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: 0)
            sink.postMouse(type: .leftMouseDown, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
            return true
        case .touchMove:
            let point = mapPoint(event.x, event.y, in: displayBounds)
            let prev = legacyLastPoint ?? point
            let dx = Int64((point.x - prev.x).rounded())
            let dy = Int64((point.y - prev.y).rounded())
            legacyLastPoint = point
            lastCursorPoint = point
            if (event.flags & 0x0001) != 0 || legacyPrimaryDown {
                sink.postMouse(type: .leftMouseDragged, at: point, button: .left, deltaX: dx, deltaY: dy, clickState: clickCount)
            } else {
                sink.postMouse(type: .mouseMoved, at: point, button: .left, deltaX: dx, deltaY: dy, clickState: 0)
            }
            return true
        case .touchUp, .touchCancel:
            let point = mapPoint(event.x, event.y, in: displayBounds)
            let prev = legacyLastPoint ?? point
            let dx = Int64((point.x - prev.x).rounded())
            let dy = Int64((point.y - prev.y).rounded())
            legacyLastPoint = point
            lastCursorPoint = point
            legacyPrimaryDown = false
            sink.postMouse(type: .leftMouseUp, at: point, button: .left, deltaX: dx, deltaY: dy, clickState: clickCount)
            return true
        case .scroll:
            let point = mapPoint(event.x, event.y, in: displayBounds)
            if legacyLastPoint == nil || !displayBounds.contains(legacyLastPoint!) {
                sink.warpCursor(to: point)
                sink.postMouse(type: .mouseMoved, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: 0)
                legacyLastPoint = point
            }
            scrollRemainderY += Double(event.dy) * Double(displayBounds.height) * 1.25
            scrollRemainderX += Double(event.dx) * Double(displayBounds.width) * 1.25
            let vertical = Int32(scrollRemainderY.rounded(.towardZero))
            let horizontal = Int32(scrollRemainderX.rounded(.towardZero))
            scrollRemainderY -= Double(vertical)
            scrollRemainderX -= Double(horizontal)
            guard vertical != 0 || horizontal != 0 else { return false }
            sink.postScroll(at: point, deltaX: horizontal, deltaY: vertical, phase: .changed)
            return true
        case .key:
            return false
        }
    }

    // MARK: - Pointer & Coordinate Helpers

    private func performLeftClick(at point: CGPoint, now: TimeInterval) {
        clickCount = nextClickCount(at: point, now: now)
        lastCursorPoint = point
        sink.warpCursor(to: point)
        sink.postMouse(type: .mouseMoved, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: 0)
        sink.postMouse(type: .leftMouseDown, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
        sink.postMouse(type: .leftMouseUp, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: clickCount)
    }

    private func performRightClick(at point: CGPoint) {
        lastCursorPoint = point
        sink.warpCursor(to: point)
        sink.postMouse(type: .mouseMoved, at: point, button: .left, deltaX: 0, deltaY: 0, clickState: 0)
        sink.postMouse(type: .rightMouseDown, at: point, button: .right, deltaX: 0, deltaY: 0, clickState: 1)
        sink.postMouse(type: .rightMouseUp, at: point, button: .right, deltaX: 0, deltaY: 0, clickState: 1)
    }

    private func nextClickCount(at point: CGPoint, now: TimeInterval) -> Int64 {
        let interval = NSEvent.doubleClickInterval > 0 ? NSEvent.doubleClickInterval : 0.5
        if let prevDown = lastClickPoint,
           (now - lastClickTime) <= interval,
           hypot(point.x - prevDown.x, point.y - prevDown.y) <= 18.0 {
            clickCount = min(clickCount + 1, 3)
        } else {
            clickCount = 1
        }
        lastClickTime = now
        lastClickPoint = point
        return clickCount
    }

    private func currentTrackpadCursor(in bounds: CGRect) -> CGPoint {
        if let trackpadCursor {
            return trackpadCursor
        }
        if let systemPoint = sink.currentCursorLocation() {
            return systemPoint
        }
        if let lastCursorPoint {
            return lastCursorPoint
        }
        return CGPoint(x: bounds.midX, y: bounds.midY)
    }

    private func mapPoint(_ x: Float, _ y: Float, in bounds: CGRect) -> CGPoint {
        CGPoint(
            x: bounds.origin.x + bounds.width * CGFloat(clamp(x)),
            y: bounds.origin.y + bounds.height * CGFloat(clamp(y))
        )
    }

    private func clampPoint(_ point: CGPoint, to bounds: CGRect) -> CGPoint {
        CGPoint(
            x: min(max(point.x, bounds.minX), max(bounds.minX, bounds.maxX - 1)),
            y: min(max(point.y, bounds.minY), max(bounds.minY, bounds.maxY - 1))
        )
    }

    private func clamp(_ value: Float) -> Float {
        min(max(value, 0), 1)
    }
}

/// Global facade used by `AppController` for injecting normalized Device input into macOS.
enum InputInjector {
    private static let sink = LiveTouchInputSink()
    private static let engine = TouchInputEngine(sink: sink, mode: .compatibleGestures)
    private static var cachedProbe = LiveTouchInputSink.probeCapabilityDetail()

    static func makeEngine(mode: TouchInputMode) -> TouchInputEngine {
        TouchInputEngine(sink: sink, mode: mode)
    }

    static var mode: TouchInputMode {
        get { engine.mode }
        set { engine.mode = newValue }
    }

    static func refreshCapability() -> TouchCapabilityStatus {
        cachedProbe = LiveTouchInputSink.probeCapabilityDetail()
        return capabilityStatus()
    }

    static func capabilityStatus() -> TouchCapabilityStatus {
        engine.capabilityStatus(probeDetail: cachedProbe.detail)
    }

    @discardableResult
    static func inject(_ event: DMInputEventValue, displayBounds: CGRect) -> Bool {
        engine.process(event, displayBounds: displayBounds)
    }

    static func tick(displayBounds: CGRect) {
        engine.tick(displayBounds: displayBounds)
    }

    static func reset(displayBounds: CGRect? = nil) {
        engine.reset(displayBounds: displayBounds)
    }
}

struct DMInputEventValue {
    enum Kind {
        case touchDown, touchMove, touchUp, touchCancel, scroll, key
    }

    let kind: Kind
    let pointerID: Int
    let flags: Int
    let x: Float
    let y: Float
    let dx: Float
    let dy: Float
    let keycode: UInt32

    var isMultiTouchFrame: Bool { (flags & 0x0002) != 0 }
    var isFrameCommit: Bool { (flags & 0x0004) != 0 }
    var reportedContactCount: Int { (flags & 0x0F00) >> 8 }

    init(_ raw: DMInputEvent) {
        switch raw.kind {
        case 1: kind = .touchDown
        case 2: kind = .touchMove
        case 3: kind = .touchUp
        case 4: kind = .touchCancel
        case 5: kind = .scroll
        default: kind = .key
        }
        pointerID = Int(raw.pointer_id)
        flags = Int(raw.flags)
        x = raw.x
        y = raw.y
        dx = raw.dx
        dy = raw.dy
        keycode = raw.keycode
    }

    init(
        kind: Kind,
        pointerID: Int = 0,
        flags: Int = 0,
        x: Float = 0,
        y: Float = 0,
        dx: Float = 0,
        dy: Float = 0,
        keycode: UInt32 = 0
    ) {
        self.kind = kind
        self.pointerID = pointerID
        self.flags = flags
        self.x = x
        self.y = y
        self.dx = dx
        self.dy = dy
        self.keycode = keycode
    }
}
