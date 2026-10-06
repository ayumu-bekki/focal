import XCTest

/// GUI スモークテスト（12 章）と、グリッドのスクロール性能（9.1 章: 実装方式の比較）。
/// カタログは scripts/ui-test.sh が CLI で用意し、TEST_RUNNER_ 付きの環境変数で渡す。
final class FocalUITests: XCTestCase {
    private func launch(grid: String? = nil, extra: [String: String] = [:]) -> XCUIApplication {
        let app = XCUIApplication()
        let env = ProcessInfo.processInfo.environment
        for key in ["FOCAL_CATALOG", "FOCAL_CACHE"] { if let v = env[key] { app.launchEnvironment[key] = v } }
        if let grid { app.launchEnvironment["FOCAL_GRID"] = grid }
        // 前回の起動で保存されたウィンドウの大きさを使わない（テストごとに同じ大きさで撮る）
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        for (k, v) in extra { app.launchEnvironment[k] = v }
        // 表示を CPU で描いて比べるとき（FOCAL_GPU=0）
        if let v = env["FOCAL_GPU"] { app.launchEnvironment["FOCAL_GPU"] = v }
        // 前回のウィンドウの大きさを復元しない（毎回同じ条件で撮る）
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        return app
    }

    private func saveScreenshot(_ app: XCUIApplication, name: String) {
        let shot = app.windows.firstMatch.screenshot()
        let att = XCTAttachment(screenshot: shot)
        att.name = name
        att.lifetime = .keepAlways
        add(att)
    }

