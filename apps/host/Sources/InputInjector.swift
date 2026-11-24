import CoreGraphics
import Foundation

/// Translates normalized Device input back into macOS events on the target
/// Extended Display. Scroll is a wheel event, so it needs no cursor position.
enum InputInjector {
    static func inject(_ event: DMInputEventValue, displayBounds: CGRect) -> Bool {
        switch event.kind {
        case .touchDown, .touchMove, .touchUp, .touchCancel:
            return injectPointer(event, displayBounds: displayBounds)
        case .scroll:
            return injectScroll(event)
        case .key:
            return false
        }
    }

    private static func injectPointer(_ event: DMInputEventValue, displayBounds: CGRect) -> Bool {
        let point = CGPoint(
            x: displayBounds.origin.x + displayBounds.width * CGFloat(clamp(event.x)),
            y: displayBounds.origin.y + displayBounds.height * CGFloat(clamp(event.y))
        )
        guard let move = CGEvent(
            mouseEventSource: nil,
            mouseType: .mouseMoved,
            mouseCursorPosition: point,
            mouseButton: .left
        ) else {
            return false
        }
        move.post(tap: .cghidEventTap)

        let isPrimaryDown = (event.flags & 0x0001) != 0
        switch event.kind {
        case .touchDown:
            postMouse(.leftMouseDown, at: point)
        case .touchUp, .touchCancel:
            postMouse(.leftMouseUp, at: point)
        case .touchMove:
            if isPrimaryDown {
                postMouse(.leftMouseDragged, at: point)
            }
        default:
            break
        }
        return true
    }

    private static func postMouse(_ type: CGEventType, at point: CGPoint) {
        guard let event = CGEvent(
            mouseEventSource: nil,
            mouseType: type,
            mouseCursorPosition: point,
            mouseButton: .left
        ) else {
            return
        }
        event.post(tap: .cghidEventTap)
    }

    private static func injectScroll(_ event: DMInputEventValue) -> Bool {
        /* dx/dy are normalized display sizes; map to whole wheel lines. */
        let vertical = Int32((-event.dy * 40).rounded())
        let horizontal = Int32((event.dx * 40).rounded())
        guard vertical != 0 || horizontal != 0 else {
            return false
        }
        guard let scroll = CGEvent(
            scrollWheelEvent2Source: nil,
            units: .pixel,
            wheelCount: 2,
            wheel1: vertical,
            wheel2: horizontal,
            wheel3: 0
        ) else {
            return false
        }
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
