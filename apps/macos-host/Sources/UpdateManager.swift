import AppKit
import CryptoKit
import Foundation

enum UpdateConfig {
    static let manifestURLString =
        "https://github.com/SupremacySakura/desk-mate/releases/latest/download/update-manifest.json"
    static let defaultReleasesURLString =
        "https://github.com/SupremacySakura/desk-mate/releases/latest"
    static let publicKeyHex =
        "4a4128a91fed00f0bb0f5f157e56f8fae6749b4575dff2d21bdc8546e0d98391"
    static let maxArtifactBytes: UInt64 = 256 * 1024 * 1024

    static var platformKey: String {
        #if arch(arm64)
        return "macos-arm64"
        #else
        return "macos-x64"
        #endif
    }
}

enum UpdatePhase: Equatable {
    case idle
    case checking
    case upToDate
    case available
    case downloading
    case verifying
    case installing
    case failed
}

struct UpdateStateSnapshot: Equatable {
    var currentVersion: String = "1.5.0"
    var phase: UpdatePhase = .idle
    var availableVersion: String? = nil
    var releaseNotes: String = ""
    var releaseURL: String = UpdateConfig.defaultReleasesURLString
    var progress: Double = 0.0
    var downloadedBytes: UInt64 = 0
    var totalBytes: UInt64 = 0
    var statusText: String = "点击检查更新以获取最新版本"
    var errorMessage: String? = nil
    var lastCheckedText: String = ""
    var autoCheckEnabled: Bool = true
    var mirrorPrefix: String = ""
}

struct ParsedSemVer: Comparable {
    let major: Int
    let minor: Int
    let patch: Int
    let prerelease: String

    init?(_ raw: String) {
        var text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.hasPrefix("v") || text.hasPrefix("V") {
            text.removeFirst()
        }
        if let plusIdx = text.firstIndex(of: "+") {
            text = String(text[..<plusIdx])
        }
        var pre = ""
        var core = text
        if let dashIdx = text.firstIndex(of: "-") {
            core = String(text[..<dashIdx])
            pre = String(text[text.index(after: dashIdx)...])
            if pre.isEmpty { return nil }
        }
        let parts = core.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 3 else { return nil }
        for part in parts {
            if part.isEmpty || (part.count > 1 && part.first == "0") {
                return nil
            }
        }
        guard let maj = Int(parts[0]), let min = Int(parts[1]), let pat = Int(parts[2]),
              maj >= 0, min >= 0, pat >= 0 else {
            return nil
        }
        self.major = maj
        self.minor = min
        self.patch = pat
        self.prerelease = pre
    }

    static func < (lhs: ParsedSemVer, rhs: ParsedSemVer) -> Bool {
        if lhs.major != rhs.major { return lhs.major < rhs.major }
        if lhs.minor != rhs.minor { return lhs.minor < rhs.minor }
        if lhs.patch != rhs.patch { return lhs.patch < rhs.patch }
        if lhs.prerelease.isEmpty && rhs.prerelease.isEmpty { return false }
        if lhs.prerelease.isEmpty { return false }
        if rhs.prerelease.isEmpty { return true }
        return lhs.prerelease < rhs.prerelease
    }
}

private struct DecodedManifest: Decodable {
    struct Artifact: Decodable {
        let url: String
        let sha256: String
        let size: UInt64
        let signature: String
    }

    let schema: Int
    let version: String
    let tag: String?
    let published_at: String?
    let release_url: String?
    let notes: String?
    let artifacts: [String: Artifact]
}

final class UpdateManager: NSObject, URLSessionDownloadDelegate {
    private static let autoCheckPref = "autoCheckUpdates"
    private static let mirrorPrefixPref = "updateMirrorPrefix"

    var onChange: ((UpdateStateSnapshot) -> Void)?

