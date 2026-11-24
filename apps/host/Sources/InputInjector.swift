import AppKit
import CoreGraphics
import Foundation

/// Translates normalized Device input back into macOS HID events on the target
/// Extended Display.
enum InputInjector {
    private static let eventSource = CGEventSource(stateID: .hidSystemState)
    private static var lastPoint: CGPoint?
    private static var lastDownTime: TimeInterval = 0
    private static var lastDownPoint: CGPoint?
    private static var clickCount: Int64 = 1
    private static var scrollRemainderX: Double = 0
    private static var scrollRemainderY: Double = 0

    static func inject(_ event: DMInputEventValue, displayBounds: CGRect) -> Bool {
        switch event.kind {
        case .touchDown, .touchMove, .touchUp, .touchCancel:
            return injectPointer(event, displayBounds: displayBounds)
        case .scroll:
            return injectScroll(event, displayBounds: displayBounds)
        case .key:
            return false
        }
    }

    private static func mapPoint(_ x: Float, _ y: Float, in bounds: CGRect) -> CGPoint {
        CGPoint(
            x: bounds.origin.x + bounds.width * CGFloat(clamp(x)),
            y: bounds.origin.y + bounds.height * CGFloat(clamp(y))
        )
    }

    private static func injectPointer(_ event: DMInputEventValue, displayBounds: CGRect) -> Bool {
        let point = mapPoint(event.x, event.y, in: displayBounds)
        let prev = lastPoint ?? point
        let deltaX = Int64((point.x - prev.x).rounded())
        let deltaY = Int64((point.y - prev.y).rounded())
        let isPrimaryDown = (event.flags & 0x0001) != 0

        switch event.kind {
        case .touchDown:
            let now = ProcessInfo.processInfo.systemUptime
            let interval = NSEvent.doubleClickInterval > 0 ? NSEvent.doubleClickInterval : 0.5
            if let prevDown = lastDownPoint,
               (now - lastDownTime) <= interval,
               hypot(point.x - prevDown.x, point.y - prevDown.y) <= 18.0 {
                clickCount = min(clickCount + 1, 3)
            } else {
                clickCount = 1
            }
            lastDownTime = now
            lastDownPoint = point
            lastPoint = point
            // Ensure cross-display cursor teleport succeeds before clicking.
            CGWarpMouseCursorPosition(point)
            CGAssociateMouseAndMouseCursorPosition(1)
            postMouse(.mouseMoved, at: point, deltaX: 0, deltaY: 0, clickState: 0)
            postMouse(.leftMouseDown, at: point, deltaX: 0, deltaY: 0, clickState: clickCount)
        case .touchMove:
            lastPoint = point
            if isPrimaryDown {
                // Never post .mouseMoved during a drag; doing so cancels macOS
                // window dragging and text selection.
                postMouse(.leftMouseDragged, at: point, deltaX: deltaX, deltaY: deltaY, clickState: clickCount)
            } else {
                postMouse(.mouseMoved, at: point, deltaX: deltaX, deltaY: deltaY, clickState: 0)
            }
        case .touchUp, .touchCancel:
            lastPoint = point
            postMouse(.leftMouseUp, at: point, deltaX: deltaX, deltaY: deltaY, clickState: clickCount)
        default:
            return false
        }
        return true
    }

    private static func postMouse(
        _ type: CGEventType,
        at point: CGPoint,
        deltaX: Int64,
        deltaY: Int64,
        clickState: Int64
    ) {
        guard let event = CGEvent(
            mouseEventSource: eventSource,
            mouseType: type,
            mouseCursorPosition: point,
            mouseButton: .left
        ) else {
            return
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

    private static func injectScroll(_ event: DMInputEventValue, displayBounds: CGRect) -> Bool {
        let point = mapPoint(event.x, event.y, in: displayBounds)
        if lastPoint == nil || !displayBounds.contains(lastPoint!) {
            CGWarpMouseCursorPosition(point)
            CGAssociateMouseAndMouseCursorPosition(1)
            postMouse(.mouseMoved, at: point, deltaX: 0, deltaY: 0, clickState: 0)
            lastPoint = point
        }

        // Scale normalized finger travel to display points with sub-pixel
        // accumulation so gentle two-finger scrolling never rounds to zero.
        scrollRemainderY += Double(event.dy) * Double(displayBounds.height) * 1.25
        scrollRemainderX += Double(event.dx) * Double(displayBounds.width) * 1.25
        let vertical = Int32(scrollRemainderY.rounded(.towardZero))
        let horizontal = Int32(scrollRemainderX.rounded(.towardZero))
        scrollRemainderY -= Double(vertical)
        scrollRemainderX -= Double(horizontal)

        guard vertical != 0 || horizontal != 0 else {
            return false
        }
        guard let scroll = CGEvent(
            scrollWheelEvent2Source: eventSource,
            units: .pixel,
            wheelCount: 2,
            wheel1: vertical,
            wheel2: horizontal,
            wheel3: 0
        ) else {
            return false
        }
        scroll.location = point
        scroll.post(tap: .cghidEventTap)
        return true
    }

    private static func clamp(_ value: Float) -> Float {
        min(max(value, 0), 1)
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
}
