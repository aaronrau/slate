import XCTest
import UIKit

final class FilesSidebarTests: XCTestCase {
    @MainActor
    func testOpenNestedFileFromCompactDrawerOrWideSidebar() throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        let tablet = UIDevice.current.userInterfaceIdiom == .pad
        app.launchArguments += ["-Slate.dark", tablet ? "NO" : "YES"]
        app.launch()
        let toggle = app.buttons["files-toggle"]
        XCTAssertTrue(toggle.waitForExistence(timeout: 10))
        let openFolder = app.buttons["files-open-folder"]
        if tablet {
            XCTAssertTrue(openFolder.waitForExistence(timeout: 5))
            toggle.tap()
            XCTAssertFalse(openFolder.exists)
        } else {
            XCTAssertFalse(openFolder.exists)
        }
        toggle.tap()
        XCTAssertTrue(openFolder.waitForExistence(timeout: 5))
        XCTAssertTrue(openFolder.isHittable)
        let root = app.buttons["Sidebar Fixture"]
        XCTAssertTrue(root.waitForExistence(timeout: 8), "The UI fixture must be in the app’s Documents folder.")
        root.tap()
        let nested = app.buttons["Nested"]
        XCTAssertTrue(nested.waitForExistence(timeout: 5))
        nested.tap()
        let file = app.buttons["tree-file-Canvas.txt"]
        XCTAssertTrue(file.waitForExistence(timeout: 5))
        attach(app, name: tablet ? "iPad Files sidebar" : "iPhone Files drawer")
        file.tap()
        let editor = app.textViews["Source editor"]
        let contents = NSPredicate(format: "value CONTAINS %@", "Slate drawer test")
        expectation(for: contents, evaluatedWith: editor)
        waitForExpectations(timeout: 8)
        XCTAssertEqual(openFolder.exists, tablet)
        attach(app, name: tablet ? "iPad selected document" : "iPhone selected document")
        if !tablet { toggle.tap() }
        XCTAssertTrue(file.waitForExistence(timeout: 5))
        XCTAssertTrue(file.isSelected)
        nested.tap()
        XCTAssertFalse(file.exists)
    }

    @MainActor
    func testCreateSiblingThenCancelAndConfirmDeletingDirtyFile() {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launch()
        let tablet = UIDevice.current.userInterfaceIdiom == .pad
        let files = app.buttons["files-toggle"]
        XCTAssertTrue(files.waitForExistence(timeout: 8))
        if !tablet { files.tap() }
        let fixture = app.buttons["Sidebar Fixture"]
        XCTAssertTrue(fixture.waitForExistence(timeout: 5))
        fixture.tap()
        let nested = app.buttons["Nested"]
        XCTAssertTrue(nested.waitForExistence(timeout: 5))
        nested.press(forDuration: 1.1)
        app.buttons["folder-new-file"].tap()
        let name = "Slate-\(UUID().uuidString.prefix(8)).txt"
        let field = app.textFields["new-file-name"]
        XCTAssertTrue(field.waitForExistence(timeout: 5))
        field.tap()
        field.typeText(name)
        app.buttons["Create"].tap()
        let editor = app.textViews["Source editor"]
        let tab = app.buttons["document-tab-" + name]
        XCTAssertTrue(tab.waitForExistence(timeout: 5))
        editor.tap()
        editor.typeText("draft edits")
        let options = app.buttons["File and appearance options"]
        options.tap()
        app.buttons["current-delete-file"].tap()
        let confirm = app.alerts.firstMatch
        XCTAssertTrue(confirm.waitForExistence(timeout: 5))
        XCTAssertTrue(confirm.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", name)).firstMatch.exists)
        XCTAssertTrue(confirm.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "unsaved changes")).firstMatch.exists)
        confirm.buttons["Cancel"].tap()
        XCTAssertTrue((editor.value as? String)?.contains("draft edits") == true)
        if !tablet { files.tap() }
        let row = app.buttons["tree-file-" + name]
        XCTAssertTrue(row.waitForExistence(timeout: 5))
        row.press(forDuration: 1.1)
        app.buttons["Delete File…"].tap()
        XCTAssertTrue(confirm.waitForExistence(timeout: 5))
        confirm.buttons["Delete"].tap()
        let disappeared = NSPredicate(format: "exists == false")
        expectation(for: disappeared, evaluatedWith: row)
        waitForExpectations(timeout: 8)
        XCTAssertTrue(app.buttons["tree-file-Canvas.txt"].exists)
        if !tablet { files.tap() }
        XCTAssertFalse(tab.exists)
        XCTAssertEqual(editor.value as? String ?? "", "")
    }

    @MainActor
    private func attach(_ app: XCUIApplication, name: String) {
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = name
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }
}
