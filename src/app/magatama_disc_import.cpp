// See magatama_disc_import.h. Image access (CHD, .cue/.bin/.iso) is disc_extract's; ZSO and the full ISO9660 tree
// walk are here.
#include "magatama_disc_import.h"

#include "disc_extract.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fukami::disc::detail::SectorSource;
constexpr uint32_t kSector = 2048;

// SLUS_209.11 of Shin Megami Tensei: Nocturne (USA).
#if defined(MAGATAMA_IMPORT_TEST_ELF_SIZE)
constexpr uint64_t kElfSize = MAGATAMA_IMPORT_TEST_ELF_SIZE;
#else
constexpr uint64_t kElfSize = 4551696;
#endif
#if defined(MAGATAMA_IMPORT_TEST_ELF)
constexpr const char *kElfSha256 = MAGATAMA_IMPORT_TEST_ELF; // the tests' synthetic images
#else
constexpr const char *kElfSha256 = "a26e86e59cab447d9b1839ec11ab82b1557230bcc99746b78e3b5d4359dcc6ff";
#endif

uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t(le32(p + 4)) << 32); }

// ---------------------------------------------------------------------------------------------------------------
// ZSO: maxcso's LZ4 ISO. Header "ZISO", header size, uncompressed size (u64), block size, version, index alignment;
// then (blocks + 1) u32 index entries: the block's file offset >> alignment, bit 31 set when stored uncompressed.
// Each block is one raw LZ4 block.

// Raw LZ4 block decoder (the block format: sequences of literals then a back-reference).
bool lz4Block(const uint8_t *src, size_t srcSize, uint8_t *dst, size_t dstSize)
{
    const uint8_t *ip = src, *const iend = src + srcSize;
    uint8_t *op = dst, *const oend = dst + dstSize;
    while (ip < iend)
    {
        const unsigned token = *ip++;
        size_t literals = token >> 4;
        if (literals == 15)
        {
            unsigned byte;
            do
            {
                if (ip >= iend)
                    return false;
                byte = *ip++;
                literals += byte;
            } while (byte == 255);
        }
        if (size_t(iend - ip) < literals || size_t(oend - op) < literals)
            return false;
        std::memcpy(op, ip, literals);
        ip += literals;
        op += literals;
        if (ip >= iend)
            break; // the last sequence has literals only
        if (iend - ip < 2)
            return false;
        const size_t offset = size_t(ip[0]) | (size_t(ip[1]) << 8);
        ip += 2;
        if (offset == 0 || offset > size_t(op - dst))
            return false;
        size_t match = (token & 15u) + 4;
        if ((token & 15u) == 15)
        {
            unsigned byte;
            do
            {
                if (ip >= iend)
                    return false;
                byte = *ip++;
                match += byte;
            } while (byte == 255);
        }
        if (size_t(oend - op) < match)
            return false;
        const uint8_t *from = op - offset;
        for (size_t i = 0; i < match; ++i) // may overlap: byte by byte
            op[i] = from[i];
        op += match;
    }
    return op == oend;
}

class ZsoSource final : public SectorSource
{
public:
    static std::unique_ptr<ZsoSource> open(const fs::path &path)
    {
        auto source = std::unique_ptr<ZsoSource>(new ZsoSource);
        source->m_file.open(path, std::ios::binary);
        uint8_t header[24];
        if (!source->m_file.read(reinterpret_cast<char *>(header), sizeof header) || std::memcmp(header, "ZISO", 4) != 0)
            return nullptr;
        const uint64_t size = le64(header + 8);
        source->m_blockSize = le32(header + 16);
        source->m_align = header[21];
        if (source->m_blockSize < kSector || source->m_blockSize % kSector || source->m_blockSize > (1u << 20) ||
            source->m_align > 16 || size == 0)
            return nullptr;
        const uint64_t blocks = (size + source->m_blockSize - 1) / source->m_blockSize;
        if (blocks > (1u << 26))
            return nullptr;
        source->m_index.resize(size_t(blocks) + 1);
        std::vector<uint8_t> raw(source->m_index.size() * 4);
        source->m_file.seekg(le32(header + 4));
        if (!source->m_file.read(reinterpret_cast<char *>(raw.data()), std::streamsize(raw.size())))
            return nullptr;
        for (size_t i = 0; i < source->m_index.size(); ++i)
            source->m_index[i] = le32(raw.data() + i * 4);
        source->m_sectors = uint32_t(std::min<uint64_t>(size / kSector, UINT32_MAX));
        source->m_block.resize(source->m_blockSize);
        return source;
    }

