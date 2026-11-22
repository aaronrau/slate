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
    private func attach(_ app: XCUIApplication, name: String) {
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = name
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }
}
