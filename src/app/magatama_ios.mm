// Magatama for iPhone: the app entry point. The game itself is the unchanged product main() (main_product.cpp,
// renamed magatama_game_main on iOS) run inside UIKit through SDL_UIKitRunApp.
//
// Files (visible in the Files app under "On My iPhone > Magatama", UIFileSharingEnabled):
//   Documents/disc/SLUS_209.11 + the rest of the disc's files   (copy your own disc here)
//   Documents/memcard/                                          save card folder
//   Documents/magatama.ini                                      settings (the in-game menu writes it)
//   Documents/logs/run-<time>.log                               this run's log (stderr)
// The environment matches scripts/magatama_run.sh's defaults; an explicit variable wins.
#include "fukami_app.h"
#include "fukami_settings.h"

// This file owns main(); SDL must not rename it to SDL_main.
#define SDL_MAIN_HANDLED 1
#include <SDL.h>
#include <SDL_main.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <mach/mach.h>
#include <pthread.h>
#include <thread>
#include <chrono>
#include <map>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <exception>
#include <csignal>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>

int magatama_game_main(int argc, char **argv);

namespace {
namespace fs = std::filesystem;

fs::path documents()
{
    NSArray<NSString *> *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return paths.count ? fs::path([paths.firstObject fileSystemRepresentation]) : fs::path("/tmp");
}
fs::path bundle() { return fs::path([[[NSBundle mainBundle] bundlePath] fileSystemRepresentation]); }

void def(const char *name, const std::string &value) { setenv(name, value.c_str(), 0); }

std::string g_elf;

// Documents/magatama-env.txt: NAME=value lines (blank lines and # comments ignored) applied before the defaults
// below, so they win. For trying runtime options on a device without a new build. A commented template is
// written on first launch.
void loadEnvFile(const fs::path &docs)
{
    const fs::path file = docs / "magatama-env.txt";
    std::error_code ec;
    if (!fs::exists(file, ec))
    {
        if (FILE *f = std::fopen(file.c_str(), "w"))
        {
            std::fputs("# Magatama runtime options, read at launch. Remove the # in front of a line to use it;\n"
                       "# put # back (or delete the line) to return to the default. Force-quit and reopen to apply.\n"
                       "#\n"
                       "# Extra software-render helper threads in the graphics library (default 3):\n"
                       "#RRV_PCSX2_GS_SW_THREADS=1\n"
                       "#\n"
                       "# VU1 and graphics on one thread instead of two (slower; default off):\n"
                       "#RRV_VU1GS_SPLIT=0\n"
                       "#\n"
                       "# Let the game build the next frame while the last one is still drawing (experimental; default off):\n"
                       "#MAGATAMA_FINISH_RUNAHEAD=1\n"
                       "#\n"
                       "# What the Metal HUD shows (Apple's metric names, comma-separated; the default adds thermal):\n"
                       "#MTL_HUD_ELEMENTS=device,fps,frameinterval,gputime,frameintervalgraph\n",
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
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#')
            continue;
        line.erase(0, start);
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            std::fprintf(stderr, "[ios] magatama-env.txt: ignoring \"%s\" (want NAME=value)\n", line.c_str());
            continue;
        }
        const std::string name = line.substr(0, eq), value = line.substr(eq + 1);
        setenv(name.c_str(), value.c_str(), 1);
        std::fprintf(stderr, "[ios] magatama-env.txt: %s=%s\n", name.c_str(), value.c_str());
    }
    std::fclose(f);
}

// Crash report into the log: signal, fault address, thread, the backtrace with each frame's image and offset, and
// where the app's own images were loaded (for symbolication against the build). A C++ exception that escapes
// prints its message. Then the default action runs, so iOS still records its own crash report.
void writeLoadedImages()
{
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; ++i)
    {
        const char *name = _dyld_get_image_name(i);
        if (name && (std::strstr(name, "Magatama.app/") || i == 0))
            std::fprintf(stderr, "[crash]   image %s base=%p slide=0x%lx\n", name, (void *)_dyld_get_image_header(i),
                         (unsigned long)_dyld_get_image_vmaddr_slide(i));
    }
}
void writeBacktrace()
{
    void *frames[64];
    const int n = backtrace(frames, 64);
    for (int i = 0; i < n; ++i)
    {
        Dl_info info{};
        if (dladdr(frames[i], &info) && info.dli_fbase)
            std::fprintf(stderr, "[crash]   #%d %s+0x%lx %s\n", i, std::strrchr(info.dli_fname, '/') ? std::strrchr(info.dli_fname, '/') + 1 : info.dli_fname,
                         (unsigned long)((char *)frames[i] - (char *)info.dli_fbase), info.dli_sname ? info.dli_sname : "");
        else
            std::fprintf(stderr, "[crash]   #%d %p\n", i, frames[i]);
    }
}
void crashSignal(int sig, siginfo_t *info, void *context)
{
    char name[64] = {};
    pthread_getname_np(pthread_self(), name, sizeof name);
    uint64_t pc = 0, lr = 0;
    if (auto *uc = static_cast<ucontext_t *>(context); uc && uc->uc_mcontext)
    {
        pc = __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
        lr = __darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
    }
    std::fprintf(stderr, "[crash] signal %d (%s) addr=%p pc=0x%llx lr=0x%llx thread=%s\n", sig, strsignal(sig),
                 info ? info->si_addr : nullptr, (unsigned long long)pc, (unsigned long long)lr, name[0] ? name : "?");
    for (const uint64_t a : {pc, lr})
    {
        Dl_info d{};
        if (a && dladdr((void *)a, &d) && d.dli_fbase)
            std::fprintf(stderr, "[crash]   %s at %s+0x%llx %s\n", a == pc ? "pc" : "lr",
                         std::strrchr(d.dli_fname, '/') ? std::strrchr(d.dli_fname, '/') + 1 : d.dli_fname,
                         (unsigned long long)(a - (uint64_t)d.dli_fbase), d.dli_sname ? d.dli_sname : "");
    }
    writeBacktrace();
    writeLoadedImages();
    std::fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}
void installCrashReport()
{
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof altStack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = crashSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT})
        sigaction(sig, &sa, nullptr);
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

