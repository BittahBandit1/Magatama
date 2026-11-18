// Magatama for Android: the app entry point. SDL3's Java activity (org.libsdl.app.SDLActivity, packaged by
// scripts/magatama_build_android.sh) loads libSDL3.so, libSDL2.so (sdl2-compat) and this library (libmain.so), then
// calls SDL_main on its game thread. The game itself is the unchanged product main() (main_product.cpp, renamed
// magatama_game_main on Android).
//
// Files, in Documents/Magatama on the device's shared storage (reachable over USB and with any file manager; the
// app asks for "All files access" once, since Android only lets an app read files other apps put there with it):
//   disc/SLUS_209.11 + the rest of the disc's files   (copy your own disc here)
//   memcard/                                          save card folder (memcard/mc0/BASLUS-20911... as on the Mac)
//   magatama.ini                                      settings (the in-game menu writes it)
//   magatama-env.txt                                  optional NAME=value runtime options (template written once)
//   logs/run-<time>.log                               this run's log (stderr)
// The resource package (PCSX2's shaders and fonts) is extracted from the APK's assets to the internal files folder
// (<files>/game/package) when the APK changes; the GS bridge is one of the APK's native libraries.
#include "fukami_app.h"

#include <SDL.h>

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <unistd.h>
#include <exception>
#include <filesystem>
#include <string>
#include <unwind.h>
#include <vector>

int magatama_game_main(int argc, char **argv);

namespace {
namespace fs = std::filesystem;

fs::path g_external; // Documents/Magatama
fs::path g_internal; // /data/data/<package>/files
std::string g_elf;

void def(const char *name, const std::string &value) { setenv(name, value.c_str(), 0); }

void logcat(const char *text) { __android_log_write(ANDROID_LOG_INFO, "Magatama", text); }

// --- Crash report into the log (as on iOS): signal, fault address, backtrace with each frame's library + offset.
struct Trace { void **frames; int count, max; };
_Unwind_Reason_Code unwindFrame(_Unwind_Context *context, void *arg)
{
    auto *trace = static_cast<Trace *>(arg);
    if (const uintptr_t pc = _Unwind_GetIP(context); pc && trace->count < trace->max)
        trace->frames[trace->count++] = reinterpret_cast<void *>(pc);
    return trace->count < trace->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}
void writeBacktrace()
{
    void *frames[48];
    Trace trace{frames, 0, 48};
    _Unwind_Backtrace(unwindFrame, &trace);
    for (int i = 0; i < trace.count; ++i)
    {
        Dl_info info{};
        if (dladdr(frames[i], &info) && info.dli_fbase)
        {
            const char *name = std::strrchr(info.dli_fname, '/');
            std::fprintf(stderr, "[crash]   #%d %s+0x%lx %s\n", i, name ? name + 1 : info.dli_fname,
                         (unsigned long)((char *)frames[i] - (char *)info.dli_fbase), info.dli_sname ? info.dli_sname : "");
        }
        else
            std::fprintf(stderr, "[crash]   #%d %p\n", i, frames[i]);
    }
}
struct sigaction g_previous[32];
void crashSignal(int sig, siginfo_t *info, void *context)
{
    std::fprintf(stderr, "[crash] signal %d (%s) addr=%p\n", sig, strsignal(sig), info ? info->si_addr : nullptr);
    writeBacktrace();
    std::fflush(stderr);
    // Hand over to the previous handler (Android's debuggerd), so a tombstone is written too.
    sigaction(sig, &g_previous[sig], nullptr);
    raise(sig);
}
void installCrashReport()
{
    struct sigaction sa{};
    sa.sa_sigaction = crashSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT})
        sigaction(sig, &sa, &g_previous[sig]);
    std::set_terminate([] {
        std::string what = "unknown";
        if (auto e = std::current_exception())
        {
            try { std::rethrow_exception(e); }
            catch (const std::exception &x) { what = x.what(); }
            catch (...) {}
        }
        std::fprintf(stderr, "[crash] uncaught C++ exception: %s\n", what.c_str());
        writeBacktrace();
        std::fflush(stderr);
        std::abort();
    });
}

