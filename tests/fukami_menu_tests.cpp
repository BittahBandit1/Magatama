// Fukami in-game menu (src/app/fukami_menu.cpp) against a fake bridge UI
// table: no UI work while closed, every menu setting drawn, changes saved to
// the settings file with its comments kept, live options applied, restart-only
// options flagged, Esc/B backing out of a combo before the menu, fullscreen
// persisted only when it differs.
#include "fukami_app.h"
#include "fukami_menu.h"
#include "fukami_settings.h"
#include "magatama_display_settings.h"
#include "../src/host/rrv_audio_output.h"
#include "../src/gs-backend/rrv_nocturne_aspect.h"

#include "../tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#if defined(_WIN32)
#include <process.h>
#include <stdlib.h>
#define getpid _getpid
// _putenv_s(name, "") removes the variable, as unsetenv does.
#define unsetenv(name) _putenv_s(name, "")
#else
#include <unistd.h>
#endif
#include <vector>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace fs = std::filesystem;

// ---- fukami::app stand-ins (fukami_app.mm is the real one) -----------------
namespace {
bool g_canRestart = false;
fs::path g_saves;
fs::path g_logs;
std::vector<fs::path> g_revealed;
} // namespace

namespace fukami::app {
bool inApp() { return g_canRestart; }
std::filesystem::path iniPath() { return {}; }
std::filesystem::path defaultIniPath() { return {}; }
bool canRestart() { return g_canRestart; }
void restart() {}
std::filesystem::path savesFolder() { return g_saves; }
std::filesystem::path logsFolder() { return g_logs; }
void reveal(const std::filesystem::path &path) { g_revealed.push_back(path); }
} // namespace fukami::app

