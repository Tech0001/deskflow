// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
import AppKit
import FileProvider
import CryptoKit

func finish(_ object: Any? = nil, _ error: Error? = nil) {
    if let error {
        FileHandle.standardError.write(Data(("Deskflow Files: \(error)\n").utf8)); exit(1)
    }
    if let object, let data = try? JSONSerialization.data(withJSONObject: object) {
        FileHandle.standardOutput.write(data + Data([10]))
    }
    exit(0)
}

@main struct Host {
    static func main() {
        let args = CommandLine.arguments
        guard args.count == 2 else { exit(2) }
        do {
            if args[1] == "resolve" {
                let data = FileHandle.standardInput.readDataToEndOfFile()
                guard data.count <= 1_048_576, let paths = try JSONSerialization.jsonObject(with: data) as? [String] else { exit(2) }
                let directory = try storage().appendingPathComponent("offers", isDirectory: true)
                for child in (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? [] {
                    guard let stored = try? Data(contentsOf: child.appendingPathComponent("paths.json")),
                          let previous = try? JSONSerialization.jsonObject(with: stored) as? [String], previous == paths,
                          let (offer, bytes) = try? loadOffer(child.lastPathComponent),
                          (Int64(offer.expires) ?? 0) > Int64(Date().timeIntervalSince1970 * 1000) else { continue }
                    FileHandle.standardOutput.write(bytes); exit(0)
                }
                exit(1)
            }
            if args[1] == "remove-all" {
                NSFileProviderManager.getDomainsWithCompletionHandler { domains, error in
                    if let error { finish(nil, error) }
                    let group = DispatchGroup()
                    for domain in domains {
                        group.enter()
                        NSFileProviderManager.remove(domain) { _ in group.leave() }
                    }
                    group.notify(queue: .main) { finish() }
                }
            } else if args[1] == "publish" {
                let data = FileHandle.standardInput.readDataToEndOfFile()
                guard data.count <= 1_048_576 else { exit(2) }
                let offer = try JSONDecoder().decode(Offer.self, from: data)
                guard [1, 2].contains(offer.version), !offer.entries.isEmpty, offer.entries.count <= 4096,
                      (Int64(offer.expires) ?? 0) > Int64(Date().timeIntervalSince1970 * 1000) else { exit(2) }
                let id = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
                let target = try offerURL(id)
                try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true,
                                                       attributes: [.posixPermissions: 0o700])
                try data.write(to: target, options: .atomic)
                try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: target.path)
                let domain = NSFileProviderDomain(identifier: .init(id), displayName: "Deskflow \(id.prefix(8))")
                let publish = {
                    guard let manager = NSFileProviderManager(for: domain) else { finish(nil, NSFileProviderError(.serverUnreachable)); return }
                    let roots = offer.entries.indices.filter { !offer.entries[$0].path.contains("/") }
                    let group = DispatchGroup(), lock = NSLock()
                    var paths: [Int: String] = [:], failure: Error?
                    for i in roots {
                        group.enter()
                        manager.getUserVisibleURL(for: entryID(i)) { url, error in
                            lock.lock()
                            if let url { paths[i] = url.path } else { failure = error ?? NSFileProviderError(.noSuchItem) }
                            lock.unlock(); group.leave()
                        }
                    }
                    group.notify(queue: .main) {
                        if let failure { finish(nil, failure) }
                        let result = roots.compactMap { paths[$0] }
                        do {
                            try JSONSerialization.data(withJSONObject: result).write(to: target.deletingLastPathComponent().appendingPathComponent("paths.json"), options: .atomic)
                            finish(result)
                        } catch { finish(nil, error) }
                    }
                }
                NSFileProviderManager.getDomainsWithCompletionHandler { domains, error in
                    if let error { finish(nil, error) }
                    // Expired selections cannot initiate new transfers. Remove
                    // them at publication rather than collecting domains forever.
                    for old in domains where old.identifier != domain.identifier {
                        if let (oldOffer, _) = try? loadOffer(old.identifier.rawValue),
                           (Int64(oldOffer.expires) ?? 0) < Int64(Date().timeIntervalSince1970 * 1000) {
                            NSFileProviderManager.remove(old) { error in
                                if error == nil, let oldStorage = try? offerURL(old.identifier.rawValue) {
                                    try? FileManager.default.removeItem(at: oldStorage.deletingLastPathComponent())
                                }
                            }
                        }
                    }
                    if domains.contains(where: { $0.identifier == domain.identifier }) { publish() }
                    else { NSFileProviderManager.add(domain) { error in if let error { finish(nil, error) }; publish() } }
                }
            } else { exit(2) }
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 45))
            finish(nil, NSError(domain: NSCocoaErrorDomain, code: NSFileReadUnknownError))
        } catch { finish(nil, error) }
    }
}