// --- The resource package from the APK's assets. assets/game/package.list holds one relative path per line (files
// only; folders are implied) and, on the first line, "# <build id>"; it is copied whenever that id changes.
bool readAsset(const char *name, std::vector<char> &out)
{
    SDL_RWops *rw = SDL_RWFromFile(name, "rb"); // SDL reads relative paths from the APK's assets
    if (!rw)
        return false;
    const Sint64 size = SDL_RWsize(rw);
    out.resize(size > 0 ? size_t(size) : 0);
    const bool ok = size >= 0 && (size == 0 || SDL_RWread(rw, out.data(), 1, out.size()) == out.size());
    SDL_RWclose(rw);
    return ok;
}

bool extractPackage(const fs::path &game)
{
    std::vector<char> list;
    if (!readAsset("game/package.list", list))
    {
        std::fprintf(stderr, "[android] the APK has no game/package.list asset\n");
        return false;
    }
    const std::string text(list.begin(), list.end());
    const std::string id = text.substr(0, text.find('\n'));
    std::error_code ec;
    const fs::path stamp = game / "package.id";
    if (FILE *f = std::fopen(stamp.c_str(), "r"))
    {
        char current[256] = {};
        std::fgets(current, sizeof current, f);
        std::fclose(f);
        if (id == current && fs::exists(game / "package" / "identity", ec))
            return true;
    }
    std::fprintf(stderr, "[android] extracting the resource package (%s)\n", id.c_str());
    fs::remove_all(game / "package", ec);
    size_t count = 0;
    for (size_t at = text.find('\n'); at != std::string::npos && at + 1 < text.size();)
    {
        const size_t end = text.find('\n', at + 1);
        const std::string relative = text.substr(at + 1, (end == std::string::npos ? text.size() : end) - at - 1);
        at = end;
        if (relative.empty())
            continue;
        std::vector<char> data;
        if (!readAsset(("game/package/" + relative).c_str(), data))
        {
            std::fprintf(stderr, "[android] missing asset game/package/%s\n", relative.c_str());
            return false;
        }
        const fs::path target = game / "package" / relative;
        fs::create_directories(target.parent_path(), ec);
        FILE *out = std::fopen(target.c_str(), "wb");
        if (!out || (!data.empty() && std::fwrite(data.data(), 1, data.size(), out) != data.size()))
        {
            if (out)
                std::fclose(out);
            std::fprintf(stderr, "[android] cannot write %s: %s\n", target.c_str(), std::strerror(errno));
            return false;
        }
        std::fclose(out);
        ++count;
    }
    if (FILE *f = std::fopen(stamp.c_str(), "w"))
    {
        std::fputs(id.c_str(), f);
        std::fclose(f);
    }
    std::fprintf(stderr, "[android] extracted %zu package files\n", count);
    return true;
}

// Same file format as iOS (src/app/magatama_ios.mm loadEnvFile).
void loadEnvFile(const fs::path &file)
{
    std::error_code ec;
    if (!fs::exists(file, ec))
    {
        if (FILE *f = std::fopen(file.c_str(), "w"))
        {
            std::fputs("# Magatama runtime options, read at launch. Remove the # in front of a line to use it;\n"
                       "# put # back (or delete the line) to return to the default. Close and reopen the app to apply.\n"
                       "#\n"
                       "# Resolution scale, % of the screen's resolution, 25-200 (default: the in-game menu's, 100):\n"
                       "#MAGATAMA_RESOLUTION_SCALE=75\n"
                       "#\n"
                       "# Extra software-render helper threads in the graphics library (default 3):\n"
                       "#RRV_PCSX2_GS_SW_THREADS=1\n"
                       "#\n"
                       "# VU1 and graphics on two separate threads instead of one (default off):\n"
                       "#RRV_VU1GS_SPLIT=1\n"
                       "#\n"
                       "# Hide the FPS / frame-time overlay:\n"
                       "#RRV_PCSX2_GS_PERF_OSD=0\n",
                       f);
            std::fclose(f);
        }
        return;
    }
    FILE *f = std::fopen(file.c_str(), "r");
    if (!f)
        return;
    char buffer[1024];
    while (std::fgets(buffer, sizeof buffer, f))
    {
        std::string line(buffer);
        while (!line.empty() && std::strchr("\r\n \t", line.back()))
            line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#')
            continue;
        line.erase(0, start);
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1);
        std::fprintf(stderr, "[android] magatama-env.txt: %s\n", line.c_str());
    }
    std::fclose(f);
}