namespace {

int failures = 0;

void expect(bool ok, const std::string &what)
{
    if (!ok)
    {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

// ---- fake UI -----------------------------------------------------------------
std::vector<std::string> g_calls;       // every UI call, in order
std::map<std::string, int> g_intSet;    // label -> value a widget reports as changed
std::map<std::string, float> g_floatSet;
std::string g_press;                    // button label to report as activated
bool g_popup = false;
bool g_rejectRender = false;

void record(const std::string &call) { g_calls.push_back(call); }
bool called(const std::string &prefix)
{
    for (const auto &call : g_calls)
        if (call.rfind(prefix, 0) == 0)
            return true;
    return false;
}
int count(const std::string &prefix)
{
    int n = 0;
    for (const auto &call : g_calls)
        n += call.rfind(prefix, 0) == 0 ? 1 : 0;
    return n;
}
std::string strip(const char *label)
{
    std::string text(label ? label : "");
    if (text.size() > 2 && text.compare(text.size() - 2, 2, " *") == 0)
        text.resize(text.size() - 2);
    return text;
}
int takeInt(const char *label, int *value)
{
    const auto it = g_intSet.find(strip(label));
    if (it == g_intSet.end())
        return 0;
    *value = it->second;
    g_intSet.erase(it);
    return 1;
}

RrvPcsx2GsBridgeUi fakeUi()
{
    RrvPcsx2GsBridgeUi ui{};
    ui.struct_size = sizeof(ui);
    ui.display_width = 1920.0f;
    ui.display_height = 1080.0f;
    ui.scale = 2.0f;
    ui.add_key = [](uint32_t key, int down) { record("key " + std::to_string(key) + " " + std::to_string(down)); };
    ui.add_mouse_position = [](float, float) { record("mouse"); };
    ui.add_mouse_button = [](uint32_t, int) { record("mouse-button"); };
    ui.add_mouse_wheel = [](float, float) { record("wheel"); };
    ui.clear_input = [] { record("clear"); };
    ui.consumes_cancel = [] { return g_popup ? 1 : 0; };
    ui.begin_menu = [](const char *title, int appearing) {
        record(std::string("begin ") + title + " " + std::to_string(appearing));
        return 1;
    };
    ui.end_menu = [] { record("end"); };
    ui.section = [](const char *label) { record(std::string("section ") + label); };
    ui.text = [](const char *text) { record(std::string("text ") + text); };
    ui.text_disabled = [](const char *text) { record(std::string("disabled-text ") + text); };
    ui.text_wrapped = [](const char *text) { record(std::string("wrapped ") + text); };
    ui.text_warning = [](const char *text) { record(std::string("warning ") + text); };
    ui.separator = [] {};
    ui.same_line = [] { record("same-line"); };
    ui.spacing = [] {};
    ui.push_id = [](int) {};
    ui.pop_id = [] {};
    ui.begin_disabled = [](int disabled) { record("disabled " + std::to_string(disabled)); };
    ui.end_disabled = [] {};
    ui.tooltip = [](const char *) {};
    ui.default_focus = [] { record("default-focus"); };
    ui.checkbox = [](const char *label, int *value) {
        record("checkbox " + strip(label) + " " + std::to_string(*value));
        return takeInt(label, value);
    };
    ui.combo = [](const char *label, int *index, const char *const *items, int count) {
        record("combo " + strip(label) + " " + items[*index] + " of " + std::to_string(count));
        return takeInt(label, index);
    };
    ui.slider_int = [](const char *label, int *value, int min, int max) {
        record("slider " + strip(label) + " " + std::to_string(*value) + " " + std::to_string(min) + ".." +
               std::to_string(max));
        return takeInt(label, value);
    };
    ui.slider_float = [](const char *label, float *value, float, float, const char *) {
        record("slider-float " + strip(label));
        const auto it = g_floatSet.find(strip(label));
        if (it == g_floatSet.end())
            return 0;
        *value = it->second;
        g_floatSet.erase(it);
        return 1;
    };
    ui.button = [](const char *label) {
        record(std::string("button ") + label);
        if (g_press == label)
        {
            g_press.clear();
            return 1;
        }
        return 0;
    };
    ui.set_aspect = [](const char *value) {
        record(std::string("set-aspect ") + value);
        return 1;
    };
    ui.set_render_option = [](const char* key,const char* value) {
        record(std::string("set-render ")+key+" "+value);return g_rejectRender ? 0 : 1;
    };
    ui.set_integer_scaling = [](int on) { record("set-integer " + std::to_string(on)); };
    ui.set_fxaa = [](int on) { record("set-fxaa " + std::to_string(on)); };
    ui.set_cas = [](int value) {
        record("set-cas " + std::to_string(value));
        return 1;
    };
    return ui;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void frame(fukami::menu::Menu &menu)
{
    g_calls.clear();
    const RrvPcsx2GsBridgeUi ui = fakeUi();
    fukami::menu::Menu::frameCallback(&menu, &ui);
}

fukami::menu::Input key(uint32_t code, bool down)
{
    fukami::menu::Input input;
    input.code = code;
    input.down = down;
    return input;
}

const std::string kIni =
    "# Fukami settings\n"
    "[display]\n"
    "; picture\n"
    "ratio = 16:9\n"
    "fullscreen = false\n"
    "\n"
    "[rendering]\n"
    "scale = 4\n"
    "fxaa = false\n"
    "\n"
    "[env]\n"
    "RRV_CUSTOM = 1\n";

void testClosedMenuDoesNothing(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    frame(menu);
    expect(g_calls.empty(), "no UI calls while closed and never opened");
    menu.open();
    expect(!menu.isOpen(), "the menu cannot open before the overlay is available");
    frame(menu);
    expect(g_calls.empty(), "still no UI calls");
    menu.post(key(RRV_PCSX2_GS_UI_KEY_DOWN, true));
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    expect(!called("key "), "input posted while closed is dropped");
    menu.close();
    frame(menu);
    expect(g_calls == std::vector<std::string>{"clear"}, "closing releases ImGui input once");
    frame(menu);
    expect(g_calls.empty(), "and then nothing");
}

void testDrawsEveryMenuSetting(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    expect(called("clear") && called("begin Magatama 1") && called("default-focus") && called("end"),
           "first open frame clears input, appears, focuses Resume");
    int inMenu = 0;
    for (const auto &spec : fukami::settings::schema())
    {
        if (!spec.inMenu || (spec.section=="game" && spec.key=="logging" && !fukami::app::inApp()))
            continue;
        ++inMenu;
        expect(called("checkbox " + std::string(spec.label)) || called("combo " + std::string(spec.label)) ||
                   called("slider " + std::string(spec.label)) || called("slider-float " + std::string(spec.label)) ||
                   (spec.section == "audio" && spec.key == "mute" && called("button Mute")),
               "row drawn for " + std::string(spec.label));
    }
    expect(count("checkbox ") + count("combo ") + count("slider ") + count("slider-float ") + count("button Mute") == inMenu,
           "exactly one row per menu setting");
    for (const char *section : {"section Audio", "section Display", "section Rendering", "section Input", "section Game",
                                "section Performance"})
        expect(called(section), std::string("section drawn: ") + section);
    expect(called("slider Rendering resolution 4 1..8"), "integer slider shows the file's value and range");
    expect(called("combo Aspect ratio 4:3 of 6"), "Nocturne ratios available in Escape menu");
    expect(!called("combo Picture fit") && !called("combo Widescreen"), "legacy fit and RR5 camera controls stay hidden");
    expect(called("combo Texture filtering PS2 (stock) of 2"), "the texture filter shows the stock choice by name");
    expect(called("checkbox Split VU1 / GS 1") && called("slider Pacer spin (ms) 0 0..17"),
           "the VU1/GS split and the pacer spin are menu settings (schema defaults without a default ini)");
    for (const char *hidden : {"Unpaced", "Inline", "Headless", "Developer mode"})
        expect(!called(std::string("checkbox ") + hidden), std::string("ini only: ") + hidden);
    expect(called("wrapped Settings: " + ini.string()), "settings file path shown");
    expect(!called("warning Restart"), "nothing pending at open");
    expect(!called("section Files"), "no Files section without folders");
    expect(!called("checkbox Write logs"), "app-only session logger is hidden in Terminal mode");
    frame(menu);
    expect(called("begin Magatama 0"), "later frames are not appearing");
}

void testLiveChangeSavesAndApplies(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    g_intSet["FXAA"] = 1;
    frame(menu);
    expect(called("set-fxaa 1"), "FXAA applied live");
    const std::string text = readFile(ini);
    expect(text.find("fxaa = true\n") != std::string::npos, "FXAA saved");
    expect(text.find("# Fukami settings\n[display]\n; picture\n") == 0 && text.find("RRV_CUSTOM = 1") != std::string::npos,
           "comments and unknown sections kept");
    frame(menu);
    expect(!called("warning Restart"), "a live setting needs no restart");

    g_intSet["Sharpening (CAS)"] = 40;
    frame(menu);
    expect(called("set-cas 40") && readFile(ini).find("cas = 40\n") != std::string::npos, "CAS applied and saved");
}

void testAudioControls(const fs::path& ini)
{
    using namespace rrv::audio;
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true); menu.open(); frame(menu);
    expect(called("slider Volume (%) 100 0..100") && called("button Mute"), "audio controls draw at defaults");
    g_intSet["Volume (%)"] = 37; frame(menu);
    expect(outputVolume() == 37 && !outputMuted(), "volume updates actual shared output state immediately");
    g_press = "Mute"; frame(menu);
    expect(outputMuted() && outputVolume() == 37, "mute preserves chosen volume");
    frame(menu); expect(called("button Unmute"), "muted button offers Unmute");
    g_intSet["Volume (%)"] = 23; frame(menu);
    expect(outputMuted() && outputVolume() == 23, "volume can change while staying muted");
    menu.close();
    fukami::menu::Menu relaunched(ini);
    expect(outputMuted() && outputVolume() == 23, "both audio choices survive a new launch");
    relaunched.setAvailable(true); relaunched.open();
    g_press = "Unmute"; frame(relaunched);
    expect(!outputMuted() && outputVolume() == 23, "unmute restores the saved volume");
    const auto saved = readFile(ini);
    expect(saved.find("volume = 23") != std::string::npos && saved.find("mute = false") != std::string::npos,
           "audio preferences saved without replacing unrelated settings");
    expect(saved.find("RRV_CUSTOM = 1") != std::string::npos && !called("warning Restart"), "audio preserves unrelated preferences and needs no restart");
    expect(!relaunched.takeRestartRequest() && !relaunched.takeQuitRequest(), "audio never restarts or quits the game");
    g_intSet["Volume (%)"] = 101; frame(relaunched);
    expect(outputVolume() == 23 && readFile(ini) == saved, "invalid volume cannot alter output or saved settings");
}

void testRestartOnlyChange(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    g_intSet["Rendering resolution"] = 6;
    frame(menu);
    expect(readFile(ini).find("scale = 6\n") != std::string::npos, "live rendering setting saved");
    expect(called("set-render scale 6"), "resolution reaches renderer callback");
    frame(menu);
    expect(!called("warning Restart"), "resolution changes need no game restart");
    expect(!called("disabled-text Quit and relaunch to apply."), "live resolution requires no relaunch");
    expect(called("button Restart"), "Restart remains visible next to Quit when unavailable");
    g_canRestart = true;
    g_press = "Restart";
    frame(menu);
    expect(called("button Restart") && !menu.takeRestartRequest(), "Restart waits for unsaved-progress confirmation");
    g_press = "Yes";
    frame(menu);
    expect(menu.takeRestartRequest() && !menu.takeQuitRequest(), "confirmed Restart requests only a restart");
    menu.open();
    frame(menu);
    g_canRestart = false;
    g_intSet["Rendering resolution"] = 4;
    frame(menu);
    frame(menu);
    expect(!called("warning Restart"), "back to the launch value: nothing pending");

    expect(!called("slider-float Car detail distance"), "RR5-specific car controls remain hidden in Magatama");
}

void testRendererReadback(const fs::path& ini)
{
    fukami::menu::Menu menu(ini);menu.setAvailable(true);menu.open();auto ui=fakeUi();
    ui.get_render_option=[](const char* key)->const char* {
        if(std::string_view(key)=="scale")return "2";
        if(std::string_view(key)=="mipmap")return "true";
        return nullptr;
    };
    g_calls.clear();fukami::menu::Menu::frameCallback(&menu,&ui);
    expect(called("slider Rendering resolution 2 1..8"),"actual renderer scale wins over saved/requested value");
    expect(called("checkbox Mipmaps 1"),"actual renderer mipmap value is shown");
    const auto before=readFile(ini);
    g_calls.clear();fukami::menu::Menu::frameCallback(&menu,&ui);
    expect(readFile(ini)==before,"renderer observation does not rewrite user preferences");
    expect(!called("warning Restart"),"actual live renderer values do not create restart warnings");
}

void testEffectiveTerminalDefaults(const fs::path& ini)
{
    std::vector<std::pair<std::string,std::string>> previous;
    const char* names[]={"MAGATAMA_DISC","RRV_VU1GS_SPLIT","RRV_PCSX2_GS_FULL_HWMIPMAP"};
    for(const char* n:names){const char* v=std::getenv(n);previous.emplace_back(n,v?v:"");}
    setenv("MAGATAMA_DISC","test-only",1);unsetenv("RRV_VU1GS_SPLIT");unsetenv("RRV_PCSX2_GS_FULL_HWMIPMAP");
    std::ofstream(ini,std::ios::trunc)<<"";
    fukami::menu::Menu menu(ini);menu.setAvailable(true);menu.open();frame(menu);
    expect(called("checkbox Split VU1 / GS 0"),"Terminal menu shows actual unsplit runtime default");
    expect(called("checkbox Mipmaps 1"),"Terminal menu shows actual mipmap runtime default");
    expect(!called("warning Restart"),"effective defaults do not falsely claim restart pending");
    for(const auto& [n,v]:previous){if(v.empty())unsetenv(n.c_str());else setenv(n.c_str(),v.c_str(),1);}
}

void testLiveRendererControls(const fs::path& ini)
{
    fukami::menu::Menu menu(ini);menu.setAvailable(true);menu.open();frame(menu);
    struct Change { const char* label;const char* key;int widget;const char* value; };
    for (const auto& c : std::vector<Change>{
        {"Rendering resolution","scale",1,"1"},
        {"Rendering resolution","scale",8,"8"},
        {"Rendering resolution","scale",4,"4"},
        {"Edge anti-aliasing (AA1)","aa1",0,"false"},
        {"Edge anti-aliasing (AA1)","aa1",1,"true"},
        {"Anisotropic filtering","aniso",4,"16"},
        {"Texture filtering","texture_filter",1,"bilinear"},
        {"2D filtering","sprite_filter",1,"ps2"},
        {"Mipmaps","mipmap",1,"true"}}) {
        g_intSet[c.label]=c.widget;frame(menu);
        expect(called(std::string("set-render ")+c.key+" "+c.value),"actual renderer callback: "+std::string(c.key));
        frame(menu);expect(!called("warning Restart"),"renderer change has no restart marker: "+std::string(c.key));
        expect(!menu.takeRestartRequest()&&!menu.takeQuitRequest(),"live renderer change preserves running game");
    }
    const auto before=readFile(ini);
    g_rejectRender=true;g_intSet["Rendering resolution"]=2;frame(menu);g_rejectRender=false;
    expect(readFile(ini)==before,"renderer rejection does not persist a false live value");
    expect(called("warning Could not apply Rendering resolution"),"renderer failure is visible");
    auto old=fakeUi();old.struct_size=offsetof(RrvPcsx2GsBridgeUi,set_render_option);
    g_calls.clear();g_intSet["Rendering resolution"]=3;
    fukami::menu::Menu::frameCallback(&menu,&old);
    expect(called("begin"),"previous bridge UI prefix still draws the menu");
    expect(readFile(ini)==before,"previous bridge cannot falsely accept unsupported live rendering");
    // Reselecting auto uses the drawable already observed, without requiring
    // a resize event to update the centered HUD/projection ratio.
    magatama::aspect::surface(3440,1440);
    g_intSet["Aspect ratio"]=1;frame(menu);
    expect(std::abs(magatama::aspect::target.load()-3440.f/1440.f)<1e-6,"auto aspect updates immediately to existing window");
    expect(called("set-aspect stretch"),"world/HUD and presentation aspect update together");
}

void testMenuRestartAudit(const fs::path& ini)
{
    using namespace fukami::settings;
    for(const auto& spec:schema()){
        if(!spec.inMenu || (spec.section=="game" && spec.key=="logging"))continue;
        const bool live=fukami::menu::appliesLive(spec.section,spec.key);
        expect(live==(spec.apply==Apply::live),"restart label matches actual handler: "+std::string(spec.key));
        std::cout << "AUDIT " << spec.section << "." << spec.key << " | " << spec.label << " | " << (live?"Live":"Restart") << "\n";
    }
    fukami::menu::Menu menu(ini);menu.setAvailable(true);menu.open();frame(menu);
    g_intSet["Integer scaling"]=1;frame(menu);
    expect(called("set-integer 1"),"integer scaling routes to bridge immediately");
    g_intSet["Metal HUD"]=0;frame(menu);
    expect(menu.takeMetalHudRequest()==0,"Metal HUD queues host request immediately");
    frame(menu);expect(!called("warning Restart"),"live display changes never produce restart banner");
    const auto saved=magatamaSavedEnvironment(*IniDocument::load(ini));
    auto get=[&](const std::string& key){for(auto& pair:saved)if(pair.first==key)return pair.second;return std::string();};
    expect(get("RRV_PCSX2_GS_INTEGER_SCALING")=="1"&&get("MTL_HUD_ENABLED")=="0","live settings also survive Terminal restart");
    for (auto section:{"rendering","input","timing","display"}) {
        for(const auto& spec:schema()){
            if(spec.section!=section || !spec.inMenu || spec.key=="scale" || spec.key=="nocturne_aspect")continue;
            IniDocument doc;const std::string v=spec.kind==Kind::boolean?"false":(spec.choices.empty()?"0":std::string(spec.choices.back()));
            if(!valid(spec,v))continue;
            doc.set(spec.section,spec.key,v);const auto env=magatamaSavedEnvironment(doc);
            expect(env.size()==1,"saved startup binding exists for "+std::string(spec.key));
            if(!env.empty())expect(env[0].second==(spec.kind==Kind::boolean?"0":v),"saved startup value including false/off is exact");
        }
    }
    expect(magatamaSavedEnvironment(IniDocument::parse("[rendering]\naniso=999\nfxaa=maybe\n")).empty(),"invalid saved values never reach renderer");
}

void testRestartTopRow(const fs::path& ini)
{
    g_canRestart = true;
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true); menu.open(); frame(menu);
    auto quit = std::find(g_calls.begin(), g_calls.end(), "button Quit");
    auto restart = std::find(g_calls.begin(), g_calls.end(), "button Restart");
    expect(quit != g_calls.end() && restart != g_calls.end() && quit < restart,
           "Restart is after Quit even without pending settings");
    expect(quit != g_calls.end() && restart != g_calls.end() && quit < restart &&
           std::find(quit, restart, "same-line") != restart,
           "Restart follows Quit on the same row");
    expect(count("button Restart") == 1, "only one Restart button is drawn");
    g_press = "Restart"; frame(menu);
    expect(!menu.takeRestartRequest(), "opening restart confirmation does not relaunch");
    frame(menu);
    expect(called("begin Restart 1") && called("default-focus") && called("button No"),
           "restart confirmation starts on No");
    g_press = "No"; frame(menu); frame(menu);
    expect(menu.isOpen() && !menu.takeRestartRequest(), "No returns to game menu without restarting");
    g_press = "Restart"; frame(menu);frame(menu);
    menu.post(key(RRV_PCSX2_GS_UI_KEY_ESCAPE,true));frame(menu);
    expect(menu.isOpen() && !menu.takeRestartRequest(), "Escape cancels restart without closing game menu");
    g_canRestart = false;
    g_press = "Restart";frame(menu);
    expect(!menu.takeRestartRequest(), "unavailable launch mode cannot restart");
}

void testNocturneDisplay(const fs::path& ini)
{
    auto neutral = [](const char*) -> const char* { return nullptr; };
    using fukami::settings::IniDocument;
    auto fallback = magatama::displaySettings(IniDocument{}, neutral);
    expect(fallback.aspect == "off" && fallback.scale == "8", "existing no-file Magatama defaults preserved");
    auto invalid = magatama::displaySettings(IniDocument::parse("[display]\nnocturne_aspect = bad\n[rendering]\nscale = 99\n"), neutral);
    expect(invalid.aspect == "off" && invalid.scale == "8", "invalid saved display options fall back safely");
    const auto saved = readFile(ini);
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);menu.open();frame(menu);
    for (int index=0; index<int(fukami::settings::find("display", "nocturne_aspect")->choices.size()); ++index) {
        g_intSet["Aspect ratio"] = index; frame(menu);frame(menu);
        const auto doc = IniDocument::load(ini);
        const auto launch = magatama::displaySettings(*doc, neutral);
        const auto& spec = *fukami::settings::find("display", "nocturne_aspect");
        expect(launch.aspect == spec.choices[index], "each menu ratio persists into the next product launch");
        expect(!called("set-aspect"), "ratio never changes fit alone without the camera/HUD projection");
    }
    for (int scale : {1,4,8}) {
        g_intSet["Rendering resolution"] = scale; frame(menu);
        const auto launch = magatama::displaySettings(*IniDocument::load(ini), neutral);
        expect(launch.scale == std::to_string(scale), "resolution slider persists into product launch");
    }
    auto overridden = magatama::displaySettings(*IniDocument::load(ini), [](const char* name) -> const char* {
        return std::string(name) == "MAGATAMA_ASPECT" ? "16:10" : "2";
    });
    expect(overridden.aspect == "16:10" && overridden.scale == "2", "explicit launch/replay overrides beat saved menu values");
    std::ofstream(ini, std::ios::trunc) << saved;
}

void testBackKeys(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    menu.post(key(RRV_PCSX2_GS_UI_KEY_DOWN, true));
    g_popup = true;
    menu.post(key(RRV_PCSX2_GS_UI_KEY_ESCAPE, true));
    frame(menu);
    expect(called("key " + std::to_string(RRV_PCSX2_GS_UI_KEY_DOWN) + " 1"), "navigation key forwarded");
    expect(called("key " + std::to_string(RRV_PCSX2_GS_UI_KEY_ESCAPE) + " 1") && menu.isOpen(),
           "Esc with a combo open goes to ImGui");
    g_popup = false;
    menu.post(key(RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_RIGHT, true));
    frame(menu);
    expect(!menu.isOpen() && !called("begin "), "B with no popup closes the menu");
    expect(called("clear"), "closing clears ImGui input");

    menu.open();
    frame(menu);
    g_press = "Resume";
    frame(menu);
    expect(!menu.isOpen(), "Resume closes");
    menu.open();
    frame(menu);
    g_press = "Quit";
    frame(menu);
    expect(!menu.takeQuitRequest(), "Quit waits for existing unsaved-progress confirmation");
    frame(menu);
    g_press = "Yes";
    frame(menu);
    expect(menu.takeQuitRequest() && !menu.takeQuitRequest(), "confirmed Quit requests one clean stop");
}

#if defined(__APPLE__) && TARGET_OS_OSX
void testLauncherChangesWhileMenuClosed(const fs::path &ini)
{
    setenv("MAGATAMA_LAUNCHER_SETTINGS", "1", 1);
    fukami::menu::Menu menu(ini);
    unsetenv("MAGATAMA_LAUNCHER_SETTINGS");
    expect(fukami::settings::updateIni(ini, {
        {"audio", "volume", "45"},
        {"display", "fullscreen", "true"},
        {"rendering", "fxaa", "true"},
        {"rendering", "frame_rate", "60"},
    }), "launcher changes saved");
    frame(menu);
    expect(!menu.isOpen() && !called("begin "), "the in-game menu stays closed");
    expect(rrv::audio::outputVolume() == 45 && called("set-fxaa 1"), "launcher changes apply live");
    expect(menu.takeFullscreenRequest() == 1, "launcher fullscreen is sent to the window thread");
    expect(!menu.takeRestartRequest(), "launcher settings never request a restart");
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    expect(called("warning Restart to apply: Frame rate"), "the in-game menu shows pending launcher changes");
    expect(readFile(ini).find("RRV_CUSTOM = 1") != std::string::npos, "unrelated ini values are preserved");
}
#endif

void testFullscreen(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.fullscreenChanged(false, false);
    menu.open();
    frame(menu);
    expect(called("checkbox Fullscreen 0"), "fullscreen checkbox shows the window state");
    g_intSet["Fullscreen"] = 1;
    frame(menu);
    expect(menu.takeFullscreenRequest() == 1 && menu.takeFullscreenRequest() == -1, "menu requests fullscreen from the host");
    expect(readFile(ini).find("fullscreen = false\n") != std::string::npos, "the menu itself does not write fullscreen");
    menu.fullscreenChanged(true, true);
    expect(readFile(ini).find("fullscreen = true\n") != std::string::npos, "a real change is saved");
    const auto stamp = fs::last_write_time(ini);
    std::string before = readFile(ini);
    menu.fullscreenChanged(true, true);
    expect(readFile(ini) == before && fs::last_write_time(ini) == stamp, "an unchanged state is not rewritten");
    frame(menu);
    expect(called("checkbox Fullscreen 1"), "checkbox follows the window");
}

// A setting the file does not have (written by an older app version) shows the app's bundled default, as the
// launcher used it; opening the menu does not write the file, and a change saves only that setting.
void testPackageDefaults(const fs::path &ini)
{
    fs::path defaults = ini;
    defaults += ".default";
    std::ofstream(defaults, std::ios::trunc)
        << "[display]\nratio = 4:3\n[rendering]\nscale = 2\naniso = 8\n[timing]\nsplit_gs = true\npacer_spin = 17\n";
    fukami::menu::Menu menu(ini, defaults);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    expect(called("combo Anisotropic filtering 8x of 5"), "a setting missing from the file shows the app default");
    expect(called("checkbox Split VU1 / GS 1") && called("slider Pacer spin (ms) 17 0..17"),
           "the Linux package case: an old file without split_gs/pacer_spin shows the split on and spin 17");
    expect(called("slider Rendering resolution 4 1..8") && called("combo Aspect ratio 4:3 of 6"),
           "settings the file has keep the file's value");
    expect(!called("warning Restart"), "an inherited default is not a pending change");
    expect(readFile(ini) == kIni, "showing the menu does not rewrite the file");
    g_intSet["Anisotropic filtering"] = 0;
    frame(menu);
    const std::string text = readFile(ini);
    expect(text.find("aniso = 0\n") != std::string::npos && text.find("scale = 4\n") != std::string::npos &&
               text.find("ratio = 16:9\n") != std::string::npos,
           "a change saves that setting only; the others keep the file's values");
    frame(menu);
    expect(called("combo Anisotropic filtering Off of 5") && !called("warning Restart"),
           "anisotropy applies without a restart warning");
    g_intSet["Split VU1 / GS"] = 0;
    frame(menu);
    expect(readFile(ini).find("\n[timing]\nsplit_gs = false\n") != std::string::npos,
           "switching the split off in the menu writes split_gs = false (it then wins over the app default)");
    expect(readFile(ini).find("pacer_spin") == std::string::npos, "the untouched setting is still not written");
    frame(menu);
    expect(called("checkbox Split VU1 / GS 0") && called("slider Pacer spin (ms) 17 0..17"),
           "the split shows off, the spin still follows the app default");
    fs::remove(defaults);
}

void testFilesAndNoIni()
{
    g_saves = "/tmp/fukami-saves";
    g_logs = "/tmp/fukami-logs";
    fukami::menu::Menu menu{fs::path()};
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    expect(called("section Files") && called("button Show saves") && called("button Show logs"), "Files buttons shown");
    expect(called("wrapped No settings file"), "no-file note");
    expect(called("disabled 1"), "restart-only rows are read-only without a file");
    g_press = "Show saves";
    frame(menu);
    expect(g_revealed.size() == 1 && g_revealed[0] == g_saves, "Show saves reveals the card folder");
    g_intSet["FXAA"] = 1;
    frame(menu);
    expect(called("set-fxaa 1"), "live settings still apply without a file");
    g_saves.clear();
    g_logs.clear();
}

} // namespace

