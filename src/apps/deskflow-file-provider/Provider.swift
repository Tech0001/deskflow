// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
import Foundation
import FileProvider
import UniformTypeIdentifiers

final class ClipboardItem: NSObject, NSFileProviderItem {
    let itemIdentifier: NSFileProviderItemIdentifier
    let offer: Offer
    init(_ id: NSFileProviderItemIdentifier, _ offer: Offer) { itemIdentifier = id; self.offer = offer }
    var index: Int? { entryIndex(itemIdentifier, offer) }
    var entry: Entry? { index.map { offer.entries[$0] } }
    var filename: String { entry.map { ($0.path as NSString).lastPathComponent } ?? "Deskflow Files" }
    var parentItemIdentifier: NSFileProviderItemIdentifier {
        guard let entry else { return .rootContainer }
        let parent = (entry.path as NSString).deletingLastPathComponent
        if parent.isEmpty || parent == "." { return .rootContainer }
        guard let i = offer.entries.firstIndex(where: { $0.path == parent && $0.directory }) else { return .rootContainer }
        return entryID(i)
    }
    var contentType: UTType {
        if entry == nil || entry!.directory { return .folder }
        return UTType(filenameExtension: (filename as NSString).pathExtension) ?? .data
    }
    var documentSize: NSNumber? { entry.flatMap { Int64($0.size).map { NSNumber(value: $0) } } }
    var capabilities: NSFileProviderItemCapabilities { [.allowsReading] }
    var fileSystemFlags: NSFileProviderFileSystemFlags {
        // Native copies retain these POSIX permissions. Keep received copies
        // writable even though the provider does not accept remote mutations.
        entry == nil || entry!.directory ? [.userReadable, .userWritable, .userExecutable] : [.userReadable, .userWritable]
    }
    var contentPolicy: NSFileProviderContentPolicy { .downloadLazily }
    var itemVersion: NSFileProviderItemVersion {
        let version = entry?.revision ?? entry?.sha256 ?? "directory"
        return NSFileProviderItemVersion(contentVersion: Data(version.utf8), metadataVersion: Data(version.utf8))
    }
}

final class ClipboardEnumerator: NSObject, NSFileProviderEnumerator {
    let container: NSFileProviderItemIdentifier
    let offer: Offer
    let anchor = NSFileProviderSyncAnchor(Data("1".utf8))
    init(_ container: NSFileProviderItemIdentifier, _ offer: Offer) { self.container = container; self.offer = offer }
    func invalidate() {}
    func enumerateItems(for observer: NSFileProviderEnumerationObserver, startingAt page: NSFileProviderPage) {
        let items = offer.entries.indices.map { ClipboardItem(entryID($0), offer) }
            .filter { $0.parentItemIdentifier == container || (container == .workingSet && $0.parentItemIdentifier == .rootContainer) }
        observer.didEnumerate(items)
        observer.finishEnumerating(upTo: nil)
    }
    func enumerateChanges(for observer: NSFileProviderChangeObserver, from anchor: NSFileProviderSyncAnchor) {
        observer.finishEnumeratingChanges(upTo: self.anchor, moreComing: false)
    }
    func currentSyncAnchor(completionHandler: @escaping (NSFileProviderSyncAnchor?) -> Void) { completionHandler(anchor) }
}

