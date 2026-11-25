#import "MagatamaSettingsBridge.h"

#include "fukami_settings.h"
#include <TargetConditionals.h>

namespace {
namespace settings = fukami::settings;

bool shown(const settings::Spec &spec)
{
#if TARGET_OS_IPHONE
    if (spec.section == "display" && spec.key == "fullscreen")
        return false; // the iOS game always fills the screen
#endif
    return spec.inMenu && !(spec.section == "game" && spec.key == "logging");
}

NSString *string(std::string_view value)
{
    if (value.empty())
        return @"";
    return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

NSString *identifier(const settings::Spec &spec)
{
    return [NSString stringWithFormat:@"%@.%@", string(spec.section), string(spec.key)];
}
}

NSData *MagatamaReadSettings(NSString *path)
{
    const auto doc = settings::IniDocument::load(path.fileSystemRepresentation);
    if (!doc)
        return nil;

    NSMutableArray *rows = [NSMutableArray array];
    for (const auto &spec : settings::schema())
    {
        if (!shown(spec))
            continue;
        NSString *kind;
        switch (spec.kind)
        {
        case settings::Kind::boolean: kind = @"toggle"; break;
        case settings::Kind::choice: kind = @"choice"; break;
        case settings::Kind::integer: kind = @"integer"; break;
        default: continue;
        }
        std::string value = settings::value(*doc, spec);
        if (spec.kind == settings::Kind::boolean)
        {
            // Match platform startup defaults when no valid value was saved.
            if (const auto saved = doc->get(spec.section, spec.key); !saved || !settings::valid(spec, *saved))
            {
                if (spec.key == "mipmap") value = "true";
#if TARGET_OS_OSX
                if (spec.key == "split_gs") value = "false";
#endif
            }
            value = settings::boolValue(value) ? "true" : "false";
        }
#if TARGET_OS_IPHONE
        if (spec.key == "resolution_scale")
        {
            const char *fallback = std::getenv("MAGATAMA_RESOLUTION_SCALE_DEFAULT");
            const auto saved = doc->get(spec.section, spec.key);
            if (fallback && settings::valid(spec, fallback) && (!saved || !settings::valid(spec, *saved)))
                value = fallback;
        }
#endif
        NSMutableArray *choices = [NSMutableArray array];
        for (auto choice : spec.choices)
            [choices addObject:string(choice)];
        [rows addObject:@{
            @"id": identifier(spec), @"section": string(spec.section), @"label": string(spec.label),
            @"help": string(spec.help), @"kind": kind, @"value": string(value), @"choices": choices,
            @"minimum": @(static_cast<int>(spec.minValue)), @"maximum": @(static_cast<int>(spec.maxValue)),
            @"live": @(spec.apply == settings::Apply::live)
        }];
    }
    return [NSJSONSerialization dataWithJSONObject:rows options:0 error:nil];
}

BOOL MagatamaWriteSettings(NSString *path, NSDictionary<NSString *, NSString *> *changes)
{
    std::vector<settings::Override> updates;
    for (const auto &spec : settings::schema())
    {
        if (!shown(spec))
            continue;
        if (NSString *value = changes[identifier(spec)])
        {
            if (!settings::valid(spec, value.UTF8String))
                return NO;
            updates.push_back({std::string(spec.section), std::string(spec.key), value.UTF8String});
        }
    }
    return updates.size() == changes.count && settings::updateIni(path.fileSystemRepresentation, updates);
}