// Every 5 s: CPU use of each thread (percent of one core) into the log, busiest first, so a slow device shows
// which thread is the bottleneck. Mach thread info only; nothing in the game is touched.
void startThreadCpuLog()
{
    std::thread([] {
        pthread_setname_np("magatama-cpulog");
        std::map<uint64_t, double> last; // thread id -> cpu seconds
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            thread_act_array_t threads = nullptr;
            mach_msg_type_number_t count = 0;
            if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
                continue;
            struct Row { double pct; std::string name; };
            std::vector<Row> rows;
            double total = 0;
            std::map<uint64_t, double> now;
            for (mach_msg_type_number_t i = 0; i < count; ++i)
            {
                thread_identifier_info_data_t id{};
                mach_msg_type_number_t n = THREAD_IDENTIFIER_INFO_COUNT;
                thread_basic_info_data_t basic{};
                mach_msg_type_number_t m = THREAD_BASIC_INFO_COUNT;
                if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &n) == KERN_SUCCESS &&
                    thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&basic, &m) == KERN_SUCCESS)
                {
                    const double cpu = basic.user_time.seconds + basic.user_time.microseconds * 1e-6 +
                                       basic.system_time.seconds + basic.system_time.microseconds * 1e-6;
                    now[id.thread_id] = cpu;
                    auto it = last.find(id.thread_id);
                    const double pct = it == last.end() ? 0 : (cpu - it->second) / 5.0 * 100.0;
                    total += pct;
                    char name[64] = {};
                    if (pthread_t p = pthread_from_mach_thread_np(threads[i]))
                        pthread_getname_np(p, name, sizeof name);
                    if (pct >= 3)
                        rows.push_back({pct, name[0] ? name : ("tid" + std::to_string(id.thread_id))});
                }
                mach_port_deallocate(mach_task_self(), threads[i]);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_act_t));
            last = std::move(now);
            std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.pct > b.pct; });
            std::string line;
            for (const Row &r : rows)
                line += " " + r.name + "=" + std::to_string(int(r.pct + 0.5)) + "%";
            std::fprintf(stderr, "[ios-cpu] total=%d%% threads=%u%s\n", int(total + 0.5), count, line.c_str());
        }
    }).detach();
}