void showMessage(const char *title, const std::string &text)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, text.c_str(), nullptr);
}

// dev.magatama.MagatamaActivity (src/app/android) helpers, called on the activity object: FindClass from this native
// thread would not see the app's classes.
struct Activity
{
    JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    jobject self = static_cast<jobject>(SDL_AndroidGetActivity());
    jmethodID method(const char *name, const char *signature)
    {
        if (!env || !self)
            return nullptr;
        jclass cls = env->GetObjectClass(self);
        jmethodID id = env->GetMethodID(cls, name, signature);
        env->DeleteLocalRef(cls);
        if (env->ExceptionCheck())
        {
            env->ExceptionClear();
            return nullptr;
        }
        return id;
    }
    bool hasStorageAccess()
    {
        jmethodID id = method("magatamaHasStorageAccess", "()Z");
        return id && env->CallBooleanMethod(self, id);
    }
    void requestStorageAccess()
    {
        if (jmethodID id = method("magatamaRequestStorageAccess", "()V"))
            env->CallVoidMethod(self, id);
    }
    std::string documentsPath()
    {
        std::string path = "/storage/emulated/0/Documents";
        if (jmethodID id = method("magatamaDocumentsPath", "()Ljava/lang/String;"))
            if (auto text = static_cast<jstring>(env->CallObjectMethod(self, id)))
            {
                const char *chars = env->GetStringUTFChars(text, nullptr);
                path = chars;
                env->ReleaseStringUTFChars(text, chars);
                env->DeleteLocalRef(text);
            }
        return path;
    }
    float displayRefreshRate()
    {
        jmethodID id = method("magatamaDisplayRefreshRate", "()F");
        return id ? env->CallFloatMethod(self, id) : 0.0f;
    }
    ~Activity()
    {
        if (env && self)
            env->DeleteLocalRef(self);
    }
};

// Until the user grants access: explain once, open the system setting, then wait for the app to come back with it.
bool ensureStorageAccess(Activity &activity)
{
    if (activity.hasStorageAccess())
        return true;
    logcat("asking for storage access");
    showMessage("Magatama needs storage access",
                "Magatama keeps your disc, saves and settings in the Documents/Magatama folder, so you can copy "
                "files there over USB or with a file manager.\n\nOn the next screen, turn on \"Allow access to manage "
                "all files\" for Magatama, then come back to the app.");
    activity.requestStorageAccess();
    for (int waited = 0; waited < 10 * 60 * 10; ++waited) // up to 10 minutes
    {
        SDL_Delay(100);
        SDL_PumpEvents();
        if (activity.hasStorageAccess())
            return true;
    }
    return false;
}
} // namespace

namespace fukami::app {
std::filesystem::path defaultIniPath() { return {}; }
void restart() {} // Android: close and reopen the app; settings apply on the next launch
void reveal(const std::filesystem::path &) {}
} // namespace fukami::app

// Surface lifecycle (MagatamaActivity.GameSurface, UI thread): forwarded to the GS bridge, which presents on its own
// thread and must let go of the window before Android destroys it (tools/patches/android/bridge-android-surface.diff).
namespace {
void *bridgeSymbol(const char *name)
{
    void *bridge = dlopen("librrv-pcsx2-gs-bridge.so", RTLD_NOW | RTLD_NOLOAD);
    void *symbol = bridge ? dlsym(bridge, name) : nullptr;
    if (bridge)
        dlclose(bridge); // RTLD_NOLOAD took a reference; the game's own handle keeps it loaded
    return symbol;
}
} // namespace

