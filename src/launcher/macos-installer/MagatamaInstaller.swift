// Magatama Installer for macOS (SwiftUI). It ships no game code: drop your own Shin Megami Tensei: Nocturne (USA)
// disc image on the window and it builds Magatama.app on this Mac from that disc, with Apple's command-line
// compiler and the prebuilt runtime in Contents/Resources/kit (magatama_install.py does the work; this is its window).
// Built by scripts/magatama_macos_release.sh.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

final class InstallerModel: ObservableObject {
    enum Phase { case checking, needsTools, ready, working, finished, failed }
    @Published var phase: Phase = .checking
    @Published var step = 0
    @Published var stepName = ""
    @Published var progress: Double = 0
    @Published var detail = ""
    @Published var message = ""
    @Published var appURL: URL?
    private var process: Process?
    static let steps = ["Import the disc", "Translate the game code", "Compile", "Link", "Put Magatama.app together"]

    init() { checkTools() }

    // Apple's Command Line Tools (or Xcode) give the compiler, the linker and python3.
    func checkTools() {
        phase = .checking
        DispatchQueue.global().async {
            let ok = Self.succeeds("/usr/bin/xcode-select", ["-p"]) && Self.succeeds("/usr/bin/xcrun", ["--find", "clang++"])
            DispatchQueue.main.async { self.phase = ok ? .ready : .needsTools }
        }
    }

    func installTools() {
        _ = Self.succeeds("/usr/bin/xcode-select", ["--install"])
        message = "Follow the Command Line Tools installer, then click Check Again."
    }

    private static func succeeds(_ tool: String, _ arguments: [String]) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = arguments
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    func install(_ image: URL) {
        guard phase == .ready || phase == .failed || phase == .finished else { return }
        guard let kit = Bundle.main.resourceURL?.appendingPathComponent("kit"),
              let script = Bundle.main.resourceURL?.appendingPathComponent("magatama_install.py") else { return }
        phase = .working
        step = 0
        stepName = Self.steps[0]
        progress = 0
        detail = image.lastPathComponent
        message = ""
        let scoped = image.startAccessingSecurityScopedResource()
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        p.arguments = [script.path, "--kit", kit.path, "--image", image.path]
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        var pending = ""
        pipe.fileHandleForReading.readabilityHandler = { handle in
            pending += String(decoding: handle.availableData, as: UTF8.self)
            while let newline = pending.firstIndex(of: "\n") {
                let line = String(pending[..<newline])
                pending = String(pending[pending.index(after: newline)...])
                DispatchQueue.main.async { self.handle(line) }
            }
        }
        p.terminationHandler = { finished in
            pipe.fileHandleForReading.readabilityHandler = nil
            if scoped { image.stopAccessingSecurityScopedResource() }
            DispatchQueue.main.async {
                self.process = nil
                if self.appURL != nil && finished.terminationStatus == 0 {
                    self.phase = .finished
                } else {
                    self.phase = .failed
                    if self.message.isEmpty {
                        self.message = finished.terminationReason == .uncaughtSignal ? "Cancelled." : "The build stopped. See install.log."
                    }
                }
            }
        }
        do {
            try p.run()
            process = p
        } catch {
            phase = .failed
            message = "Could not start the build: \(error.localizedDescription)"
        }
    }

    private func handle(_ line: String) {
        let parts = line.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: false).map(String.init)
        guard let kind = parts.first else { return }
        switch kind {
        case "step":
            if parts.count >= 2, let n = Int(parts[1]) {
                step = n
                stepName = parts.count == 3 ? parts[2] : ""
                progress = 0
                detail = ""
            }
        case "progress":
            if parts.count >= 3, let done = Double(parts[1]) {
                let rest = parts[2].split(separator: " ", maxSplits: 1).map(String.init)
                if let total = Double(rest[0]), total > 0 { progress = done / total }
                detail = rest.count > 1 ? rest[1] : ""
            }
        case "log":
            detail = parts.dropFirst().joined(separator: " ")
        case "done":
            appURL = URL(fileURLWithPath: parts.dropFirst().joined(separator: " "))
        case "error":
            message = parts.dropFirst().joined(separator: " ")
        default:
            message += (message.isEmpty ? "" : "\n") + line // continuation of an error
        }
    }

    func cancel() { process?.terminate() }

    func openApp() {
        if let app = appURL { NSWorkspace.shared.openApplication(at: app, configuration: .init()) }
    }

    func revealLog() {
        let log = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/Magatama/install.log")
        NSWorkspace.shared.activateFileViewerSelecting([log])
    }
}

