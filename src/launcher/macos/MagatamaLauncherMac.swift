// Magatama launcher for macOS (SwiftUI). Imports the user's own disc image (.iso, .chd or .zso) into ~/magatama/disc
// with the bundled magatama-disc-import tool (src/app/magatama_disc_import_tool.cpp), and starts the game the way
// scripts/magatama_run.sh does. Built by scripts/magatama_build_launcher_mac.sh into "Magatama Launcher.app".
import AppKit
import SwiftUI
import UniformTypeIdentifiers

// Where things are. Two layouts:
//  - Magatama.app as the installer builds it (src/launcher/macos-installer): the game is Contents/MacOS/Fukami
//    beside this launcher, and the disc, saves, settings and logs are in ~/Library/Application Support/Magatama.
//  - the developer layout: the game runs from the build tree (~/magatama/runtime, scripts/magatama_run.sh) and
//    the data is in ~/magatama. MAGATAMA_HOME overrides ~/magatama.
enum Paths {
    static var bundledGame: URL { Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/Fukami") }
    static var bundled: Bool { FileManager.default.isExecutableFile(atPath: bundledGame.path) }
    static var home: URL {
        if bundled {
            return FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Library/Application Support/Magatama")
        }
        if let custom = ProcessInfo.processInfo.environment["MAGATAMA_HOME"], !custom.isEmpty {
            return URL(fileURLWithPath: custom)
        }
        return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("magatama")
    }
    static var disc: URL { home.appendingPathComponent("disc") }
    static var runScript: URL { home.appendingPathComponent("runtime/scripts/magatama_run.sh") }
    static var settingsINI: URL {
        if let custom = ProcessInfo.processInfo.environment["RRV_FUKAMI_INI"], !custom.isEmpty {
            let base = bundled ? home : home.appendingPathComponent("runtime", isDirectory: true)
            return URL(fileURLWithPath: custom, relativeTo: base).absoluteURL
        }
        return home.appendingPathComponent("magatama.ini")
    }
    static var iosBuildScript: URL { home.appendingPathComponent("runtime/scripts/magatama_build_ios.sh") }
    static var iosBuildLog: URL { home.appendingPathComponent("runtime/build-ios-launcher.log") }
    static var iosBuilds: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("magatama/ios-builds")
    }
    static var importTool: URL {
        Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/magatama-disc-import")
    }
}

final class LauncherModel: ObservableObject {
    @Published var installed = false
    @Published var importing = false
    @Published var progress: Double = 0
    @Published var currentFile = ""
    @Published var message = ""
    @Published var failed = false
    @Published var running = false
    @Published private(set) var buildingIOS = false
    @Published private(set) var iosBuildMessage = ""
    @Published private(set) var iosBuildFailed = false
    private var iosBuildProcess: Process?
    private var importProcess: Process?
    private var gameProcess: Process?

    init() { refresh() }

    func refresh() {
        DispatchQueue.global(qos: .userInitiated).async {
            let output = Self.run(Paths.importTool, ["--check", Paths.disc.path])
            let ok = output.contains("installed 1")
            DispatchQueue.main.async { self.installed = ok }
        }
    }

    private static func run(_ tool: URL, _ arguments: [String]) -> String {
        let process = Process()
        process.executableURL = tool
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        do { try process.run() } catch { return "" }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return String(decoding: data, as: UTF8.self)
    }

