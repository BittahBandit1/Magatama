#pragma once

// Signals between the host (presentation, on-screen pad) and the game layer (src/product/patches.cpp), both linked
// into the one app binary.

#include <atomic>

namespace rrv::host {

// F12 (or MAGATAMA_RAM_DUMP_DIR and a request): the game layer writes the 32 MB of guest RAM to
// $MAGATAMA_RAM_DUMP_DIR/ram-<n>.bin at its next frame. A diagnostic for finding game state (memory diffs).
inline std::atomic<int> ramDumpRequests{0};

// The game's main (field) menu: 1 open, 0 closed, -1 unknown (not detected for this game/version). Written by the
// game layer each frame, read by the on-screen pad (it shows a D-pad while the menu is open).
inline std::atomic<int> fieldMenuOpen{-1};

} // namespace rrv::host
