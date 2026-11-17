// Magatama launcher for iPhone and iPad: shown before the game starts. Imports the user's own disc image (.iso,
// .chd or .zso) into Documents/disc through the C importer (src/app/magatama_disc_import.h, the bridging header),
// then starts the game.
//
// magatama_ios.mm calls magatama_ios_run_launcher() from SDL's app start-up, before SDL creates its window; this
// shows its own window and runs the main run loop until Play is pressed.
import SwiftUI
import UIKit
import UniformTypeIdentifiers

final class LauncherModel: ObservableObject {
    let discDir: String
    @Published var installed = false
    @Published var importing = false
    @Published var progress: Double = 0
    @Published var currentFile = ""
    @Published var message = ""
    @Published var failed = false
    var play = false
    private var cancelRequested = false

    init(discDir: String) {
        self.discDir = discDir
        refresh()
    }

    func refresh() {
        let dir = discDir
        DispatchQueue.global(qos: .userInitiated).async {
            let ok = magatama_disc_installed(dir) == 1
            DispatchQueue.main.async { self.installed = ok }
        }
    }

    func cancel() { cancelRequested = true }

    func importImage(_ url: URL) {
        importing = true
        failed = false
        message = ""
        progress = 0
        currentFile = url.lastPathComponent
        cancelRequested = false
        let scoped = url.startAccessingSecurityScopedResource()
        let dir = discDir
        DispatchQueue.global(qos: .userInitiated).async {
            var text = [CChar](repeating: 0, count: 512)
            let context = Unmanaged.passUnretained(self).toOpaque()
            let status = magatama_disc_import(url.path, dir, { done, total, file, context in
                let model = Unmanaged<LauncherModel>.fromOpaque(context!).takeUnretainedValue()
                let fraction = total > 0 ? Double(done) / Double(total) : 0
                let name = file.map { String(cString: $0) } ?? ""
                DispatchQueue.main.async {
                    model.progress = fraction
                    if !name.isEmpty { model.currentFile = name }
                }
                return model.cancelRequested ? 0 : 1
            }, context, &text, text.count)
            if scoped { url.stopAccessingSecurityScopedResource() }
            let result = String(cString: text)
            DispatchQueue.main.async {
                self.importing = false
                self.failed = status != 0
                self.message = result
                self.refresh()
            }
        }
    }
}

// The system file picker for disc images.
struct DiscPicker: UIViewControllerRepresentable {
    let onPick: (URL) -> Void

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        var types: [UTType] = []
        for ext in ["iso", "chd", "zso", "bin", "cue"] {
            if let type = UTType(filenameExtension: ext) { types.append(type) }
        }
        types.append(.diskImage)
        types.append(.data)
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: types, asCopy: false)
        picker.allowsMultipleSelection = false
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ controller: UIDocumentPickerViewController, context: Context) {}
    func makeCoordinator() -> Coordinator { Coordinator(onPick: onPick) }

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let onPick: (URL) -> Void
        init(onPick: @escaping (URL) -> Void) { self.onPick = onPick }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            if let url = urls.first { onPick(url) }
        }
    }
}

struct LauncherView: View {
    @ObservedObject var model: LauncherModel
    let onPlay: () -> Void
    @State private var picking = false

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.05, green: 0.05, blue: 0.09), Color(red: 0.12, green: 0.08, blue: 0.16)],
                           startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()
            VStack(spacing: 22) {
                Spacer()
                Text("MAGATAMA")
                    .font(.system(size: 54, weight: .heavy, design: .serif))
                    .tracking(8)
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
                            .frame(maxWidth: 420)
                        Text("\(Int(model.progress * 100))%  \(model.currentFile)")
                            .font(.footnote.monospacedDigit())
                            .foregroundColor(.white.opacity(0.7))
                            .lineLimit(1)
                        Button("Cancel") { model.cancel() }
                            .foregroundColor(.white.opacity(0.8))
                    }
                } else {
                    HStack(spacing: 16) {
                        Button {
                            picking = true
                        } label: {
                            Label(model.installed ? "Replace Disc" : "Import Disc", systemImage: "opticaldisc")
                                .frame(minWidth: 170)
                        }
                        .buttonStyle(.bordered)
                        .tint(.white)
                        Button {
                            onPlay()
                        } label: {
                            Label("Play", systemImage: "play.fill")
                                .frame(minWidth: 170)
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(.purple)
                        .disabled(!model.installed)
                    }
                    .controlSize(.large)
                }
                Text("Your own Nocturne (USA) disc image: .iso, .chd or .zso")
                    .font(.footnote)
                    .foregroundColor(.white.opacity(0.45))
                Spacer().frame(height: 24)
            }
            .padding(.horizontal, 24)
        }
        .sheet(isPresented: $picking) {
            DiscPicker { url in
                picking = false
                model.importImage(url)
            }
            .ignoresSafeArea()
        }
    }

    @ViewBuilder private var status: some View {
        if !model.message.isEmpty {
            Text(model.message)
                .font(.callout)
                .foregroundColor(model.failed ? .red.opacity(0.9) : .green.opacity(0.9))
                .multilineTextAlignment(.center)
        } else {
            Text(model.installed ? "Disc ready" : "No disc imported yet")
                .font(.callout)
                .foregroundColor(model.installed ? .green.opacity(0.9) : .white.opacity(0.6))
        }
    }
}

// Landscape only, like the game (RULE: CONTRIBUTING.md, "Hard rules": Magatama is always horizontal on iOS).
final class LauncherHostingController: UIHostingController<LauncherView> {
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .landscape }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
}

@_cdecl("magatama_ios_run_launcher")
public func magatamaIosRunLauncher(_ discDir: UnsafePointer<CChar>) -> Int32 {
    let model = LauncherModel(discDir: String(cString: discDir))
    let window = UIWindow(frame: UIScreen.main.bounds)
    window.windowLevel = .normal + 1
    window.rootViewController = LauncherHostingController(rootView: LauncherView(model: model) { model.play = true })
    window.makeKeyAndVisible()
    while !model.play {
        _ = RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05))
    }
    window.isHidden = true
    window.rootViewController = nil
    return 1
}
