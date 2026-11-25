import CoreGraphics
import Foundation

struct BridgeLinkSnapshot: Decodable {
    let linkId: UInt64
    let state: String
    let running: Bool
    let negotiated: String
    let inputEnabled: Bool
    let framesSubmitted: UInt64
    let framesSent: UInt64
    let framesRejected: UInt64
    let framesDroppedBacklog: UInt64
    let inputEvents: UInt64
    let reconnects: UInt64
    let rxBytes: UInt64
    let txBytes: UInt64
    let bulkErrors: UInt64
    let deviceId: String
    let deviceName: String
    let deviceManufacturer: String
    let deviceSerial: String
    let error: String
}

struct BridgeSnapshot: Decodable {
    let activeLinkCount: Int
    let links: [BridgeLinkSnapshot]
    let blockedDevices: [String]
    let state: String
    let running: Bool
    let negotiated: String
    let inputEnabled: Bool
    let inputEvents: UInt64
    let txBytes: UInt64
    let deviceId: String
    let deviceName: String
    let deviceManufacturer: String
    let deviceSerial: String
    let deviceBlocked: Bool
    let blockedDeviceName: String
    let error: String
}

/// Independent runtime session state for one connected USB client device.
final class DeviceChannel {
    let linkID: UInt64
    var deviceID: String = ""
    var deviceName: String = ""
    var deviceManufacturer: String = ""
    var deviceSerial: String = ""

    var bridgeState: String = "idle"
    var negotiated: String = "none"
    var bridgeError: String = ""
    var captureError: String? = nil

    var captureWanted: Bool = false
    var inputWanted: Bool = false
    var captureStarting: Bool = false
    var removedFromBridge: Bool = false

    var position: DisplayPosition
    var virtualSerial: UInt32 = 0
    var manualDisplayID: UInt32? = nil
    var displaySession: DisplaySession? = nil
    var capture: CapturePipeline? = nil
    var destroyDisplayTask: Task<Void, Never>? = nil

    let inputEngine: TouchInputEngine
    var inputEvents: UInt64 = 0
    var txBytes: UInt64 = 0
    var lastMetricTime: Double = ProcessInfo.processInfo.systemUptime
    var lastMetricFrames: Int = 0
    var lastMetricTxBytes: UInt64 = 0
    var liveFps: Int = 0
    var liveBitrateMbps: Double = 0.0
    var nextKeyframeRetry: Double = 0.0
    var lastReportedEncoded: Int = -1
    var lastDeviceTouchTime: Double = 0.0
    var loggedFirstInputBatch: Bool = false
    var lastBlockedInputLogTime: Double = 0.0

    init(linkID: UInt64, position: DisplayPosition, inputEngine: TouchInputEngine) {
        self.linkID = linkID
        self.position = position
        self.inputEngine = inputEngine
    }
}