    private let lock = NSLock()
    private var state = UpdateStateSnapshot()
    private var latestArtifact: DecodedManifest.Artifact?
    private var downloadSession: URLSession?
    private var activeTask: URLSessionDownloadTask?
    private var downloadContinuation: CheckedContinuation<URL, Error>?
    private var lastProgressPublish = Date.distantPast

    override init() {
        super.init()
        let ver = (Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String) ?? "1.5.0"
        state.currentVersion = ver
        if UserDefaults.standard.object(forKey: Self.autoCheckPref) != nil {
            state.autoCheckEnabled = UserDefaults.standard.bool(forKey: Self.autoCheckPref)
        } else {
            state.autoCheckEnabled = true
        }
        state.mirrorPrefix = UserDefaults.standard.string(forKey: Self.mirrorPrefixPref) ?? ""
    }

    func snapshot() -> UpdateStateSnapshot {
        lock.lock()
        defer { lock.unlock() }
        return state
    }

    var autoCheckEnabled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return state.autoCheckEnabled
    }

    func setAutoCheckEnabled(_ enabled: Bool) {
        UserDefaults.standard.set(enabled, forKey: Self.autoCheckPref)
        mutateState { $0.autoCheckEnabled = enabled }
    }

    func setMirrorPrefix(_ raw: String) {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        UserDefaults.standard.set(trimmed, forKey: Self.mirrorPrefixPref)
        mutateState { $0.mirrorPrefix = trimmed }
    }

    private func setLatestArtifact(_ artifact: DecodedManifest.Artifact?) {
        lock.lock()
        latestArtifact = artifact
        lock.unlock()
    }

    private func beginDownloadSession(_ session: URLSession) {
        lock.lock()
        downloadSession = session
        lock.unlock()
    }

    private func startTask(
        _ session: URLSession,
        url: URL,
        userAgent: String,
        continuation: CheckedContinuation<URL, Error>
    ) {
        lock.lock()
        downloadContinuation = continuation
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 30)
        request.setValue(userAgent, forHTTPHeaderField: "User-Agent")
        let task = session.downloadTask(with: request)
        activeTask = task
        lock.unlock()
        task.resume()
    }

    private func currentVersionString() -> String {
        lock.lock()
        defer { lock.unlock() }
        return state.currentVersion
    }

    func openReleasesPage() {
        let target = snapshot().releaseURL
        guard let url = URL(string: target) else { return }
        NSWorkspace.shared.open(url)
    }

    func checkForUpdates(manual: Bool) {
        let snap = snapshot()
        if snap.phase == .checking || snap.phase == .downloading ||
           snap.phase == .verifying || snap.phase == .installing {
            return
        }

        mutateState {
            $0.phase = .checking
            $0.errorMessage = nil
            $0.statusText = "正在从 GitHub 检查最新版本..."
        }

        let manifestURLString = Self.applyMirrorPrefix(
            UpdateConfig.manifestURLString,
            mirrorPrefix: snap.mirrorPrefix
        )
        guard let url = URL(string: manifestURLString) else {
            mutateState {
                $0.phase = .failed
                $0.statusText = "更新地址无效"
                $0.errorMessage = "加速前缀或更新地址格式无效"
            }
            return
        }

        Task.detached(priority: .utility) { [weak self] in
            guard let self else { return }
            do {
                var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 20)
                request.setValue("DeskMate-macOS-Host/\(snap.currentVersion)", forHTTPHeaderField: "User-Agent")
                let (data, response) = try await URLSession.shared.data(for: request)
                guard let http = response as? HTTPURLResponse, (200...299).contains(http.statusCode) else {
                    let code = (response as? HTTPURLResponse)?.statusCode ?? -1
                    throw NSError(domain: "DeskMateUpdate", code: code, userInfo: [
                        NSLocalizedDescriptionKey: "GitHub 返回状态码 HTTP \(code)"
                    ])
                }

                let decoded = try JSONDecoder().decode(DecodedManifest.self, from: data)
                guard decoded.schema == 1 else {
                    throw NSError(domain: "DeskMateUpdate", code: 2, userInfo: [
                        NSLocalizedDescriptionKey: "不支持的更新清单版本 schema=\(decoded.schema)"
                    ])
                }
                guard let candidateSemVer = ParsedSemVer(decoded.version),
                      let currentSemVer = ParsedSemVer(snap.currentVersion) else {
                    throw NSError(domain: "DeskMateUpdate", code: 3, userInfo: [
                        NSLocalizedDescriptionKey: "清单版本号格式无效: \(decoded.version)"
                    ])
                }

                let relURL = (decoded.release_url?.hasPrefix("https://") == true)
                    ? decoded.release_url!
                    : UpdateConfig.defaultReleasesURLString
                let checkedAt = Self.formatCurrentTime()

                guard candidateSemVer > currentSemVer else {
                    self.setLatestArtifact(nil)
                    self.mutateState {
                        $0.phase = .upToDate
                        $0.availableVersion = nil
                        $0.releaseNotes = ""
                        $0.releaseURL = relURL
                        $0.lastCheckedText = checkedAt
                        $0.statusText = "已是最新版本 (v\($0.currentVersion))"
                        $0.errorMessage = nil
                    }
                    return
                }

                guard let artifact = decoded.artifacts[UpdateConfig.platformKey] else {
                    self.setLatestArtifact(nil)
                    self.mutateState {
                        $0.phase = .upToDate
                        $0.availableVersion = decoded.version
                        $0.releaseURL = relURL
                        $0.lastCheckedText = checkedAt
                        $0.statusText = "发现 v\(decoded.version)，但暂未包含 \(UpdateConfig.platformKey) 更新包"
                        $0.errorMessage = nil
                    }
                    return
                }

                guard artifact.url.hasPrefix("https://"),
                      artifact.size > 0,
                      artifact.size <= UpdateConfig.maxArtifactBytes,
                      Self.verifyEd25519Signature(
                          version: decoded.version,
                          platform: UpdateConfig.platformKey,
                          sha256: artifact.sha256,
                          size: artifact.size,
                          signature: artifact.signature
                      ) else {
                    throw NSError(domain: "DeskMateUpdate", code: 4, userInfo: [
                        NSLocalizedDescriptionKey: "更新清单签名校验失败，已拒绝安装来源不明的更新"
                    ])
                }

                self.setLatestArtifact(artifact)

                self.mutateState {
                    $0.phase = .available
                    $0.availableVersion = decoded.version
                    $0.releaseNotes = decoded.notes ?? ""
                    $0.releaseURL = relURL
                    $0.totalBytes = artifact.size
                    $0.downloadedBytes = 0
                    $0.progress = 0.0
                    $0.lastCheckedText = checkedAt
                    $0.statusText = "发现新版本 v\(decoded.version)"
                    $0.errorMessage = nil
                }
            } catch {
                if !manual {
                    self.mutateState {
                        if $0.phase == .checking {
                            $0.phase = .idle
                            $0.statusText = "自动检查失败，可稍后手动重试"
                            $0.errorMessage = nil
                        }
                    }
                } else {
                    self.mutateState {
                        $0.phase = .failed
                        $0.statusText = "检查更新失败"
                        $0.errorMessage = error.localizedDescription
                    }
                }
            }
        }
    }

    func downloadAndInstall(prepareForExit: @escaping () async -> Void) {
        lock.lock()
        let snap = state
        let artifact = latestArtifact
        lock.unlock()

        guard let targetVersion = snap.availableVersion, let artifact else {
            return
        }
        guard snap.phase != .downloading && snap.phase != .verifying && snap.phase != .installing else {
            return
        }

        let currentAppURL = Bundle.main.bundleURL
        guard currentAppURL.pathExtension == "app" else {
            mutateState {
                $0.phase = .failed
                $0.statusText = "当前运行环境不支持原地自更新"
                $0.errorMessage = "未检测到 .app 包路径，请从 Releases 手动下载更新。"
            }
            return
        }
        let parentDir = currentAppURL.deletingLastPathComponent()
        if !FileManager.default.isWritableFile(atPath: parentDir.path) {
            mutateState {
                $0.phase = .failed
                $0.statusText = "安装目录无写入权限"
                $0.errorMessage = "当前所在目录 (\(parentDir.path)) 不可写，请将 DeskMate.app 移至 /Applications 或手动下载。"
            }
            return
        }

        let downloadURLString = Self.applyMirrorPrefix(artifact.url, mirrorPrefix: snap.mirrorPrefix)
        guard let downloadURL = URL(string: downloadURLString) else {
            mutateState {
                $0.phase = .failed
                $0.statusText = "下载地址无效"
                $0.errorMessage = "无法解析下载地址: \(downloadURLString)"
            }
            return
        }

        mutateState {
            $0.phase = .downloading
            $0.progress = 0.0
            $0.downloadedBytes = 0
            $0.totalBytes = artifact.size
            $0.statusText = "正在下载 v\(targetVersion)..."
            $0.errorMessage = nil
        }

        Task.detached(priority: .userInitiated) { [weak self] in
            guard let self else { return }
            let workDir = FileManager.default.temporaryDirectory
                .appendingPathComponent("DeskMateUpdate-\(UUID().uuidString)", isDirectory: true)
            do {
                try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)
                let tempZip = try await self.downloadFile(from: downloadURL)
                let localZip = workDir.appendingPathComponent("update.zip")
                if FileManager.default.fileExists(atPath: localZip.path) {
                    try FileManager.default.removeItem(at: localZip)
                }
                try FileManager.default.moveItem(at: tempZip, to: localZip)

                self.mutateState {
                    $0.phase = .verifying
                    $0.progress = 1.0
                    $0.statusText = "正在校验 SHA-256 与 Ed25519 签名..."
                }

                let (actualSha, actualSize) = try Self.sha256AndSize(of: localZip)
                guard actualSize == artifact.size,
                      actualSha == artifact.sha256.lowercased(),
                      Self.verifyEd25519Signature(
                          version: targetVersion,
                          platform: UpdateConfig.platformKey,
                          sha256: artifact.sha256,
                          size: artifact.size,
                          signature: artifact.signature
                      ) else {
                    throw NSError(domain: "DeskMateUpdate", code: 5, userInfo: [
                        NSLocalizedDescriptionKey: "下载文件哈希或 Ed25519 签名校验不通过，已中止安装"
                    ])
                }

                self.mutateState {
                    $0.phase = .installing
                    $0.statusText = "正在解压并替换应用..."
                }

                let extractDir = workDir.appendingPathComponent("extracted", isDirectory: true)
                try FileManager.default.createDirectory(at: extractDir, withIntermediateDirectories: true)
                try Self.runProcess("/usr/bin/ditto", ["-xk", localZip.path, extractDir.path])

                let extractedApp = try Self.findAppBundle(in: extractDir)
                try Self.runProcess("/usr/bin/codesign", ["--verify", "--deep", "--strict", extractedApp.path])

                await prepareForExit()

                let swapScript = workDir.appendingPathComponent("swap-and-relaunch.sh")
                let pid = ProcessInfo.processInfo.processIdentifier
                let backupAppPath = parentDir.appendingPathComponent(".DeskMate.app.bak-\(pid)").path
                let scriptBody = """
                #!/bin/sh
                OLD_PID="\(pid)"
                TARGET="\(Self.shEscape(currentAppURL.path))"
                NEW_APP="\(Self.shEscape(extractedApp.path))"
                BACKUP="\(Self.shEscape(backupAppPath))"
                WORKDIR="\(Self.shEscape(workDir.path))"

                i=0
                while kill -0 "$OLD_PID" 2>/dev/null && [ "$i" -lt 150 ]; do
                  sleep 0.1
                  i=$((i + 1))
                done

                rm -rf "$BACKUP"
                if [ -e "$TARGET" ]; then
                  mv "$TARGET" "$BACKUP" || exit 1
                fi
                if ! mv "$NEW_APP" "$TARGET"; then
                  if [ -e "$BACKUP" ] && [ ! -e "$TARGET" ]; then
                    mv "$BACKUP" "$TARGET"
                  fi
                  exit 1
                fi
                rm -rf "$BACKUP"
                rm -rf "$WORKDIR"
                /usr/bin/open -n "$TARGET"
                """
                try scriptBody.write(to: swapScript, atomically: true, encoding: .utf8)
                try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: swapScript.path)

                let runner = Process()
                runner.executableURL = URL(fileURLWithPath: "/bin/sh")
                runner.arguments = [swapScript.path]
                try runner.run()

                await MainActor.run {
                    NSApp.terminate(nil)
                }
            } catch {
                try? FileManager.default.removeItem(at: workDir)
                self.mutateState {
                    $0.phase = .failed
                    $0.statusText = "安装更新失败"
                    $0.errorMessage = error.localizedDescription
                }
            }
        }
    }

    private func downloadFile(from url: URL) async throws -> URL {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 30
        config.timeoutIntervalForResource = 600
        let session = URLSession(configuration: config, delegate: self, delegateQueue: nil)
        beginDownloadSession(session)
        let ua = "DeskMate-macOS-Host/\(currentVersionString())"

        return try await withCheckedThrowingContinuation { continuation in
            startTask(session, url: url, userAgent: ua, continuation: continuation)
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64, totalBytesWritten: Int64,
                    totalBytesExpectedToWrite: Int64) {
        let now = Date()
        lock.lock()
        let shouldEmit = now.timeIntervalSince(lastProgressPublish) >= 0.1
        if shouldEmit {
            lastProgressPublish = now
            state.downloadedBytes = UInt64(max(0, totalBytesWritten))
            if totalBytesExpectedToWrite > 0 {
                state.totalBytes = UInt64(totalBytesExpectedToWrite)
            }
            if state.totalBytes > 0 {
                state.progress = min(1.0, Double(state.downloadedBytes) / Double(state.totalBytes))
            }
            let pct = Int((state.progress * 100.0).rounded())
            state.statusText = "正在下载更新 (\(pct)%)..."
        }
        let snapshotCopy = state
        lock.unlock()

        if shouldEmit {
            notify(snapshotCopy)
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didFinishDownloadingTo location: URL) {
        let persistent = FileManager.default.temporaryDirectory
            .appendingPathComponent("dm-dl-\(UUID().uuidString).zip")
        do {
            try FileManager.default.copyItem(at: location, to: persistent)
            finishDownload(.success(persistent), session: session)
        } catch {
            finishDownload(.failure(error), session: session)
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error {
            finishDownload(.failure(error), session: session)
        }
    }

    private func finishDownload(_ result: Result<URL, Error>, session: URLSession) {
        lock.lock()
        let cont = downloadContinuation
        downloadContinuation = nil
        activeTask = nil
        downloadSession = nil
        lock.unlock()

        session.finishTasksAndInvalidate()
        cont?.resume(with: result)
    }

    private func mutateState(_ block: (inout UpdateStateSnapshot) -> Void) {
        lock.lock()
        block(&state)
        let copy = state
        lock.unlock()
        notify(copy)
    }

    private func notify(_ snapshot: UpdateStateSnapshot) {
        DispatchQueue.main.async { [weak self] in
            self?.onChange?(snapshot)
        }
    }

    // MARK: - Verification & Utility Helpers

    static func applyMirrorPrefix(_ url: String, mirrorPrefix: String) -> String {
        var prefix = mirrorPrefix.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !prefix.isEmpty else { return url }
        guard prefix.hasPrefix("https://") || prefix.hasPrefix("http://") else { return url }
        if !prefix.hasSuffix("/") { prefix += "/" }
        if url.hasPrefix(prefix) { return url }
        if url.hasPrefix("https://github.com/") ||
           url.hasPrefix("https://objects.githubusercontent.com/") ||
           url.hasPrefix("https://release-assets.githubusercontent.com/") {
            return prefix + url
        }
        return url
    }

    static func verifyEd25519Signature(
        version: String,
        platform: String,
        sha256: String,
        size: UInt64,
        signature: String,
        publicKeyHex: String = UpdateConfig.publicKeyHex
    ) -> Bool {
        guard let pkData = decodeHex(publicKeyHex), pkData.count == 32,
              let sigData = decodeHex(signature), sigData.count == 64,
              let pubKey = try? Curve25519.Signing.PublicKey(rawRepresentation: pkData) else {
            return false
        }
        let payload = "deskmate-update-v1\n\(version)\n\(platform)\n\(sha256.lowercased())\n\(size)"
        guard let msgData = payload.data(using: .ascii) else { return false }
        return pubKey.isValidSignature(sigData, for: msgData)
    }

    private static func sha256AndSize(of fileURL: URL) throws -> (String, UInt64) {
        let handle = try FileHandle(forReadingFrom: fileURL)
        defer { try? handle.close() }
        var hasher = SHA256()
        var total: UInt64 = 0
        while true {
            let chunk = try handle.read(upToCount: 65536) ?? Data()
            if chunk.isEmpty { break }
            total += UInt64(chunk.count)
            hasher.update(data: chunk)
        }
        let digest = hasher.finalize()
        let hex = digest.map { String(format: "%02x", $0) }.joined()
        return (hex, total)
    }

    private static func findAppBundle(in dir: URL) throws -> URL {
        let direct = dir.appendingPathComponent("DeskMate.app", isDirectory: true)
        if FileManager.default.fileExists(atPath: direct.path) {
            return direct
        }
        let items = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)
        if let app = items.first(where: { $0.pathExtension == "app" }) {
            return app
        }
        throw NSError(domain: "DeskMateUpdate", code: 6, userInfo: [
            NSLocalizedDescriptionKey: "解压后的更新包中未找到 DeskMate.app"
        ])
    }

    private static func runProcess(_ launchPath: String, _ arguments: [String]) throws {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: launchPath)
        proc.arguments = arguments
        let pipe = Pipe()
        proc.standardError = pipe
        try proc.run()
        proc.waitUntilExit()
        if proc.terminationStatus != 0 {
            let errData = pipe.fileHandleForReading.readDataToEndOfFile()
            let errText = String(data: errData, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            throw NSError(domain: "DeskMateUpdate", code: Int(proc.terminationStatus), userInfo: [
                NSLocalizedDescriptionKey: "\(URL(fileURLWithPath: launchPath).lastPathComponent) 失败: \(errText)"
            ])
        }
    }

    private static func decodeHex(_ hex: String) -> Data? {
        let chars = Array(hex.utf8)
        guard chars.count % 2 == 0 else { return nil }
        var bytes = [UInt8]()
        bytes.reserveCapacity(chars.count / 2)
        for i in stride(from: 0, to: chars.count, by: 2) {
            guard let hi = nibble(chars[i]), let lo = nibble(chars[i + 1]) else {
                return nil
            }
            bytes.append((hi << 4) | lo)
        }
        return Data(bytes)
    }

    private static func nibble(_ ch: UInt8) -> UInt8? {
        switch ch {
        case 48...57: return ch - 48
        case 97...102: return ch - 87
        case 65...70: return ch - 55
        default: return nil
        }
    }

    private static func shEscape(_ text: String) -> String {
        return text.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
            .replacingOccurrences(of: "$", with: "\\$")
            .replacingOccurrences(of: "`", with: "\\`")
    }

    private static func formatCurrentTime() -> String {
        let fmt = DateFormatter()
        fmt.dateFormat = "HH:mm:ss"
        return fmt.string(from: Date())
    }
}
