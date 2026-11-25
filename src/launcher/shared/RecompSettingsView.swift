import SwiftUI

struct RecompSettingsView: View {
    struct Setting: Decodable, Identifiable {
        let id: String
        let section: String
        let label: String
        let help: String
        let kind: String
        let value: String
        let choices: [String]
        let minimum: Int
        let maximum: Int
        let live: Bool
    }

    let path: String
    @Environment(\.dismiss) private var dismiss
    @State private var settings: [Setting] = []
    @State private var edits: [String: String] = [:]
    @State private var error: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Recomp Settings").font(.title2.bold())
            Text(settingsHelp)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Form {
                ForEach(["audio", "display", "rendering", "input", "timing"], id: \.self) { section in
                    let rows = settings.filter { $0.section == section }
                    if !rows.isEmpty {
                        Section(section == "timing" ? "Performance" : section.capitalized) {
                            ForEach(rows) { setting in
                                control(setting)
                                    .help(setting.help)
                            }
                        }
                    }
                }
            }
#if os(macOS)
            .formStyle(.grouped)
#endif
            if let error {
                Text(error).foregroundStyle(.red)
            }
            HStack {
                Spacer()
                Button(edits.isEmpty ? "Close" : "Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Apply") {
                    if MagatamaWriteSettings(path, edits) {
                        dismiss()
                    } else {
                        error = "Could not save the settings file."
                    }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(edits.isEmpty)
            }
        }
        .padding(20)
#if os(macOS)
        .frame(width: 540, height: 580)
#endif
        .task {
            if let data = MagatamaReadSettings(path),
               let rows = try? JSONDecoder().decode([Setting].self, from: data) {
                settings = rows
            } else {
                error = "Could not read the settings file."
            }
        }
    }

    private var settingsHelp: String {
#if os(iOS)
        "These settings apply when you press Play. During gameplay, open the in-game menu to change live settings."
#else
        "Live settings apply during gameplay. Settings marked Next launch take effect when the game starts again."
#endif
    }

    @ViewBuilder private func control(_ setting: Setting) -> some View {
        let label = setting.live ? setting.label : "\(setting.label) · Next launch"
        let value = Binding(
            get: { edits[setting.id] ?? setting.value },
            set: { edits[setting.id] = $0 == setting.value ? nil : $0 }
        )
        switch setting.kind {
        case "toggle":
            Toggle(label, isOn: Binding(
                get: { value.wrappedValue == "true" },
                set: { value.wrappedValue = $0 ? "true" : "false" }
            ))
        case "choice":
            Picker(label, selection: value) {
                ForEach(setting.choices, id: \.self) { choice in
                    Text(choiceLabel(choice, for: setting.id)).tag(choice)
                }
            }
        default:
            let current = Int(value.wrappedValue) ?? setting.minimum
            Slider(value: Binding(
                get: { Double(current) },
                set: { value.wrappedValue = String(Int($0)) }
            ), in: Double(setting.minimum)...Double(setting.maximum),
               step: setting.id == "rendering.resolution_scale" ? 5 : 1) {
                Text("\(label) (\(current))")
            }
        }
    }

    private func choiceLabel(_ choice: String, for id: String) -> String {
        switch (id, choice) {
        case ("rendering.aniso", "0"): return "Off"
        case ("rendering.aniso", _): return "\(choice)×"
        case ("rendering.frame_rate", _): return "\(choice) FPS"
        case ("rendering.render_mode", "full"): return "Full frames"
        case ("rendering.render_mode", "field"): return "Fields"
        case (_, "ps2"): return "PS2"
        case (_, "auto"): return "Automatic"
        default: return choice.capitalized
        }
    }
}