extern "C" JNIEXPORT void JNICALL Java_dev_magatama_MagatamaActivity_nativeSurfaceLost(JNIEnv *, jclass)
{
    using Fn = void (*)();
    if (auto fn = reinterpret_cast<Fn>(bridgeSymbol("rrv_pcsx2_gs_bridge_android_surface_lost")))
        fn();
}

extern "C" JNIEXPORT void JNICALL Java_dev_magatama_MagatamaActivity_nativeSurfaceReady(JNIEnv *env, jclass,
                                                                                    jobject surface, jint width,
                                                                                    jint height)
{
    using Fn = void (*)(void *, uint32_t, uint32_t);
    auto fn = reinterpret_cast<Fn>(bridgeSymbol("rrv_pcsx2_gs_bridge_android_surface_ready"));
    if (!fn || !surface || width <= 0 || height <= 0)
        return;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window)
        return;
    fn(window, static_cast<uint32_t>(width), static_cast<uint32_t>(height)); // the bridge takes its own reference
    ANativeWindow_release(window);
}

extern "C" __attribute__((visibility("default"))) int SDL_main(int, char **)
{
    const char *internal = SDL_AndroidGetInternalStoragePath();
    if (!internal)
    {
        logcat("no app storage folder");
        showMessage("Magatama", std::string("Android gave no app storage folder: ") + SDL_GetError());
        return 1;
    }
    g_internal = internal;
    Activity activity;
    if (!ensureStorageAccess(activity))
    {
        showMessage("Magatama", "Without storage access Magatama cannot read your disc. Open Magatama again to "
                                "grant it.");
        return 1;
    }
    g_external = fs::path(activity.documentsPath()) / "Magatama";
    const std::string external = g_external.string();
    std::error_code ec;
    for (const char *dir : {"disc", "memcard", "logs"})
        fs::create_directories(g_external / dir, ec);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    const fs::path log = g_external / "logs" / (std::string("run-") + stamp + ".log");
    if (!std::freopen(log.c_str(), "w", stderr))
        logcat("cannot open the log file");
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    // PCSX2's console (inside the bridge) writes straight to file descriptors 1 and 2, which an Android app has
    // pointed at /dev/null: point both at the log as well.
    if (const int fd = fileno(stderr); fd >= 0)
    {
        if (fd != STDERR_FILENO)
            dup2(fd, STDERR_FILENO);
        dup2(fd, STDOUT_FILENO);
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    installCrashReport();
    std::fprintf(stderr, "[android] Magatama build %s starting; files=%s\n", MAGATAMA_ANDROID_BUILD, external.c_str());

    // PCSX2 (inside the bridge) keeps its settings under $XDG_CONFIG_HOME/PCSX2 and only accepts a folder that
    // already exists; an Android process has no HOME.
    setenv("HOME", internal, 0);
    setenv("XDG_CONFIG_HOME", (g_internal / "config").c_str(), 0);
    fs::create_directories(g_internal / "config" / "PCSX2" / "inis", ec);

    const fs::path game = g_internal / "game";
    fs::create_directories(game, ec);
    if (!extractPackage(game))
    {
        showMessage("Magatama", "The app's resource package could not be prepared. See the log in "
                                "Android/data/<app>/files/logs.");
        return 1;
    }
    setenv("MAGATAMA_ANDROID_GAME", game.c_str(), 1);

    loadEnvFile(g_external / "magatama-env.txt");
    const fs::path disc = g_external / "disc";
    g_elf = (disc / "SLUS_209.11").string();
    def("MAGATAMA_DISC", disc.string());
    def("RRV_GATE3_BOUND_WORKLOAD", (game / "package" / "bound-workload.txt").string());
    def("RRV_GATE3_MC_ROOT", (g_external / "memcard").string());
    def("RRV_GATE3_MC_PERSISTENT", "1");
    def("RRV_GATE3_LIVE_PAD", "1");
    def("RRV_GIF_PATH_LATENCY", "0");
    def("RRV_GATE3_REALTIME", "1");
    {
        // A 60, 120 or 240 Hz screen (most Android devices): the game runs at exactly 60.00 so its frames stay on the
        // display's grid (gate3_temporal_runtime.inc, RRV_GATE3_DISPLAY_RATE_60). Measured on the Nova at 120 Hz: at
        // 59.94 one frame in every 2-4 s stayed up a refresh longer. Other rates (90, 144 Hz) keep 59.94.
        const float hz = activity.displayRefreshRate();
        const float multiple = hz / 60.0f;
        const bool even = hz > 0.0f && std::fabs(multiple - std::round(multiple)) < 0.01f && std::round(multiple) >= 1.0f;
        std::fprintf(stderr, "[android] display %.2f Hz%s\n", hz, even ? ": the game runs at 60.00" : "");
        if (even)
            def("RRV_GATE3_DISPLAY_RATE_60", "1");
    }
    def("MAGATAMA_SILENT_SOUND", "1");
    def("MAGATAMA_SOUND", "1");
    def("MAGATAMA_SOUND_NATIVE", "1"); // the original IOP sound modules: music, effects and movie audio
    def("RRV_GS_CONTROL", "producer");
    def("RRV_PCSX2_GS_VSYNC", "1");
    def("RRV_GS_EXECUTION", "worker-sync");
    def("RRV_VU1GS_EXECUTION", "owner-async");
    // MAGATAMA_PERF_HINT=1: ADPF clock hints for the game/VU1/GS threads (rrv_mac_qos.h). Off: no measurable gain on
    // the Nova (60-step soak: same 98.7% of frames on time with and without).
    def("RRV_VU1GS_SPLIT", "1"); // VU1 on its own thread: frees the GS owner (Oni spot: 107 -> 111 fields/2 s)
    def("RRV_FUKAMI_INI", (g_external / "magatama.ini").string());
    def("MAGATAMA_IPU", "1");
    def("MAGATAMA_IPU_WORKER_TIMING", "1");
    def("RRV_VU_AOT", "1");
    def("RRV_WINDOW_FULLSCREEN", "1");
    def("RRV_PCSX2_GS_FULL_SCALE", "2");
    def("RRV_PCSX2_GS_CONSOLE", "1"); // PCSX2's own messages in the log (Android bring-up)
    def("MAGATAMA_PERF_CORES", "1"); // game/GS/VU threads on the performance cores (src/gs-backend/rrv_mac_qos.h)
    def("RRV_PAD_MERGE_KEYBOARD", "1"); // keyboard keys (ADB, a Bluetooth keyboard) work beside the built-in controls
    def("RRV_PCSX2_GS_PERF_OSD", "1"); // FPS / frame-time overlay (the Metal HUD's role on Apple devices)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // Back button: the in-game menu (Escape), not "leave the app".
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

    if (!fs::is_regular_file(g_elf, ec))
    {
        std::fprintf(stderr, "[android] no game ELF at %s: copy the disc files into the disc folder\n", g_elf.c_str());
        showMessage("Magatama needs your disc",
                    "Copy your own Shin Megami Tensei: Nocturne (USA) disc files (SLUS_209.11 and the rest) into\n\n" +
                        disc.string() + "\n\nthen open Magatama again.");
        return 1;
    }
    std::string exe = "libmain.so";
    char *argv[] = {exe.data(), g_elf.data(), nullptr};
    const int status = magatama_game_main(2, argv);
    std::fprintf(stderr, "[android] game exited with status %d\n", status);
    std::fflush(stderr);
    if (status != 0)
        showMessage("Magatama stopped", "Magatama could not keep running (status " + std::to_string(status) +
                                            "). The log is in Documents/Magatama/logs.");
    // Worker threads (CPU log, audio, GS) would keep a returned SDL_main's process alive: end it here.
    std::_Exit(status);
}
