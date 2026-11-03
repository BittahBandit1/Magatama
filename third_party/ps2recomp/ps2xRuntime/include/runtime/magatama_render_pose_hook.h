#pragma once
#include <cstdint>
// Nocturne render diagnostics only. Configured before guest/owner threads run;
// null normally. Source bytes and all guest/VIF state remain immutable here.
using MagatamaRenderUnpackObserver = void (*)(uint32_t source, uint32_t command,
    uint32_t destination, const uint8_t* data, uint32_t bytes);
extern MagatamaRenderUnpackObserver g_magatamaRenderUnpackObserver;

// Optional rendering-copy replacement. Input storage is immutable; false leaves
// the original UNPACK source unchanged. At most208bytes, owner-thread only.
using MagatamaRenderUnpackTransform = bool (*)(uint32_t source,uint32_t command,
 const uint8_t* data,uint32_t bytes,uint8_t* output);
extern MagatamaRenderUnpackTransform g_magatamaRenderUnpackTransform;
