import FocalCore
import XCTest

/// Swift ラッパーのテスト（12 章）。テスト用 RAW（tests/data）がなければ skip する
final class FocalCoreTests: XCTestCase {
    private var tmp: URL!

    private static let dataDir = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent()  // apps/macos
        .deletingLastPathComponent().deletingLastPathComponent()  // リポジトリ
        .appendingPathComponent("tests/data")

    override func setUpWithError() throws {
        tmp = FileManager.default.temporaryDirectory.appendingPathComponent("focal-xctest-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: tmp)
    }

    private func requireData() throws -> URL {
        let sony = Self.dataDir.appendingPathComponent("sony_ilce7m3.ARW")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: sony.path), "tests/data/fetch.sh でテスト用 RAW を取得する")
        // 日本語（NFD）の名前のフォルダに 2 枚置く
        let lib = tmp.appendingPathComponent("lib").appendingPathComponent("写真".decomposedStringWithCanonicalMapping)
        try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: sony, to: lib.appendingPathComponent("がっこう.ARW".decomposedStringWithCanonicalMapping))
        try FileManager.default.copyItem(at: Self.dataDir.appendingPathComponent("canon_eos_m50.CR3"),
                                         to: lib.appendingPathComponent("IMG_0001.CR3"))
        return tmp.appendingPathComponent("lib")
    }

    private func scan(_ catalog: Catalog, _ root: Int64) async throws -> ScanStats {
        var stats: ScanStats?
        for try await ev in catalog.scan(rootID: root, thumbnailCache: nil) {
            if case .finished(let s) = ev { stats = s }
        }
        return try XCTUnwrap(stats)
    }

    func testChangedAdjustments() {
        let base = DevelopSettings()
        XCTAssertEqual(base.changedAdjustments(from: base), 0)
        var moved = base
        moved.exposure = 1
        moved.rotate90 = 1  // 切り取り・回転は見ない
        let one = moved.changedAdjustments(from: base)
        XCTAssertNotEqual(one, 0)
        moved.clarity = 20
        let two = moved.changedAdjustments(from: base)
        XCTAssertNotEqual(two, one)
        XCTAssertEqual(two & one, one)  // 項目が増えると、前の項目は残る
    }

    func testAPIVersion() {
        XCTAssertTrue(FocalCoreInfo.isCompatible)
    }

    func testOpenErrorsAreThrown() {
        XCTAssertThrowsError(try Catalog(url: tmp)) { error in  // ディレクトリはカタログにできない
            XCTAssertEqual((error as? FocalError)?.code, .database)
        }
    }

    func testScanQueryAndEdit() async throws {
        let lib = try requireData()
        let catalog = try Catalog(url: tmp.appendingPathComponent("c.sqlite"))
        let root = try catalog.addRoot(lib)
        let stats = try await scan(catalog, root)
        XCTAssertEqual(stats.added, 2)

        let ids = try catalog.photoIDs()
        XCTAssertEqual(ids.count, 2)
        let photos = try catalog.photos(ids: ids)
        XCTAssertEqual(photos.map(\.fileName), ["がっこう.ARW", "IMG_0001.CR3"])  // NFC で返る
        XCTAssertEqual(photos[0].cameraMake, "Sony")
        XCTAssertEqual(photos[0].iso, 400)

        try catalog.setRating(4, for: [ids[0]])
        try catalog.setFlag(.picked, for: [ids[1]])
        var f = PhotoFilter()
        f.minRating = 4
        XCTAssertEqual(try catalog.count(f), 1)
        f = PhotoFilter()
        f.flag = .picked
        XCTAssertEqual(try catalog.photoIDs(f), [ids[1]])

        let tag = try catalog.ensureTag("場所/学校")
        try catalog.addTag(tag, to: ids)
        XCTAssertEqual(try catalog.tags().map(\.path), ["場所", "場所/学校"])
        XCTAssertEqual(try catalog.tags(ofPhoto: ids[0]).first?.photoCount, 2)

        let folders = try catalog.folders(rootID: root)
        XCTAssertEqual(folders.map(\.relativePath), ["", "写真"])

        XCTAssertThrowsError(try catalog.setRating(7, for: ids))
    }

    func testScanOfMissingRootFails() async throws {
        let catalog = try Catalog(url: tmp.appendingPathComponent("c.sqlite"))
        do {
            _ = try await scan(catalog, 12345)
            XCTFail("expected error")
        } catch let e as FocalError {
            XCTAssertEqual(e.code, .notFound)
        }
    }

    func testThumbnailsAndCancellation() async throws {
        let lib = try requireData()
        let catalog = try Catalog(url: tmp.appendingPathComponent("c.sqlite"))
        _ = try await scan(catalog, try catalog.addRoot(lib))
        let ids = try catalog.photoIDs()

        let thumbs = try Thumbnailer(catalog: catalog, cacheDirectory: tmp.appendingPathComponent("thumbs"), threads: 1)
        let url = try await thumbs.thumbnailURL(for: ids[0])
        XCTAssertTrue(FileManager.default.fileExists(atPath: url.path))

        // 存在しない写真は失敗
        do {
            _ = try await thumbs.thumbnailURL(for: 999_999)
            XCTFail("expected error")
        } catch is FocalError {}

        // まとめて要求してキャンセルすると、ほとんどは開始前に取り消される
        let thumbs2 = try Thumbnailer(catalog: catalog, cacheDirectory: tmp.appendingPathComponent("t2"), threads: 1)
        var ok = 0, cancelled = 0
        await withTaskGroup(of: Bool.self) { g in
            for i in 0..<20 { g.addTask { (try? await thumbs2.thumbnailURL(for: ids[i % 2])) != nil } }
            g.cancelAll()
            for await r in g { if r { ok += 1 } else { cancelled += 1 } }
        }
        XCTAssertEqual(ok + cancelled, 20)
        XCTAssertGreaterThan(cancelled, 10)
    }

    // MARK: v3.19

    func testAlbumFoldersAndSmartAlbums() async throws {
        let lib = try requireData()
        let catalog = try Catalog(url: tmp.appendingPathComponent("c.sqlite"))
        _ = try await scan(catalog, try catalog.addRoot(lib))
        let ids = try catalog.photoIDs()

        let trips = try catalog.createAlbumFolder("Trips")
        let hokkaido = try catalog.createAlbum("Hokkaido", parentID: trips)
        XCTAssertThrowsError(try catalog.createAlbum("Hokkaido", parentID: trips))
        XCTAssertThrowsError(try catalog.addToAlbum(trips, photoIDs: ids))  // フォルダには足せない
        try catalog.addToAlbum(hokkaido, photoIDs: ids)

        var smart = try catalog.createSmartAlbum(
            "Canon", queryJSON: #"{"rules":[{"field":"camera","op":"contains","value":"Canon"}]}"#)
        XCTAssertThrowsError(try catalog.createSmartAlbum("Bad", queryJSON: "{"))
        var f = PhotoFilter()
        f.smartAlbumID = smart
        XCTAssertEqual(try catalog.count(f), 1)
        XCTAssertTrue(try catalog.smartQuery(smart).contains("Canon"))
        try catalog.setSmartQuery(smart, queryJSON: #"{"rules":[]}"#)
        XCTAssertEqual(try catalog.count(f), 2)

        let albums = try catalog.albums()
        XCTAssertEqual(albums.map(\.kind), [.folder, .album, .smart])
        XCTAssertEqual(albums[1].parentID, trips)
        XCTAssertEqual(albums[2].photoCount, 2)
        XCTAssertFalse(albums[0].acceptsPhotos)
        XCTAssertTrue(albums[1].acceptsPhotos)

        try catalog.moveAlbum(hokkaido, toParent: nil)
        XCTAssertNil(try catalog.albums().first { $0.id == hokkaido }?.parentID)
        XCTAssertThrowsError(try catalog.moveAlbum(trips, toParent: hokkaido))  // フォルダでない親
        try catalog.deleteAlbum(trips)
        smart = try catalog.createSmartAlbum("All", queryJSON: #"{"rules":[]}"#)
        XCTAssertEqual(try catalog.albums().count, 3)
    }

    func testRootsHaveVolumeInfo() throws {
        let lib = tmp.appendingPathComponent("lib")
        try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
        let catalog = try Catalog(url: tmp.appendingPathComponent("c.sqlite"))
        try catalog.addRoot(lib)
        let root = try XCTUnwrap(try catalog.roots().first)
        XCTAssertTrue(root.isOnline)
        XCTAssertFalse(root.volumeID.isEmpty)
        XCTAssertFalse(root.volumeName.isEmpty)
        XCTAssertEqual(try catalog.refreshVolumes(), 0)
        let details = try catalog.rootDetails(rootID: root.id)
        XCTAssertTrue(details.isOnline)
        XCTAssertEqual(details.photos, 0)
        XCTAssertNotNil(details.totalBytes)
        XCTAssertFalse(details.mountPoint.isEmpty)
        XCTAssertNotEqual(details.kind, .unknown)
        XCTAssertEqual(try catalog.nestedRootCount(), 0)
        XCTAssertEqual(try catalog.mergeNestedRoots(), 0)
        // 追加したルートの中のフォルダを追加しても、新しいルートはできない
        let inner = lib.appendingPathComponent("inner")
        try FileManager.default.createDirectory(at: inner, withIntermediateDirectories: true)
        XCTAssertEqual(try catalog.addRoot(inner), root.id)
        XCTAssertEqual(try catalog.roots().count, 1)
        XCTAssertNotNil(catalog.folderLocation(forPath: lib))
        XCTAssertNil(catalog.folderLocation(forPath: tmp.appendingPathComponent("elsewhere")))
        XCTAssertThrowsError(try catalog.relocateRoot(root.id, to: tmp.appendingPathComponent("no-such-dir")))
        try catalog.setRootLabel("Main", rootID: root.id)
        XCTAssertEqual(try catalog.roots().first?.displayName, "Main")
    }

    func testImportFromCard() async throws {
        _ = try requireData()
        let card = tmp.appendingPathComponent("card/DCIM/100CANON")
        try FileManager.default.createDirectory(at: card, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: Self.dataDir.appendingPathComponent("canon_eos_m50.CR3"),
                                         to: card.appendingPathComponent("IMG_0001.CR3"))
        let catalog = try Catalog(url: tmp.appendingPathComponent("c2.sqlite"))
        let album = try catalog.createAlbum("FromCard")
        let source = tmp.appendingPathComponent("card")
        let summary = try Catalog.summarizeCard(source)
        XCTAssertEqual(summary.shots, 1)

        var options = CardImportOptions(source: source, destination: tmp.appendingPathComponent("Photos"))
        options.albumID = album
        var phases = Set<Catalog.CardImportEvent.Phase>()
        var result: CardImportResult?
        for try await ev in catalog.importFromCard(options) {
            switch ev {
            case .progress(let phase, _, _, _, _, _): phases.insert(phase)
            case .finished(let r): result = r
            }
        }
        let r = try XCTUnwrap(result)
        XCTAssertEqual(r.imported, 1)
        XCTAssertEqual(r.added, 1)
        XCTAssertNotNil(r.rootID)
        XCTAssertTrue(phases.contains(.copying))
        XCTAssertTrue(FileManager.default.fileExists(
            atPath: tmp.appendingPathComponent("Photos/2018/2018-07-01/IMG_0001.CR3").path))
        var f = PhotoFilter()
        f.albumID = album
        XCTAssertEqual(try catalog.count(f), 1)

        // 2 回目は取り込み済み
        var second: CardImportResult?
        for try await ev in catalog.importFromCard(options) {
            if case .finished(let r) = ev { second = r }
        }
        XCTAssertEqual(second?.skippedDuplicates, 1)
        XCTAssertEqual(second?.imported, 0)

        XCTAssertNoThrow(try Catalog.importSources())
    }

    func testPresets() throws {
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("focal-presets-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: tmp) }
        let store = try PresetStore(userDirectory: tmp.appendingPathComponent("user"),
                                    builtInDirectory: tmp.appendingPathComponent("builtin"))
        XCTAssertTrue(try store.list().isEmpty)

        var look = DevelopSettings()
        look.exposure = 0.8
        look.clarity = 25
        look.rotate90 = 1
        look.cropW = 0.5
        let id = try store.save(name: "風景", from: look)
        let list = try store.list()
        XCTAssertEqual(list.map(\.name), ["風景"])
        XCTAssertFalse(list[0].isBuiltIn)

        // 重ねると、調整はプリセットの値、切り取り・回転は写真のまま
        var photo = DevelopSettings()
        photo.contrast = 40
        photo.rotate90 = 3
        photo.cropW = 0.8
        let merged = try store.applying(id: id, to: photo)
        XCTAssertEqual(merged.exposure, 0.8)
        XCTAssertEqual(merged.clarity, 25)
        XCTAssertEqual(merged.contrast, 0)
        XCTAssertEqual(merged.rotate90, 3)
        XCTAssertEqual(merged.cropW, 0.8)

        // レンズ補正はオン・オフだけがプリセットに入り、オンのプリセットだけが写真をオンにする
        var lensLook = DevelopSettings()
        lensLook.lensEnabled = true
        lensLook.lensID = "Some|Lens"
        let lensID = try store.save(name: "補正あり", from: lensLook)
        var lensPhoto = DevelopSettings()
        lensPhoto.lensID = "Mine|Lens"
        let withLens = try store.applying(id: lensID, to: lensPhoto)
        XCTAssertTrue(withLens.lensEnabled)
        XCTAssertEqual(withLens.lensID, "Mine|Lens")  // レンズの選択は写真のまま
        var lensOn = DevelopSettings()
        lensOn.lensEnabled = true
        XCTAssertTrue(try store.applying(id: id, to: lensOn).lensEnabled)  // オフのプリセットは補正を切らない
        try store.delete(id: lensID)

        XCTAssertThrowsError(try store.applying(id: "user:none", to: photo))
        XCTAssertThrowsError(try store.delete(id: "builtin:x"))
        try store.delete(id: id)
        XCTAssertTrue(try store.list().isEmpty)
    }

    func testBackup() async throws {
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("focal-backup-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: tmp) }
        let package = tmp.appendingPathComponent("My Catalog.focalcatalog")
        try FileManager.default.createDirectory(at: package, withIntermediateDirectories: true)
        let catalog = try Catalog(url: package.appendingPathComponent("catalog.sqlite"))
        XCTAssertEqual(try catalog.name(), "My Catalog")
        XCTAssertNil(try catalog.lastBackupDate())
        XCTAssertTrue(try catalog.isBackupDue(intervalDays: 7))
        XCTAssertFalse(try catalog.isBackupDue(intervalDays: -1))

        let dest = tmp.appendingPathComponent("backups")
        var outcome: BackupOutcome?
        var lastProgress = 0.0
        for try await ev in catalog.backup(to: dest) {
            switch ev {
            case .progress(let f): lastProgress = f
            case .finished(let r): outcome = r
            }
        }
        let r = try XCTUnwrap(outcome)
        XCTAssertFalse(r.skippedDamaged)
        XCTAssertTrue(FileManager.default.fileExists(atPath: try XCTUnwrap(r.url).appendingPathComponent("catalog.sqlite").path))
        XCTAssertEqual(lastProgress, 1.0)
        XCTAssertNotNil(try catalog.lastBackupDate())
        XCTAssertFalse(try catalog.isBackupDue(intervalDays: 7))
        XCTAssertTrue(try catalog.isBackupDue(intervalDays: 0))

        XCTAssertEqual(try catalog.backups(in: dest).count, 1)
        XCTAssertEqual(try catalog.pruneBackups(in: dest, keep: 5), 0)
    }

    func testPreferences() throws {
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("focal-prefs-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: tmp) }
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        let url = tmp.appendingPathComponent("c.sqlite")
        do {
            let catalog = try Catalog(url: url)
            XCTAssertNil(try catalog.preference("import_destination"))
            try catalog.setPreference("import_destination", "/Volumes/Photos")
            try catalog.setPreference("rescan_on_launch", "0")
            XCTAssertThrowsError(try catalog.preference("Bad Name"))
        }
        let again = try Catalog(url: url)
        XCTAssertEqual(try again.preference("import_destination"), "/Volumes/Photos")
        XCTAssertEqual(try again.preference("rescan_on_launch"), "0")
        try again.setPreference("import_destination", nil)
        XCTAssertNil(try again.preference("import_destination"))
    }
}