#if defined(MAGATAMA_IOS_LAUNCHER)
extern "C" int magatama_ios_run_launcher(const char *discDir, const char *settingsPath); // src/launcher/ios/MagatamaLauncher.swift
#endif

int gameMain(int, char **)
{
#if defined(MAGATAMA_IOS_LAUNCHER)
    // The launcher (import the disc image, then Play) before the game's window exists.
    magatama_ios_run_launcher(fs::path(g_elf).parent_path().c_str(), fukami::app::iniPath().c_str());
#endif
    // Read after the launcher has saved its edits and before the game creates Metal or VU/GS workers.
    namespace settings = fukami::settings;
    const auto doc = settings::IniDocument::load(fukami::app::iniPath()).value_or(settings::IniDocument{});
    const auto defaults = settings::IniDocument::load(fukami::app::defaultIniPath()).value_or(settings::IniDocument{});
    const auto effective = settings::withDefaults(doc, defaults);
    const auto hud = settings::value(effective, *settings::find("rendering", "metal_hud"));
    def("MTL_HUD_ENABLED", settings::boolValue(hud) ? "1" : "0");
    if (std::strcmp(std::getenv("MTL_HUD_ENABLED"), "1") == 0)
        def("MTL_HUD_ELEMENTS", "device,layersize,memory,thermal,fps,frameinterval,gputime,presentdelay,frameintervalgraph");
    const auto split = settings::value(effective, *settings::find("timing", "split_gs"));
    def("RRV_VU1GS_SPLIT", settings::boolValue(split) ? "1" : "0");
    [UIApplication sharedApplication].idleTimerDisabled = YES;
    startThreadCpuLog();
    std::fprintf(stderr, "[ios] low power mode=%d thermal state=%ld\n",
                 int([NSProcessInfo processInfo].lowPowerModeEnabled), long([NSProcessInfo processInfo].thermalState));
    const fs::path elf = g_elf;
    std::error_code ec;
    if (!fs::is_regular_file(elf, ec))
    {
        std::fprintf(stderr, "[ios] no game ELF at %s: import a disc image in the launcher\n", elf.c_str());
        const std::string text = "Import your own Shin Megami Tensei: Nocturne (USA) disc image (.iso, .chd or .zso) "
                                 "in the launcher, or copy the disc's files into the Magatama folder in the Files app "
                                 "as disc/SLUS_209.11 and the rest, then open Magatama again.\n\nLooked for: " +
                                 elf.string();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Magatama needs your disc", text.c_str(), nullptr);
        return 1;
    }
    std::string exe = (bundle() / "Magatama").string();
    char *argv[] = {exe.data(), g_elf.data(), nullptr};
    const int status = magatama_game_main(2, argv);
    // The game has ended (Quit in the menu): close the app rather than leave it on its last frame.
    std::fprintf(stderr, "[ios] game ended (%d): closing the app\n", status);
    std::fflush(nullptr);
    _exit(status);
}
} // namespace

namespace fukami::app {
std::filesystem::path defaultIniPath()
{
    const fs::path defaults = bundle() / "magatama-default.ini";
    std::error_code ec;
    return fs::is_regular_file(defaults, ec) ? defaults : fs::path{};
}
void restart() {} // iOS apps cannot relaunch themselves; settings apply on the next launch
void reveal(const std::filesystem::path &) {}
} // namespace fukami::app

