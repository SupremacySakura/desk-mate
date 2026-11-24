import AppKit
import Foundation

@MainActor
final class LifecycleMonitor {
    private var distributedObservers: [NSObjectProtocol] = []
    private var workspaceObservers: [NSObjectProtocol] = []
    private(set) var events: [String] = []
    private(set) var terminationEvent: String?
    private(set) var registered = false

    func start() {
        guard !registered else { return }
        registered = true

        let distributed = DistributedNotificationCenter.default()
        distributedObservers.append(distributed.addObserver(
            forName: Notification.Name("com.apple.screenIsLocked"),
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in
                self?.record("screenIsLocked", terminate: true)
            }
        })
        distributedObservers.append(distributed.addObserver(
            forName: Notification.Name("com.apple.screenIsUnlocked"),
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in
                self?.record("screenIsUnlocked", terminate: false)
            }
        })

        let workspace = NSWorkspace.shared.notificationCenter
        workspaceObservers.append(workspace.addObserver(
            forName: NSWorkspace.willSleepNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in
                self?.record("systemWillSleep", terminate: true)
            }
        })
        workspaceObservers.append(workspace.addObserver(
            forName: NSWorkspace.didWakeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in
                self?.record("systemDidWake", terminate: false)
            }
        })
        workspaceObservers.append(workspace.addObserver(
            forName: NSWorkspace.screensDidSleepNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in
                self?.record("screensDidSleep", terminate: true)
            }
        })
        workspaceObservers.append(workspace.addObserver(
            forName: NSWorkspace.screensDidWakeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in
                self?.record("screensDidWake", terminate: false)
            }
        })
    }

    func stop() {
        let distributed = DistributedNotificationCenter.default()
        let workspace = NSWorkspace.shared.notificationCenter
        for observer in distributedObservers {
            distributed.removeObserver(observer)
        }
        for observer in workspaceObservers {
            workspace.removeObserver(observer)
        }
        distributedObservers.removeAll()
        workspaceObservers.removeAll()
        registered = false
    }

    deinit {
        let distributed = DistributedNotificationCenter.default()
        let workspace = NSWorkspace.shared.notificationCenter
        for observer in distributedObservers {
            distributed.removeObserver(observer)
        }
        for observer in workspaceObservers {
            workspace.removeObserver(observer)
        }
    }

    private func record(_ event: String, terminate: Bool) {
        if !events.contains(event) {
            events.append(event)
        }
        if terminate && terminationEvent == nil {
            terminationEvent = event
        }
    }
}
