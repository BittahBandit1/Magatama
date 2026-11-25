// Fukami in-game menu; see fukami_menu.h.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "fukami_menu.h"

#include "fukami_app.h"
#include "fukami_settings.h"
#include "../gs-backend/rrv_nocturne_aspect.h"
#include "../host/rrv_audio_output.h"
#include <cstring>

// The bridge ABI header is plain C with no PCSX2 types; the host only uses its
// UI table and key codes (the bridge itself is loaded at run time).
#include "../../tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
#if defined(__APPLE__)
#define RRV_FILE_MANAGER_NAME "Finder"
#elif defined(_WIN32)
#define RRV_FILE_MANAGER_NAME "Explorer"
#else
#define RRV_FILE_MANAGER_NAME "the file manager"
#endif


namespace fukami::menu {
namespace {

std::string id(std::string_view section, std::string_view key)
{
    return std::string(section) + "." + std::string(key);
}

// [timing] holds the host speed switches the menu shows (split_gs, pacer_spin); its other keys are ini only.
// "experimental" is not an ini section: it is the page of the specs marked experimental, whatever their section.
constexpr std::string_view kSections[] = {"audio", "display", "rendering", "input", "game", "timing", "experimental"};

const char *sectionTitle(std::string_view section)
{
    if (section == "audio") return "Audio";
    if (section == "display") return "Display";
    if (section == "rendering") return "Rendering";
    if (section == "input") return "Input";
    if (section == "game") return "Game";
    if (section == "timing") return "Performance";
    if (section == "experimental") return "Experimental";
    return "Other";
}

bool availableInMenu(const settings::Spec& spec)
{
    // The Terminal supervisor always keeps the diagnostic log. This switch
    // controls the packaged app's session folder only.
    return spec.inMenu && !(spec.section=="game" && spec.key=="logging" && !app::inApp());
}

std::string choiceName(const settings::Spec &spec, std::string_view choice)
{
    if (spec.key == "nocturne_aspect" && choice == "auto")
        return "Automatic (window)";
    if (spec.key == "aspect" && choice.empty())
        return "Same as Widescreen";
    if (choice.empty())
        return "Automatic";
    if (spec.key == "aniso")
        return choice == "0" ? std::string("Off") : std::string(choice) + "x";
    if (spec.key == "texture_filter")
        return choice == "ps2" ? "PS2 (stock)" : "Bilinear";
    if (spec.key == "sprite_filter")
        return choice == "ps2" ? "PS2 (stock)" : "Even";
    if (spec.key == "render_mode")
        return choice == "full" ? "Full frames" : "Fields";
    std::string name(choice);
    if (name[0] >= 'a' && name[0] <= 'z')
        name[0] = static_cast<char>(name[0] - 'a' + 'A');
    return name;
}

long toLong(const std::string &value, long fallback)
{
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end && end != value.c_str() && *end == '\0' ? parsed : fallback;
}

double toDouble(const std::string &value, double fallback)
{
    char *end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end && end != value.c_str() && *end == '\0' ? parsed : fallback;
}

// One decimal, without a trailing ".0" (car_lod: [0-9]+([.][0-9]+)?).
std::string formatNumber(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", std::round(value * 10.0) / 10.0);
    std::string out(text);
    if (out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0)
        out.resize(out.size() - 2);
    return out;
}

bool isBackKey(const Input &input)
{
    return input.type == Input::Type::key && input.down &&
           (input.code == RRV_PCSX2_GS_UI_KEY_ESCAPE || input.code == RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_RIGHT);
}

} // namespace

bool appliesLive(std::string_view section, std::string_view key)
{
    // Host/window controls and serialized bridge renderer updates. No live
    // setting requests a game restart or changes original guest timing.
    return (section == "display" && (key == "fullscreen" || key == "aspect" || key == "integer_scaling" || key == "nocturne_aspect")) ||
           (section == "audio" && (key == "volume" || key == "mute")) ||
           (section == "input" && key == "touch_controls") ||
           (section == "rendering" && (key == "fxaa" || key == "cas" || key == "metal_hud" || key == "scale" ||
                                         key == "resolution_scale" || key == "aa1" || key == "aniso" ||
                                         key == "texture_filter" || key == "sprite_filter" || key == "mipmap"));
}

Menu::Menu(std::filesystem::path ini, const std::filesystem::path &defaults) :
    m_ini(std::move(ini)), m_saves(app::savesFolder()), m_logs(app::logsFolder())
{
    std::optional<settings::IniDocument> doc;
    if (!m_ini.empty())
        doc = settings::IniDocument::load(m_ini);
    if (!m_ini.empty() && !defaults.empty())
        m_defaults = settings::IniDocument::load(defaults).value_or(settings::IniDocument{});
    const settings::IniDocument shown = settings::withDefaults(doc ? *doc : settings::IniDocument{}, m_defaults);
    for (const settings::Spec &spec : settings::schema())
        m_launch[id(spec.section, spec.key)] = settings::value(shown, spec);
    // The Metal HUD is on at launch exactly when the process was started with it (MTL_HUD_ENABLED=1).
    {
        const char *hud = std::getenv("MTL_HUD_ENABLED");
        m_launch[id("rendering", "metal_hud")] = (hud && hud[0] == '1') ? "true" : "false";
    }
    // The on-screen controller follows MAGATAMA_TOUCH_CONTROLS (on unless "0").
    {
        const char *touch = std::getenv("MAGATAMA_TOUCH_CONTROLS");
        m_launch[id("input", "touch_controls")] = (touch && touch[0] == '0') ? "false" : "true";
    }
    // Magatama Terminal defaults intentionally differ from legacy RR5/app
    // schema defaults. Show the resolved running controls, not a fictional
    // default (e.g. split=true/mipmap=false when the runtime uses the reverse).
    if (std::getenv("MAGATAMA_DISC")) {
        const auto c=settings::LaunchConfig::fromEnvironment([](const char* name){return std::getenv(name);});
        const auto boolean=[&](const char* section,const char* key,bool v){m_launch[id(section,key)]=v?"true":"false";};
        boolean("display","fullscreen",c.fullscreen);
        boolean("display","integer_scaling",c.integerScaling);
        boolean("display","present_pacing",c.presentPacing);
        m_launch[id("rendering","render_mode")]=c.renderMode;
        boolean("rendering","aa1",c.aa1);boolean("rendering","fxaa",c.fxaa);
        m_launch[id("rendering","cas")]=std::to_string(c.cas);
        m_launch[id("rendering","aniso")]=std::to_string(c.aniso);
        m_launch[id("rendering","texture_filter")]=c.textureFilter;
        m_launch[id("rendering","sprite_filter")]=c.spriteFilter;
        boolean("rendering","mipmap",c.mipmap);
        boolean("input","analog",c.analog);boolean("input","rumble",c.rumble);
        boolean("timing","split_gs",c.splitGs);
        m_launch[id("timing","pacer_spin")]=std::to_string(c.pacerSpinMs);
    }
    // Show the actual launch overrides, including Terminal's 8x default.
    if (const char* scale = std::getenv("RRV_PCSX2_GS_FULL_SCALE"); scale && *scale)
        m_launch[id("rendering", "scale")] = scale;
    // MAGATAMA_RESOLUTION_SCALE overrides the saved percentage for this run; MAGATAMA_RESOLUTION_SCALE_DEFAULT is a
    // platform's default when the ini has none (legacy iPhones).
    if (const char* forced = std::getenv("MAGATAMA_RESOLUTION_SCALE"); forced && *forced)
        m_launch[id("rendering", "resolution_scale")] = forced;
    else if (const char* platform = std::getenv("MAGATAMA_RESOLUTION_SCALE_DEFAULT");
             platform && *platform && !(doc && doc->get("rendering", "resolution_scale")))
        m_launch[id("rendering", "resolution_scale")] = platform;
    if (const char* aspect = std::getenv("MAGATAMA_ASPECT"); aspect && *aspect)
        m_launch[id("display", "nocturne_aspect")] = std::string(aspect) == "off" ? "4:3" : aspect;
    m_values = m_launch;
#if defined(__APPLE__) && TARGET_OS_OSX
    m_followLauncherSettings = !m_ini.empty() && std::getenv("MAGATAMA_LAUNCHER_SETTINGS");
    if (m_followLauncherSettings)
    {
        if (doc)
            m_lastIni = *doc;
        // The first poll also catches a write made during startup.
        m_iniModified = std::filesystem::file_time_type::min();
    }
#endif
    rrv::audio::setOutputVolume(static_cast<int>(toLong(m_values[id("audio", "volume")], 100)));
    rrv::audio::setOutputMuted(settings::boolValue(m_values[id("audio", "mute")]));
    // The picture fit that an empty `aspect` stands for is the ratio this
    // process was launched with (run_gate4_product.sh: aspect defaults to it;
    // RRV_RR5_WIDESCREEN is set only for ratios other than 4:3).
    const char *ratio = std::getenv("RRV_RR5_WIDESCREEN");
    m_launchRatio = ratio && *ratio ? ratio : "4:3";
    if (!m_ini.empty() && !doc)
        m_status = "Could not read the settings file.";
}

void Menu::open()
{
    if (available() && !m_open.exchange(true, std::memory_order_acq_rel))
        std::fprintf(stderr, "[fukami-menu] open\n");
}

void Menu::close()
{
    if (m_open.exchange(false, std::memory_order_acq_rel))
        std::fprintf(stderr, "[fukami-menu] closed\n");
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_inputs.clear();
}

void Menu::post(const Input &input)
{
    if (!isOpen())
        return;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    // A stalled GS owner must not grow this without bound; keep the newest.
    if (m_inputs.size() >= 256)
        m_inputs.pop_front();
    m_inputs.push_back(input);
}

void Menu::fullscreenChanged(bool fullscreen, bool persist)
{
    m_fullscreen.store(fullscreen, std::memory_order_release);
    if (!persist || m_ini.empty())
        return;
    std::lock_guard<std::mutex> lock(m_saveMutex);
    if (!settings::updateIni(m_ini, {{"display", "fullscreen", fullscreen ? "true" : "false"}}))
        m_status = "Could not save the settings file.";
}

void Menu::frameCallback(void *menu, const RrvPcsx2GsBridgeUi *ui)
{
    if (!menu || !ui || ui->struct_size < offsetof(RrvPcsx2GsBridgeUi, set_render_option))
        return;
    // Never unwind into the bridge (built without exceptions).
    try
    {
        RrvPcsx2GsBridgeUi supported{};
        std::memcpy(&supported, ui, std::min<size_t>(ui->struct_size, sizeof(supported)));
        static_cast<Menu *>(menu)->frame(supported);
    }
    catch (...)
    {
        std::fprintf(stderr, "[fukami-menu] frame failed; menu closed\n");
        static_cast<Menu *>(menu)->close();
    }
}

void Menu::reload()
{
    if (m_ini.empty())
        return; // nothing to read; keep this session's values
    const auto doc = settings::IniDocument::load(m_ini);
    if (!doc)
    {
        setStatus("Could not read the settings file.");
        return;
    }
    // What the next launch would use: a setting the file does not have follows the app's default ini.
    const settings::IniDocument shown = settings::withDefaults(*doc, m_defaults);
    for (const settings::Spec &spec : settings::schema())
    {
        const auto key = id(spec.section, spec.key);
        // Missing controls follow this process's effective defaults. A saved
        // choice remains visible as the requested value after menu changes.
        m_values[key] = appliesLive(spec.section,spec.key) ? m_launch[key] :
            (shown.get(spec.section,spec.key) ? settings::value(shown,spec) : m_launch[key]);
    }
}

#if defined(__APPLE__) && TARGET_OS_OSX
void Menu::followLauncherSettings(const RrvPcsx2GsBridgeUi &ui)
{
    if (!m_followLauncherSettings)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextIniCheck)
        return;
    m_nextIniCheck = now + std::chrono::milliseconds(250);
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(m_ini, ec);
    if (ec || modified == m_iniModified)
        return;
    const auto doc = settings::IniDocument::load(m_ini);
    if (!doc)
        return;
    m_iniModified = modified;
    for (const auto &spec : settings::schema())
    {
        if (!availableInMenu(spec))
            continue;
        const auto saved = doc->get(spec.section, spec.key);
        if (saved == m_lastIni.get(spec.section, spec.key) || !saved || !settings::valid(spec, *saved))
            continue;
        const auto key = id(spec.section, spec.key);
        if (key == "display.fullscreen")
        {
            const bool wanted = settings::boolValue(*saved);
            if (wanted != m_fullscreen.load(std::memory_order_acquire))
                m_fullscreenRequest.store(wanted ? 1 : 0);
        }
        else if (!appliesLive(spec.section, spec.key))
            m_values[key] = *saved;
        else if (m_launch[key] != *saved && applyLive(ui, spec, *saved))
            m_values[key] = m_launch[key] = *saved;
    }
    m_lastIni = *doc;
}
#endif