int main(int argc, char **argv)
{
    @autoreleasepool
    {
        const fs::path docs = documents();
        std::error_code ec;
        for (const char *dir : {"disc", "memcard", "logs"})
            fs::create_directories(docs / dir, ec);
        // PCSX2 (inside the bridge) keeps its data in ~/Library/Application Support/PCSX2 and only accepts a folder
        // that already exists; a new iOS app has no Application Support folder yet.
        if (const char *home = std::getenv("HOME"))
            fs::create_directories(fs::path(home) / "Library/Application Support/PCSX2/inis", ec);
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
        const fs::path log = docs / "logs" / (std::string("run-") + stamp + ".log");
        std::freopen(log.c_str(), "w", stderr);
        std::setvbuf(stderr, nullptr, _IOLBF, 0);
        installCrashReport();
        NSString *build = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"];
        std::fprintf(stderr, "[ios] Magatama build %s starting; documents=%s\n", build ? build.UTF8String : "?",
                     docs.c_str());

        loadEnvFile(docs);
        const fs::path disc = docs / "disc";
        g_elf = (disc / "SLUS_209.11").string();
        def("MAGATAMA_DISC", disc.string());
        def("RRV_GATE3_BOUND_WORKLOAD", (bundle() / "bound-workload.txt").string());
        def("RRV_GATE3_MC_ROOT", (docs / "memcard").string());
        def("RRV_GATE3_MC_PERSISTENT", "1");
        def("RRV_GATE3_LIVE_PAD", "1");
        def("RRV_GIF_PATH_LATENCY", "0");
        def("RRV_GATE3_REALTIME", "1");
        def("MAGATAMA_SILENT_SOUND", "1");
        def("MAGATAMA_SOUND", "1");
        // The original IOP sound modules on the emulated IOP (src/audio/nocturne_native_sound.cpp): music, effects and
        // movie audio, as the Mac is run (MAGATAMA_SOUND_NATIVE=1). The default host driver leaves FMVs silent.
        def("MAGATAMA_SOUND_NATIVE", "1");
        // Game, GS and VU worker threads at user-interactive QoS: iOS keeps those on the performance cores
        // (src/gs-backend/rrv_mac_qos.h; the Mac launcher sets the same).
        def("MAGATAMA_MAC_QOS", "1");
        def("RRV_GS_CONTROL", "producer");
        def("RRV_PCSX2_GS_VSYNC", "1");
        def("RRV_GS_EXECUTION", "worker-sync");
        def("RRV_VU1GS_EXECUTION", "owner-async");
        def("RRV_FUKAMI_INI", (docs / "magatama.ini").string());
        def("MTL_SHADER_DEPTH_WORKAROUND", "0");
        def("MAGATAMA_IPU", "1");
        def("MAGATAMA_IPU_WORKER_TIMING", "1");
        def("RRV_VU_AOT", "1");
        def("RRV_WINDOW_FULLSCREEN", "1");
        // The picture renders at the screen's resolution (Resolution scale in the in-game menu, 25-200%); this is
        // only the renderer's first scale, until the first frame is shown.
#if defined(MAGATAMA_IOS_LEGACY)
        // Older devices (A10-class, 2 GB): half the screen's resolution unless the player chose otherwise.
        def("RRV_PCSX2_GS_FULL_SCALE", "1");
        def("MAGATAMA_RESOLUTION_SCALE_DEFAULT", "50");
#else
        def("RRV_PCSX2_GS_FULL_SCALE", "2");
#endif
        // RULE: landscape only on iPhone and iPad (CONTRIBUTING.md, "Hard rules"; Info.plist.in lists the same two).
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
        SDL_SetMainReady();
        return SDL_UIKitRunApp(argc, argv, gameMain);
    }
}