    uint32_t sectorCount() const override { return m_sectors; }

    bool readSector(uint32_t lba, uint8_t *out) override
    {
        if (lba >= m_sectors)
            return false;
        const uint64_t byte = uint64_t(lba) * kSector;
        const uint64_t block = byte / m_blockSize;
        if (block != m_cached && !load(block))
            return false;
        std::memcpy(out, m_block.data() + (byte % m_blockSize), kSector);
        return true;
    }

private:
    bool load(uint64_t block)
    {
        const uint32_t entry = m_index[size_t(block)], next = m_index[size_t(block) + 1];
        const uint64_t start = uint64_t(entry & 0x7FFFFFFFu) << m_align;
        const uint64_t end = uint64_t(next & 0x7FFFFFFFu) << m_align;
        if (end < start || end - start > m_blockSize * 2u)
            return false;
        m_compressed.resize(size_t(end - start));
        m_file.clear();
        m_file.seekg(std::streamoff(start));
        if (!m_file.read(reinterpret_cast<char *>(m_compressed.data()), std::streamsize(m_compressed.size())))
            return false;
        if (entry & 0x80000000u) // stored
        {
            if (m_compressed.size() < m_blockSize)
                return false;
            std::memcpy(m_block.data(), m_compressed.data(), m_blockSize);
        }
        else if (!lz4Block(m_compressed.data(), m_compressed.size(), m_block.data(), m_blockSize))
            return false;
        m_cached = block;
        return true;
    }

    std::ifstream m_file;
    uint32_t m_blockSize = 0, m_sectors = 0;
    uint8_t m_align = 0;
    std::vector<uint32_t> m_index;
    std::vector<uint8_t> m_block, m_compressed;
    uint64_t m_cached = UINT64_MAX;
};

std::string lowerExtension(const fs::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

// ---------------------------------------------------------------------------------------------------------------
// ISO9660 tree

struct File
{
    std::string path; // relative, '/'-separated, ";1" stripped
    uint32_t lba, size;
};

bool readExtent(SectorSource &src, uint32_t lba, uint32_t size, std::vector<uint8_t> &out)
{
    const uint32_t sectors = (size + kSector - 1) / kSector;
    if (uint64_t(lba) + sectors > src.sectorCount())
        return false;
    out.resize(size_t(sectors) * kSector);
    for (uint32_t i = 0; i < sectors; ++i)
        if (!src.readSector(lba + i, out.data() + size_t(i) * kSector))
            return false;
    return true;
}

// Walks a directory extent, adding its files and descending into its folders. depth guards against loops.
bool walk(SectorSource &src, uint32_t lba, uint32_t size, const std::string &prefix, int depth,
          std::vector<File> &files)
{
    if (depth > 16 || size > (8u << 20))
        return false;
    std::vector<uint8_t> dir;
    if (!readExtent(src, lba, size, dir))
        return false;
    size_t at = 0;
    while (at < size)
    {
        const uint8_t length = dir[at];
        if (length == 0) // records do not cross sectors: continue at the next one
        {
            at = (at / kSector + 1) * kSector;
            continue;
        }
        if (length < 34 || at + length > dir.size())
            return false;
        const uint8_t *record = dir.data() + at;
        const uint8_t nameLength = record[32];
        if (33u + nameLength > length)
            return false;
        std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
        const bool isDir = (record[25] & 2) != 0;
        at += length;
        if (nameLength == 1 && (name[0] == 0 || name[0] == 1))
            continue; // "." and ".."
        if (const size_t semicolon = name.find(';'); semicolon != std::string::npos)
            name.resize(semicolon);
        if (!name.empty() && name.back() == '.')
            name.pop_back();
        if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name == "..")
            return false;
        const std::string path = prefix + name;
        if (isDir)
        {
            if (!walk(src, le32(record + 2), le32(record + 10), path + "/", depth + 1, files))
                return false;
        }
        else
            files.push_back({path, le32(record + 2), le32(record + 10)});
    }
    return true;
}