// Bounded Terminal-agent check: call the production setting handler without
// fabricating OS input, opening a menu, changing guest scripts or saving a card.
// The launcher isolates the ini for these explicitly requested test runs.
void Menu::runDiagnostic(const RrvPcsx2GsBridgeUi& ui)
{
    const char* mode=std::getenv("MAGATAMA_MENU_TEST");
    if (!mode || (std::strcmp(mode,"resolution") && std::strcmp(mode,"rendering") && std::strcmp(mode,"audit") && std::strcmp(mode,"audio"))) return;
    ++m_diagnosticFrame;
    struct Step { uint64_t frame; const char* section; const char* key; const char* value; };
    static constexpr Step resolution[] = {
        {1400,"rendering","scale","1"}, {1500,"rendering","scale","2"},
        {1600,"rendering","scale","4"}, {1700,"rendering","scale","8"},
        {1800,"rendering","scale","3"}};
    static constexpr Step rendering[] = {
        {1450,"rendering","aa1","false"}, {1650,"rendering","aa1","true"},
        {1850,"rendering","aniso","0"}, {1950,"rendering","aniso","16"},
        {2050,"rendering","texture_filter","ps2"}, {2150,"rendering","texture_filter","bilinear"},
        {2250,"rendering","sprite_filter","ps2"}, {2350,"rendering","sprite_filter","even"},
        {2450,"rendering","mipmap","true"}, {2550,"rendering","mipmap","false"},
        {2650,"display","nocturne_aspect","16:9"}, {2850,"display","nocturne_aspect","4:3"},
        {3050,"display","nocturne_aspect","21:9"}, {3150,"display","nocturne_aspect","32:9"},
        {3250,"display","nocturne_aspect","auto"},
        {3450,"rendering","fxaa","false"}, {3550,"rendering","fxaa","true"},
        {3650,"rendering","cas","50"}, {3750,"rendering","cas","0"}};
    static constexpr Step audit[] = {
        {1400,"display","present_pacing","false"}, {1500,"rendering","render_mode","field"},
        {1600,"input","analog","false"}, {1700,"input","rumble","false"},
        {1800,"timing","split_gs","true"}, {1900,"timing","pacer_spin","1"}};
    static constexpr Step audio[] = {
        {600,"audio","volume","50"}, {660,"audio","mute","true"},
        {720,"audio","volume","25"}, {780,"audio","mute","false"},
        {840,"audio","volume","0"}, {900,"audio","volume","100"}};
    const bool scales=std::strcmp(mode,"resolution")==0;
    const bool startup=std::strcmp(mode,"audit")==0;
    const bool sound=std::strcmp(mode,"audio")==0;
    const auto* begin=scales?resolution:startup?audit:sound?audio:rendering;
    const size_t count=scales?std::size(resolution):startup?std::size(audit):sound?std::size(audio):std::size(rendering);
    for (size_t i=0;i<count;++i) {
        const auto& step=begin[i];if(step.frame!=m_diagnosticFrame)continue;
        const auto* spec=settings::find(step.section,step.key);
        if(!spec)throw std::runtime_error("Missing menu diagnostic spec");
        change(ui,*spec,step.value);
        std::fprintf(stderr,"[menu-test] frame=%llu %s.%s=%s live=%d pending-restart=%d restart-request=%d quit-request=%d\n",
                     static_cast<unsigned long long>(m_diagnosticFrame),step.section,step.key,step.value,
                     appliesLive(step.section,step.key),pendingRestart(*spec),m_restartRequest.load(),m_quitRequest.load());
        if (sound)
            std::fprintf(stderr,"[menu-audio-test] volume=%d muted=%d\n", rrv::audio::outputVolume(), rrv::audio::outputMuted());
    }
}

