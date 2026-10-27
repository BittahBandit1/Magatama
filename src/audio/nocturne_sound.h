// Magatama: host re-implementation of Nocturne's IOP sound driver SDFSDMAN.IRX
// ("sdf_sound_driver"), driving the PCSX2 SPU2 core (tools/pcsx2-spu2).
//
// The EE side is the game's own code: sdfSndSendCmd queues 128-byte command
// packets and sdfSndCmdTrans hands each to sceSifSendCmd(cid 0). That call is
// routed here (post). The driver thread then does what the IOP module did:
// loads .SMG banks (from the disc image, or from EE memory for command 0x1B),
// plays their Sony SQ sequences through an HD/BD sampler on the SPU2's 48
// voices, streams APCM files, and keeps the shared work area (bank status,
// voice map, volume tables) up to date in EE memory, which is all the EE ever
// looks at (p2sdLoadCheckMidi reads the bank status records at +0x190).
//
// Behaviour follows the community decompilation of SDFSDMAN
// (github.com/Megami-Decomps/smt3-decomp, src/irx/SDFSDMAN.c); the sequence
// and bank formats follow VGMTrans's SonyPS2 reader. The Sony libraries the
// module links (modmidi, modhsyn) are approximated, not reproduced.
#pragma once

#include <cstdint>

namespace magatama::sound
{
// Start the driver. rdram is the guest's 32 MiB EE memory; discRoot is the
// folder holding DDS3.DDT / DDS3.IMG. Returns false (and logs why) if it
// cannot run; the caller then keeps its old behaviour.
bool start(uint8_t* rdram, const char* discRoot);
bool active();

// sceSifSendCmd(cid 0, packet, size): one sdfSndCmd_t from the EE.
void post(const uint8_t* packet, uint32_t size);

// Diagnostics: print a line per command (MAGATAMA_SOUND_TRACE=1).
void setTrace(bool on);
} // namespace magatama::sound
