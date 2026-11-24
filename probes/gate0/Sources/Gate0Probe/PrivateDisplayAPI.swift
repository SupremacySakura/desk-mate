import Darwin
import Foundation
import ObjectiveC.runtime

enum PrivateDisplayAPI {
    static let frameworkPath = "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"

    static let requiredSelectors: [String: [String]] = [
        "CGVirtualDisplayDescriptor": [
            "init",
            "setName:",
            "setVendorID:",
            "setProductID:",
            "setSerialNum:",
            "setMaxPixelsWide:",
            "setMaxPixelsHigh:",
            "setSizeInMillimeters:",
            "setQueue:",
            "setRedPrimary:",
            "setGreenPrimary:",
            "setBluePrimary:",
            "setWhitePoint:",
        ],
        "CGVirtualDisplayMode": [
            "initWithWidth:height:refreshRate:",
            "width",
            "height",
            "refreshRate",
        ],
        "CGVirtualDisplaySettings": [
            "setHiDPI:",
            "setModes:",
            "setRotation:",
        ],
        "CGVirtualDisplay": [
            "initWithDescriptor:",
            "applySettings:",
            "displayID",
            "hiDPI",
            "modes",
        ],
    ]

    static func probe() -> PrivateAPIRecord {
        let handle = dlopen(frameworkPath, RTLD_LAZY | RTLD_LOCAL)
        defer {
            if let handle {
                dlclose(handle)
            }
        }

        var classAvailability: [String: Bool] = [:]
        var selectorAvailability: [String: [String: Bool]] = [:]

        for (className, selectors) in requiredSelectors.sorted(by: { $0.key < $1.key }) {
            let cls: AnyClass? = NSClassFromString(className)
            classAvailability[className] = cls != nil
            let available = methodNames(for: cls)
            selectorAvailability[className] = Dictionary(
                uniqueKeysWithValues: selectors.map { ($0, available.contains($0)) }
            )
        }

        let allClasses = classAvailability.values.allSatisfy { $0 }
        let allSelectors = selectorAvailability.values.allSatisfy { selectors in
            selectors.values.allSatisfy { $0 }
        }
        let available = handle != nil && allClasses && allSelectors

        return PrivateAPIRecord(
            frameworkPath: frameworkPath,
            classes: classAvailability,
            requiredSelectors: selectorAvailability,
            staticAvailability: available ? "available" : "unavailable",
            liveCreation: "not-executed",
            notes: [
                "Availability is observed on this exact macOS build only.",
                "The classes are private and have no compatibility or distribution guarantee.",
                "Static presence does not prove display creation, HiDPI mode switching, or cleanup.",
            ]
        )
    }

    private static func methodNames(for cls: AnyClass?) -> Set<String> {
        guard let cls else { return [] }
        var count: UInt32 = 0
        guard let methods = class_copyMethodList(cls, &count) else { return [] }
        defer { free(methods) }

        var names = Set<String>()
        for index in 0..<Int(count) {
            names.insert(NSStringFromSelector(method_getName(methods[index])))
        }
        return names
    }
}
