// magatama-disc-import: the disc importer as a command, for the macOS launcher (which runs it and reads its output).
//   magatama-disc-import <image> <disc-folder>    import; prints "progress <done> <total> <file>" lines, then
//                                                 "result <status> <message>" (status 0 = ok)
//   magatama-disc-import --check <disc-folder>    prints "installed 1" or "installed 0"
#include "magatama_disc_import.h"

#include <cstdio>
#include <cstring>

namespace {
int report(uint64_t done, uint64_t total, const char *file, void *)
{
    static uint64_t last = UINT64_MAX;
    if (done == last && *file == 0)
        return 1;
    last = done;
    std::printf("progress %llu %llu %s\n", (unsigned long long)done, (unsigned long long)total, file);
    std::fflush(stdout);
    return 1;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--check") == 0)
    {
        std::printf("installed %d\n", magatama_disc_installed(argv[2]));
        return 0;
    }
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: magatama-disc-import <image> <disc-folder> | --check <disc-folder>\n");
        return 2;
    }
    char message[512] = {};
    const int status = magatama_disc_import(argv[1], argv[2], report, nullptr, message, sizeof message);
    std::printf("result %d %s\n", status, message);
    return status == MAGATAMA_IMPORT_OK ? 0 : 1;
}
