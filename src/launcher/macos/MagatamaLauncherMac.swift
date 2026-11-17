// Magatama launcher for macOS (SwiftUI). Imports the user's own disc image (.iso, .chd or .zso) into ~/magatama/disc
// with the bundled magatama-disc-import tool (src/app/magatama_disc_import_tool.cpp), and starts the game the way
// scripts/magatama_run.sh does. Built by scripts/magatama_build_launcher_mac.sh into "Magatama Launcher.app".
import AppKit
import SwiftUI
import UniformTypeIdentifiers

// Where things are. The game runs from the Magatama build tree (~/magatama/runtime) until it ships as an app bundle;
// MAGATAMA_HOME overrides ~/magatama.
enum Paths {
    static var home: URL {
        if let custom = ProcessInfo.processInfo.environment["MAGATAMA_HOME"], !custom.isEmpty {
            return URL(fileURLWithPath: custom)
        }
        return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("magatama")
    }
    static var disc: URL { home.appendingPathComponent("disc") }
    static var runScript: URL { home.appendingPathComponent("runtime/scripts/magatama_run.sh") }
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
        guard FileManager.default.isExecutableFile(atPath: "/bin/zsh") else { return }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/zsh")
        process.arguments = ["-f", Paths.runScript.path, Paths.disc.path]
        process.currentDirectoryURL = Paths.home.appendingPathComponent("runtime")
        process.terminationHandler = { _ in
            DispatchQueue.main.async {
                self.running = false
                self.gameProcess = nil
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
}

struct LauncherView: View {
    @ObservedObject var model: LauncherModel
    @State private var picking = false

    private var imageTypes: [UTType] {
        ["iso", "chd", "zso", "bin", "cue"].compactMap { UTType(filenameExtension: $0) } + [.diskImage]
    }

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.05, green: 0.05, blue: 0.09), Color(red: 0.12, green: 0.08, blue: 0.16)],
                           startPoint: .top, endPoint: .bottom)
            VStack(spacing: 18) {
                Spacer()
                Text("MAGATAMA")
                    .font(.system(size: 46, weight: .heavy, design: .serif))
                    .tracking(7)
                    .foregroundColor(.white)
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
                Text("Your own Nocturne (USA) disc image: .iso, .chd or .zso")
                    .font(.footnote)
                    .foregroundColor(.white.opacity(0.45))
                Spacer().frame(height: 18)
            }
            .padding(.horizontal, 24)
        }
        .frame(width: 560, height: 380)
        .fileImporter(isPresented: $picking, allowedContentTypes: imageTypes) { result in
            if case .success(let url) = result { model.importImage(url) }
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