@objc(ClipboardProvider)
final class ClipboardProvider: NSObject, NSFileProviderReplicatedExtension, NSFileProviderThumbnailing {
    private static let transfers = DispatchSemaphore(value: 1)
    let domain: NSFileProviderDomain
    let lock = NSLock()
    var children: [UUID: Process] = [:]
    var invalidated = false
    required init(domain: NSFileProviderDomain) { self.domain = domain; super.init() }
    func invalidate() {
        lock.lock()
        invalidated = true
        let running = Array(children.values)
        lock.unlock()
        for process in running where process.isRunning { process.terminate() }
    }
    func item(for identifier: NSFileProviderItemIdentifier, request: NSFileProviderRequest,
              completionHandler: @escaping (NSFileProviderItem?, Error?) -> Void) -> Progress {
        do {
            let (offer, _) = try loadOffer(domain.identifier.rawValue)
            guard identifier == .rootContainer || entryIndex(identifier, offer) != nil else { throw NSFileProviderError(.noSuchItem) }
            completionHandler(ClipboardItem(identifier, offer), nil)
        } catch { completionHandler(nil, error) }
        return Progress(totalUnitCount: 1)
    }
    func enumerator(for identifier: NSFileProviderItemIdentifier, request: NSFileProviderRequest) throws -> NSFileProviderEnumerator {
        let (offer, _) = try loadOffer(domain.identifier.rawValue)
        guard identifier == .rootContainer || identifier == .workingSet ||
                entryIndex(identifier, offer).map({ offer.entries[$0].directory }) == true else { throw NSFileProviderError(.noSuchItem) }
        return ClipboardEnumerator(identifier, offer)
    }
    func fetchContents(for identifier: NSFileProviderItemIdentifier, version: NSFileProviderItemVersion?,
                       request: NSFileProviderRequest, completionHandler: @escaping (URL?, NSFileProviderItem?, Error?) -> Void) -> Progress {
        let progress = Progress(totalUnitCount: 1)
        progress.kind = .file
        progress.isCancellable = true
        DispatchQueue.global().async { [self] in
            while Self.transfers.wait(timeout: .now() + 0.1) != .success {
                if progress.isCancelled {
                    completionHandler(nil, nil, NSError(domain: NSCocoaErrorDomain, code: NSUserCancelledError))
                    return
                }
            }
            defer { Self.transfers.signal() }
            var staging: URL?
            let token = UUID()
            let process = Process()
            defer {
                lock.lock(); children.removeValue(forKey: token); lock.unlock()
            }
            do {
                let (offer, bytes) = try loadOffer(domain.identifier.rawValue)
                guard let index = entryIndex(identifier, offer), !offer.entries[index].directory,
                      let expires = Int64(offer.expires), expires > Int64(Date().timeIntervalSince1970 * 1000) else {
                    throw NSFileProviderError(.serverUnreachable)
                }
                let item = ClipboardItem(identifier, offer)
                if let version, version != item.itemVersion { throw NSError(domain: NSCocoaErrorDomain, code: NSFileReadUnknownError) }
                progress.totalUnitCount = Int64(offer.entries[index].size) ?? 0
                guard let helper = Bundle.main.url(forAuxiliaryExecutable: "deskflow-file-transfer") else {
                    throw NSError(domain: NSCocoaErrorDomain, code: NSFileNoSuchFileError)
                }
                process.executableURL = helper
                process.currentDirectoryURL = try storage()
                process.arguments = [String(index)]
                let input = Pipe(), output = Pipe()
                process.standardInput = input; process.standardOutput = output
                // Diagnostics must not fill an unread pipe during a transfer.
                process.standardError = FileHandle.nullDevice
                lock.lock()
                if invalidated || progress.isCancelled { lock.unlock(); throw NSError(domain: NSCocoaErrorDomain, code: NSUserCancelledError) }
                do {
                    try process.run()
                    children[token] = process
                    lock.unlock()
                } catch { lock.unlock(); throw error }
                progress.cancellationHandler = { if process.isRunning { process.terminate() } }
                if progress.isCancelled && process.isRunning { process.terminate() }
                try input.fileHandleForWriting.write(contentsOf: bytes)
                try input.fileHandleForWriting.close()
                var pending = Data(), cached: URL?
                while true {
                    let chunk = output.fileHandleForReading.availableData
                    if chunk.isEmpty { break }
                    pending.append(chunk)
                    while let newline = pending.firstIndex(of: 10) {
                        let line = pending[..<newline]; pending.removeSubrange(...newline)
                        guard let object = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else { continue }
                        if object["event"] as? String == "progress", let n = object["done"] as? String, let done = Int64(n) {
                            progress.completedUnitCount = done
                        }
                        if object["event"] as? String == "complete", let path = object["path"] as? String {
                            cached = URL(fileURLWithPath: path)
                        }
                    }
                    if pending.count > 65536 { throw NSFileProviderError(.serverUnreachable) }
                }
                process.waitUntilExit()
                if progress.isCancelled { throw NSError(domain: NSCocoaErrorDomain, code: NSUserCancelledError) }
                guard process.terminationStatus == 0, let cached, let manager = NSFileProviderManager(for: domain) else {
                    throw NSFileProviderError(.serverUnreachable)
                }
                let target = try manager.temporaryDirectoryURL().appendingPathComponent(UUID().uuidString)
                staging = target
                try FileManager.default.copyItem(at: cached, to: target)
                try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: target.path)
                if progress.isCancelled { throw NSError(domain: NSCocoaErrorDomain, code: NSUserCancelledError) }
                completionHandler(target, item, nil)
            } catch {
                if process.isRunning { process.terminate(); process.waitUntilExit() }
                if let staging { try? FileManager.default.removeItem(at: staging) }
                completionHandler(nil, nil, error)
            }
        }
        return progress
    }
    func fetchThumbnails(for identifiers: [NSFileProviderItemIdentifier], requestedSize: CGSize,
                         perThumbnailCompletionHandler: @escaping (NSFileProviderItemIdentifier, Data?, Error?) -> Void,
                         completionHandler: @escaping (Error?) -> Void) -> Progress {
        for id in identifiers { perThumbnailCompletionHandler(id, nil, nil) }
        completionHandler(nil)
        return Progress(totalUnitCount: 1)
    }
    func createItem(basedOn itemTemplate: NSFileProviderItem, fields: NSFileProviderItemFields, contents: URL?,
                    options: NSFileProviderCreateItemOptions = [], request: NSFileProviderRequest,
                    completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        completionHandler(nil, fields, false, NSError(domain: NSCocoaErrorDomain, code: NSFileWriteNoPermissionError)); return Progress()
    }
    func modifyItem(_ item: NSFileProviderItem, baseVersion: NSFileProviderItemVersion, changedFields: NSFileProviderItemFields,
                    contents: URL?, options: NSFileProviderModifyItemOptions = [], request: NSFileProviderRequest,
                    completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        completionHandler(nil, changedFields, false, NSError(domain: NSCocoaErrorDomain, code: NSFileWriteNoPermissionError)); return Progress()
    }
    func deleteItem(identifier: NSFileProviderItemIdentifier, baseVersion: NSFileProviderItemVersion,
                    options: NSFileProviderDeleteItemOptions = [], request: NSFileProviderRequest,
                    completionHandler: @escaping (Error?) -> Void) -> Progress {
        completionHandler(NSError(domain: NSCocoaErrorDomain, code: NSFileWriteNoPermissionError)); return Progress()
    }
}