int fail(char *message, size_t size, int status, const std::string &text)
{
    if (message && size)
    {
        std::strncpy(message, text.c_str(), size - 1);
        message[size - 1] = 0;
    }
    return status;
}

std::string sha256File(const fs::path &path, uint64_t &size)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    fukami::disc::detail::Sha256 sha;
    std::vector<char> buffer(1 << 16);
    size = 0;
    while (in)
    {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        sha.update(buffer.data(), size_t(in.gcount()));
        size += uint64_t(in.gcount());
    }
    return sha.finishHex();
}

} // namespace

extern "C" int magatama_disc_installed(const char *discDir)
{
    uint64_t size = 0;
    return discDir && sha256File(fs::path(discDir) / "SLUS_209.11", size) == kElfSha256 && size == kElfSize;
}

extern "C" int magatama_disc_import(const char *image, const char *discDir, MagatamaImportProgress progress,
                                    void *context, char *message, size_t messageSize)
{
    if (!image || !discDir)
        return fail(message, messageSize, MAGATAMA_IMPORT_CANNOT_OPEN, "No disc image was chosen.");
    const fs::path imagePath(image), dest(discDir);

    // Open the image as 2048-byte sectors.
    std::unique_ptr<SectorSource> source;
    fukami::disc::Result error{fukami::disc::Status::ok, {}};
    const std::string ext = lowerExtension(imagePath);
    if (ext == ".zso")
        source = ZsoSource::open(imagePath);
    else if (ext == ".iso" || ext == ".bin" || ext == ".cue" || ext == ".chd")
        source = fukami::disc::detail::openImage(imagePath, error);
    if (!source)
    {
        // No (or a misleading) extension, as Android's /proc/self/fd paths have: go by the file's magic.
        char magic[8] = {};
        {
            std::ifstream probe(imagePath, std::ios::binary);
            probe.read(magic, sizeof magic);
        }
        if (std::memcmp(magic, "ZISO", 4) == 0)
            source = ZsoSource::open(imagePath);
        else if (std::memcmp(magic, "MComprHD", 8) == 0)
            source = fukami::disc::detail::openChd(imagePath, error);
        else
            source = fukami::disc::detail::openBin(imagePath, error);
        if (!source)
            return fail(message, messageSize, MAGATAMA_IMPORT_CANNOT_OPEN,
                        "This file could not be read as a disc image (.iso, .chd or .zso).");
    }

    // Primary volume descriptor at sector 16, root directory record at offset 156.
    std::vector<uint8_t> pvd(kSector);
    if (source->sectorCount() <= 16 || !source->readSector(16, pvd.data()) || pvd[0] != 1 ||
        std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
        return fail(message, messageSize, MAGATAMA_IMPORT_NOT_ISO9660, "This image has no PS2 disc filesystem.");
    std::vector<File> files;
    if (!walk(*source, le32(pvd.data() + 156 + 2), le32(pvd.data() + 156 + 10), "", 0, files))
        return fail(message, messageSize, MAGATAMA_IMPORT_DAMAGED, "The disc's file list could not be read.");

    // Nocturne (USA): SYSTEM.CNF boots SLUS_209.11.
    auto find = [&](const char *name) {
        return std::find_if(files.begin(), files.end(), [name](const File &f) {
            return f.path.size() == std::strlen(name) &&
                   std::equal(f.path.begin(), f.path.end(), name,
                              [](char a, char b) { return std::toupper((unsigned char)a) == b; });
        });
    };
    const auto cnf = find("SYSTEM.CNF");
    std::vector<uint8_t> cnfData;
    if (cnf == files.end() || cnf->size > 4096 || !readExtent(*source, cnf->lba, cnf->size, cnfData) ||
        std::string(cnfData.begin(), cnfData.begin() + cnf->size).find("SLUS_209.11") == std::string::npos ||
        find("SLUS_209.11") == files.end())
        return fail(message, messageSize, MAGATAMA_IMPORT_WRONG_DISC,
                    "This is not Shin Megami Tensei: Nocturne (USA, SLUS-20911).");

    uint64_t total = 0;
    for (const File &f : files)
        total += f.size;
    std::error_code ec;
    const fs::path parent = dest.parent_path().empty() ? fs::path(".") : dest.parent_path();
    fs::create_directories(parent, ec);
    const fs::space_info space = fs::space(parent, ec);
    if (!ec && space.available < total + (16u << 20))
        return fail(message, messageSize, MAGATAMA_IMPORT_NO_SPACE,
                    "Not enough free space: " + std::to_string((total >> 20) + 16) + " MB needed.");

    // Copy everything into a temporary folder beside the destination.
    const fs::path temp = parent / (dest.filename().string() + ".importing");
    fs::remove_all(temp, ec);
    struct Cleanup
    {
        fs::path path;
        bool keep = false;
        ~Cleanup()
        {
            std::error_code e;
            if (!keep)
                fs::remove_all(path, e);
        }
    } cleanup{temp};
    uint64_t done = 0;
    std::vector<uint8_t> buffer(256 * kSector);
    for (const File &f : files)
    {
        if (progress && !progress(done, total, f.path.c_str(), context))
            return fail(message, messageSize, MAGATAMA_IMPORT_CANCELLED, "Import cancelled.");
        const fs::path out = temp / fs::path(f.path);
        fs::create_directories(out.parent_path(), ec);
        std::FILE *file = std::fopen(out.string().c_str(), "wb");
        if (!file)
            return fail(message, messageSize, MAGATAMA_IMPORT_WRITE_FAILED, "Could not write " + f.path + ".");
        uint64_t left = f.size;
        uint32_t lba = f.lba;
        bool ok = true;
        while (left && ok)
        {
            const uint32_t sectors = uint32_t(std::min<uint64_t>(256, (left + kSector - 1) / kSector));
            for (uint32_t s = 0; s < sectors && ok; ++s)
                ok = source->readSector(lba + s, buffer.data() + size_t(s) * kSector);
            if (!ok)
                break;
            const size_t n = size_t(std::min<uint64_t>(left, uint64_t(sectors) * kSector));
            if (std::fwrite(buffer.data(), 1, n, file) != n)
            {
                std::fclose(file);
                return fail(message, messageSize, MAGATAMA_IMPORT_WRITE_FAILED,
                            "Could not write " + f.path + " (is the storage full?).");
            }
            lba += sectors;
            left -= n;
            done += n;
            if (progress && !progress(done, total, f.path.c_str(), context))
            {
                std::fclose(file);
                return fail(message, messageSize, MAGATAMA_IMPORT_CANCELLED, "Import cancelled.");
            }
        }
        if (std::fclose(file) != 0 || !ok)
            return fail(message, messageSize, ok ? MAGATAMA_IMPORT_WRITE_FAILED : MAGATAMA_IMPORT_DAMAGED,
                        ok ? "Could not write " + f.path + "."
                           : "A read error occurred in the disc image (" + f.path + ").");
    }

    // The game program must be the USA release's, byte for byte.
    if (!magatama_disc_installed(temp.string().c_str()))
        return fail(message, messageSize, MAGATAMA_IMPORT_DAMAGED,
                    "SLUS_209.11 does not match the USA release: the image is damaged, modified or another version.");

    // Swap it in.
    const fs::path old = parent / (dest.filename().string() + ".old");
    fs::remove_all(old, ec);
    if (fs::exists(dest, ec))
    {
        fs::rename(dest, old, ec);
        if (ec)
            return fail(message, messageSize, MAGATAMA_IMPORT_WRITE_FAILED, "Could not replace the old disc folder.");
    }
    fs::rename(temp, dest, ec);
    if (ec)
    {
        std::error_code back;
        fs::rename(old, dest, back);
        return fail(message, messageSize, MAGATAMA_IMPORT_WRITE_FAILED, "Could not move the new disc into place.");
    }
    cleanup.keep = true;
    fs::remove_all(old, ec);
    if (progress)
        progress(total, total, "", context);
    return fail(message, messageSize, MAGATAMA_IMPORT_OK,
                "Imported " + std::to_string(files.size()) + " files (" + std::to_string(total >> 20) + " MB).");
}