    @MainActor
    func testSmoke() throws {
        try XCTSkipIf(ProcessInfo.processInfo.environment["FOCAL_CATALOG"] == nil, "scripts/ui-test.sh から実行する")
        let app = launch()
        let window = app.windows["library"]  // タイトルにはカタログ名のサブタイトルが付く
        XCTAssertTrue(window.waitForExistence(timeout: 10))

        let count = app.staticTexts["photoCount"]
        XCTAssertTrue(count.waitForExistence(timeout: 10))
        let expected = ProcessInfo.processInfo.environment["FOCAL_EXPECTED_COUNT"] ?? "5"
        XCTAssertEqual(count.value as? String ?? count.label, "1 / \(expected)")

        // サムネイルが読み込まれるのを少し待ってから撮る
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 5))
        sleep(2)
        saveScreenshot(app, name: "grid")

        // キー操作: → で次へ、3 で ★3、P でピック、V でビューア
        grid.click()
        app.typeKey(.rightArrow, modifierFlags: [])
        XCTAssertEqual(count.value as? String ?? count.label, "2 / \(expected)")
        app.typeText("3")
        app.typeText("p")
        // インスペクタのフラグ（ピック | なし | リジェクト のセグメントコントロール）。表示言語に依らないよう位置で確かめる
        let flag = app.radioGroups["flagPicker"]
        XCTAssertTrue(flag.waitForExistence(timeout: 3))
        func selected() -> Int? {
            (0..<3).first { flag.radioButtons.element(boundBy: $0).value as? Int == 1 }
        }
        XCTAssertEqual(selected(), 0)  // P でピック
        // インスペクタでも変えられる: リジェクト → なし
        flag.radioButtons.element(boundBy: 2).click()
        XCTAssertEqual(selected(), 2)
        flag.radioButtons.element(boundBy: 1).click()
        XCTAssertEqual(selected(), 1)
        flag.radioButtons.element(boundBy: 0).click()
        XCTAssertEqual(selected(), 0)
        app.typeText("v")
        XCTAssertTrue(app.descendants(matching: .any)["viewer"].waitForExistence(timeout: 3))
        sleep(1)
        saveScreenshot(app, name: "viewer")
        app.typeText("v")
        XCTAssertTrue(grid.waitForExistence(timeout: 3))
    }

    // MARK: 現像（M3）

    private func requireCatalog() throws {
        try XCTSkipIf(ProcessInfo.processInfo.environment["FOCAL_CATALOG"] == nil, "scripts/ui-test.sh から実行する")
    }

    /// 計測値の表示（FOCAL_FRAME_STATS=1）
    private func developStats(_ app: XCUIApplication) -> String {
        let e = app.staticTexts["developStats"]
        return e.value as? String ?? e.label
    }

    private func waitReady(_ app: XCUIApplication, timeout: TimeInterval = 30) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if developStats(app).contains("stage=ready") { return true }
            usleep(200_000)
        }
        return false
    }

    private func exposure(_ app: XCUIApplication) -> XCUIElement { app.sliders["exposureSlider"] }

    private func waitGone(_ e: XCUIElement, timeout: TimeInterval = 3) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if !e.exists { return true }
            usleep(100_000)
        }
        return false
    }

    @MainActor
    func testDevelop() throws {
        try requireCatalog()
        var app = launch(extra: ["FOCAL_FRAME_STATS": "1"])
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        grid.click()
        app.typeText("v")
        XCTAssertTrue(app.descendants(matching: .any)["developView"].waitForExistence(timeout: 5))
        XCTAssertTrue(waitReady(app), developStats(app))
        sleep(1)
        saveScreenshot(app, name: "develop-fit")

        // 露出 +1EV（-5〜+5 の 60% の位置）
        let slider = exposure(app)
        XCTAssertTrue(slider.waitForExistence(timeout: 5))
        XCTAssertEqual(slider.normalizedSliderPosition, 0.5, accuracy: 0.01)
        slider.adjust(toNormalizedSliderPosition: 0.6)
        XCTAssertEqual(slider.normalizedSliderPosition, 0.6, accuracy: 0.02)
        sleep(1)
        saveScreenshot(app, name: "develop-ev+1")

        // Undo / Redo（Edit メニューと ⌘Z）
        app.typeKey("z", modifierFlags: .command)
        XCTAssertEqual(slider.normalizedSliderPosition, 0.5, accuracy: 0.01)
        app.typeKey("z", modifierFlags: [.command, .shift])
        XCTAssertEqual(slider.normalizedSliderPosition, 0.6, accuracy: 0.02)

        // 区分の開閉: ライトをたたむと露出のスライダーが消え、開くと戻る。ヒストグラムは上に固定
        let lightGroup = app.buttons["lightGroup"]
        XCTAssertTrue(lightGroup.exists)
        lightGroup.click()
        XCTAssertTrue(waitGone(slider))
        XCTAssertTrue(app.descendants(matching: .any)["histogram"].exists)
        saveScreenshot(app, name: "develop-light-collapsed")
        lightGroup.click()
        XCTAssertTrue(slider.waitForExistence(timeout: 3))
        // メタデータも開閉できる
        let metadataGroup = app.buttons["metadataGroup"]
        XCTAssertTrue(metadataGroup.exists)
        let flag = app.radioGroups["flagPicker"]
        XCTAssertTrue(flag.exists)
        metadataGroup.click()
        XCTAssertTrue(waitGone(flag))
        metadataGroup.click()
        XCTAssertTrue(flag.waitForExistence(timeout: 3))

        // ホワイトバランス: 撮影時の設定でも色かぶりを動かせ、動かすとカスタムになる。ダブルクリックで撮影時の設定に戻る
        let wbMode = app.popUpButtons["wbMode"]
        XCTAssertTrue(wbMode.waitForExistence(timeout: 3))
        let asShot = wbMode.value as? String
        let tint = app.sliders["tintSlider"]
        XCTAssertTrue(tint.isEnabled)
        tint.adjust(toNormalizedSliderPosition: 0.7)
        XCTAssertNotEqual(wbMode.value as? String, asShot)
        tint.doubleClick()
        XCTAssertEqual(wbMode.value as? String, asShot)

        // 100% 表示
        app.descendants(matching: .any)["developView"].click()
        app.typeText("z")
        sleep(2)
        saveScreenshot(app, name: "develop-100")
        app.typeText("z")

        // 終了して起動し直すと編集が復元される（7.4 章）
        app.typeKey("q", modifierFlags: .command)
        XCTAssertTrue(app.wait(for: .notRunning, timeout: 10))
        app = launch(extra: ["FOCAL_FRAME_STATS": "1"])
        XCTAssertTrue(app.descendants(matching: .any)["photoGrid"].waitForExistence(timeout: 10))
        app.descendants(matching: .any)["photoGrid"].click()
        app.typeText("v")
        XCTAssertTrue(waitReady(app), developStats(app))
        XCTAssertEqual(exposure(app).normalizedSliderPosition, 0.6, accuracy: 0.02)
        // 後片付け: 既定値に戻す（ダブルクリック）
        exposure(app).doubleClick()
        XCTAssertEqual(exposure(app).normalizedSliderPosition, 0.5, accuracy: 0.01)
    }

    // MARK: レンズ補正（v3.27）

    @MainActor
    func testLensCorrection() throws {
        try requireCatalog()
        let app = launch(extra: ["FOCAL_FRAME_STATS": "1"])
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        grid.click()
        app.typeText("v")
        XCTAssertTrue(waitReady(app), developStats(app))

        // 区分は閉じている。開くと、スイッチ・レンズ名・スライダーが出る。使うまでスライダーは触れない
        let group = app.buttons["lensGroup"]
        XCTAssertTrue(group.waitForExistence(timeout: 5))
        group.click()
        let toggle = app.descendants(matching: .any)["lensToggle"]
        XCTAssertTrue(toggle.waitForExistence(timeout: 3))
        XCTAssertEqual(toggle.value as? Int, 0)
        let distortion = app.sliders["lensDistortionSlider"]
        XCTAssertTrue(distortion.exists)
        XCTAssertFalse(distortion.isEnabled)
        XCTAssertTrue(app.descendants(matching: .any)["lensName"].exists)
        saveScreenshot(app, name: "lens-off")

        toggle.click()
        XCTAssertEqual(toggle.value as? Int, 1)
        XCTAssertTrue(distortion.isEnabled)
        XCTAssertEqual(distortion.normalizedSliderPosition, 1.0, accuracy: 0.01)
        distortion.adjust(toNormalizedSliderPosition: 0.5)
        XCTAssertEqual(distortion.normalizedSliderPosition, 0.5, accuracy: 0.03)
        sleep(1)
        saveScreenshot(app, name: "lens-on")

        // レンズを選ぶ: 検索して結果から選ぶと手動になり、「自動」で戻る
        app.buttons["lensChooseButton"].click()
        let field = app.textFields["lensSearchField"]
        XCTAssertTrue(field.waitForExistence(timeout: 3))
        field.click()
        field.typeText("nikkor z 24-70")
        let results = app.descendants(matching: .any)["lensResults"]
        XCTAssertTrue(results.waitForExistence(timeout: 3))
        saveScreenshot(app, name: "lens-picker")
        app.buttons["lensAutoButton"].click()
        XCTAssertTrue(waitGone(field))

        // 後片付け: 見出しの「リセット」で既定に戻す
        toggle.click()
        XCTAssertEqual(toggle.value as? Int, 0)
        distortion.doubleClick()
    }

    // MARK: ジオメトリ（M4）

    @MainActor
    func testGeometry() throws {
        try requireCatalog()
        let app = launch(extra: ["FOCAL_FRAME_STATS": "1"])
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        grid.click()
        app.typeText("v")
        XCTAssertTrue(waitReady(app), developStats(app))
        let view = app.descendants(matching: .any)["developView"]

        // クロップモード
        app.typeText("c")
        let done = app.buttons["cropDone"]
        XCTAssertTrue(done.waitForExistence(timeout: 3))
        let straighten = app.sliders["straightenSlider"]
        XCTAssertEqual(straighten.normalizedSliderPosition, 0.5, accuracy: 0.01)

        // 縦横比 1:1
        let aspect = app.popUpButtons["aspectPicker"]
        // 切り取っていない写真は、縦横比の既定が「元の比率」
        XCTAssertTrue(["元の比率", "Original"].contains(aspect.value as? String ?? ""), "aspect = \(String(describing: aspect.value))")
        aspect.click()
        app.menuItems["1:1"].click()
        sleep(1)
        saveScreenshot(app, name: "crop-1x1")

        // 水平線ツール: 右下がりの線を引くと傾き補正が負になる
        app.descendants(matching: .any)["levelTool"].firstMatch.click()
        let from = view.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.45))
        let to = view.coordinate(withNormalizedOffset: CGVector(dx: 0.7, dy: 0.52))
        from.press(forDuration: 0.1, thenDragTo: to)
        sleep(1)
        XCTAssertLessThan(straighten.normalizedSliderPosition, 0.49)
        saveScreenshot(app, name: "crop-level")

        // 確定（Return）
        app.typeKey(.return, modifierFlags: [])
        XCTAssertTrue(done.waitForNonExistence(timeout: 3))
        sleep(1)
        saveScreenshot(app, name: "crop-done")

        // 90° 回転して 100% 表示
        view.click()
        app.typeText("]")
        sleep(1)
        app.typeText("z")
        sleep(2)
        saveScreenshot(app, name: "rotated-100")
        app.typeText("z")

        // Undo で回転とクロップを戻す → クロップモードの傾き補正は 0
        app.typeKey("z", modifierFlags: .command)
        app.typeKey("z", modifierFlags: .command)
        app.typeText("c")
        XCTAssertTrue(done.waitForExistence(timeout: 3))
        XCTAssertEqual(app.sliders["straightenSlider"].normalizedSliderPosition, 0.5, accuracy: 0.01)
        app.typeKey(.escape, modifierFlags: [])
        XCTAssertTrue(done.waitForNonExistence(timeout: 3))

        // Redo で戻して、グリッドのサムネイルが編集後（回転・クロップ済み）になる
        app.typeKey("z", modifierFlags: [.command, .shift])
        app.typeKey("z", modifierFlags: [.command, .shift])
        app.typeText("v")
        sleep(4)
        saveScreenshot(app, name: "grid-after-edit")
    }

    // MARK: 書き出しと色（M5）

    @MainActor
    func testExport() throws {
        try requireCatalog()
        try XCTSkipIf(ProcessInfo.processInfo.environment["FOCAL_EXPORT_DIR"] == nil, "scripts/ui-test.sh から実行する")
        let app = launch(extra: ["FOCAL_EXPORT_DIR": ProcessInfo.processInfo.environment["FOCAL_EXPORT_DIR"]!])
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        grid.click()
        app.typeKey("a", modifierFlags: .command)  // すべて選択
        app.typeKey("e", modifierFlags: [.command, .shift])
        let start = app.buttons["exportStart"]
        XCTAssertTrue(start.waitForExistence(timeout: 5))
        app.checkBoxes["exportResize"].click()
        let edge = app.textFields["exportLongEdge"]
        edge.doubleClick()
        edge.typeText("1024\t")
        sleep(1)
        saveScreenshot(app, name: "export-settings")
        start.click()
        let summary = app.staticTexts["exportSummary"]
        XCTAssertTrue(summary.waitForExistence(timeout: 120))
        let text = summary.value as? String ?? summary.label
        XCTAssertTrue(text.contains("5"), text)  // 5 枚中 5 枚
        saveScreenshot(app, name: "export-finished")
        app.buttons["exportClose"].click()
    }

    /// 表示の色が二重変換されていないか（8.2 章）: ビューアの表示を撮って sRGB に戻し、
    /// 同じ写真を CLI で書き出した sRGB の画像とマスごとの平均色で比べる
    @MainActor
    func testDisplayColor() throws {
        try requireCatalog()
        let env = ProcessInfo.processInfo.environment
        guard let refText = env["FOCAL_COLOR_REF"], let index = env["FOCAL_COLOR_INDEX"].flatMap(Int.init) else {
            throw XCTSkip("scripts/ui-test.sh から実行する")
        }
        let reference = refText.split(separator: ";").map { $0.split(separator: ",").compactMap { Double($0) } }
        let cols = 48, rows = 32
        XCTAssertEqual(reference.count, cols * rows)

        var extra = ["FOCAL_FRAME_STATS": "1"]
        if let screen = env["FOCAL_WINDOW_SCREEN"] { extra["FOCAL_WINDOW_SCREEN"] = screen }  // 外部ディスプレイで確認するとき
        let app = launch(extra: extra)
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        grid.click()
        for _ in 1..<index { app.typeKey(.rightArrow, modifierFlags: []) }
        app.typeText("v")
        XCTAssertTrue(waitReady(app), developStats(app))
        sleep(2)
        let view = app.descendants(matching: .any)["developView"]
        let shot = view.screenshot()
        saveScreenshot(app, name: "color-check")

        // スクリーンショット（ディスプレイの色空間）→ sRGB に描き込む（ColorSync が変換する）
        guard let cg = shot.image.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
            return XCTFail("no screenshot")
        }
        let w = cg.width, h = cg.height
        let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        let px = ctx.data!.assumingMemoryBound(to: UInt8.self)
        func pixel(_ x: Int, _ y: Int) -> (Double, Double, Double) {
            let p = px + (y * w + x) * 4  // CGContext の行は上から
            return (Double(p[0]), Double(p[1]), Double(p[2]))
        }
        // 写真の範囲: フィット表示なので、基準画像の縦横比でビューの中央に収めた矩形（端の 1% は除く）
        let aspect = env["FOCAL_COLOR_ASPECT"].flatMap(Double.init) ?? 1.5
        let fitW = min(Double(w), Double(h) * aspect), fitH = fitW / aspect
        let inset = 0.01
        let minX = Int((Double(w) - fitW) / 2 + fitW * inset), maxX = Int((Double(w) + fitW) / 2 - fitW * inset)
        let minY = Int((Double(h) - fitH) / 2 + fitH * inset), maxY = Int((Double(h) + fitH) / 2 - fitH * inset)
        // 2 つの仮説で比べる:
        //  正しい色管理: 画面（sRGB に戻したもの）= 基準
        //  タグ付けの誤り: 画面 = 基準の値を Display P3 とみなして表示したもの（二重変換・無変換も同じ向きにずれる）
        let p3 = CGColorSpace(name: CGColorSpace.displayP3)!, srgb = CGColorSpace(name: CGColorSpace.sRGB)!
        var correct = 0.0, wrong = 0.0, worst = 0.0
        for r in 0..<rows {
            for c in 0..<cols {
                let x0 = minX + (maxX - minX) * c / cols, x1 = minX + (maxX - minX) * (c + 1) / cols
                let y0 = minY + (maxY - minY) * r / rows, y1 = minY + (maxY - minY) * (r + 1) / rows
                var sum = (0.0, 0.0, 0.0), n = 0.0
                for y in y0..<y1 { for x in x0..<x1 { let p = pixel(x, y); sum.0 += p.0; sum.1 += p.1; sum.2 += p.2; n += 1 } }
                let shown = [sum.0 / n, sum.1 / n, sum.2 / n]
                let ref = reference[r * cols + c]
                let misread = CGColor(colorSpace: p3, components: [ref[0] / 255, ref[1] / 255, ref[2] / 255, 1])!
                    .converted(to: srgb, intent: .relativeColorimetric, options: nil)!.components!.prefix(3).map { $0 * 255 }
                let d = (0..<3).map { abs(shown[$0] - ref[$0]) }.reduce(0, +) / 3
                let dw = (0..<3).map { abs(shown[$0] - misread[$0]) }.reduce(0, +) / 3
                correct += d
                wrong += dw
                worst = max(worst, d)
            }
        }
        let n = Double(cols * rows)
        let stats = app.staticTexts["frameStats"]
        let screen = (stats.value as? String ?? stats.label).components(separatedBy: " ")
            .first { $0.hasPrefix("screen=") } ?? "screen=?"
        let result = String(format: "%@ mean diff %.2f (worst cell %.2f) / if mis-tagged %.2f (8-bit sRGB, %dx%d cells)",
                            screen, correct / n, worst, wrong / n, cols, rows)
        print("COLOR \(result)")
        let att = XCTAttachment(string: result)
        att.name = "color-diff"
        att.lifetime = .keepAlways
        add(att)
        XCTAssertLessThan(correct / n, 2.0, result)
        XCTAssertLessThan(correct * 2.5, wrong, result)  // 正しい色管理の仮説の方が明らかに近い
    }

    /// 5 機種を順に開いて、プレビュー表示まで・現像可能までの時間を記録する（11.1 章）
    @MainActor
    func testDevelopTimings() throws {
        try requireCatalog()
        let app = launch(extra: ["FOCAL_FRAME_STATS": "1"])
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        grid.click()
        app.typeText("v")
        var lines: [String] = []
        for i in 0..<5 {
            XCTAssertTrue(waitReady(app), developStats(app))
            // スライダーを 20 回動かして、操作から表示までの時間を測る
            let slider = exposure(app)
            for k in 0..<20 { slider.adjust(toNormalizedSliderPosition: 0.45 + 0.1 * CGFloat(k % 5) / 4) }
            sleep(1)
            slider.doubleClick()  // 既定値に戻す
            let s = developStats(app)
            lines.append("photo \(i + 1): \(s)")
            print("DEVELOP[\(i + 1)] \(s)")
            sleep(2)  // 次の写真の先読みを待つ（実際の使い方: 1 枚見てから次へ）
            app.typeKey(.rightArrow, modifierFlags: [])
        }
        let att = XCTAttachment(string: lines.joined(separator: "\n"))
        att.name = "develop-timings"
        att.lifetime = .keepAlways
        add(att)
    }

    /// 日本語表示（String Catalog）のスクリーンショット
    @MainActor
    func testJapanese() throws {
        try requireCatalog()
        let app = XCUIApplication()
        let env = ProcessInfo.processInfo.environment
        for key in ["FOCAL_CATALOG", "FOCAL_CACHE"] { if let v = env[key] { app.launchEnvironment[key] = v } }
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES", "-AppleLanguages", "(ja)", "-AppleLocale", "ja_JP"]
        app.launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        XCTAssertTrue(app.staticTexts["すべての写真"].waitForExistence(timeout: 5))
        grid.click()
        app.typeText("v")
        sleep(3)
        saveScreenshot(app, name: "japanese-viewer")
    }

    /// 絞り込みバー（9.5 章）: 評価で絞り込むと件数が変わり、バーを閉じても要約が出て、解除で戻る
    @MainActor
    func testFilterBar() throws {
        try requireCatalog()
        let app = launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        let count = app.staticTexts["photoCount"]
        let expected = ProcessInfo.processInfo.environment["FOCAL_EXPECTED_COUNT"] ?? "5"
        XCTAssertEqual(count.value as? String ?? count.label, "1 / \(expected)")
        // 1 枚目に ★3
        grid.click()
        app.typeText("3")

        app.buttons["filterButton"].click()
        let rating = app.popUpButtons["filterRating"]
        XCTAssertTrue(rating.waitForExistence(timeout: 3))
        rating.click()
        rating.menuItems.element(boundBy: 3).click()  // 「すべて」「★1 以上」「★2 以上」「★3 以上」の 4 番目
        XCTAssertTrue(waitValue(count, "1 / 1"))
        saveScreenshot(app, name: "filter-bar")

        // バーを閉じても、絞り込み中は要約が出る
        app.buttons["filterButton"].click()
        XCTAssertTrue(app.staticTexts["filterSummary"].waitForExistence(timeout: 3))
        app.buttons["filterButton"].click()
        app.buttons["filterClear"].click()
        XCTAssertTrue(waitValue(count, "1 / \(expected)"))
        XCTAssertFalse(app.staticTexts["filterSummary"].exists)

        // 後片付け: 評価を外す
        grid.click()
        app.typeText("0")
    }

    /// フィルムストリップ（9.5 章）: 現像画面の下に出て、クリックした写真を現像する
    @MainActor
    func testFilmstrip() throws {
        try requireCatalog()
        let app = launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        let expected = ProcessInfo.processInfo.environment["FOCAL_EXPECTED_COUNT"] ?? "5"
        // ツールバーの「現像」で切り替える
        let mode = app.radioGroups["modePicker"]
        XCTAssertTrue(mode.waitForExistence(timeout: 3))
        mode.radioButtons.element(boundBy: 1).click()
        let strip = app.descendants(matching: .any)["filmstrip"]
        XCTAssertTrue(strip.waitForExistence(timeout: 5))
        XCTAssertTrue(app.descendants(matching: .any)["developView"].exists)
        let cells = strip.descendants(matching: .any).matching(identifier: "photoCell")
        XCTAssertGreaterThanOrEqual(cells.count, 3)
        cells.element(boundBy: 2).click()
        XCTAssertTrue(waitValue(app.staticTexts["photoCount"], "3 / \(expected)"))
        sleep(1)
        saveScreenshot(app, name: "filmstrip")
        // 「一覧」に戻ると、同じ写真が選ばれている
        mode.radioButtons.element(boundBy: 0).click()
        XCTAssertTrue(grid.waitForExistence(timeout: 3))
        XCTAssertEqual(app.staticTexts["photoCount"].value as? String, "3 / \(expected)")
    }

    /// サイドバー（9.5 章）: 最近の取り込み、アルバムを作る・ドラッグで足す・削除する（写真は残る）
    @MainActor
    func testAlbums() throws {
        try requireCatalog()
        let app = launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        let count = app.staticTexts["photoCount"]
        let expected = ProcessInfo.processInfo.environment["FOCAL_EXPECTED_COUNT"] ?? "5"

        // 最近の取り込み = CLI で取り込んだ全部
        app.descendants(matching: .any)["sidebarRecent"].click()
        XCTAssertTrue(waitValue(count, "1 / \(expected)"))

        // アルバムを作ると、そのアルバム（空）を表示する
        chooseNewAlbumItem(app, 0)
        let field = app.textFields["albumName"]
        XCTAssertTrue(field.waitForExistence(timeout: 3))
        field.doubleClick()
        field.typeKey("a", modifierFlags: .command)
        field.typeText("UITest Album")
        app.buttons["albumNameOK"].click()
        let row = app.descendants(matching: .any).matching(identifier: "album-UITest Album").firstMatch
        XCTAssertTrue(row.waitForExistence(timeout: 3))
        XCTAssertTrue(waitValue(count, "0 / 0"))

        // すべての写真から 2 枚目をアルバムへドラッグ
        app.descendants(matching: .any)["sidebarAll"].click()
        XCTAssertTrue(waitValue(count, "1 / \(expected)"))
        let cell = grid.descendants(matching: .any).matching(identifier: "photoCell").element(boundBy: 1)
        XCTAssertTrue(cell.waitForExistence(timeout: 3))
        cell.click()
        cell.press(forDuration: 0.6, thenDragTo: row)
        row.click()
        XCTAssertTrue(waitValue(count, "1 / 1"))
        saveScreenshot(app, name: "album")

        // 削除しても写真は残る
        row.rightClick()
        app.menuItems["deleteAlbum"].click()
        let sheetButtons = app.sheets.buttons
        let confirm = sheetButtons.element(boundBy: 0)
        XCTAssertTrue(confirm.waitForExistence(timeout: 3))
        confirm.click()
        XCTAssertTrue(waitGone(row))
        // 「すべての写真」に戻る（見ていた写真は選んだまま）
        XCTAssertTrue(waitValue(count, "2 / \(expected)"))
    }

    /// v3.19: アルバムのフォルダ・スマートアルバム・ストレージのツリー
    @MainActor
    func testLibraryOrganization() throws {
        try requireCatalog()
        let app = launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        let count = app.staticTexts["photoCount"]
        let expected = ProcessInfo.processInfo.environment["FOCAL_EXPECTED_COUNT"] ?? "5"
        func any(_ id: String) -> XCUIElement { app.descendants(matching: .any).matching(identifier: id).firstMatch }

        // フォルダ: 追加したフォルダ（名前 lib）が直接並ぶ。選ぶとそのフォルダの写真を出す
        // 見出しは AppKit が ＋ ボタンとまとめるので、識別子は "sidebarFoldersHeader-sidebarAddFolder" のようにつながる
        XCTAssertTrue(app.descendants(matching: .any).matching(NSPredicate(format: "identifier BEGINSWITH 'sidebarFoldersHeader'"))
            .firstMatch.waitForExistence(timeout: 5))
        XCTAssertFalse(app.descendants(matching: .any).matching(NSPredicate(format: "identifier BEGINSWITH 'volume-'"))
            .firstMatch.exists)  // ボリュームの階層は出さない
        let root = any("folder-lib")
        XCTAssertTrue(root.waitForExistence(timeout: 5))
        root.click()
        XCTAssertTrue(waitValue(count, "1 / \(expected)"))
        any("sidebarAll").click()

        // アルバムのフォルダを作る
        chooseNewAlbumItem(app, 2)
        let nameField = app.textFields["albumName"]
        XCTAssertTrue(nameField.waitForExistence(timeout: 3))
        nameField.doubleClick()
        nameField.typeKey("a", modifierFlags: .command)
        nameField.typeText("Trips")
        app.buttons["albumNameOK"].click()
        let folder = any("album-Trips")
        XCTAssertTrue(folder.waitForExistence(timeout: 3))

        // アルバムを作って、フォルダへドラッグして入れる（あとでフォルダを消すと一緒に消えることで確かめる）
        chooseNewAlbumItem(app, 0)
        let albumField = app.textFields["albumName"]
        XCTAssertTrue(albumField.waitForExistence(timeout: 3))
        albumField.doubleClick()
        albumField.typeKey("a", modifierFlags: .command)
        albumField.typeText("Hokkaido")
        app.buttons["albumNameOK"].click()
        let dragged = any("album-Hokkaido")
        XCTAssertTrue(dragged.waitForExistence(timeout: 3))
        dragged.press(forDuration: 0.6, thenDragTo: folder)
        sleep(1)

        // フォルダの中にスマートアルバムを作る（条件を空にすると、すべての写真が対象）
        folder.rightClick()
        app.menuItems.matching(NSPredicate(format: "title BEGINSWITH 'New Smart Album Here' OR title BEGINSWITH 'ここに新規スマートアルバム'"))
            .firstMatch.click()
        let smartName = app.textFields["smartAlbumName"]
        XCTAssertTrue(smartName.waitForExistence(timeout: 3))
        smartName.doubleClick()
        smartName.typeKey("a", modifierFlags: .command)
        smartName.typeText("Favorites")
        XCTAssertTrue(app.buttons["smartRemoveRule"].waitForExistence(timeout: 3))
        saveScreenshot(app, name: "smart-album-sheet")
        app.buttons["smartRemoveRule"].click()
        app.buttons["smartAlbumOK"].click()
        let smart = any("album-Favorites")
        XCTAssertTrue(smart.waitForExistence(timeout: 3))
        XCTAssertTrue(waitValue(count, "1 / \(expected)"))  // 作ると、そのスマートアルバムを表示する
        saveScreenshot(app, name: "smart-album")

        // フォルダを削除すると、中のスマートアルバムも消える（写真は残る）
        folder.rightClick()
        app.menuItems["deleteAlbum"].click()
        let confirm = app.sheets.buttons.element(boundBy: 0)
        XCTAssertTrue(confirm.waitForExistence(timeout: 3))
        confirm.click()
        XCTAssertTrue(waitGone(folder))
        XCTAssertTrue(waitGone(smart))
        XCTAssertTrue(waitGone(dragged))  // ドラッグでフォルダに入っていた
        // 「すべての写真」に戻る（見ていた写真は選んだまま。何枚目かは前の状態による）
        XCTAssertTrue(waitUntil { (count.value as? String ?? count.label).hasSuffix("/ \(expected)") })
        XCTAssertTrue(any("sidebarAll").isSelected || any("sidebarAll").exists)
    }

    /// 利用者向けガイド（docs/user-guide.md）用のスクリーンショット。計測値の表示（FOCAL_FRAME_STATS）を出さずに撮る。
    /// 撮った画像は ui-test.sh の出力フォルダから docs/images/ へ手で選んで入れる（scripts/make-doc-images.sh）
    @MainActor
    func testDocScreenshots() throws {
        try requireCatalog()
        let app = launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        sleep(3)
        saveScreenshot(app, name: "doc-grid")

        // 現像（フィット）。4 枚目 = ニコンのカラーチェッカーの写真は避け、1 枚目の風景で撮る
        grid.click()
        app.typeText("v")
        XCTAssertTrue(app.descendants(matching: .any)["developView"].waitForExistence(timeout: 5))
        sleep(4)
        saveScreenshot(app, name: "doc-develop")

        // 露出・ハイライトを少し動かした状態
        let slider = app.sliders["exposureSlider"]
        XCTAssertTrue(slider.waitForExistence(timeout: 5))
        slider.adjust(toNormalizedSliderPosition: 0.58)
        let shadows = app.sliders["shadowsSlider"]
        if shadows.exists { shadows.adjust(toNormalizedSliderPosition: 0.65) }
        sleep(3)
        saveScreenshot(app, name: "doc-develop-edited")

        // 切り取りモード + 水平線ツール
        app.typeText("c")
        XCTAssertTrue(app.buttons["cropDone"].waitForExistence(timeout: 3))
        let view = app.descendants(matching: .any)["developView"]
        app.descendants(matching: .any)["levelTool"].firstMatch.click()
        view.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.45))
            .press(forDuration: 0.1, thenDragTo: view.coordinate(withNormalizedOffset: CGVector(dx: 0.7, dy: 0.52)))
        sleep(2)
        saveScreenshot(app, name: "doc-crop")
        app.typeKey(.escape, modifierFlags: [])  // 取り消して、編集はカタログに残さない
        sleep(1)
        app.typeKey(.escape, modifierFlags: [])
        sleep(1)
    }

    /// 公開サイト（site/）のトップページ用のスクリーンショット: 一覧と現像中（ダーク。アプリの既定の外観）。
    /// 撮った画像は apps/macos/scripts/make-doc-images.py が site/assets/ へ入れる
    @MainActor
    func testLandingScreenshots() throws {
        try requireCatalog()
        let app = launch(extra: ["FOCAL_APPEARANCE": "dark"])
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        sleep(3)
        saveScreenshot(app, name: "landing-grid")
        grid.click()
        app.typeText("v")
        XCTAssertTrue(app.descendants(matching: .any)["developView"].waitForExistence(timeout: 5))
        sleep(4)
        // 露出・シャドウを少し動かして、現像中らしい状態にする（編集はこのテスト用のカタログに残るだけ）
        let slider = app.sliders["exposureSlider"]
        XCTAssertTrue(slider.waitForExistence(timeout: 5))
        slider.adjust(toNormalizedSliderPosition: 0.56)
        let shadows = app.sliders["shadowsSlider"]
        if shadows.exists { shadows.adjust(toNormalizedSliderPosition: 0.62) }
        sleep(3)
        saveScreenshot(app, name: "landing-develop")
        app.typeKey(.escape, modifierFlags: [])
    }

    /// 現像画面のフィルムストリップ: 取っ手のドラッグで高さが変わり、下へ引くと隠れ、ボタンで戻る
    @MainActor
    func testFilmstripResize() throws {
        try requireCatalog()
        let app = launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        grid.click()
        app.typeText("v")
        let filmstrip = app.descendants(matching: .any)["filmstrip"]
        XCTAssertTrue(filmstrip.waitForExistence(timeout: 5))
        let handle = app.descendants(matching: .any)["filmstripHandle"]
        XCTAssertTrue(handle.waitForExistence(timeout: 3))
        sleep(2)  // レイアウトが落ち着いてから、最初の高さを測る
        let h0 = filmstrip.frame.height

        func drag(_ dy: CGFloat) {
            let from = handle.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.5))
            from.press(forDuration: 0.1, thenDragTo: from.withOffset(CGVector(dx: 0, dy: dy)))
            sleep(1)
        }
        drag(-80)  // 上へ引く: 高くなる
        XCTAssertGreaterThan(filmstrip.frame.height, h0 + 50, "h0=\(h0) now=\(filmstrip.frame.height)")
        saveScreenshot(app, name: "filmstrip-tall")
        drag(100)  // 下へ引く: 低くなる（172 → 72。最小は 64）
        XCTAssertLessThan(filmstrip.frame.height, h0, "h0=\(h0) now=\(filmstrip.frame.height)")
        XCTAssertGreaterThan(filmstrip.frame.height, 50)
        drag(400)  // さらに下へ引く: 隠れる
        XCTAssertTrue(waitGone(filmstrip))
        saveScreenshot(app, name: "filmstrip-hidden")

        // ボタンで出す（高さは直前のまま）
        app.buttons["filmstripToggle"].click()
        XCTAssertTrue(filmstrip.waitForExistence(timeout: 3))
        // 隠れているところから上へ引いても出る
        app.buttons["filmstripToggle"].click()
        XCTAssertTrue(waitGone(filmstrip))
        drag(-100)
        XCTAssertTrue(filmstrip.waitForExistence(timeout: 3))
    }

    /// サイドバーのフォルダ: 読み込み先の ★、右クリックの ✓、「情報を見る…」のポップオーバー
    @MainActor
    func testRootInfoAndDestination() throws {
        try requireCatalog()
        // 実際の設定（読み込み先）を書き換えないよう、環境変数で別の場所を読み込み先にしておく
        let app = launch(extra: ["FOCAL_IMPORT_DEST": NSTemporaryDirectory() + "focal-ui-dest-\(UUID().uuidString)"])
        XCTAssertTrue(app.descendants(matching: .any)["photoGrid"].waitForExistence(timeout: 10))
        func any(_ id: String) -> XCUIElement { app.descendants(matching: .any).matching(identifier: id).firstMatch }
        let folder = any("folder-lib")
        XCTAssertTrue(folder.waitForExistence(timeout: 5))
        func isDestination() -> Bool { (any("folder-lib").value as? String ?? "").contains(where: { _ in true }) && ["読み込み先", "Import destination"].contains(any("folder-lib").value as? String ?? "") }
        XCTAssertFalse(isDestination())  // 読み込み先は別の場所

        // 右クリックで読み込み先にすると、★ が付く（アクセシビリティの値に「読み込み先」が出る）
        folder.rightClick()
        let destination = app.menuItems.matching(NSPredicate(format: "title == '読み込み先にする' OR title == 'Set as Import Destination'")).firstMatch
        XCTAssertTrue(destination.waitForExistence(timeout: 3))
        destination.click()
        XCTAssertTrue(waitUntil { isDestination() })
        saveScreenshot(app, name: "root-destination-star")

        // 「情報を見る…」: パス・写真の枚数が出る
        folder.rightClick()
        let info = app.menuItems.matching(NSPredicate(format: "title == '情報を見る…' OR title == 'Get Info…'")).firstMatch
        XCTAssertTrue(info.waitForExistence(timeout: 3))
        info.click()
        // ポップオーバーの中は別のウィンドウ扱い。場所と写真の枚数の文字が出ている
        let pop = app.popovers.firstMatch
        XCTAssertTrue(pop.waitForExistence(timeout: 5))
        func has(_ part: String) -> Bool {
            pop.staticTexts.matching(NSPredicate(format: "label CONTAINS %@ OR value CONTAINS %@", part, part)).firstMatch.exists
        }
        XCTAssertTrue(waitUntil { has("/lib") }, "場所")
        XCTAssertTrue(has("5 枚") || has("5 photos"), "写真の枚数")
        XCTAssertTrue(has("接続中") || has("Connected"), "状態")
        saveScreenshot(app, name: "root-info")
        app.typeKey(.escape, modifierFlags: [])
    }

    /// 読み込み先のフォルダをカタログから外すと、残っているフォルダの先頭が読み込み先になる
    @MainActor
    func testRemovingImportDestinationMovesIt() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_MULTI_CATALOG"] == nil || env["FOCAL_MULTI_DIR"] == nil, "scripts/ui-test.sh から実行する")
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_MULTI_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_IMPORT_DEST"] = env["FOCAL_MULTI_DIR"]! + "/c"  // 読み込み先は c（実際の設定は変えない）
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))
        func any(_ id: String) -> XCUIElement { app.descendants(matching: .any).matching(identifier: id).firstMatch }
        func isDestination(_ name: String) -> Bool { ["読み込み先", "Import destination"].contains(any("folder-\(name)").value as? String ?? "") }
        for name in ["a", "b", "c"] { XCTAssertTrue(any("folder-\(name)").waitForExistence(timeout: 5), name) }
        XCTAssertTrue(waitUntil { isDestination("c") })
        XCTAssertFalse(isDestination("a") || isDestination("b"))

        func remove(_ name: String) {
            any("folder-\(name)").rightClick()
            let info = app.menuItems.matching(NSPredicate(format: "title == '情報を見る…' OR title == 'Get Info…'")).firstMatch
            XCTAssertTrue(info.waitForExistence(timeout: 3))
            info.click()
            let item = app.popovers.firstMatch.buttons.matching(NSPredicate(format: "label BEGINSWITH 'カタログから外す' OR label BEGINSWITH 'Remove from Catalog'")).firstMatch
            XCTAssertTrue(item.waitForExistence(timeout: 3))
            item.click()
            let confirm = app.sheets.buttons.element(boundBy: 0)
            XCTAssertTrue(confirm.waitForExistence(timeout: 3))
            confirm.click()
            XCTAssertTrue(waitGone(any("folder-\(name)")))
        }

        remove("b")  // 読み込み先ではないフォルダを外しても、読み込み先は変わらない
        XCTAssertTrue(isDestination("c"))
        remove("c")  // 読み込み先を外すと、残っている先頭（a）に移る
        XCTAssertTrue(waitUntil { isDestination("a") }, "a に移る")
        saveScreenshot(app, name: "destination-moved")
    }

    /// 重なったフォルダ（以前の二重登録の跡）がカタログにあると、起動時に統合するか聞く。統合すると 1 つになり、
    /// ★などの情報の多い行が残る
    @MainActor
    func testMergeNestedRootsPrompt() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_NESTED_CATALOG"] == nil, "scripts/ui-test.sh から実行する")
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_NESTED_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))
        func any(_ id: String) -> XCUIElement { app.descendants(matching: .any).matching(identifier: id).firstMatch }
        let merge = app.buttons["mergeNestedRoots"]
        XCTAssertTrue(merge.waitForExistence(timeout: 5))
        saveScreenshot(app, name: "merge-prompt")
        merge.click()
        // 統合したというお知らせ（OK で閉じる）
        let ok = app.sheets.buttons.element(boundBy: 0)
        XCTAssertTrue(ok.waitForExistence(timeout: 5))
        ok.click()
        // ルートは 1 つになり、写真も 1 枚（二重の跡が消えた）
        sleep(2)
        saveScreenshot(app, name: "merge-done")
        // 写真は 1 枚（統合の前は、二重の跡で 2 枚だった）。sub は nested の中のサブフォルダとして残る
        XCTAssertTrue(waitValue(app.staticTexts["photoCount"], "1 / 1"), "count = \(app.staticTexts["photoCount"].value ?? "?")")
        XCTAssertTrue(any("folder-sub").exists)
    }

    /// サブフォルダを選んでから、その上のフォルダ（ルート）を選んでもサイドバーが崩れない
    @MainActor
    func testSidebarSubfolderThenRoot() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_NESTED_CATALOG"] == nil, "scripts/ui-test.sh から実行する")
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_NESTED_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))
        func any(_ id: String) -> XCUIElement { app.descendants(matching: .any).matching(identifier: id).firstMatch }
        // 同じ fixture を別のテストが統合済みなら、確認は出ない
        let merge = app.buttons["mergeNestedRoots"]
        if merge.waitForExistence(timeout: 5) {
            merge.click()
            let ok = app.sheets.buttons.element(boundBy: 0)
            XCTAssertTrue(ok.waitForExistence(timeout: 5))
            ok.click()
        }
        sleep(2)
        let sub = any("folder-sub")
        XCTAssertTrue(sub.waitForExistence(timeout: 5))
        sub.click()
        sleep(1)
        saveScreenshot(app, name: "sidebar-sub-selected")
        // 副題に現在地（ルート › サブフォルダ・枚数）が出る。フォルダは絞り込みのチップには出さない
        func subtitle(_ part: String) -> Bool {
            app.staticTexts.matching(NSPredicate(format: "value CONTAINS %@", part)).firstMatch.exists
        }
        XCTAssertTrue(waitUntil { subtitle("nested › sub") }, "副題に現在地")
        XCTAssertTrue(subtitle("1 枚") || subtitle("1 photos"), "副題に枚数")
        XCTAssertFalse(any("filterSummary").exists, "フォルダだけの選択ではチップを出さない")
        // 親フォルダの行（AX には出ないので、sub の 32pt 上の位置を押す）
        sub.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0)).withOffset(CGVector(dx: 0, dy: -32)).click()
        sleep(1)
        saveScreenshot(app, name: "sidebar-root-selected")
        XCTAssertTrue(any("sidebarAll").exists)
        XCTAssertTrue(app.descendants(matching: .any).matching(NSPredicate(format: "identifier BEGINSWITH 'sidebarLibraryHeader'")).firstMatch.exists)
        // 子を選んだまま、親を畳む
        sub.click()
        sleep(1)
        let chevron = sub.coordinate(withNormalizedOffset: CGVector(dx: 0, dy: 0)).withOffset(CGVector(dx: -26, dy: -32 + 8))
        chevron.click()
        sleep(1)
        saveScreenshot(app, name: "sidebar-collapsed-with-selected-child")
        XCTAssertTrue(app.descendants(matching: .any).matching(NSPredicate(format: "identifier BEGINSWITH 'sidebarLibraryHeader'")).firstMatch.exists)
    }

    /// 一覧の選択: クリックは 1 枚、⌘クリックは 1 枚ずつ追加・解除、⇧クリックは起点からその写真までの範囲
    @MainActor
    func testGridSelection() throws {
        try requireCatalog()
        let app = launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        let cells = grid.descendants(matching: .any).matching(identifier: "photoCell")
        XCTAssertTrue(cells.element(boundBy: 4).waitForExistence(timeout: 5))
        // 選んでいる写真の位置（1 始まり）。1 枚のときは selectionCount が出ないので、現在の位置（photoCount）を見る
        func selected() -> [Int] {
            let multi = app.staticTexts["selectionCount"]
            if multi.exists, let v = multi.value as? String {
                return v.split(separator: ",").compactMap { Int($0) }.map { $0 - 1 }
            }
            let c = app.staticTexts["photoCount"]
            let label = c.value as? String ?? c.label
            return [(Int(label.split(separator: " ").first ?? "") ?? 0) - 1]
        }
        func click(_ i: Int, _ mods: XCUIElement.KeyModifierFlags = []) {
            if mods.isEmpty { cells.element(boundBy: i).click() } else {
                XCUIElement.perform(withKeyModifiers: mods) { cells.element(boundBy: i).click() }
            }
            usleep(300_000)
        }

        sleep(2)  // 起動直後のレイアウトとサムネイルの読み込みを待つ
        click(1)
        XCTAssertEqual(selected(), [1])
        click(3, .shift)  // 1 → 3 の範囲
        XCTAssertEqual(selected(), [1, 2, 3])
        click(4, .shift)  // 起点は 1 のまま、1 → 4
        XCTAssertEqual(selected(), [1, 2, 3, 4])
        click(2, .shift)  // 起点は 1 のまま、1 → 2 に縮む
        XCTAssertEqual(selected(), [1, 2])
        click(0, .shift)  // 起点より前: 0 → 1
        XCTAssertEqual(selected(), [0, 1])

        click(3)  // ふつうのクリックは 1 枚にして、起点にする
        XCTAssertEqual(selected(), [3])
        click(1, .command)  // ⌘ は 1 枚ずつ追加（範囲にしない）
        XCTAssertEqual(selected(), [1, 3])
        click(4, .command)
        XCTAssertEqual(selected(), [1, 3, 4])
        click(3, .command)  // もう一度で解除
        XCTAssertEqual(selected(), [1, 4])
        click(2, .shift)  // ⌘ で最後に触れた 4 を起点に、4 → 2
        XCTAssertEqual(selected(), [2, 3, 4])
        saveScreenshot(app, name: "selection-range")
    }

    /// v3.19: アルバムの上へ／下へ移動と、カバー写真のサムネイル（先頭の写真）
    @MainActor
    func testAlbumOrderAndCover() throws {
        try requireCatalog()
        let app = launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        func any(_ id: String) -> XCUIElement { app.descendants(matching: .any).matching(identifier: id).firstMatch }
        for name in ["Family", "Travel"] {
            chooseNewAlbumItem(app, 0)
            let field = app.textFields["albumName"]
            XCTAssertTrue(field.waitForExistence(timeout: 3))
            field.doubleClick()
            field.typeKey("a", modifierFlags: .command)
            field.typeText(name)
            app.buttons["albumNameOK"].click()
            XCTAssertTrue(any("album-\(name)").waitForExistence(timeout: 3))
        }
        let a = any("album-Family"), b = any("album-Travel")
        XCTAssertLessThan(a.frame.minY, b.frame.minY)  // 作った順

        // B を上へ
        b.rightClick()
        app.menuItems["moveAlbumUp"].click()
        XCTAssertTrue(waitUntil { any("album-Travel").frame.minY < any("album-Family").frame.minY })
        // いちばん上の B は「上へ」が選べない
        any("album-Travel").rightClick()
        XCTAssertFalse(app.menuItems["moveAlbumUp"].isEnabled)
        app.typeKey(.escape, modifierFlags: [])

        // 写真を足すと、カバー（先頭の写真）のサムネイルが行に出る
        any("sidebarAll").click()
        let cell = grid.descendants(matching: .any).matching(identifier: "photoCell").element(boundBy: 1)
        XCTAssertTrue(cell.waitForExistence(timeout: 3))
        cell.click()
        cell.press(forDuration: 0.6, thenDragTo: any("album-Family"))
        sleep(2)
        saveScreenshot(app, name: "album-cover")

        // あと片づけ（ほかのテストの枚数に影響しないよう、アルバムを消す）
        for name in ["Family", "Travel"] {
            any("album-\(name)").rightClick()
            app.menuItems["deleteAlbum"].click()
            let confirm = app.sheets.buttons.element(boundBy: 0)
            XCTAssertTrue(confirm.waitForExistence(timeout: 3))
            confirm.click()
            XCTAssertTrue(waitGone(any("album-\(name)")))
        }
    }

    /// v3.19: 写真の削除。普通の Delete では何も起きず、⌥⌃⇧ Delete だけが確認を出す。メニューは ⌥⌃ を押して開いたときだけ有効
    @MainActor
    func testDeleteRequiresChord() throws {
        try requireCatalog()
        let app = launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        let count = app.staticTexts["photoCount"]
        let expected = ProcessInfo.processInfo.environment["FOCAL_EXPECTED_COUNT"] ?? "5"
        grid.click()

        // 普通の Delete・⌘ Delete・⌥ Delete では何も起きない
        for mods: XCUIElement.KeyModifierFlags in [[], .command, .option] {
            app.typeKey(.delete, modifierFlags: mods)
            XCTAssertFalse(app.buttons["deleteConfirm"].waitForExistence(timeout: 1))
        }
        XCTAssertTrue(app.sheets.count == 0)

        // ⌥⌃⇧ Delete: 確認が出る。取り消すと何も消えない
        app.typeKey(.delete, modifierFlags: [.option, .control, .shift])
        let confirm = app.buttons["deleteConfirm"]
        XCTAssertTrue(confirm.waitForExistence(timeout: 3))
        saveScreenshot(app, name: "delete-confirm")
        app.typeKey(.escape, modifierFlags: [])
        XCTAssertTrue(waitGone(confirm))
        XCTAssertEqual(count.value as? String ?? count.label, "1 / \(expected)")

        // メニュー: 修飾キーなしで開くと「ゴミ箱に移動…」は無効
        let menu = app.menuBars.menuBarItems.matching(NSPredicate(format: "title == '写真' OR title == 'Photo'")).firstMatch
        XCTAssertTrue(menu.waitForExistence(timeout: 3))
        menu.click()
        let item = app.menuItems.matching(NSPredicate(format: "title == 'ゴミ箱に移動…' OR title == 'Move to Trash…'")).firstMatch
        XCTAssertTrue(item.waitForExistence(timeout: 3))
        XCTAssertFalse(item.isEnabled)
        app.typeKey(.escape, modifierFlags: [])
        // ⌥⌃ を押して開くと有効
        XCUIElement.perform(withKeyModifiers: [.option, .control]) { menu.click() }
        XCTAssertTrue(item.waitForExistence(timeout: 3))
        XCTAssertTrue(waitUntil { item.isEnabled })
        saveScreenshot(app, name: "delete-menu")
        app.typeKey(.escape, modifierFlags: [])
    }

    /// v3.19: 取り込んだ写真を ⌥⌃⇧ Delete で削除する（ゴミ箱の代わりに FOCAL_TRASH_DIR へ移す）
    @MainActor
    func testDeleteImportedPhoto() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_IMPORT_SOURCE"] == nil || env["FOCAL_DELETE_CATALOG"] == nil, "scripts/ui-test.sh から実行する")
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_DELETE_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_IMPORT_SOURCE"] = env["FOCAL_IMPORT_SOURCE"]
        app.launchEnvironment["FOCAL_IMPORT_DEST"] = env["FOCAL_DELETE_DEST"]
        app.launchEnvironment["FOCAL_TRASH_DIR"] = env["FOCAL_DELETE_TRASH"]
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))
        app.typeKey("i", modifierFlags: [.command, .shift])
        XCTAssertTrue(app.buttons["importStart"].waitForExistence(timeout: 5))
        app.buttons["importStart"].click()
        XCTAssertTrue(app.staticTexts["importResult"].waitForExistence(timeout: 30))
        app.buttons["importShow"].click()
        let count = app.staticTexts["photoCount"]
        XCTAssertTrue(waitValue(count, "1 / 1"))
        app.descendants(matching: .any)["photoGrid"].click()

        app.typeKey(.delete, modifierFlags: [.option, .control, .shift])
        let confirm = app.buttons["deleteConfirm"]
        XCTAssertTrue(confirm.waitForExistence(timeout: 3))
        confirm.click()
        XCTAssertTrue(waitValue(count, "0 / 0"))
        saveScreenshot(app, name: "deleted")
    }

    /// v3.19: カードの取り込み。取り込み元と読み込み先は ui-test.sh が用意する
    @MainActor
    func testCardImport() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_IMPORT_SOURCE"] == nil || env["FOCAL_IMPORT_CATALOG"] == nil, "scripts/ui-test.sh から実行する")
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_IMPORT_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_IMPORT_SOURCE"] = env["FOCAL_IMPORT_SOURCE"]
        app.launchEnvironment["FOCAL_IMPORT_DEST"] = env["FOCAL_IMPORT_DEST"]
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))

        app.typeKey("i", modifierFlags: [.command, .shift])
        let start = app.buttons["importStart"]
        XCTAssertTrue(start.waitForExistence(timeout: 5))
        let summary = app.staticTexts["importSummary"]
        XCTAssertTrue(summary.waitForExistence(timeout: 5))
        XCTAssertTrue(waitUntil { (summary.value as? String ?? summary.label).contains("1") })  // カードの写真 1 枚
        saveScreenshot(app, name: "import-sheet")
        start.click()
        let result = app.staticTexts["importResult"]
        XCTAssertTrue(result.waitForExistence(timeout: 30))
        saveScreenshot(app, name: "import-finished")
        app.buttons["importShow"].click()
        // 「最近の取り込み」に取り込んだ写真だけが出る
        XCTAssertTrue(waitValue(app.staticTexts["photoCount"], "1 / 1"))
        XCTAssertTrue(app.descendants(matching: .any)["sidebarRecent"].exists)
        saveScreenshot(app, name: "import-recent")
    }

    /// 現像プリセット: 取り込みシートで選ぶと取り込んだ写真に重なり、現像画面のメニューから適用・保存できる
    @MainActor
    func testPresets() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_PRESET_CARD"] == nil || env["FOCAL_PRESETS"] == nil, "scripts/ui-test.sh から実行する")
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_PRESET_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_IMPORT_SOURCE"] = env["FOCAL_PRESET_CARD"]
        app.launchEnvironment["FOCAL_IMPORT_DEST"] = env["FOCAL_PRESET_DEST"]
        app.launchEnvironment["FOCAL_PRESETS"] = env["FOCAL_PRESETS"]
        app.launchEnvironment["FOCAL_BUILTIN_PRESETS"] = env["FOCAL_BUILTIN_PRESETS"]
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))

        // 取り込みシート: 既定は「なし」。Focal 標準を選んで取り込む
        app.typeKey("i", modifierFlags: [.command, .shift])
        let start = app.buttons["importStart"]
        XCTAssertTrue(start.waitForExistence(timeout: 5))
        let picker = app.popUpButtons["importPreset"]
        XCTAssertTrue(picker.waitForExistence(timeout: 5))
        picker.click()
        let standard = picker.menuItems["Focal Standard"]
        XCTAssertTrue(standard.waitForExistence(timeout: 3))
        saveScreenshot(app, name: "import-preset-menu")
        standard.click()
        XCTAssertTrue(waitUntil { (picker.value as? String ?? "") == "Focal Standard" })
        start.click()
        XCTAssertTrue(app.staticTexts["importResult"].waitForExistence(timeout: 30))
        app.buttons["importShow"].click()
        XCTAssertTrue(waitValue(app.staticTexts["photoCount"], "1 / 1"))

        // 現像画面: 露出が Focal 標準の +1.5 EV（スライダーの位置 0.65）になっている
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 5))
        grid.click()
        app.typeText("v")
        let slider = app.sliders["exposureSlider"]
        XCTAssertTrue(slider.waitForExistence(timeout: 10))
        XCTAssertTrue(waitUntil { abs(slider.normalizedSliderPosition - 0.65) < 0.02 }, "position \(slider.normalizedSliderPosition)")

        // 調整を変えて、自分のプリセットとして保存する。メニューに出る
        slider.adjust(toNormalizedSliderPosition: 0.4)
        let presets = app.menuButtons["presetsMenu"]
        presets.click()
        let save = presets.menuItems.matching(NSPredicate(format: "title BEGINSWITH '現像の調整' OR title BEGINSWITH 'いまの調整' OR title BEGINSWITH 'Save Settings'")).firstMatch
        XCTAssertTrue(save.waitForExistence(timeout: 3))
        save.click()
        let name = app.textFields["presetName"]
        XCTAssertTrue(name.waitForExistence(timeout: 3))
        name.click()
        name.typeText("My Look")
        saveScreenshot(app, name: "preset-name-sheet")
        app.buttons["presetNameOK"].click()
        XCTAssertTrue(waitGone(name))
        // 保存のファイルができている
        let dir = env["FOCAL_PRESETS"]!
        XCTAssertTrue(waitUntil { ((try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []).contains("My Look.focalpreset") })

        // Focal 標準を適用すると +1.5 EV に戻る（1 回の Undo で、保存前の位置に戻る）
        presets.click()
        let apply = presets.menuItems["Focal Standard"]
        XCTAssertTrue(apply.waitForExistence(timeout: 3))
        XCTAssertTrue(presets.menuItems["My Look"].exists)
        saveScreenshot(app, name: "preset-menu")
        apply.click()
        XCTAssertTrue(waitUntil { abs(slider.normalizedSliderPosition - 0.65) < 0.02 }, "position \(slider.normalizedSliderPosition)")
    }

    /// 右ペインのプリセット: 一覧から適用、チェック・変更あり・更新（上書き）、＋で保存、名前の変更、削除、「なし」
    @MainActor
    func testPresetPanel() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_PRESET_CARD"] == nil || env["FOCAL_PRESETS"] == nil, "scripts/ui-test.sh から実行する")
        // このテストだけの空のプリセットのフォルダ（ほかのテストの保存と混ざらないように）
        let userDir = NSTemporaryDirectory() + "focal-ui-presets-\(UUID().uuidString)"
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_PRESETS"] = userDir
        app.launchEnvironment["FOCAL_BUILTIN_PRESETS"] = env["FOCAL_BUILTIN_PRESETS"]
        app.launchEnvironment["FOCAL_IMPORT_DEST"] = NSTemporaryDirectory() + "focal-ui-dest-\(UUID().uuidString)"
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        grid.click()
        app.typeText("v")
        func any(_ id: String) -> XCUIElement { app.descendants(matching: .any).matching(identifier: id).firstMatch }
        func selected(_ id: String) -> Bool { (any(id).value as? String) == "selected" }
        let slider = app.sliders["exposureSlider"]
        XCTAssertTrue(slider.waitForExistence(timeout: 10))
        XCTAssertTrue(any("presetRow-Focal Standard").waitForExistence(timeout: 5))
        // 先に「なし」にして、調整のない状態から始める（同じカタログを使うほかのテストが、この写真を編集していることがある）
        any("presetRow-none").click()
        XCTAssertTrue(waitUntil { selected("presetRow-none") }, "調整のない写真は「なし」にチェック")
        saveScreenshot(app, name: "preset-panel-none")

        // 一覧から適用: 露出 +1.5（位置 0.65）、チェックが Focal Standard に移る
        any("presetRow-Focal Standard").click()
        XCTAssertTrue(waitUntil { abs(slider.normalizedSliderPosition - 0.65) < 0.02 })
        XCTAssertTrue(waitUntil { selected("presetRow-Focal Standard") })
        XCTAssertFalse(selected("presetRow-none"))

        // 動かすとチェックが外れ、「変更あり」。同梱のプリセットには「更新」は出ない
        slider.adjust(toNormalizedSliderPosition: 0.4)
        XCTAssertTrue(waitUntil { !selected("presetRow-Focal Standard") })
        XCTAssertFalse(any("presetRow-Focal Standard-update").exists)

        // ＋ で保存 → 一覧に増え、チェックが付く
        any("presetAdd").click()
        let name = app.textFields["presetName"]
        XCTAssertTrue(name.waitForExistence(timeout: 3))
        name.click()
        name.typeText("Panel Look")
        app.buttons["presetNameOK"].click()
        XCTAssertTrue(any("presetRow-Panel Look").waitForExistence(timeout: 3))
        XCTAssertTrue(waitUntil { selected("presetRow-Panel Look") })
        saveScreenshot(app, name: "preset-panel-saved")

        // さらに動かすと「更新」が出て、押すと上書き（チェックが戻る）
        slider.adjust(toNormalizedSliderPosition: 0.55)
        let update = any("presetRow-Panel Look-update")
        XCTAssertTrue(update.waitForExistence(timeout: 3))
        XCTAssertFalse(selected("presetRow-Panel Look"))
        saveScreenshot(app, name: "preset-panel-modified")
        update.click()
        XCTAssertTrue(waitUntil { selected("presetRow-Panel Look") })

        // 名前の変更（右クリック）
        any("presetRow-Panel Look").rightClick()
        let rename = app.menuItems.matching(NSPredicate(format: "title BEGINSWITH '名前を変更' OR title BEGINSWITH 'Rename'")).firstMatch
        XCTAssertTrue(rename.waitForExistence(timeout: 3))
        rename.click()
        let field = app.textFields["presetName"]
        XCTAssertTrue(field.waitForExistence(timeout: 3))
        field.click()
        field.typeKey("a", modifierFlags: .command)
        field.typeText("Renamed")
        app.buttons["presetNameOK"].click()
        XCTAssertTrue(any("presetRow-Renamed").waitForExistence(timeout: 3))
        XCTAssertTrue(waitGone(any("presetRow-Panel Look")))
        XCTAssertTrue(waitUntil { selected("presetRow-Renamed") }, "名前を変えても中身は同じ")

        // 「なし」: 調整が初期値に戻る
        any("presetRow-none").click()
        XCTAssertTrue(waitUntil { abs(slider.normalizedSliderPosition - 0.5) < 0.02 })
        XCTAssertTrue(waitUntil { selected("presetRow-none") })

        // 削除（右クリック）
        any("presetRow-Renamed").rightClick()
        // メニューバーにも同じ名前の項目があるので、開いているポップアップの（押せる）項目を選ぶ
        let deletes = app.menuItems.matching(NSPredicate(format: "title == '削除' OR title == 'Delete'"))
        let deadline = Date().addingTimeInterval(3)
        while Date() < deadline, !(deletes.allElementsBoundByIndex.contains { $0.isHittable }) { usleep(100_000) }
        let delete = deletes.allElementsBoundByIndex.first(where: { $0.isHittable }) ?? deletes.firstMatch
        XCTAssertTrue(delete.exists)
        delete.click()
        XCTAssertTrue(waitGone(any("presetRow-Renamed")))
    }

    /// 「Focal について」: 版に β などの種類が添えられている（26.0.0 β）
    @MainActor
    func testAboutPanelShowsVersion() throws {
        try requireCatalog()
        let app = launch()
        XCTAssertTrue(app.descendants(matching: .any)["photoGrid"].waitForExistence(timeout: 10))
        let about = app.menuBars.menuItems.matching(NSPredicate(format: "title BEGINSWITH 'Focal について' OR title BEGINSWITH 'About Focal'")).firstMatch
        XCTAssertTrue(about.waitForExistence(timeout: 3) || { app.menuBars.menuBarItems["Focal"].click(); return about.waitForExistence(timeout: 3) }())
        about.click()
        let text = app.staticTexts.matching(NSPredicate(format: "value CONTAINS '26.0.0'")).firstMatch
        XCTAssertTrue(text.waitForExistence(timeout: 5))
        saveScreenshot(app, name: "about-panel")
        let value = text.value as? String ?? ""
        XCTAssertTrue(value.contains("β") || value.contains("beta"), "版に種類が出る: \(value)")
        // レンズ DB（Lensfun）の日付が併記される（v3.27）
        // クレジットは文章のビュー（textView）になるので、要素の種類を問わず探す
        let lens = app.descendants(matching: .any).matching(NSPredicate(format: "value CONTAINS 'Lensfun' OR label CONTAINS 'Lensfun'")).firstMatch
        XCTAssertTrue(lens.waitForExistence(timeout: 3))
        app.typeKey("w", modifierFlags: .command)
    }

    /// RAW 以外の写真（v3.22）: RAW と同じ名前の JPEG は 1 枚、PNG・JPEG だけの写真もグリッドに出る。現像できるのは RAW だけ
    @MainActor
    func testNonRawPhotos() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_MIXED_CATALOG"] == nil, "scripts/ui-test.sh から実行する")
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_MIXED_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_IMPORT_DEST"] = NSTemporaryDirectory() + "focal-ui-dest-\(UUID().uuidString)"
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        // RAW + 同じ名前の JPEG は 1 枚、PNG だけ・JPEG だけを足して 3 枚
        XCTAssertTrue(waitValue(app.staticTexts["photoCount"], "1 / 3"), "count = \(app.staticTexts["photoCount"].value ?? "?")")
        saveScreenshot(app, name: "non-raw-grid")

        func any(_ id: String) -> XCUIElement { app.descendants(matching: .any).matching(identifier: id).firstMatch }
        let cells = grid.descendants(matching: .any).matching(identifier: "photoCell")
        XCTAssertTrue(cells.element(boundBy: 2).waitForExistence(timeout: 5))
        var notDevelopable = 0
        var developable = 0
        for i in 0..<3 {
            cells.element(boundBy: i).click()
            app.typeText("v")
            XCTAssertTrue(app.descendants(matching: .any)["developView"].waitForExistence(timeout: 5))
            if any("notDevelopable").waitForExistence(timeout: 3) {
                notDevelopable += 1
                XCTAssertFalse(app.sliders["exposureSlider"].exists, "RAW 以外には現像のスライダーを出さない")
                saveScreenshot(app, name: "non-raw-viewer-\(i)")
            } else {
                XCTAssertTrue(app.sliders["exposureSlider"].waitForExistence(timeout: 10))
                developable += 1
            }
            app.typeKey(.escape, modifierFlags: [])
            XCTAssertTrue(grid.waitForExistence(timeout: 5))
        }
        XCTAssertEqual(developable, 1, "RAW は 1 枚")
        XCTAssertEqual(notDevelopable, 2, "PNG と JPEG")
    }

    /// 設定の「カタログ」タブ: カタログの情報が出る。ルートを外すと情報は保管され、最適化のボタンで消える
    @MainActor
    func testCatalogInfoAndOptimize() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_DETACH_CATALOG"] == nil, "scripts/ui-test.sh から実行する")
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_DETACH_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_IMPORT_DEST"] = NSTemporaryDirectory() + "focal-ui-dest-\(UUID().uuidString)"
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))
        func any(_ id: String) -> XCUIElement { app.descendants(matching: .any).matching(identifier: id).firstMatch }
        func text(_ part: String) -> Bool {
            app.staticTexts.matching(NSPredicate(format: "value CONTAINS %@ OR label CONTAINS %@", part, part)).firstMatch.exists
        }
        func openCatalogTab() {
            app.typeKey(",", modifierFlags: .command)
            let tab = app.toolbars.buttons.matching(NSPredicate(format: "label == 'カタログ' OR label == 'Catalog' OR title == 'カタログ' OR title == 'Catalog'")).firstMatch
            XCTAssertTrue(tab.waitForExistence(timeout: 5))
            tab.click()
        }
        func closeSettings() { app.typeKey("w", modifierFlags: .command) }

        // 1 枚、保管している情報はない
        XCTAssertTrue(waitValue(app.staticTexts["photoCount"], "1 / 1"))
        openCatalogTab()
        XCTAssertTrue(waitUntil { text("1 枚") || text("1 photos") }, "写真の数")
        XCTAssertTrue(waitUntil { text("なし") || text("None") }, "保管している情報はない")
        saveScreenshot(app, name: "catalog-settings")
        closeSettings()

        // ルートを外す（右クリック ▸ 情報を見る… ▸ カタログから外す…）
        let folder = any("folder-detach")
        XCTAssertTrue(folder.waitForExistence(timeout: 5))
        folder.rightClick()
        let info = app.menuItems.matching(NSPredicate(format: "title == '情報を見る…' OR title == 'Get Info…'")).firstMatch
        XCTAssertTrue(info.waitForExistence(timeout: 3))
        info.click()
        let remove = app.popovers.firstMatch.buttons.matching(NSPredicate(format: "label BEGINSWITH 'カタログから外す' OR label BEGINSWITH 'Remove from Catalog'")).firstMatch
        XCTAssertTrue(remove.waitForExistence(timeout: 3))
        remove.click()
        let confirm = app.sheets.buttons.element(boundBy: 0)
        XCTAssertTrue(confirm.waitForExistence(timeout: 3))
        confirm.click()
        XCTAssertTrue(waitGone(any("folder-detach")))

        // 保管している情報が 1 枚分ある → 最適化で消える
        openCatalogTab()
        XCTAssertTrue(waitUntil { text("1 枚（うち") || text("1 photos (1 with edits)") || text("1 photos (0 with edits)") }, "保管している情報")
        saveScreenshot(app, name: "catalog-settings-saved")
        let optimize = app.buttons.matching(NSPredicate(format: "label IN %@", ["最適化…", "Optimize…"])).firstMatch
        XCTAssertTrue(optimize.waitForExistence(timeout: 3))
        optimize.click()
        let go = app.sheets.buttons.matching(NSPredicate(format: "label IN %@", ["削除して最適化", "Delete and Optimize"])).firstMatch
        XCTAssertTrue(go.waitForExistence(timeout: 3))
        go.click()
        XCTAssertTrue(waitUntil(timeout: 30) { text("削除しました") || text("Removed the saved information") }, "最適化の結果")
        XCTAssertTrue(waitUntil { text("なし") || text("None") }, "最適化したあとは保管している情報がない")
        saveScreenshot(app, name: "catalog-settings-optimized")
        closeSettings()
    }

    /// カタログのバックアップ: 設定から今すぐ取れる。終了時に聞かれ、取ってから終了する（Lightroom Classic と同じ）
    @MainActor
    func testBackupCancelAfterClosingWindow() throws {
        try requireCatalog()
        let env = ProcessInfo.processInfo.environment
        let app = XCUIApplication()
        app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_CATALOG"]
        app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
        app.launchEnvironment["FOCAL_BACKUP_DIR"] = NSTemporaryDirectory() + "focal-ui-backups-\(UUID().uuidString)"
        app.launchEnvironment["FOCAL_BACKUP_INTERVAL"] = "0"
        app.launchEnvironment["FOCAL_BACKUP_ASK"] = "1"
        app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()
        XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))
        // 閉じるボタン（⌘W）で最後のウィンドウを閉じると終了の流れになる。キャンセルしたら、ダイアログは閉じたままで、終了しない
        app.typeKey("w", modifierFlags: .command)
        let start = app.buttons.matching(NSPredicate(format: "label IN %@ OR title IN %@", ["バックアップして終了", "Back Up and Quit"], ["バックアップして終了", "Back Up and Quit"])).firstMatch
        XCTAssertTrue(start.waitForExistence(timeout: 5))
        app.buttons.matching(NSPredicate(format: "label IN %@ OR title IN %@", ["キャンセル", "Cancel"], ["キャンセル", "Cancel"])).firstMatch.click()
        XCTAssertTrue(waitGone(start))
        sleep(3)
        XCTAssertFalse(start.exists, "キャンセルしたあと、ダイアログがまた出てはいけない")
        XCTAssertEqual(app.state, .runningForeground, "終了しない")
    }

    @MainActor
    func testBackupNowAndOnQuit() throws {
        try requireCatalog()
        let env = ProcessInfo.processInfo.environment
        let dir = NSTemporaryDirectory() + "focal-ui-backups-\(UUID().uuidString)"
        func backups() -> [String] {
            ((try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []).filter { $0.hasSuffix(".focalcatalog") }
        }
        func launchApp(interval: String?) -> XCUIApplication {
            let app = XCUIApplication()
            app.launchEnvironment["FOCAL_CATALOG"] = env["FOCAL_CATALOG"]
            app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
            app.launchEnvironment["FOCAL_BACKUP_DIR"] = dir
            if let interval {
                app.launchEnvironment["FOCAL_BACKUP_INTERVAL"] = interval
                app.launchEnvironment["FOCAL_BACKUP_ASK"] = "1"
            }
            app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
            app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
            app.launch()
            XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))
            return app
        }
        func panelButton(_ app: XCUIApplication, _ titles: [String]) -> XCUIElement {
            app.buttons.matching(NSPredicate(format: "label IN %@ OR title IN %@", titles, titles)).firstMatch
        }

        // 1. 設定の「カタログ」タブから、今すぐバックアップ
        var app = launchApp(interval: nil)
        app.typeKey(",", modifierFlags: .command)
        let tab = app.toolbars.buttons.matching(NSPredicate(format: "label == 'カタログ' OR label == 'Catalog' OR title == 'カタログ' OR title == 'Catalog'")).firstMatch
        XCTAssertTrue(tab.waitForExistence(timeout: 5))
        tab.click()
        let now = panelButton(app, ["今すぐバックアップ", "Back Up Now"])
        XCTAssertTrue(now.waitForExistence(timeout: 5))
        now.click()
        XCTAssertTrue(waitUntil(timeout: 30) { backups().count == 1 }, "バックアップができる")
        saveScreenshot(app, name: "backup-settings")
        // できたバックアップは、そのまま開けるカタログ（catalog.sqlite がある）
        XCTAssertTrue(FileManager.default.fileExists(atPath: dir + "/" + backups()[0] + "/catalog.sqlite"))
        app.typeKey("w", modifierFlags: .command)
        app.terminate()

        // 2. 終了時（毎回）: 聞かれる。キャンセルすると終了しない
        app = launchApp(interval: "0")
        app.typeKey("q", modifierFlags: .command)
        let start = panelButton(app, ["バックアップして終了", "Back Up and Quit"])
        XCTAssertTrue(start.waitForExistence(timeout: 5), "終了時にバックアップを聞く")
        saveScreenshot(app, name: "backup-quit-ask")
        panelButton(app, ["キャンセル", "Cancel"]).click()
        XCTAssertTrue(waitGone(start))
        XCTAssertTrue(app.windows["library"].exists, "キャンセルすると終了しない")
        XCTAssertEqual(backups().count, 1)

        // 3. もう一度終了: 「バックアップせずに終了」では取らずに終了
        app.typeKey("q", modifierFlags: .command)
        let skip = panelButton(app, ["バックアップせずに終了", "Skip and Quit"])
        XCTAssertTrue(skip.waitForExistence(timeout: 5))
        skip.click()
        XCTAssertTrue(app.wait(for: .notRunning, timeout: 20), "終了する")
        XCTAssertEqual(backups().count, 1, "スキップしたので増えない")

        // 4. 「バックアップして終了」: 取ってから終了する
        app = launchApp(interval: "0")
        app.typeKey("q", modifierFlags: .command)
        let go = panelButton(app, ["バックアップして終了", "Back Up and Quit"])
        XCTAssertTrue(go.waitForExistence(timeout: 5))
        go.click()
        XCTAssertTrue(app.wait(for: .notRunning, timeout: 30), "バックアップのあとで終了する")
        XCTAssertTrue(waitUntil(timeout: 10) { backups().count == 2 }, "終了時のバックアップができる")
    }

    /// 設定: 取り込みの設定はカタログごと（別のカタログには影響しない）。キャッシュの最大サイズの選択肢は 500 MB〜20 GB
    @MainActor
    func testImportSettingsArePerCatalog() throws {
        let env = ProcessInfo.processInfo.environment
        try XCTSkipIf(env["FOCAL_MIXED_CATALOG"] == nil || env["FOCAL_DETACH_CATALOG"] == nil, "scripts/ui-test.sh から実行する")
        func launchApp(_ catalog: String) -> XCUIApplication {
            let app = XCUIApplication()
            app.launchEnvironment["FOCAL_CATALOG"] = catalog
            app.launchEnvironment["FOCAL_CACHE"] = env["FOCAL_CACHE"]
            app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
            app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
            app.launch()
            XCTAssertTrue(app.windows["library"].waitForExistence(timeout: 10))
            return app
        }
        func tab(_ app: XCUIApplication, _ titles: [String]) {
            app.typeKey(",", modifierFlags: .command)
            let t = app.toolbars.buttons.matching(NSPredicate(format: "label IN %@ OR title IN %@", titles, titles)).firstMatch
            XCTAssertTrue(t.waitForExistence(timeout: 5))
            t.click()
        }
        func rescanSwitch(_ app: XCUIApplication) -> XCUIElement {
            app.descendants(matching: .any).matching(identifier: "rescanOnLaunch").firstMatch
        }
        func rescanIsOn(_ app: XCUIApplication) -> Bool {
            let e = rescanSwitch(app)
            XCTAssertTrue(e.waitForExistence(timeout: 5))
            return "\(e.value ?? "")" == "1"
        }

        // 取り込みの設定は「カタログ」タブの中にある（「取り込み」のタブはない）
        // カタログ A（mixed）で、起動時の再スキャンをオフにする
        var app = launchApp(env["FOCAL_MIXED_CATALOG"]!)
        tab(app, ["カタログ", "Catalog"])
        let importTab = app.toolbars.buttons.matching(NSPredicate(format: "label IN %@ OR title IN %@", ["取り込み", "Import"], ["取り込み", "Import"])).firstMatch
        XCTAssertFalse(importTab.exists, "「取り込み」のタブはない（カタログのタブの中に移した）")
        XCTAssertTrue(rescanIsOn(app), "既定はオン")
        rescanSwitch(app).click()
        XCTAssertTrue(waitUntil { !rescanIsOn(app) })
        saveScreenshot(app, name: "settings-import")
        app.typeKey("w", modifierFlags: .command)
        app.terminate()

        // カタログ B（detach）は影響を受けない（オンのまま）
        app = launchApp(env["FOCAL_DETACH_CATALOG"]!)
        tab(app, ["カタログ", "Catalog"])
        XCTAssertTrue(rescanIsOn(app), "別のカタログには影響しない")
        // キャッシュの最大サイズの選択肢
        app.toolbars.buttons.matching(NSPredicate(format: "label IN %@ OR title IN %@", ["キャッシュ", "Cache"], ["キャッシュ", "Cache"])).firstMatch.click()
        let limit = app.popUpButtons["previewCacheLimit"]
        XCTAssertTrue(limit.waitForExistence(timeout: 5))
        limit.click()
        for t in ["500 MB", "1 GB", "2 GB", "5 GB", "10 GB", "20 GB"] {
            XCTAssertTrue(limit.menuItems[t].waitForExistence(timeout: 3), "選択肢 \(t)")
        }
        saveScreenshot(app, name: "settings-cache-sizes")
        app.typeKey(.escape, modifierFlags: [])
        app.typeKey("w", modifierFlags: .command)
        app.terminate()

        // カタログ A に戻ると、オフのまま（カタログに保存されている）。元に戻しておく
        app = launchApp(env["FOCAL_MIXED_CATALOG"]!)
        tab(app, ["カタログ", "Catalog"])
        XCTAssertFalse(rescanIsOn(app), "カタログに保存されている")
        rescanSwitch(app).click()
        XCTAssertTrue(waitUntil { rescanIsOn(app) })
        app.typeKey("w", modifierFlags: .command)
    }

    /// ファイルメニューの並び: カタログ / 登録したフォルダ / アルバム / 取り込みと書き出し が区切りでグループになっている
    @MainActor
    func testFileMenuGroups() throws {
        try requireCatalog()
        let app = launch()
        XCTAssertTrue(app.descendants(matching: .any)["photoGrid"].waitForExistence(timeout: 10))
        let barItem = app.menuBars.menuBarItems.matching(NSPredicate(format: "title == 'ファイル' OR title == 'File'")).firstMatch
        barItem.click()
        let menu = barItem.menus.firstMatch
        XCTAssertTrue(menu.waitForExistence(timeout: 3))
        saveScreenshot(app, name: "file-menu")
        // 項目と区切りの並び（"-" は区切り）。標準の項目（閉じる・プリントなど）は含めず、Focal の項目だけを順に見る
        let ours: [[String]] = [
            ["新規カタログ…", "New Catalog…"], ["カタログを開く…", "Open Catalog…"],
            ["フォルダを追加…", "Add Folder…"], ["すべてのフォルダを再スキャン", "Rescan All Folders"],
            ["新規アルバム…", "New Album…"], ["新規スマートアルバム…", "New Smart Album…"],
            ["新規アルバムフォルダ…", "New Album Folder…"],
            ["カードから取り込む…", "Import from Card…"], ["書き出し…", "Export…"],
        ]
        let items = menu.menuItems.allElementsBoundByIndex
        let titles = items.map { $0.title }
        var positions: [Int] = []
        for names in ours {
            guard let i = titles.firstIndex(where: { names.contains($0) }) else {
                XCTFail("メニューにない: \(names[0])  (\(titles))")
                return
            }
            positions.append(i)
        }
        XCTAssertEqual(positions, positions.sorted(), "並びが決めた順")
        // グループの区切り: 隣り合う項目の間に区切り（title が空）が入るのはグループの境目だけ
        func separated(_ a: Int, _ b: Int) -> Bool { (positions[a] + 1..<positions[b]).contains { titles[$0].isEmpty } }
        XCTAssertFalse(separated(0, 1), "新規カタログとカタログを開くは同じグループ")
        XCTAssertTrue(separated(1, 2), "カタログとフォルダの間に区切り")
        XCTAssertFalse(separated(2, 3), "フォルダを追加と再スキャンは同じグループ")
        XCTAssertTrue(separated(3, 4), "フォルダとアルバムの間に区切り")
        XCTAssertFalse(separated(4, 5) || separated(5, 6), "アルバムの 3 項目は同じグループ")
        XCTAssertTrue(separated(6, 7), "アルバムと取り込み・書き出しの間に区切り")
        XCTAssertFalse(separated(7, 8), "取り込みと書き出しは同じグループ")
        XCTAssertFalse(titles.contains { $0.hasPrefix("カタログ情報") || $0.hasPrefix("Catalog Information") }, "カタログ情報は設定に統合した")
        app.typeKey(.escape, modifierFlags: [])
    }

    /// n: 0 新規アルバム、1 新規スマートアルバム、2 新規アルバムフォルダ（メニュー項目の識別子は取れないので名前で選ぶ）
    private func chooseNewAlbumItem(_ app: XCUIApplication, _ n: Int) {
        openNewAlbumMenu(app)
        let titles = [["新規アルバム…", "New Album…"], ["新規スマートアルバム…", "New Smart Album…"],
                      ["新規アルバムフォルダ…", "New Album Folder…"]][n]
        // メニューバーにも同じ名前の項目があるので、開いているポップアップの（押せる）項目だけを選ぶ
        let query = app.menuItems.matching(NSPredicate(format: "title == %@ OR title == %@", titles[0], titles[1]))
        let deadline = Date().addingTimeInterval(3)
        while Date() < deadline, !(query.allElementsBoundByIndex.contains { $0.isHittable }) { usleep(100_000) }
        let item = query.allElementsBoundByIndex.first(where: { $0.isHittable }) ?? query.firstMatch
        XCTAssertTrue(item.waitForExistence(timeout: 3))
        item.click()
    }

    /// 「アルバム」見出しの ＋ メニューを開く。サイドバーの見出しは AppKit が 1 つのアクセシビリティ要素にまとめる
    /// （識別子が "sidebarAlbumsHeader-newAlbum" のようにつながる）ので、＋ だけは取れない。右端から一定の距離を押す
    private func openNewAlbumMenu(_ app: XCUIApplication) {
        let header = app.descendants(matching: .any)
            .matching(NSPredicate(format: "identifier ENDSWITH 'newAlbum'")).firstMatch
        XCTAssertTrue(header.waitForExistence(timeout: 5))
        let width = max(header.frame.width, 1)
        header.coordinate(withNormalizedOffset: CGVector(dx: 1 - 12 / width, dy: 0.5)).click()
    }

    private func waitUntil(timeout: TimeInterval = 5, _ cond: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if cond() { return true }
            usleep(100_000)
        }
        return false
    }

    private func waitValue(_ e: XCUIElement, _ value: String, timeout: TimeInterval = 5) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if (e.value as? String ?? e.label) == value { return true }
            usleep(100_000)
        }
        return false
    }

    /// ダーク表示（設定の「外観」。テストでは FOCAL_APPEARANCE で指定）のスクリーンショット
    @MainActor
    func testDarkAppearance() throws {
        try requireCatalog()
        let app = launch(extra: ["FOCAL_APPEARANCE": "dark", "FOCAL_FRAME_STATS": "1"])
        let grid = app.descendants(matching: .any)["photoGrid"]
        XCTAssertTrue(grid.waitForExistence(timeout: 10))
        sleep(2)
        saveScreenshot(app, name: "dark-grid")
        grid.click()
        app.typeText("v")
        XCTAssertTrue(waitReady(app))
        sleep(1)
        saveScreenshot(app, name: "dark-viewer")
    }

    /// 最小幅のウィンドウで列が切れないか（スクリーンショットで確認）
    @MainActor
    func testNarrowWindow() throws {
        try requireCatalog()
        let app = launch(extra: ["FOCAL_WINDOW_SIZE": "1180x700"])
        XCTAssertTrue(app.descendants(matching: .any)["photoGrid"].waitForExistence(timeout: 10))
        sleep(2)
        saveScreenshot(app, name: "narrow-grid")
        app.descendants(matching: .any)["photoGrid"].click()
        app.typeText("v")
        sleep(3)
        saveScreenshot(app, name: "narrow-viewer")
    }

    /// 10 万件のカタログでスクロールのヒッチを計測する（FOCAL_SCROLL_TEST=1 のときだけ）
    @MainActor
    func testScrollPerformanceCollectionView() throws { try measureScroll(grid: "collectionView") }

    @MainActor
    func testScrollPerformanceLazyGrid() throws { try measureScroll(grid: "lazy") }

    /// アプリ自身が決まった速度でスクロールし（FOCAL_SCROLL_BENCH）、その間のヒッチを数える。
    /// XCUITest でスクロールすると、そのたびにアクセシビリティの木を取得して 70ms ほど止まるので使わない
    @MainActor
    private func measureScroll(grid impl: String) throws {
        try XCTSkipUnless(ProcessInfo.processInfo.environment["FOCAL_SCROLL_TEST"] == "1", "FOCAL_SCROLL_TEST=1 で実行する")
        let app = XCUIApplication()
        let env = ProcessInfo.processInfo.environment
        for key in ["FOCAL_CATALOG", "FOCAL_CACHE"] { if let v = env[key] { app.launchEnvironment[key] = v } }
        app.launchEnvironment["FOCAL_GRID"] = impl
        app.launchEnvironment["FOCAL_FRAME_STATS"] = "1"
        app.launchEnvironment["FOCAL_SCROLL_BENCH"] = "1"
        app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
        app.launch()

        // 起動 3 秒後に始まり 12 秒で終わる。その間は何も問い合わせない
        sleep(20)
        let stats = app.staticTexts["frameStats"]
        XCTAssertTrue(stats.waitForExistence(timeout: 10))
        let summary = stats.value as? String ?? stats.label
        XCTAssertTrue(summary.hasPrefix("done"), summary)
        print("SCROLL[\(impl)] \(summary)")
        let att = XCTAttachment(string: summary)
        att.name = "scroll-\(impl)"
        att.lifetime = .keepAlways
        add(att)
    }

    // MARK: 初回起動（カタログの作成）

    /// カタログがないと初回起動の画面を出し、「カタログを作成」で既定の場所に作って空のカタログを開く。
    /// 作ったカタログを FOCAL_CATALOG で開き直せる
    @MainActor
    func testFirstRunCreatesCatalog() throws {
        let env = ProcessInfo.processInfo.environment
        let home = try XCTUnwrap(env["FOCAL_FIRST_RUN_HOME"], "scripts/ui-test.sh から実行する")
        func start(_ extra: [String: String]) -> XCUIApplication {
            let app = XCUIApplication()
            app.launchEnvironment["FOCAL_CACHE"] = home + "/thumbs"
            app.launchEnvironment["FOCAL_WINDOW_SIZE"] = "1440x900"
            for (k, v) in extra { app.launchEnvironment[k] = v }
            app.launchArguments += ["-ApplePersistenceIgnoreState", "YES"]
            app.launch()
            return app
        }
        var app = start(["FOCAL_CATALOG_HOME": home])
        let create = app.buttons["welcomeCreate"]
        XCTAssertTrue(create.waitForExistence(timeout: 10))
        let location = app.staticTexts["welcomeLocation"]
        XCTAssertEqual(location.value as? String ?? location.label, home)
        saveScreenshot(app, name: "first-run-welcome")
        create.click()
        XCTAssertTrue(app.buttons["emptyAddFolder"].waitForExistence(timeout: 10))
        saveScreenshot(app, name: "first-run-empty")
        app.terminate()

        app = start(["FOCAL_CATALOG": home + "/Focal Catalog.focalcatalog"])
        XCTAssertTrue(app.buttons["emptyAddFolder"].waitForExistence(timeout: 10))
        XCTAssertFalse(app.buttons["welcomeCreate"].exists)
        app.terminate()
    }
}
