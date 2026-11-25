import Foundation

struct PairedDevice: Identifiable, Codable, Equatable {
    let id: String
    var defaultName: String
    var customName: String?
    var serial: String
    var manufacturer: String
    var allowed: Bool
    var lastConnectedAt: Date
    var firstPairedAt: Date
    var lastResolution: String?
    var preferredPosition: DisplayPosition? = nil

    var displayName: String {
        let trimmed = customName?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return trimmed.isEmpty ? defaultName : trimmed
    }
}

struct ScannedUsbDevice: Decodable, Equatable {
    let id: String
    let name: String
    let manufacturer: String
    let serial: String
    let inAccessoryMode: Bool
}

final class PairedDeviceStore {
    private let defaults: UserDefaults
    private let storageKey = "pairedDevicesV1"
    private(set) var devices: [PairedDevice] = []

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        load()
    }

    @discardableResult
    func recordSeenDevice(
        id: String,
        name: String,
        manufacturer: String,
        serial: String,
        resolution: String? = nil,
        markConnected: Bool = true
    ) -> Bool {
        let cleanID = id.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanID.isEmpty else { return false }

        let fallbackName = manufacturer.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            ? "HarmonyOS 平板"
            : "\(manufacturer.trimmingCharacters(in: .whitespacesAndNewlines)) 平板"
        let resolvedName = name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? fallbackName : name
        let now = Date()
        var changed = false

        if let index = devices.firstIndex(where: { $0.id == cleanID }) {
            var updated = devices[index]
            if !Self.isGenericName(resolvedName) && (Self.isGenericName(updated.defaultName) || updated.defaultName != resolvedName) {
                updated.defaultName = resolvedName
                changed = true
            }
            if !serial.isEmpty && updated.serial != serial {
                updated.serial = serial
                changed = true
            }
            if !manufacturer.isEmpty && updated.manufacturer != manufacturer {
                updated.manufacturer = manufacturer
                changed = true
            }
            if let resolution, !resolution.isEmpty, resolution != "NONE", updated.lastResolution != resolution {
                updated.lastResolution = resolution
                changed = true
            }
            if markConnected && now.timeIntervalSince(updated.lastConnectedAt) >= 1.0 {
                updated.lastConnectedAt = now
                changed = true
            }
            if changed {
                devices[index] = updated
                sortDevices()
                save()
            }
            return changed
        }

        let created = PairedDevice(
            id: cleanID,
            defaultName: resolvedName,
            customName: nil,
            serial: serial,
            manufacturer: manufacturer,
            allowed: true,
            lastConnectedAt: now,
            firstPairedAt: now,
            lastResolution: (resolution?.isEmpty == false && resolution != "NONE") ? resolution : nil
        )
        devices.append(created)
        sortDevices()
        save()
        return true
    }

    func setAllowed(id: String, allowed: Bool) {
        guard let index = devices.firstIndex(where: { $0.id == id }) else { return }
        guard devices[index].allowed != allowed else { return }
        devices[index].allowed = allowed
        save()
    }

    func rename(id: String, customName: String) {
        guard let index = devices.firstIndex(where: { $0.id == id }) else { return }
        let trimmed = customName.trimmingCharacters(in: .whitespacesAndNewlines)
        devices[index].customName = trimmed.isEmpty ? nil : trimmed
        save()
    }

    func remove(id: String) {
        let before = devices.count
        devices.removeAll { $0.id == id }
        if devices.count != before {
            save()
        }
    }

    func isAllowed(id: String) -> Bool {
        devices.first(where: { $0.id == id })?.allowed ?? true
    }

    func blockedDeviceIDs() -> [String] {
        devices.filter { !$0.allowed }.map(\.id)
    }

    func displayName(for id: String) -> String? {
        devices.first(where: { $0.id == id })?.displayName
    }

    func preferredPosition(for id: String) -> DisplayPosition? {
        devices.first(where: { $0.id == id })?.preferredPosition
    }

    func setPreferredPosition(id: String, position: DisplayPosition?) {
        guard let index = devices.firstIndex(where: { $0.id == id }) else { return }
        guard devices[index].preferredPosition != position else { return }
        devices[index].preferredPosition = position
        save()
    }

    static func relativeTimeText(from date: Date, now: Date = Date()) -> String {
        let seconds = max(0, now.timeIntervalSince(date))
        if seconds < 60 {
            return "刚刚"
        }
        let minutes = Int(seconds / 60)
        if minutes < 60 {
            return "\(minutes) 分钟前"
        }
        let hours = Int(seconds / 3600)
        if hours < 24 {
            return "\(hours) 小时前"
        }
        let days = Int(seconds / 86400)
        if days < 30 {
            return "\(days) 天前"
        }
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd"
        return formatter.string(from: date)
    }

    private static func isGenericName(_ name: String) -> Bool {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ||
            trimmed == "Android" ||
            trimmed == "Accessory" ||
            trimmed == "DeskMate" ||
            trimmed == "DeskMate Extended Display" ||
            trimmed == "DeskMate 平板设备" ||
            trimmed == "DeskMate 设备" ||
            trimmed == "HarmonyOS 平板" ||
            trimmed == "HarmonyOS 设备"
    }

    private func sortDevices() {
        devices.sort { $0.lastConnectedAt > $1.lastConnectedAt }
    }

    private func load() {
        guard let data = defaults.data(forKey: storageKey),
              let decoded = try? JSONDecoder().decode([PairedDevice].self, from: data) else {
            devices = []
            return
        }
        devices = decoded
        sortDevices()
    }

    private func save() {
        guard let data = try? JSONEncoder().encode(devices) else { return }
        defaults.set(data, forKey: storageKey)
    }
}