void Menu::frame(const RrvPcsx2GsBridgeUi &ui)
{
    // Read back actual renderer state, not inferred ini/environment defaults.
    // Updates are serialized after present, before this next owner callback.
    if (ui.get_render_option) {
        for (const char* key : {"scale","aa1","aniso","texture_filter","sprite_filter","mipmap","fxaa","cas"}) {
            const char* value=ui.get_render_option(key);
            const auto* spec=settings::find("rendering",key);
            if(value && spec && settings::valid(*spec,value)) {
                m_launch[id("rendering",key)]=value;
                m_values[id("rendering",key)]=value;
            }
        }
    }
#if defined(__APPLE__) && TARGET_OS_OSX
    followLauncherSettings(ui);
#endif
    runDiagnostic(ui);
    followDisplay(ui);
    std::deque<Input> inputs;
    {
        std::lock_guard<std::mutex> lock(m_inputMutex);
        inputs.swap(m_inputs);
    }
    if (!isOpen())
    {
        if (m_idleOverlay)
            m_idleOverlay(m_idleOverlayContext);
        if (m_wasOpen)
        {
            // Release anything ImGui still holds down from the menu session.
            m_wasOpen = false;
            ui.clear_input();
        }
        return;
    }
    const bool appearing = !m_wasOpen;
    m_wasOpen = true;
    if (appearing)
    {
        ui.clear_input();
        reload();
    }
    for (const Input &input : inputs)
    {
        // Esc / B back out: close an open combo (or finish a slider) first,
        // else the menu.
        if (isBackKey(input) && !ui.consumes_cancel())
        {
            if (m_confirmation != Confirmation::none)
            {
                // Back out of the quit confirmation to the menu, not out of the menu.
                m_confirmation = Confirmation::none;
                m_pageAppearing = true;
                continue;
            }
            close();
            break;
        }
        switch (input.type)
        {
        case Input::Type::key: ui.add_key(input.code, input.down ? 1 : 0); break;
        case Input::Type::mousePosition: ui.add_mouse_position(input.x, input.y); break;
        case Input::Type::mouseButton: ui.add_mouse_button(input.code, input.down ? 1 : 0); break;
        case Input::Type::mouseWheel: ui.add_mouse_wheel(input.x, input.y); break;
        }
    }
    if (!isOpen())
    {
        m_wasOpen = false;
        m_confirmation = Confirmation::none;
        ui.clear_input();
        return;
    }
    if (appearing)
        m_confirmation = Confirmation::none;

    if (m_confirmation != Confirmation::none)
    {
        // Both actions discard unsaved progress: No starts focused.
        const bool restarting = m_confirmation == Confirmation::restart;
        const bool first = m_pageAppearing;
        m_pageAppearing = false;
        if (ui.begin_menu(restarting ? "Restart" : "Quit", first ? 1 : 0))
        {
            ui.text_wrapped("Are you sure? Unsaved progress will be lost.");
            ui.spacing();
            if (ui.button("No"))
            {
                m_confirmation = Confirmation::none;
                m_pageAppearing = true;
            }
            if (first)
                ui.default_focus();
            ui.same_line();
            if (ui.button("Yes"))
            {
                if (restarting)
                    m_restartRequest.store(true);
                else
                    m_quitRequest.store(true);
                m_confirmation = Confirmation::none;
                close();
            }
        }
        ui.end_menu();
        return;
    }
    const bool menuAppearing = appearing || m_pageAppearing;
    m_pageAppearing = false;

    if (ui.begin_menu("Magatama", menuAppearing ? 1 : 0))
    {
        if (ui.button("Resume"))
            close();
        if (menuAppearing)
            ui.default_focus();
        ui.tooltip("Back to the game (Esc, F1, Home or a PS4/PS5 touchpad click).");
        ui.same_line();
        if (ui.button("Quit"))
        {
            m_confirmation = Confirmation::quit;
            m_pageAppearing = true;
        }
        ui.tooltip("Quit the game. Your save card is kept.");
        const bool canRestart = app::canRestart();
#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
        // No Restart on iOS: an iOS app cannot relaunch itself (app::restart() does nothing there), so the button
        // could only ever be greyed out. Quit closes the app; settings that need a restart apply when it is reopened.
        ui.same_line();
        ui.begin_disabled(canRestart ? 0 : 1);
        if (ui.button("Restart") && canRestart)
        {
            m_confirmation = Confirmation::restart;
            m_pageAppearing = true;
        }
        ui.tooltip(canRestart ? "Restart the game with saved settings. Unsaved progress will be lost." :
                                "Launch with magatama_run.sh or the app to enable Restart.");
        ui.end_disabled();
#endif

        std::vector<std::string> pending;
        for (const settings::Spec &spec : settings::schema())
            if (availableInMenu(spec) && pendingRestart(spec))
                pending.emplace_back(spec.label);
        if (!pending.empty())
        {
            std::string names;
            for (const std::string &name : pending)
                names += (names.empty() ? "" : ", ") + name;
            ui.spacing();
#if defined(__APPLE__) && TARGET_OS_IPHONE
            ui.text_warning(("Quit and reopen Magatama to apply: " + names + ".").c_str());
#else
            ui.text_warning(("Restart to apply: " + names + ".").c_str());
            if (!canRestart)
                ui.text_disabled("Quit and relaunch to apply.");
#endif
        }

        int row = 0;
        for (std::string_view section : kSections)
        {
            ui.section(sectionTitle(section));
            for (const settings::Spec &spec : settings::schema())
                if (availableInMenu(spec) && (spec.experimental ? section == "experimental" : spec.section == section))
                    drawRow(ui, spec, row++);
        }
        if (!pending.empty())
            ui.text_disabled("* restart to apply");

        if (!m_saves.empty() || !m_logs.empty())
        {
            ui.section("Files");
            if (!m_saves.empty())
            {
                if (ui.button("Show saves"))
                    app::reveal(m_saves);
                ui.tooltip("Show the save card folder in " RRV_FILE_MANAGER_NAME ".");
            }
            if (!m_logs.empty())
            {
                if (!m_saves.empty())
                    ui.same_line();
                if (ui.button("Show logs"))
                    app::reveal(m_logs);
                ui.tooltip("Show this session's log folder in " RRV_FILE_MANAGER_NAME ".");
            }
        }
        ui.spacing();
        ui.separator();
        if (m_ini.empty())
            ui.text_wrapped("No settings file: changes last until you quit.");
        else
            ui.text_wrapped(("Settings: " + m_ini.string()).c_str());
        const std::string line = status();
        if (!line.empty())
            ui.text_warning(line.c_str());
    }
    ui.end_menu();
}