    func importImage(_ url: URL) {
        importing = true
        failed = false
        message = ""
        progress = 0
        currentFile = url.lastPathComponent
        let scoped = url.startAccessingSecurityScopedResource()
        let process = Process()
        process.executableURL = Paths.importTool
        process.arguments = [url.path, Paths.disc.path]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        var pending = ""
        var result = ""
        pipe.fileHandleForReading.readabilityHandler = { handle in
            let chunk = String(decoding: handle.availableData, as: UTF8.self)
            pending += chunk
            while let newline = pending.firstIndex(of: "\n") {
                let line = String(pending[..<newline])
                pending = String(pending[pending.index(after: newline)...])
                if line.hasPrefix("progress ") {
                    let parts = line.split(separator: " ", maxSplits: 3, omittingEmptySubsequences: false)
                    if parts.count >= 3, let done = Double(parts[1]), let total = Double(parts[2]), total > 0 {
                        let name = parts.count == 4 ? String(parts[3]) : ""
                        DispatchQueue.main.async {
                            self.progress = done / total
                            if !name.isEmpty { self.currentFile = name }
                        }
                    }
                } else if line.hasPrefix("result ") {
                    result = line
                }
            }
        }
        process.terminationHandler = { finished in
            pipe.fileHandleForReading.readabilityHandler = nil
            if scoped { url.stopAccessingSecurityScopedResource() }
            // "result <status> <message>"
            let parts = result.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: false)
            let status = parts.count >= 2 ? Int(parts[1]) ?? -1 : -1
            let text = parts.count == 3 ? String(parts[2]) : "The importer stopped unexpectedly."
            DispatchQueue.main.async {
                self.importing = false
                self.importProcess = nil
                self.failed = status != 0
                self.message = finished.terminationReason == .uncaughtSignal ? "Import cancelled." : text
                self.refresh()
            }
        }
        do {
            try process.run()
            importProcess = process
        } catch {
            if scoped { url.stopAccessingSecurityScopedResource() }
            importing = false
            failed = true
            message = "Could not start the importer: \(error.localizedDescription)"
        }
    }

    func cancelImport() { importProcess?.terminate() }

    func play() {
        let process = Process()
        if Paths.bundled {
            configureBundledGame(process)
        } else {
            process.executableURL = URL(fileURLWithPath: "/bin/zsh")
            process.arguments = ["-f", Paths.runScript.path, Paths.disc.path]
            process.currentDirectoryURL = Paths.home.appendingPathComponent("runtime")
        }
        var environment = process.environment ?? ProcessInfo.processInfo.environment
        environment["RRV_FUKAMI_INI"] = Paths.settingsINI.path
        environment["MAGATAMA_LAUNCHER_SETTINGS"] = "1"
        process.environment = environment
        process.terminationHandler = { finished in
            DispatchQueue.main.async {
                self.running = false
                self.gameProcess = nil
                // 75: the game asked to be restarted (the menu's Restart, settings that apply at launch).
                if Paths.bundled && finished.terminationStatus == 75 { self.play() }
            }
        }
        do {
            try process.run()
            gameProcess = process
            running = true
            message = ""
        } catch {
            failed = true
            message = "Could not start the game: \(error.localizedDescription)"
        }
    }

    func buildIOS() {
        guard !Paths.bundled, !buildingIOS else { return }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/zsh")
        process.arguments = ["-f", Paths.iosBuildScript.path]
        process.currentDirectoryURL = Paths.home.appendingPathComponent("runtime")
        var environment = ProcessInfo.processInfo.environment
        // Finder launches do not normally inherit Homebrew's bin directory.
        let path = environment["PATH"] ?? "/usr/bin:/bin:/usr/sbin:/sbin"
        environment["PATH"] = path + ":/opt/homebrew/bin:/usr/local/bin"
        environment["MAGATAMA_IOS_REQUEST_FILE"] = "/dev/null"
        environment["MAGATAMA_IOS_SIM"] = "0"
        environment["MAGATAMA_IOS_LEGACY"] = "0"
        environment.removeValue(forKey: "MAGATAMA_IOS_BUILD_NO")
        process.environment = environment

        iosBuildFailed = false
        do {
            try Data().write(to: Paths.iosBuildLog)
            let log = try FileHandle(forWritingTo: Paths.iosBuildLog)
            process.standardOutput = log
            process.standardError = log
            process.terminationHandler = { finished in
                try? log.close()
                DispatchQueue.main.async {
                    self.iosBuildProcess = nil
                    self.buildingIOS = false
                    self.iosBuildFailed = finished.terminationStatus != 0
                    self.iosBuildMessage = self.iosBuildFailed
                        ? "iOS build failed. Open the build log for details."
                        : "Unsigned iOS build ready."
                }
            }
            try process.run()
            iosBuildProcess = process
            buildingIOS = true
            iosBuildMessage = "Building an unsigned iOS app…"
        } catch {
            iosBuildFailed = true
            iosBuildMessage = "Could not start the iOS build. \(error.localizedDescription)"
        }
    }

    // The game inside Magatama.app, with the environment scripts/magatama_run.sh gives it; its output goes to
    // logs/run-<time>.log.
    private func configureBundledGame(_ process: Process) {
        let home = Paths.home
        let fm = FileManager.default
        for dir in ["memcard", "logs"] {
            try? fm.createDirectory(at: home.appendingPathComponent(dir), withIntermediateDirectories: true)
        }
        let contents = Bundle.main.bundleURL.appendingPathComponent("Contents")
        var env = ProcessInfo.processInfo.environment
        let defaults: [String: String] = [
            "RRV_GATE3_BOUND_WORKLOAD": contents.appendingPathComponent("Resources/workload/bound-workload.txt").path,
            "RRV_GATE3_MC_ROOT": home.appendingPathComponent("memcard").path,
            "RRV_GATE3_MC_PERSISTENT": "1",
            "RRV_GATE3_LIVE_PAD": "1",
            "RRV_GIF_PATH_LATENCY": "0",
            "RRV_GATE3_REALTIME": "1",
            "MAGATAMA_SILENT_SOUND": "1",
            "MAGATAMA_SOUND": "1",
            "MAGATAMA_SOUND_NATIVE": "1", // the game's own IOP sound modules (music, effects, movie audio)
            "MAGATAMA_MAC_QOS": "1",
            "MAGATAMA_DISC": Paths.disc.path,
            "RRV_GS_CONTROL": "producer",
            "RRV_PCSX2_GS_VSYNC": "1",
            "RRV_GS_EXECUTION": "worker-sync",
            "RRV_VU1GS_EXECUTION": "owner-async",
            "MTL_SHADER_DEPTH_WORKAROUND": "0",
            "MAGATAMA_IPU": "1",
            "MAGATAMA_IPU_WORKER_TIMING": "1",
            "RRV_VU_AOT": "1",
            "MAGATAMA_RESTART_LAUNCHER": "1",
            // The game binary has Fukami's app front end built in (src/app/fukami_app.mm); this launcher is the
            // front end here, so the game starts as the child it starts.
            "RRV_FUKAMI_CHILD": "1",
            // The PCSX2 bridge's libraries are in Contents/Frameworks.
            "DYLD_LIBRARY_PATH": contents.appendingPathComponent("Frameworks").path,
        ]
        for (key, value) in defaults where env[key] == nil { env[key] = value }
        process.environment = env
        process.executableURL = Paths.bundledGame
        process.arguments = [Paths.disc.appendingPathComponent("SLUS_209.11").path]
        process.currentDirectoryURL = home
        let stamp = DateFormatter()
        stamp.dateFormat = "yyyyMMdd-HHmmss"
        let log = home.appendingPathComponent("logs/run-\(stamp.string(from: Date())).log")
        fm.createFile(atPath: log.path, contents: nil)
        if let handle = try? FileHandle(forWritingTo: log) {
            process.standardOutput = handle
            process.standardError = handle
        }
    }
}

