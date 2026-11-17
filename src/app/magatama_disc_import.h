// Magatama disc import: turns the user's own Shin Megami Tensei: Nocturne (USA, SLUS-20911) disc image into the
// disc folder the game reads (SLUS_209.11 and the rest of the disc's files, the ELF's folder being cdrom0:).
//
// Accepts .iso (and .bin/.cue), .chd and .zso images. Every file of the image's ISO9660 tree is copied into a
// temporary folder next to the destination; the disc is accepted only when SYSTEM.CNF boots SLUS_209.11 and that
// file's SHA-256 is the USA release's; then the temporary folder replaces the destination. Nothing game-derived is
// embedded here except that one fingerprint.
//
// The C interface is what the launchers call: Swift on iOS and macOS (through the magatama-disc-import tool on macOS),
// JNI on Android.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum MagatamaImportStatus
{
    MAGATAMA_IMPORT_OK = 0,
    MAGATAMA_IMPORT_CANNOT_OPEN = 1, // unreadable, or not a disc image this importer knows
    MAGATAMA_IMPORT_NOT_ISO9660 = 2, // an image, but no ISO9660 filesystem
    MAGATAMA_IMPORT_WRONG_DISC = 3,  // a PS2 disc, but not Nocturne (USA)
    MAGATAMA_IMPORT_DAMAGED = 4,     // read error, or SLUS_209.11 does not match
    MAGATAMA_IMPORT_NO_SPACE = 5,
    MAGATAMA_IMPORT_WRITE_FAILED = 6,
    MAGATAMA_IMPORT_CANCELLED = 7,
};

// Progress: bytes copied so far, the total, and the file being copied (disc path, e.g. "MOVIE/E616.IPS").
// Return 0 to cancel. Called on the importing thread.
typedef int (*MagatamaImportProgress)(uint64_t done, uint64_t total, const char *file, void *context);

// Imports image into discDir (replacing what is there only once the new disc is complete and verified). message
// receives a short user-facing sentence (may be null). Blocking; run it off the UI thread.
int magatama_disc_import(const char *image, const char *discDir, MagatamaImportProgress progress, void *context,
                         char *message, size_t messageSize);

// 1 when discDir holds the right SLUS_209.11 (size and SHA-256), else 0. Hashes 4.5 MB.
int magatama_disc_installed(const char *discDir);

#ifdef __cplusplus
}
#endif