void Menu::drawRow(const RrvPcsx2GsBridgeUi &ui, const settings::Spec &spec, int rowId)
{
    const std::string key = id(spec.section, spec.key);
    const std::string value = spec.key == "fullscreen" && spec.section == "display"
        ? (m_fullscreen.load(std::memory_order_acquire) ? "true" : "false")
        : m_values[key];
    const bool live = appliesLive(spec.section, spec.key);
    // Without a settings file a restart-only change could never apply.
    const bool readOnly = !live && m_ini.empty();
    const std::string label = std::string(spec.label) + (pendingRestart(spec) ? " *" : "");
    std::string help(spec.help);
    if (!live)
        help += help.empty() ? "Applies after a restart." : " Applies after a restart.";

    ui.push_id(rowId);
    ui.begin_disabled(readOnly ? 1 : 0);
    switch (spec.kind)
    {
    case settings::Kind::boolean:
    {
        if (spec.section == "audio" && spec.key == "mute") {
            const bool muted = settings::boolValue(value);
            if (ui.button(muted ? "Unmute" : "Mute"))
                change(ui, spec, muted ? "false" : "true");
            break;
        }
        int checked = settings::boolValue(value) ? 1 : 0;
        if (ui.checkbox(label.c_str(), &checked))
            change(ui, spec, checked ? "true" : "false");
        break;
    }
    case settings::Kind::choice:
    {
        std::vector<std::string> names;
        std::vector<const char *> items;
        for (std::string_view choice : spec.choices)
            names.push_back(choiceName(spec, choice));
        for (const std::string &name : names)
            items.push_back(name.c_str());
        const auto found = std::find(spec.choices.begin(), spec.choices.end(), value);
        int index = found == spec.choices.end() ? 0 : static_cast<int>(found - spec.choices.begin());
        if (!items.empty() && ui.combo(label.c_str(), &index, items.data(), static_cast<int>(items.size())) &&
            index >= 0 && index < static_cast<int>(spec.choices.size()))
            change(ui, spec, std::string(spec.choices[static_cast<size_t>(index)]));
        break;
    }
    case settings::Kind::integer:
    {
        const int min = static_cast<int>(spec.minValue), max = static_cast<int>(spec.maxValue);
        int number = static_cast<int>(std::clamp<long>(toLong(value, min), min, max));
        if (spec.section == "rendering" && spec.key == "scale" && settings::steamDeck())
        {
            // Steam Deck: even scales only (docs/TESTING.md T-DECK-3X-HATCH). A list, because the menu's slider
            // has no step; an odd value from the ini shows as the even one it runs as (settings::hostScale).
            static const char *const kEven[] = {"2", "4", "6", "8"};
            int index = std::clamp(settings::hostScale(number, true) / 2 - 1, 0, 3);
            if (ui.combo(label.c_str(), &index, kEven, 4) && index >= 0 && index < 4)
                change(ui, spec, kEven[index]);
            break;
        }
        if (spec.section == "rendering" && spec.key == "resolution_scale")
        {
            // Steps of 5%; the renderer follows a moment after the slider stops (followDisplay).
            if (ui.slider_int(label.c_str(), &number, min, max))
                change(ui, spec, std::to_string(std::clamp((number + 2) / 5 * 5, min, max)));
            ui.tooltip(help.c_str());
            ui.text_disabled(m_resolutionText.c_str());
            break;
        }
        if (ui.slider_int(label.c_str(), &number, min, max))
            change(ui, spec, std::to_string(number));
        break;
    }
    case settings::Kind::number:
    {
        const float min = static_cast<float>(spec.minValue), max = static_cast<float>(spec.maxValue);
        float number = static_cast<float>(std::clamp(toDouble(value, min), spec.minValue, spec.maxValue));
        if (ui.slider_float(label.c_str(), &number, min, max, "%.1f"))
            change(ui, spec, formatNumber(number));
        break;
    }
    case settings::Kind::size:
    case settings::Kind::text:
        ui.text_disabled((label + ": " + (value.empty() ? "(automatic)" : value)).c_str());
        break;
    }
    ui.tooltip(help.c_str());
    ui.end_disabled();
    ui.pop_id();
}