struct LauncherView: View {
    @ObservedObject var model: LauncherModel
    @State private var picking = false
    @State private var showingSettings = false

    private var imageTypes: [UTType] {
        ["iso", "chd", "zso", "bin", "cue"].compactMap { UTType(filenameExtension: $0) } + [.diskImage]
    }

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.05, green: 0.05, blue: 0.09), Color(red: 0.12, green: 0.08, blue: 0.16)],
                           startPoint: .top, endPoint: .bottom)
            VStack(spacing: 18) {
                Spacer()
                MagatamaLogo(height: 56)
                Text("Shin Megami Tensei: Nocturne")
                    .font(.title3)
                    .foregroundColor(.white.opacity(0.7))
                Spacer()
                status
                if model.importing {
                    VStack(spacing: 8) {
                        ProgressView(value: model.progress)
                            .progressViewStyle(.linear)
                            .tint(.purple)
                            .frame(maxWidth: 380)
                        Text("\(Int(model.progress * 100))%  \(model.currentFile)")
                            .font(.footnote.monospacedDigit())
                            .foregroundColor(.white.opacity(0.7))
                            .lineLimit(1)
                        Button("Cancel") { model.cancelImport() }
                    }
                } else {
                    HStack(spacing: 14) {
                        Button {
                            picking = true
                        } label: {
                            Label(model.installed ? "Replace Disc…" : "Import Disc…", systemImage: "opticaldisc")
                                .frame(minWidth: 150)
                        }
                        .disabled(model.running)
                        Button {
                            model.play()
                        } label: {
                            Label(model.running ? "Running…" : "Play", systemImage: "play.fill")
                                .frame(minWidth: 150)
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(.purple)
                        .keyboardShortcut(.defaultAction)
                        .disabled(!model.installed || model.running)
                    }
                    .controlSize(.large)
                }
                if !Paths.bundled && FileManager.default.isReadableFile(atPath: Paths.iosBuildScript.path) {
                    iosBuildControls
                }
                Button {
                    showingSettings = true
                } label: {
                    Label("Recomp Settings…", systemImage: "gearshape")
                }
                .keyboardShortcut(",", modifiers: .command)
                Text("Your own Nocturne (USA) disc image: .iso, .chd or .zso")
                    .font(.footnote)
                    .foregroundColor(.white.opacity(0.45))
                Spacer().frame(height: 18)
            }
            .padding(.horizontal, 24)
        }
        .frame(width: 560, height: Paths.bundled ? 410 : 470)
        .sheet(isPresented: $showingSettings) {
            RecompSettingsView(path: Paths.settingsINI.path)
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: imageTypes) { result in
            if case .success(let url) = result { model.importImage(url) }
        }
    }

    private var iosBuildControls: some View {
        VStack(spacing: 8) {
            HStack(spacing: 12) {
                if model.buildingIOS {
                    ProgressView().controlSize(.small)
                }
                Button("Build for iOS") { model.buildIOS() }
                    .disabled(model.buildingIOS)
                Button("Open iOS Builds") { NSWorkspace.shared.open(Paths.iosBuilds) }
                    .disabled(!FileManager.default.fileExists(atPath: Paths.iosBuilds.path))
                Button("Open Build Log") { NSWorkspace.shared.open(Paths.iosBuildLog) }
                    .disabled(!FileManager.default.fileExists(atPath: Paths.iosBuildLog.path))
            }
            if !model.iosBuildMessage.isEmpty {
                Text(model.iosBuildMessage)
                    .font(.callout)
                    .foregroundColor(model.iosBuildFailed ? .red.opacity(0.9) : .white.opacity(0.7))
                    .multilineTextAlignment(.center)
            }
        }
    }

    @ViewBuilder private var status: some View {
        if !model.message.isEmpty {
            Text(model.message)
                .font(.callout)
                .foregroundColor(model.failed ? .red.opacity(0.9) : .green.opacity(0.9))
                .multilineTextAlignment(.center)
        } else {
            Text(model.running ? "Magatama is running" : model.installed ? "Disc ready" : "No disc imported yet")
                .font(.callout)
                .foregroundColor(model.installed ? .green.opacity(0.9) : .white.opacity(0.6))
        }
    }
}

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
            Text("Live settings apply during gameplay. Settings marked Next launch take effect when the game starts again.")
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
            .formStyle(.grouped)
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
        .frame(width: 540, height: 580)
        .task {
            if let data = MagatamaReadSettings(path),
               let rows = try? JSONDecoder().decode([Setting].self, from: data) {
                settings = rows
            } else {
                error = "Could not read the settings file."
            }
        }
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

@main
struct MagatamaLauncherApp: App {
    @StateObject private var model = LauncherModel()
    var body: some Scene {
        WindowGroup("Magatama") {
            LauncherView(model: model)
        }
        .windowResizability(.contentSize)
    }
}

/// The Magatama wordmark (assets/branding/magatama-wordmark.png, copied into Contents/Resources), or the name in
/// text when the image is missing (a development build run outside its bundle).
struct MagatamaLogo: View {
    var height: CGFloat
    var body: some View {
        if let url = Bundle.main.url(forResource: "MagatamaWordmark", withExtension: "png"),
           let image = NSImage(contentsOf: url) {
            Image(nsImage: image).resizable().interpolation(.high).aspectRatio(contentMode: .fit).frame(height: height)
        } else {
            Text("MAGATAMA").font(.system(size: height * 0.85, weight: .heavy, design: .serif)).tracking(6)
                .foregroundColor(.white)
        }
    }
}
