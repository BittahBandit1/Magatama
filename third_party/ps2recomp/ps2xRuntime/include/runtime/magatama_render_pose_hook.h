#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
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

// Optional replacement of VIF1 chain data as it is gathered from guest RAM:
// chain[offset, offset+bytes) holds the bytes read from physical `source`; the
// hook may rewrite or resize that tail. Tag addressing is unaffected (tags are
// read from guest memory). EE thread, inside the DMA start. Default null.
using MagatamaChainPatch = void (*)(uint32_t source, std::vector<uint8_t>& chain, size_t offset, uint32_t bytes);
extern MagatamaChainPatch g_magatamaChainPatch;