void Menu::change(const RrvPcsx2GsBridgeUi &ui, const settings::Spec &spec, const std::string &value)
{
    if (!settings::valid(spec, value))
    {
        setStatus("Not a valid value for " + std::string(spec.label) + ": " + value);
        return;
    }
    const std::string key = id(spec.section, spec.key);
    if (spec.section == "display" && spec.key == "fullscreen")
    {
        // The host toggles the window on the main thread and writes the file
        // when the window actually changes (F11, the green button, the menu).
        m_fullscreenRequest.store(settings::boolValue(value) ? 1 : 0);
        return;
    }
    if (!applyLive(ui, spec, value)) {
        setStatus("Could not apply " + std::string(spec.label) + ". The current value was kept.");
        return;
    }
    m_values[key] = value;
    if (appliesLive(spec.section,spec.key)) m_launch[key] = value;
    if (save(spec.section, spec.key, value))
        std::fprintf(stderr, "[fukami-menu] saved %s=%s\n", key.c_str(), value.c_str());
}

bool Menu::applyLive(const RrvPcsx2GsBridgeUi &ui, const settings::Spec &spec, const std::string &value)
{
    std::fprintf(stderr, "[fukami-menu] applying %.*s.%.*s=%s\n", int(spec.section.size()), spec.section.data(),
                 int(spec.key.size()), spec.key.data(), value.c_str());
    if (spec.section == "audio" && spec.key == "volume")
        rrv::audio::setOutputVolume(static_cast<int>(toLong(value, 100)));
    else if (spec.section == "audio" && spec.key == "mute")
        rrv::audio::setOutputMuted(settings::boolValue(value));
    else if (spec.section == "display" && spec.key == "nocturne_aspect") {
        const char* fit = magatama::aspect::configure(value.c_str());
        return ui.set_aspect(fit ? fit : "4:3") != 0;
    }
    else if (spec.section == "display" && spec.key == "aspect")
        return ui.set_aspect(value.empty() ? m_launchRatio.c_str() : value.c_str()) != 0;
    else if (spec.section == "display" && spec.key == "integer_scaling")
        ui.set_integer_scaling(settings::boolValue(value) ? 1 : 0);
    else if (spec.section == "rendering" && spec.key == "fxaa")
        ui.set_fxaa(settings::boolValue(value) ? 1 : 0);
    else if (spec.section == "rendering" && spec.key == "cas")
        ui.set_cas(static_cast<int>(std::clamp<long>(toLong(value, 0), 0, 100)));
    else if (spec.section == "rendering" && spec.key == "metal_hud")
        m_metalHudRequest.store(settings::boolValue(value) ? 1 : 0); // the host sets it on the window's layer
    else if (spec.section == "input" && spec.key == "touch_controls")
        m_touchControlsRequest.store(settings::boolValue(value) ? 1 : 0); // the host's on-screen pad
    else if (spec.section == "rendering" && spec.key == "resolution_scale")
        return true; // followDisplay() turns it into the renderer's scale on the next frames
    else if (spec.section == "rendering" && appliesLive(spec.section, spec.key))
        return ui.set_render_option && ui.set_render_option(std::string(spec.key).c_str(), value.c_str());
    return true;
}