struct InstallerView: View {
    @ObservedObject var model: InstallerModel
    @State private var picking = false
    @State private var targeted = false
    private var imageTypes: [UTType] {
        ["iso", "chd", "zso", "bin"].compactMap { UTType(filenameExtension: $0) } + [.diskImage]
    }

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.05, green: 0.05, blue: 0.09), Color(red: 0.12, green: 0.08, blue: 0.16)],
                           startPoint: .top, endPoint: .bottom)
            VStack(spacing: 16) {
                MagatamaLogo(height: 46)
                    .padding(.top, 24)
                Text("Installer")
                    .font(.title3)
                    .foregroundColor(.white.opacity(0.7))
                Spacer(minLength: 8)
                content
                Spacer(minLength: 8)
                Text("Builds Magatama.app on this Mac from your own Shin Megami Tensei: Nocturne (USA) disc image. "
                     + "No game code is downloaded.")
                    .font(.footnote)
                    .foregroundColor(.white.opacity(0.45))
                    .multilineTextAlignment(.center)
                    .padding(.bottom, 18)
            }
            .padding(.horizontal, 28)
        }
        .frame(width: 600, height: 460)
        .fileImporter(isPresented: $picking, allowedContentTypes: imageTypes) { result in
            if case .success(let url) = result { model.install(url) }
        }
        .onDrop(of: [.fileURL], isTargeted: $targeted) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                if let url { DispatchQueue.main.async { model.install(url) } }
            }
            return true
        }
    }

    @ViewBuilder private var content: some View {
        switch model.phase {
        case .checking:
            ProgressView().controlSize(.large)
        case .needsTools:
            VStack(spacing: 12) {
                Text("Magatama is compiled on your Mac, which needs Apple's Command Line Tools (free, about 1 GB).")
                    .foregroundColor(.white.opacity(0.85))
                    .multilineTextAlignment(.center)
                HStack {
                    Button("Install Command Line Tools") { model.installTools() }
                        .buttonStyle(.borderedProminent).tint(.purple)
                    Button("Check Again") { model.checkTools() }
                }
                .controlSize(.large)
                if !model.message.isEmpty {
                    Text(model.message).font(.callout).foregroundColor(.white.opacity(0.6))
                }
            }
        case .ready, .failed:
            VStack(spacing: 12) {
                dropZone
                if model.phase == .failed {
                    Text(model.message)
                        .font(.callout.monospaced())
                        .foregroundColor(.red.opacity(0.9))
                        .lineLimit(8)
                        .multilineTextAlignment(.leading)
                        .textSelection(.enabled)
                    Button("Show install.log") { model.revealLog() }
                }
            }
        case .working:
            VStack(alignment: .leading, spacing: 10) {
                ForEach(Array(InstallerModel.steps.enumerated()), id: \.offset) { index, name in
                    HStack(spacing: 8) {
                        Image(systemName: index < model.step ? "checkmark.circle.fill" :
                              index == model.step ? "circle.dotted" : "circle")
                            .foregroundColor(index < model.step ? .green : .white.opacity(index == model.step ? 0.9 : 0.35))
                        Text(name).foregroundColor(.white.opacity(index <= model.step ? 0.9 : 0.4))
                    }
                }
                ProgressView(value: model.progress)
                    .progressViewStyle(.linear)
                    .tint(.purple)
                    .padding(.top, 6)
                Text(model.detail)
                    .font(.footnote.monospacedDigit())
                    .foregroundColor(.white.opacity(0.6))
                    .lineLimit(1)
                HStack {
                    Spacer()
                    Button("Cancel") { model.cancel() }
                }
            }
            .frame(maxWidth: 420)
        case .finished:
            VStack(spacing: 12) {
                Image(systemName: "checkmark.seal.fill").font(.system(size: 40)).foregroundColor(.green)
                Text("Magatama is ready in your Applications folder.")
                    .foregroundColor(.white.opacity(0.9))
                Button("Open Magatama") { model.openApp() }
                    .buttonStyle(.borderedProminent).tint(.purple).controlSize(.large)
            }
        }
    }

    private var dropZone: some View {
        VStack(spacing: 10) {
            Image(systemName: "opticaldisc").font(.system(size: 34)).foregroundColor(.white.opacity(0.8))
            Text("Drop your disc image here (.iso, .chd or .zso)").foregroundColor(.white.opacity(0.85))
            Button("Choose Disc Image…") { picking = true }
                .buttonStyle(.borderedProminent).tint(.purple).controlSize(.large)
        }
        .frame(maxWidth: 440, minHeight: 150)
        .background(RoundedRectangle(cornerRadius: 14).strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [7]))
            .foregroundColor(targeted ? .purple : .white.opacity(0.35)))
    }
}

@main
struct MagatamaInstallerApp: App {
    @StateObject private var model = InstallerModel()
    var body: some Scene {
        WindowGroup("Magatama Installer") {
            InstallerView(model: model)
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
