import Foundation

/// Defines how DeskMate translates incoming tablet touch contacts into macOS input.
enum TouchInputMode: String, CaseIterable, Identifiable {
    case nativeTouch = "native_touch"
    case virtualTrackpad = "virtual_trackpad"
    case compatibleGestures = "compatible_gestures"
    case singleTouchOnly = "single_touch_only"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .nativeTouch: return "原生屏幕触控 (实验性)"
        case .virtualTrackpad: return "虚拟妙控板 (实验性)"
        case .compatibleGestures: return "兼容手势映射"
        case .singleTouchOnly: return "仅单点触控"
        }
    }

    var shortLabel: String {
        switch self {
        case .nativeTouch: return "原生触控"
        case .virtualTrackpad: return "虚拟妙控板"
        case .compatibleGestures: return "兼容手势"
        case .singleTouchOnly: return "仅单点触控"
        }
    }

    var summaryText: String {
        switch self {
        case .nativeTouch:
            return "将客户端触点直接传给 Mac 应用，支持最多 10 个独立触点。不可用时采用兼容手势映射。"
        case .virtualTrackpad:
            return "将客户端触控作为妙控板输入，用于操作指针和执行 macOS 触控板手势。不可用时采用兼容手势映射。"
        case .compatibleGestures:
            return "将客户端触控映射为鼠标操作和系统快捷键。"
        case .singleTouchOnly:
            return "保留单指点击和拖动，关闭多指手势。"
        }
    }

    var compatibleGestureRows: [(fingers: String, description: String)] {
        guard self == .compatibleGestures else { return [] }
        return [
            ("单指", "点击、拖动与指针移动"),
            ("双指", "滚动、右键与前台应用缩放"),
            ("三指 / 四指", "空间切换、调度中心与 App Exposé")
        ]
    }

    var trackpadGuidanceLines: [String] {
        guard self == .virtualTrackpad else { return [] }
        return [
            "推荐在 Mac「系统设置 → 触控板 → 光标与点按」中开启「轻点以点按」，并将「辅助点按」设为「双指点按或轻点」。",
            "单指长按后移动可拖拽，抬手松开。不支持用力点按和触觉反馈。"
        ]
    }

    var usesVirtualHIDWhenAvailable: Bool {
        self == .nativeTouch || self == .virtualTrackpad
    }
}

struct TouchCapabilityStatus: Equatable {
    var virtualHIDAvailable: Bool
    var fallbackActive: Bool
    var activeBackendLabel: String
    var statusDetail: String
}