// The renderer follows the screen. The 2D (HUD, menus, text) always renders at the drawable's resolution (the display
// in fullscreen, the window otherwise): `scale`, a multiple of the PS2's 640x448 picture from 1x to 8x. The 3D scene
// renders at that times resolution_scale: `scene_scale` (the bridge's GSRrvSetSceneScale; "0" at 100%). A bridge
// without scene_scale gets the whole picture at the scene's scale. Runs every frame; a new value is applied once it
// has held for a third of a second (a resized window, a slider being dragged), as each change rebuilds the renderer's
// targets.
void Menu::followDisplay(const RrvPcsx2GsBridgeUi &ui)
{
    if (!ui.set_render_option || ui.display_width < 1.0f || ui.display_height < 1.0f)
        return;
    if (const char *test = std::getenv("MAGATAMA_MENU_TEST"); test && std::strcmp(test, "resolution") == 0)
        return; // that check sets the scale itself
    if (const char *follow = std::getenv("MAGATAMA_RESOLUTION_FOLLOW"); follow && follow[0] == '0')
        return; // tests: keep the launch scale (RRV_PCSX2_GS_FULL_SCALE)
    const long percent = std::clamp<long>(toLong(m_values[id("rendering", "resolution_scale")], 100), 25, 200);
    const bool deck = settings::steamDeck(); // even scales only there (settings::hostScale)
    const auto clampScale = [deck](double s) {
        s = std::clamp(s, 1.0, 8.0);
        return deck ? std::clamp(std::round(s / 2.0) * 2.0, 2.0, 8.0) : s;
    };
    const double fit = std::min(ui.display_width / 640.0, ui.display_height / 448.0);
    const double hud = clampScale(fit), scene = clampScale(fit * static_cast<double>(percent) / 100.0);
    char hudText[16], sceneText[16], wanted[40];
    std::snprintf(hudText, sizeof hudText, "%.2f", hud);
    std::snprintf(sceneText, sizeof sceneText, "%.2f", scene);
    std::snprintf(wanted, sizeof wanted, "%s/%s", hudText, sceneText);
    m_resolutionText = "3D at " + std::to_string(static_cast<int>(std::lround(640 * scene))) + "x" +
                       std::to_string(static_cast<int>(std::lround(448 * scene))) + " (" + sceneText +
                       "x the PS2's); HUD and text at " + std::to_string(static_cast<int>(ui.display_width)) + "x" +
                       std::to_string(static_cast<int>(ui.display_height)) + ", the screen's own.";
    if (m_resolutionApplied == wanted)
        return;
    if (m_resolutionPending != wanted)
    {
        m_resolutionPending = wanted;
        m_resolutionFrames = 0;
    }
    if (!m_resolutionApplied.empty() && ++m_resolutionFrames < 20)
        return;
    const bool sameScale = std::strcmp(hudText, sceneText) == 0;
    const bool split = ui.set_render_option("scene_scale", sameScale ? "0" : sceneText) != 0;
    const bool ok = ui.set_render_option("scale", split ? hudText : sceneText) != 0;
    std::fprintf(stderr, "[fukami-menu] resolution: %ld%% of %.0fx%.0f -> HUD %s, 3D %s%s\n", percent,
                 static_cast<double>(ui.display_width), static_cast<double>(ui.display_height), hudText, sceneText,
                 split ? "" : ok ? " (whole picture: no scene_scale in this bridge)" : " (not applied)");
    m_resolutionApplied = wanted; // also when not applied: do not ask again every frame
}

bool Menu::pendingRestart(const settings::Spec &spec) const
{
    if (appliesLive(spec.section, spec.key))
        return false;
    const std::string key = id(spec.section, spec.key);
    const auto shown = m_values.find(key);
    const auto launched = m_launch.find(key);
    return shown != m_values.end() && launched != m_launch.end() && shown->second != launched->second;
}

bool Menu::save(std::string_view section, std::string_view key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(m_saveMutex);
    if (m_ini.empty())
        return false;
    if (!settings::updateIni(m_ini, {{std::string(section), std::string(key), value}}))
    {
        m_status = "Could not save the settings file.";
        return false;
    }
    m_status.clear();
    return true;
}

void Menu::setStatus(std::string status)
{
    std::lock_guard<std::mutex> lock(m_saveMutex);
    m_status = std::move(status);
}

std::string Menu::status() const
{
    std::lock_guard<std::mutex> lock(m_saveMutex);
    return m_status;
}

} // namespace fukami::menu
