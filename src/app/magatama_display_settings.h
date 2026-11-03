#pragma once
#include "fukami_settings.h"

namespace magatama {
struct DisplaySettings { std::string aspect; std::string scale; };
template<class Getter>
DisplaySettings displaySettings(const fukami::settings::IniDocument& doc, Getter get) {
    const auto saved = [&](const char* section, const char* key, const char* fallback) {
        const auto value = doc.get(section, key);
        const auto* spec = fukami::settings::find(section, key);
        return value && spec && fukami::settings::valid(*spec, *value) ? *value : std::string(fallback);
    };
    const auto override = [&](const char* name, std::string fallback) {
        const char* value = get(name);
        return value && *value ? std::string(value) : fallback;
    };
    // Explicit launch overrides win. A missing file preserves Magatama's 8x
    // default; the menu later saves just the keys the user actually changes.
    return {override("MAGATAMA_ASPECT", saved("display", "nocturne_aspect", "off")),
            override("RRV_PCSX2_GS_FULL_SCALE", saved("rendering", "scale", "8"))};
}
}