int main()
{
    ::unsetenv("RRV_RR5_WIDESCREEN");
    const fs::path ini = fs::temp_directory_path() / ("fukami-menu-" + std::to_string(getpid()) + ".ini");
    const auto reset = [&] {
        std::ofstream(ini, std::ios::trunc) << kIni;
    };
    reset();
    testClosedMenuDoesNothing(ini);
    reset();
    testDrawsEveryMenuSetting(ini);
    reset();
    testLiveChangeSavesAndApplies(ini);
    reset();
    testAudioControls(ini);
    reset();
    testRestartOnlyChange(ini);
    testNocturneDisplay(ini);
    reset();
    testRestartTopRow(ini);
    reset();
    testMenuRestartAudit(ini);
    reset();
    testRendererReadback(ini);
    reset();
    testEffectiveTerminalDefaults(ini);
    reset();
    testLiveRendererControls(ini);
    reset();
    testBackKeys(ini);
    reset();
    testFullscreen(ini);
    reset();
    testPackageDefaults(ini);
    testFilesAndNoIni();
#if defined(__APPLE__) && TARGET_OS_OSX
    reset();
    testLauncherChangesWhileMenuClosed(ini);
    fs::remove(ini.string() + ".lock");
#endif
    fs::remove(ini);
    if (failures)
    {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "fukami menu tests passed\n";
    return 0;
}
