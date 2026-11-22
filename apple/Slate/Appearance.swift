import SwiftUI

#if os(macOS)
import AppKit
typealias NativeColor = NSColor
#else
import UIKit
typealias NativeColor = UIColor
#endif

enum Ink: String, CaseIterable, Identifiable {
    case background, surface, text, muted, accent, border
    var id: Self { self }

    var label: String {
        switch self {
        case .background: return "Document"
        case .surface: return "Panels"
        case .text: return "Text"
        case .muted: return "Secondary text"
        case .accent: return "Accent"
        case .border: return "Dividers"
        }
    }

    func initial(dark: Bool) -> String {
        let values: (String, String)
        switch self {
        case .background: values = ("#18191b", "#ffffff")
        case .surface: values = ("#202225", "#f4f5f6")
        case .text: values = ("#eeefed", "#202327")
        case .muted: values = ("#a8acb2", "#626871")
        case .accent: values = ("#9ac6ff", "#255ea7")
        case .border: values = ("#383a3e", "#d6d9dd")
        }
        return dark ? values.0 : values.1
    }
}

final class Appearance: ObservableObject {
    @Published var dark: Bool {
        didSet { UserDefaults.standard.set(dark, forKey: "Slate.dark") }
    }
    @Published private var overrides: [String: String] {
        didSet { UserDefaults.standard.set(overrides, forKey: "Slate.colors") }
    }
    init() {
        dark = UserDefaults.standard.object(forKey: "Slate.dark") as? Bool ?? true
        overrides = UserDefaults.standard.dictionary(forKey: "Slate.colors") as? [String: String] ?? [:]
    }

    private func key(_ ink: Ink) -> String { (dark ? "dark." : "light.") + ink.rawValue }
    func hex(_ ink: Ink) -> String { overrides[key(ink)] ?? ink.initial(dark: dark) }
    func color(_ ink: Ink) -> Color {
        let value = UInt32(hex(ink).dropFirst(), radix: 16) ?? 0
        return Color(.sRGB, red: Double(value >> 16) / 255, green: Double((value >> 8) & 255) / 255,
                     blue: Double(value & 255) / 255, opacity: 1)
    }
    func set(_ ink: Ink, _ color: Color) {
        #if os(macOS)
        guard let resolved = NSColor(color).usingColorSpace(.sRGB) else { return }
        let channels = [resolved.redComponent, resolved.greenComponent, resolved.blueComponent]
        #else
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        guard UIColor(color).getRed(&r, green: &g, blue: &b, alpha: &a) else { return }
        let channels = [r, g, b]
        #endif
        overrides[key(ink)] = "#" + channels.map { String(format: "%02x", Int((min(1, max(0, $0)) * 255).rounded())) }.joined()
    }
    func reset() { for ink in Ink.allCases { overrides.removeValue(forKey: key(ink)) } }
}

struct AppearanceView: View {
    @ObservedObject var appearance: Appearance
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Picker("Appearance", selection: $appearance.dark) {
                    Text("Dark").tag(true)
                    Text("Light").tag(false)
                }
                .pickerStyle(.segmented)
                ForEach(Ink.allCases) { ink in
                    ColorPicker(ink.label, selection: Binding(get: { appearance.color(ink) },
                                                              set: { appearance.set(ink, $0) }), supportsOpacity: false)
                }
                Button("Restore default colors") { appearance.reset() }
            }
            .formStyle(.grouped)
            .navigationTitle("Appearance")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
        #if os(macOS)
        .frame(width: 360, height: 440)
        #endif
    }
}
