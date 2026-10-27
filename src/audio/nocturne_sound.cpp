// Magatama: host re-implementation of Nocturne's IOP sound driver (SDFSDMAN.IRX).
// See nocturne_sound.h for the design. Structure of this file:
//   1. Disc access (DDS3.DDT / DDS3.IMG)
//   2. Bank formats: the .SMG header, Sony HD (instruments) and SQ (sequences)
//   3. The SPU2 core and the voice synthesiser (modhsyn's job)
//   4. The sequencer: 16 slots of Sony SQ playback (modmidi's job)
//   5. The driver proper: commands, banks, slots, fades, the shared work area
//   6. APCM streams, decoded in software and mixed into the output
//   7. The driver thread, output ring and public API
#include "nocturne_sound.h"

#include "rrv_pcsx2_spu2.h"
#include "rrv_sdl_audio.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace magatama::sound
{
namespace
{
// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
inline void wr32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
inline uint32_t physical(uint32_t a) { return a & 0x01FFFFFFu; }

std::atomic<bool> g_trace{false};
#define NSND_LOG(...) std::fprintf(stderr, "[sound] " __VA_ARGS__)
#define NSND_TRACE(...)                                                                                     \
    do                                                                                                       \
    {                                                                                                        \
        if (g_trace.load(std::memory_order_relaxed))                                                        \
            std::fprintf(stderr, "[sound] " __VA_ARGS__);                                                    \
    } while (0)

// EE addresses (SLUS_209.11; names from the decomp's symbol map).
constexpr uint32_t kEeWorkArea = 0x593880u;     // sdfSndMap: the EE copy of the shared work area
constexpr uint32_t kEePcmName = 0x529BE8u;      // file name p2sdLoadPcmFile copied for command 0x0F
constexpr uint32_t kEePcmState = 0x5519C8u;     // p2sdPcmState: 1 while a file stream is open
constexpr uint32_t kEeMemBank = 0x593F20u;      // {size, pending, data}: p2sdSetPcmBuffer's bank image
constexpr uint32_t kWorkAreaSize = 1440;

// Work-area layout (sdfSndWorkArea_t).
constexpr uint32_t kWaBanks = 0x10;      // 8 x {u8, u8, u16 size, u32 waveSize, u32 data, u32 spuAddr}
constexpr uint32_t kWaStat = 0x190;      // 7 x {s32 id, u16 kind, u16 pad}
constexpr uint32_t kWaEeAddr = 0x1C8;
constexpr uint32_t kWaErrCount = 0x1D8;
constexpr uint32_t kWaErrCode = 0x1DC;
constexpr uint32_t kWaVolTable = 0x1E0;  // 7 x 128
constexpr uint32_t kWaVoiceMap = 0x560;  // 2 x 32

// Bank slots: SPU2 byte address and capacity (sdfSndBankSizeTbl / sdfSndBankAddrTbl in the IRX).
constexpr uint32_t kBankSpu[7] = {0x20000, 0x48000, 0xB1800, 0x11B000, 0x180000, 0x1B1000, 0x1D2000};
constexpr uint32_t kBankSpuCap[7] = {0x28000, 0x69800, 0x69800, 0x65000, 0x31000, 0x21000, 0xE000};

constexpr int kTickHz = 240;                  // the IRX's hardware timer (4167 us)
constexpr int kFrameRate = 48000;
constexpr int kFramesPerTick = kFrameRate / kTickHz;  // 200
constexpr uint64_t kCyclesPerFrame = 768;     // IOP cycles per 48 kHz frame

// ---------------------------------------------------------------------------
// 1. Disc access
// ---------------------------------------------------------------------------
class DiscFs
{
public:
    bool open(const std::string& root)
    {
        std::ifstream ddt(root + "/DDS3.DDT", std::ios::binary);
        if (!ddt)
            return false;
        ddt_.assign(std::istreambuf_iterator<char>(ddt), std::istreambuf_iterator<char>());
        img_.open(root + "/DDS3.IMG", std::ios::binary);
        if (!img_ || ddt_.size() < 12)
            return false;
        const int32_t n = int32_t(rd32(&ddt_[8]));
        if (n >= 0)
            return false;
        walk(rd32(&ddt_[4]), uint32_t(-n), "", 0);
        return !files_.empty();
    }

    std::optional<std::vector<uint8_t>> read(std::string path)
    {
        // Accept "cdrom0:\\SOUND\\X.ADB;1", "sound/x.adb", "/sound/x.adb", "x.adb".
        if (auto c = path.find(':'); c != std::string::npos)
            path = path.substr(c + 1);
        if (auto s = path.find(';'); s != std::string::npos)
            path.resize(s);
        for (auto& ch : path)
            ch = ch == '\\' ? '/' : char(std::tolower(uint8_t(ch)));
        if (path.empty() || path[0] != '/')
            path = "/" + path;
        auto it = files_.find(path);
        if (it == files_.end())
            it = files_.find("/sound" + path);
        if (it == files_.end())
            return std::nullopt;
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint8_t> out(it->second.size);
        img_.clear();
        img_.seekg(std::streamoff(it->second.sector) * 2048);
        img_.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
        if (!img_)
            return std::nullopt;
        return out;
    }

private:
    struct Entry
    {
        uint32_t sector, size;
    };
    std::string name(uint32_t off) const
    {
        std::string s;
        while (off < ddt_.size() && ddt_[off])
            s.push_back(char(std::tolower(ddt_[off++])));
        return s;
    }
    void walk(uint32_t off, uint32_t count, const std::string& dir, int depth)
    {
        if (depth > 16)
            return;
        for (uint32_t k = 0; k < count && off + 12 * k + 12 <= ddt_.size(); ++k)
        {
            const uint8_t* e = &ddt_[off + 12 * k];
            const std::string n = name(rd32(e));
            const int32_t size = int32_t(rd32(e + 8));
            if (size < 0)
                walk(rd32(e + 4), uint32_t(-size), n.empty() ? dir : dir + "/" + n, depth + 1);
            else
                files_[dir + "/" + n] = {rd32(e + 4), uint32_t(size)};
        }
    }
    std::vector<uint8_t> ddt_;
    std::ifstream img_;
    std::mutex mutex_;
    std::map<std::string, Entry> files_;
};

// ---------------------------------------------------------------------------
// 2. Bank formats
// ---------------------------------------------------------------------------
// Sony HD: Vers / Head / Vagi / Smpl / Sset / Prog.
struct VagInfo
{
    uint32_t offset = 0;
    uint16_t rate = 44100;
    uint8_t loop = 0;
};
struct SampleParam
{
    uint16_t vag = 0;
    uint8_t velLo = 0, velHi = 127;
    uint8_t baseNote = 60;
    int8_t detune = 0;
    uint8_t pan = 64;
    uint8_t volume = 127;
    uint16_t adsr1 = 0x80FF, adsr2 = 0x5FC0;
    uint8_t spuAttr = 3;
};
struct SampleSet
{
    uint8_t velLo = 0, velHi = 127;
    std::vector<uint16_t> samples;
};
struct Split
{
    uint16_t sampleSet = 0;
    uint8_t lo = 0, hi = 127;
    uint16_t bendLo = 256, bendHi = 256;
    uint8_t volume = 127;
    uint8_t pan = 64;
    int8_t transpose = 0, detune = 0;
};
struct Program
{
    bool valid = false;
    uint8_t volume = 127;
    uint8_t pan = 64;
    int8_t transpose = 0, detune = 0;
    std::vector<Split> splits;
};
struct InstrumentSet
{
    bool valid = false;
    std::vector<VagInfo> vags;
    std::vector<SampleParam> samples;
    std::vector<SampleSet> sets;
    std::vector<Program> programs;

    bool parse(const uint8_t* p, size_t n)
    {
        *this = {};
        if (n < 0x50 || std::memcmp(p + 16, "IECSdaeH", 8) != 0)
            return false;
        const uint8_t* head = p + 16;
        const uint32_t progAt = rd32(head + 0x14), setAt = rd32(head + 0x18), smpAt = rd32(head + 0x1C),
                       vagAt = rd32(head + 0x20);
        auto table = [&](uint32_t at, auto&& each) {
            if (at == 0xFFFFFFFFu || at + 16 > n)
                return;
            const uint32_t count = rd32(p + at + 12) + 1;
            if (count > 4096 || at + 16 + 4 * count > n)
                return;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t off = rd32(p + at + 16 + 4 * i);
                if (off == 0xFFFFFFFFu || at + off >= n)
                    each(i, nullptr, size_t(0));
                else
                    each(i, p + at + off, n - (at + off));
            }
        };
        table(vagAt, [&](uint32_t, const uint8_t* e, size_t left) {
            VagInfo v;
            if (e && left >= 8)
                v = {rd32(e), rd16(e + 4), e[6]};
            vags.push_back(v);
        });
        table(smpAt, [&](uint32_t, const uint8_t* e, size_t left) {
            SampleParam s;
            if (e && left >= 42)
            {
                s.vag = rd16(e);
                s.velLo = e[2];
                s.velHi = e[4];
                s.baseNote = e[11];
                s.detune = int8_t(e[12]);
                s.pan = e[13];
                s.volume = e[16];
                s.adsr1 = rd16(e + 18);
                s.adsr2 = rd16(e + 20);
                s.spuAttr = e[41];
            }
            samples.push_back(s);
        });
        table(setAt, [&](uint32_t, const uint8_t* e, size_t left) {
            SampleSet s;
            if (e && left >= 4)
            {
                s.velLo = e[1];
                s.velHi = e[2];
                for (uint32_t k = 0; k < e[3] && 4 + 2 * k + 2 <= left; ++k)
                    s.samples.push_back(rd16(e + 4 + 2 * k));
            }
            sets.push_back(std::move(s));
        });
        table(progAt, [&](uint32_t, const uint8_t* e, size_t left) {
            Program pr;
            if (e && left >= 36)
            {
                pr.valid = true;
                const uint32_t splitAt = rd32(e);
                const uint8_t nSplit = e[4], size = e[5];
                pr.volume = e[6];
                pr.pan = e[7];
                pr.transpose = int8_t(e[8]);
                pr.detune = int8_t(e[9]);
                for (uint32_t k = 0; k < nSplit; ++k)
                {
                    const size_t at = splitAt + size_t(k) * size;
                    if (size < 20 || at + 20 > left)
                        break;
                    const uint8_t* s = e + at;
                    Split sp;
                    sp.sampleSet = rd16(s);
                    sp.lo = s[2];
                    sp.hi = s[4];
                    sp.bendLo = rd16(s + 6);
                    sp.bendHi = rd16(s + 8);
                    sp.volume = s[16];
                    sp.pan = s[17];
                    sp.transpose = int8_t(s[18]);
                    sp.detune = int8_t(s[19]);
                    pr.splits.push_back(sp);
                }
            }
            programs.push_back(std::move(pr));
        });
        valid = !programs.empty();
        return valid;
    }
};

// Sony SQ: Vers / Sequ / Midi. A sequence is a Midi block: u32 data offset, u16 ticks per quarter.
struct Sequence
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint16_t ppqn = 480;
};
std::optional<Sequence> findSequence(const std::vector<uint8_t>& sq, uint32_t index)
{
    // Chunks: walk "IECS" headers to the Midi chunk.
    size_t at = 0;
    while (at + 16 <= sq.size() && std::memcmp(&sq[at], "IECS", 4) == 0)
    {
        const uint32_t size = rd32(&sq[at + 8]);
        if (std::memcmp(&sq[at + 4], "idiM", 4) == 0)
        {
            const uint32_t max = rd32(&sq[at + 12]);
            if (index > max || at + 16 + 4 * (index + 1) > sq.size())
                return std::nullopt;
            const uint32_t off = rd32(&sq[at + 16 + 4 * index]);
            if (off == 0xFFFFFFFFu || at + off + 6 > sq.size())
                return std::nullopt;
            const size_t block = at + off;
            const uint32_t dataOff = rd32(&sq[block]);
            if (block + dataOff >= sq.size())
                return std::nullopt;
            Sequence s;
            s.ppqn = rd16(&sq[block + 4]);
            if (!s.ppqn)
                s.ppqn = 480;
            s.data = &sq[block + dataOff];
            s.size = sq.size() - (block + dataOff);
            return s;
        }
        if (size < 16)
            break;
        at += size;
    }
    return std::nullopt;
}

// A loaded bank (one of the seven bank slots).
struct Bank
{
    bool loaded = false;
    int32_t id = -1;
    uint32_t kindPacked = 0;
    std::vector<uint8_t> hd;   // instrument header ("sequence data" in the driver's naming)
    std::vector<uint8_t> sq;   // sequences ("program data" in the driver's naming)
    InstrumentSet instruments;
    uint32_t waveSize = 0;
};

// ---------------------------------------------------------------------------
// 3. SPU2 and the synthesiser
// ---------------------------------------------------------------------------
// libsd reverb modes: work-area sizes (x8 bytes) and the 32 register values of each preset
// (off, room, studio S/M/L, hall, space echo, echo, delay, pipe), as libsd/libspu2 write them.
constexpr uint32_t kRevSize[10] = {0x2, 0x4D8, 0x3E8, 0x908, 0xDFC, 0x15BC, 0x1ED8, 0x3008, 0x3008, 0x780};
constexpr uint16_t kRevPreset[10][32] = {
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x007d, 0x005b, 0x6d80, 0x54b8, 0xbed0, 0x0000, 0x0000, 0xba80, 0x5800, 0x5300, 0x04d6, 0x0333, 0x03f0, 0x0227, 0x0374, 0x01ef,
     0x0336, 0x01b7, 0x0335, 0x01b6, 0x0334, 0x01b5, 0x0334, 0x01b5, 0x0334, 0x01b5, 0x01b4, 0x0136, 0x00b8, 0x005c, 0x8000, 0x8000},
    {0x0033, 0x0025, 0x70f0, 0x4fa8, 0xbce0, 0x4410, 0xc0f0, 0x9c00, 0x5280, 0x4ec0, 0x03e4, 0x031b, 0x03a4, 0x02af, 0x0372, 0x0266,
     0x031c, 0x025d, 0x025c, 0x018e, 0x022f, 0x0135, 0x01d2, 0x00b7, 0x018f, 0x00b5, 0x00b4, 0x0080, 0x004c, 0x0026, 0x8000, 0x8000},
    {0x00b1, 0x007f, 0x70f0, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0xb4c0, 0x5280, 0x4ec0, 0x0904, 0x076b, 0x0824, 0x065f, 0x07a2, 0x0616,
     0x076c, 0x05ed, 0x05ec, 0x042e, 0x050f, 0x0305, 0x0462, 0x02b7, 0x042f, 0x0265, 0x0264, 0x01b2, 0x0100, 0x0080, 0x8000, 0x8000},
    {0x00e3, 0x00a9, 0x6f60, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0xa680, 0x5680, 0x52c0, 0x0dfb, 0x0b58, 0x0d09, 0x0a3c, 0x0bd9, 0x0973,
     0x0b59, 0x08da, 0x08d9, 0x05e9, 0x07ec, 0x04b0, 0x06ef, 0x03d2, 0x05ea, 0x031d, 0x031c, 0x0238, 0x0154, 0x00aa, 0x8000, 0x8000},
    {0x01a5, 0x0139, 0x6000, 0x5000, 0x4c00, 0xb800, 0xbc00, 0xc000, 0x6000, 0x5c00, 0x15ba, 0x11bb, 0x14c2, 0x10bd, 0x11bc, 0x0dc1,
     0x11c0, 0x0dc3, 0x0dc0, 0x09c1, 0x0bc4, 0x07c1, 0x0a00, 0x06cd, 0x09c2, 0x05c1, 0x05c0, 0x041a, 0x0274, 0x013a, 0x8000, 0x8000},
    {0x033d, 0x0231, 0x7e00, 0x5000, 0xb400, 0xb000, 0x4c00, 0xb000, 0x6000, 0x5400, 0x1ed6, 0x1a31, 0x1d14, 0x183b, 0x1bc2, 0x16b2,
     0x1a32, 0x15ef, 0x15ee, 0x1055, 0x1334, 0x0f2d, 0x11f6, 0x0c5d, 0x1056, 0x0ae1, 0x0ae0, 0x07a2, 0x0464, 0x0232, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7fff, 0x7fff, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1ffd, 0x0ffd, 0x1009, 0x0009, 0x0000, 0x0000,
     0x1009, 0x0009, 0x1fff, 0x1fff, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7fff, 0x7fff, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1ffd, 0x0ffd, 0x1009, 0x0009, 0x0000, 0x0000,
     0x1009, 0x0009, 0x1fff, 0x1fff, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0017, 0x0013, 0x70f0, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0x8500, 0x5f80, 0x54c0, 0x0371, 0x02af, 0x02e5, 0x01df, 0x02b0, 0x01d7,
     0x0358, 0x026a, 0x01d6, 0x011e, 0x012d, 0x00b1, 0x011f, 0x0059, 0x01a0, 0x00e3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
};

class Spu
{
public:
    bool create()
    {
        rrv_spu2_config cfg{};
        cfg.threaded = 0;
        cfg.output_ring_frames = 16384;
        s_ = rrv_spu2_create(&cfg);
        return s_ != nullptr;
    }
    ~Spu()
    {
        if (s_)
            rrv_spu2_destroy(s_);
    }
    void W(uint32_t off, uint16_t v) { rrv_spu2_write16(s_, t_, 0x1F900000u | off, v); }
    uint16_t R(uint32_t off) { return rrv_spu2_read16(s_, t_, 0x1F900000u | off); }
    // Sound-memory upload through core 0's DMA, as libsd does. The core times the transfer like the
    // hardware, so each piece is waited for (DMA4 done) before the next starts; a transfer started
    // while one is still running overwrites it. `wait` keeps time (and the audio) moving meanwhile.
    template <typename Wait>
    void upload(uint32_t byteAddr, const uint8_t* data, size_t bytes, Wait&& wait)
    {
        std::vector<uint16_t> words((bytes + 1) / 2);
        std::memcpy(words.data(), data, bytes);
        size_t done = 0;
        while (done < words.size())
        {
            const uint32_t n = uint32_t(std::min<size_t>(words.size() - done, 0x40000));
            dmaDone_ = false;
            rrv_spu2_dma_write(s_, t_, 0, (byteAddr / 2) + uint32_t(done), words.data() + done, n);
            done += n;
            for (int guard = 0; !dmaDone_ && guard < 100000; ++guard)
                wait();
        }
    }
    void upload(uint32_t byteAddr, const uint8_t* data, size_t bytes)
    {
        upload(byteAddr, data, bytes, [this] { advance(32); });
    }
    void advance(uint32_t frames)
    {
        t_ += uint64_t(frames) * kCyclesPerFrame;
        rrv_spu2_advance(s_, t_);
        rrv_spu2_event ev[8];
        size_t n;
        do
        {
            n = rrv_spu2_sync(s_, t_, ev, 8);
            for (size_t i = 0; i < n; ++i)
                if (ev[i].kind == RRV_SPU2_EVENT_DMA4_DONE)
                    dmaDone_ = true;
        } while (n == 8);
    }
    size_t pull(float* stereo, size_t frames) { return rrv_spu2_pull_output(s_, stereo, frames); }

    static uint32_t C(int core) { return core ? 0x400u : 0u; }
    static uint32_t X(int core) { return core ? 0x28u : 0u; }

    void init()
    {
        // Reset and enable both cores (the order libsd's sceSdInit and RR5's driver use).
        for (int c = 0; c < 2; ++c)
        {
            W(0x19A + C(c), 0);
            advance(4);
            W(0x19A + C(c), 0x8000);
            W(0x1A4 + C(c), 0xFFFF);
            W(0x1A6 + C(c), 0x00FF);
            for (uint32_t r : {0x180u, 0x182u, 0x184u, 0x186u})
                W(r + C(c), 0);
        }
        // A silent looping block for idle voices.
        static const uint8_t silent[32] = {0x00, 0x07};
        upload(0x5000, silent, sizeof silent);
        for (int c = 0; c < 2; ++c)
            for (int v = 0; v < 24; ++v)
            {
                const uint32_t r = C(c) + 0x10 * v, a = C(c) + 0x1C0 + 12 * v;
                W(r + 0, 0);
                W(r + 2, 0);
                W(r + 4, 0x1000);
                W(r + 6, 0);
                W(r + 8, 0);
                W(a + 0, 0);
                W(a + 2, 0x2800);
            }
        W(0x19A, 0xC000);
        W(0x59A, 0xC001);
        for (int c = 0; c < 2; ++c)
        {
            // Dry mix for every voice; the effect sends are set per voice when reverb is on.
            W(0x188 + C(c), 0xFFFF);
            W(0x18A + C(c), 0x00FF);
            W(0x190 + C(c), 0xFFFF);
            W(0x192 + C(c), 0x00FF);
            W(0x18C + C(c), 0);
            W(0x18E + C(c), 0);
            W(0x194 + C(c), 0);
            W(0x196 + C(c), 0);
            W(0x198 + C(c), c ? 0x0FFC : 0x0FFF);  // core 1: core 0's output dry only
            W(0x760 + X(c), 0x3FFF);   // MVOL (sdfSndInitSpu2: 0x3FFF)
            W(0x762 + X(c), 0x3FFF);
            W(0x764 + X(c), 0);        // EVOL: reverb not modelled yet
            W(0x766 + X(c), 0);
        }
        W(0x790, 0x7FFF);              // core 1 input from core 0
        W(0x792, 0x7FFF);
        W(0x33C, 0x000F);              // EEA as the IRX sets it (0x1FFFFF / 0x1FFFF)
        W(0x73C, 0x0000);
    }
    // sceSdSetEffectAttr + SD_CORE_EFFECT_ENABLE, as sdfSndInitSpu2 does for each core:
    // the effect area ends at EEA (0x1FFFFF core 0, 0x1FFFF core 1), EVOL 0x4FFF.
    void initReverb(int c, uint32_t mode)
    {
        if (mode > 9)
            return;
        const uint32_t eea = c ? 0x1FFFFu : 0x1FFFFFu;
        const uint32_t size = kRevSize[mode] * 8;
        const uint32_t esa = eea - (size - 2);
        W(0x33C + C(c), uint16_t(eea >> 17));
        W(0x2E0 + C(c), uint16_t(esa >> 17));
        W(0x2E2 + C(c), uint16_t(esa >> 1));
        const auto units8 = [&](uint32_t off, uint32_t v) {
            W(off, uint16_t((v & 0x3FFFFFFFu) >> 14));
            W(off + 2, uint16_t(v << 2));
        };
        for (uint32_t n = 0; n < 32; ++n)
        {
            const uint32_t v = uint32_t(int32_t(int16_t(kRevPreset[mode][n])));
            if (n < 2)
                units8(0x2E4 + 4 * n + C(c), v);
            else if (n < 10)
                W(0x774 + 2 * (n - 2) + X(c), uint16_t(v));
            else if (n < 30)
                units8(0x2EC + 4 * (n - 10) + C(c), v);
            else
                W(0x784 + 2 * (n - 30) + X(c), uint16_t(v));
        }
        // Clear the work area (the mode's 0x100 flag).
        const uint32_t from = esa & ~1u;
        std::vector<uint8_t> zero(eea + 1 - from, 0);
        upload(from, zero.data(), zero.size());
        W(0x19A + C(c), uint16_t(R(0x19A + C(c)) | 0x80));
        W(0x764 + X(c), 0x4FFF);
        W(0x766 + X(c), 0x4FFF);
    }
    void setMaster(uint16_t v)
    {
        for (int c = 0; c < 2; ++c)
        {
            W(0x760 + X(c), v);
            W(0x762 + X(c), v);
        }
    }

private:
    rrv_spu2* s_ = nullptr;
    uint64_t t_ = 0;
    bool dmaDone_ = false;
};

struct Voice
{
    bool busy = false;      // allocated to a note (keyed or releasing)
    bool released = false;
    int slot = -1, channel = 0, note = 0;
    uint64_t serial = 0;
    uint64_t keyTick = 0;
    // What the volume and pitch are made of, so controllers can update them.
    float baseAmp = 0;      // program * split * sample * velocity
    int basePan = 64;       // sample + split + program pans, 64 = centre
    double basePitch = 0;   // SPU2 pitch at no bend (0x1000 = 48 kHz)
    float bendRange = 2;    // semitones
};

// ---------------------------------------------------------------------------
// 4. Sequencer state
// ---------------------------------------------------------------------------
struct Channel
{
    uint8_t program = 0, bank = 0;
    uint8_t volume = 127, expression = 127, pan = 64;
    int16_t bend = 0;       // -8192..8191
    bool sustain = false;
};

struct Slot
{
    // sdfSndSlot_t
    int32_t id = -1;
    int32_t group = 0;
    int32_t vol = 32512;
    int32_t posX = 0, posY = 0, posZ = 0;
    bool is3d = false;
    int32_t radius = 0;
    int16_t volStep = 0;
    bool fadeIn = false;
    uint32_t serial = 0;
    int32_t location = -1;
    // modmidi
    bool playing = false;   // the "flags & 2" the driver tests
    int bank = -1;
    Sequence seq;
    size_t pos = 0;
    bool skipDelta = false;
    uint8_t running = 0;
    uint32_t tempo = 500000;
    double pendingTicks = 0;
    uint32_t nextDelta = 0;
    // Sony SQ loops: NRPN 99 = 0 (start) / 1 (end), then data entry 6 = the loop's number;
    // at the end, data entry LSB 38 = repeat count, 0 forever.
    std::array<size_t, 8> loopPos{};
    std::array<bool, 8> loopSkip{};
    std::array<int, 8> loopLeft{};
    int loopEndId = -1;
    uint8_t nrpn = 0x7F;
    uint8_t masterVol = 127;  // sceMidi_MidiSetVolume(..., 255, v)
    uint32_t midiLocation = 0;
    std::array<Channel, 16> ch{};
};

// ---------------------------------------------------------------------------
// 5. The driver
// ---------------------------------------------------------------------------
class Driver
{
public:
    bool start(uint8_t* rdram, const std::string& discRoot)
    {
        rdram_ = rdram;
        if (!disc_.open(discRoot))
        {
            NSND_LOG("cannot read DDS3.DDT/DDS3.IMG under %s; sound driver off\n", discRoot.c_str());
            return false;
        }
        if (!spu_.create())
        {
            NSND_LOG("SPU2 core unavailable; sound driver off\n");
            return false;
        }
        spu_.init();
        if (const char* r = std::getenv("MAGATAMA_SOUND_REVERB"); !(r && r[0] == '0'))
        {
            // sdfSndEffectAttr in the IRX: core 0 room, core 1 hall.
            spu_.initReverb(0, 1);
            spu_.initReverb(1, 5);
            reverbOn_ = true;
        }
        rrv::audio::initialize();
        audioReady_ = rrv::audio::ready();
        if (audioReady_)
            rrv::audio::setStreamSource(&Driver::pullThunk, this);
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        NSND_LOG("SDFSDMAN host driver started (audio %s)\n", audioReady_ ? "on" : "unavailable, running silent");
        return true;
    }

    void post(const uint8_t* packet, uint32_t size)
    {
        if (size < 0x14 || size > 128)
            return;
        std::array<uint8_t, 128> p{};
        std::memcpy(p.data(), packet, size);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.push_back(p);
        }
        wake_.notify_one();
    }

private:
    // ---- EE memory --------------------------------------------------------
    uint32_t eeRead32(uint32_t a) const { return rd32(rdram_ + physical(a)); }
    void eeWrite32(uint32_t a, uint32_t v) { wr32(rdram_ + physical(a), v); }
    std::string eeString(uint32_t a, size_t max = 255) const
    {
        std::string s;
        for (size_t i = 0; i < max; ++i)
        {
            const char c = char(rdram_[physical(a + uint32_t(i))]);
            if (!c)
                break;
            s.push_back(c);
        }
        return s;
    }
    // p2sdIrxSpuIopMap: the IOP DMAs the whole work area to the EE.
    void pushWorkArea()
    {
        if (!eeWorkArea_)
            return;
        std::memcpy(rdram_ + physical(eeWorkArea_), wa_.data(), kWorkAreaSize);
    }
    void pushVoiceMap()
    {
        if (!eeWorkArea_)
            return;
        std::memcpy(rdram_ + physical(eeWorkArea_) + kWaVoiceMap, wa_.data() + kWaVoiceMap, 64);
    }
    int32_t statId(int bank) const { return int32_t(rd32(&wa_[kWaStat + 8 * bank])); }
    void setStat(int bank, int32_t id, uint32_t kind)
    {
        wr32(&wa_[kWaStat + 8 * bank], uint32_t(id));
        wa_[kWaStat + 8 * bank + 4] = uint8_t(kind);
        wa_[kWaStat + 8 * bank + 5] = uint8_t(kind >> 8);
    }
    void postError(int code)
    {
        wr32(&wa_[kWaErrCount], rd32(&wa_[kWaErrCount]) + 1);
        wr32(&wa_[kWaErrCode], uint32_t(code));
        NSND_TRACE("error %d\n", code);
        pushWorkArea();
    }

    // ---- banks --------------------------------------------------------------
    int findBankById(int32_t idPacked) const
    {
        if (idPacked < 0)
            return -1;
        const int32_t id = idPacked >> 16;
        for (int i = 0; i < 7; ++i)
            if (statId(i) == id)
                return i;
        return -1;
    }
    bool bankLoaded(int kind) const
    {
        // sdfSndIsBankLoaded reads stat[kind + i] for i < 7; in practice only stat[kind] matters.
        return kind < 7 && statId(kind) >= 0;
    }
    void stopByGroup(int group)
    {
        for (int i = 0; i < 16; ++i)
            if (slots_[i].id >= 0 && slots_[i].group == group && slots_[i].playing)
            {
                if (i < 3)
                    saveResume(i);
                stopSlot(i);
            }
    }
    void unloadBank(int bank)
    {
        if (bank < 0 || bank > 6)
            return;
        stopByGroup(bank);
        banks_[bank] = {};
        setStat(bank, -1, rd16(&wa_[kWaStat + 8 * bank + 4]));
        if (bank == 1)
            bank1Loaded_ = false;
        if (bank == 3)
            bank3Loaded_ = false;
    }
    std::optional<std::vector<uint8_t>> readBankImage(const std::string& path, bool fromEe)
    {
        if (!fromEe)
            return disc_.read(path);
        // Command 0x1B: the bank image sits in EE memory (p2sdSetPcmBuffer).
        const uint32_t size = eeRead32(kEeMemBank), data = eeRead32(kEeMemBank + 8);
        if (!data || size < 0xB0 || size > 0x01000000 || physical(data) + size > 0x02000000u)
            return std::nullopt;
        return std::vector<uint8_t>(rdram_ + physical(data), rdram_ + physical(data) + size);
    }
    // sdfSndLoadSmg
    int loadSmg(const std::string& path, bool fromEe)
    {
        auto img = readBankImage(path, fromEe);
        if (!img)
        {
            NSND_LOG("bank %s not found\n", fromEe ? "(EE memory)" : path.c_str());
            postError(1);
            return -1;
        }
        const auto& f = *img;
        if (f.size() < 0xB0)
        {
            postError(2);
            return -1;
        }
        const uint32_t hdSize = rd32(&f[0x14]), waveSize = rd32(&f[0x1C]), sqSize = rd32(&f[0x24]);
        const uint32_t idPacked = rd32(&f[0x28]), kindPacked = rd32(&f[0x2C]);
        const int32_t id = int32_t(idPacked >> 16);
        if (findBankById(int32_t(idPacked)) >= 0)
        {
            NSND_TRACE("bank %04x already resident\n", unsigned(id));
            return -1;
        }
        if (!hdSize || !waveSize || 0xB0ull + hdSize + waveSize + sqSize > f.size())
        {
            postError(5);
            return -1;
        }
        int bank;
        const auto al = [](uint32_t v) { return (v + 15) & ~15u; };
        switch (kindPacked >> 24)
        {
        case 0:
            bank = 0;
            break;
        case 1:
        case 4:
            bank = 1;
            if (waveSize > 0x69800 || al(hdSize) + al(sqSize) >= 0x5401)
            {
                unloadBank(2);
                bank1Loaded_ = true;
                break;
            }
            if (!bankLoaded(1))
                break;
            bank = 2;
            if (!bankLoaded(2))
                break;
            {
                int inUse = 0;
                for (auto& s : slots_)
                    if (s.id >= 0 && s.group < 0)
                        inUse |= (s.group & 2) ? 2 : 1;
                bank = 1;
                if (inUse == 1 || (inUse != 2 && bankAltSlot_ == 1))
                    bank = 2;
                bankAltSlot_ = bank;
            }
            break;
        case 2:
            bank3Loaded_ = waveSize > 0x65000 || al(hdSize) + al(sqSize) > 0x11C20;
            if (bank3Loaded_)
                unloadBank(4);
            bank = 3;
            break;
        case 3:
            bank = 4;
            if (bank3Loaded_)
                unloadBank(3);
            break;
        case 5:
            bank = 5;
            break;
        case 6:
            bank = 6;
            break;
        default:
            return -1;
        }
        unloadBank(bank);
        int kind = bank;
        if (bank == 2 && bank1Loaded_)
        {
            unloadBank(1);
            kind = 2;
        }
        // Volume table for this kind.
        if (kind <= 6)
        {
            const int count = kind == 0 ? 32 : (kind == 5 || kind == 6) ? 2 : kind == 4 ? 0 : 128;
            for (int i = 0; i < count; ++i)
                volTable_[kind][i] = f[0x30 + i];
        }
        Bank& b = banks_[bank];
        b.hd.assign(f.begin() + 0xB0, f.begin() + 0xB0 + hdSize);
        if (bank != 4)
            b.sq.assign(f.begin() + 0xB0 + hdSize + waveSize, f.begin() + 0xB0 + hdSize + waveSize + sqSize);
        b.instruments.parse(b.hd.data(), b.hd.size());
        b.waveSize = waveSize;
        b.kindPacked = kindPacked;
        b.id = id;
        // Wave data straight into SPU2 RAM. A bank larger than its slot runs into the next one, as on the PS2.
        const uint32_t spu = kBankSpu[bank];
        const uint32_t room = 0x1E0000u - spu;
        spu_.upload(spu, &f[0xB0 + hdSize], std::min(waveSize, room), [this] { renderFrames(32); });
        b.loaded = true;
        // Work area: bank record and status.
        uint8_t* rec = &wa_[kWaBanks + 16 * bank];
        rec[2] = uint8_t(al(hdSize) + (bank == 4 ? 0 : al(sqSize)));
        rec[3] = uint8_t((al(hdSize) + (bank == 4 ? 0 : al(sqSize))) >> 8);
        wr32(rec + 4, waveSize);
        wr32(rec + 12, spu);
        setStat(bank, id, kindPacked);
        pushWorkArea();
        NSND_TRACE("bank %04x kind %u -> slot %d (hd %u, wave %u, sq %u, %zu programs)\n", unsigned(id),
                   unsigned(kindPacked >> 24), bank, hdSize, waveSize, sqSize, b.instruments.programs.size());
        return 0;
    }

    // ---- slots --------------------------------------------------------------
    const uint8_t* volTableFor(int kind) const
    {
        if (kind < 0 || kind == 4 || kind > 6)
            return nullptr;
        return volTable_[kind].data();
    }
    // The IRX indexes every table with id & 0x7F; the 32-entry sequence table runs on into the BGM table.
    int volEntry(int kind, int index) const
    {
        if (kind < 0 || kind == 4 || kind > 6)
            return 127;
        if (kind == 0 && index >= 32)
            return volTable_[1][index - 32];
        if ((kind == 5 || kind == 6) && index >= 2)
            return 127;
        return volTable_[kind][index];
    }
    void applyVolume(int32_t id, int slot, int vol)
    {
        const int kind = findBankById(id);
        if (kind < 0)
            return;
        int v = volEntry(kind, id & 0x7F);
        v = (vol * v) >> 7;
        if (slot < 3)
            v = (v * seqVolume_) >> 7;
        slots_[slot].masterVol = uint8_t(std::clamp(v, 0, 127));
        refreshSlotVoices(slot);
    }
    bool slotMuted(int32_t id) const
    {
        const int kind = findBankById(id);
        if (kind < 0)
            return false;
        return volEntry(kind, id & 0x7F) == 0;
    }
    // sdfSndPutMsg: pan centre, expression full, bend centre on all 16 channels.
    void resetChannels(int slot)
    {
        for (auto& c : slots_[slot].ch)
        {
            c.pan = 64;
            c.expression = 127;
            c.bend = 0;
        }
    }
    bool midiSelect(int slot, int seqIndex, int bank, uint32_t location)
    {
        Slot& s = slots_[slot];
        auto seq = findSequence(banks_[bank].sq, uint32_t(seqIndex < 0 ? 0 : seqIndex));
        if (!seq)
        {
            NSND_TRACE("slot %d: sequence %d not in bank slot %d\n", slot, seqIndex, bank);
            postError(7);
            return false;
        }
        s.bank = bank;
        s.seq = *seq;
        s.pos = 0;
        s.skipDelta = false;
        s.running = 0;
        s.tempo = 500000;
        s.pendingTicks = 0;
        s.loopPos.fill(0);
        s.loopSkip.fill(false);
        s.loopLeft.fill(-1);
        s.loopEndId = -1;
        s.nrpn = 0x7F;
        s.ch = {};
        s.midiLocation = 0;
        readDelta(s);
        // MidiSetLocation: run the sequence silently up to the saved point.
        if (location > 0)
        {
            silent_ = true;
            size_t guard = 0;
            while (s.pos < location && s.pos < s.seq.size && guard++ < 1000000)
                if (!stepEvent(slot))
                    break;
            silent_ = false;
        }
        return true;
    }
    void stopSlot(int i)
    {
        Slot& s = slots_[i];
        noteOffAll(i, true);
        s.playing = false;
        s.is3d = false;
        s.fadeIn = false;
        s.volStep = 0;
        s.location = -1;
        s.id = -1;
    }
    void saveResume(int slot)
    {
        const int32_t id = slots_[slot].id;
        if (slot < 0 || uint32_t(id - 0x400000) > 0xBFFFFFu || uint16_t(id) >= 0x10)
            return;
        const int32_t loc = slots_[slot].location >= 0 ? slots_[slot].location : int32_t(slots_[slot].midiLocation);
        for (auto& r : resume_)
            if (r.first == id)
            {
                r.second = loc;
                return;
            }
        resume_[resumeIdx_] = {id, loc};
        resumeIdx_ = (resumeIdx_ + 1) & 31;
    }
    int allocBgmSlot()
    {
        int slot = -1;
        uint32_t serial = 0xFFFFFFFFu;
        for (int i = 0; i < 3; ++i)
            if (!slots_[i].playing && i != lastBgmSlot_ && serial >= slots_[i].serial)
            {
                serial = slots_[i].serial;
                slot = i;
            }
        if (slot >= 0)
            return lastBgmSlot_ = slot;
        serial = 0xFFFFFFFFu;
        for (int i = 0; i < 3; ++i)
            if (slots_[i].serial < serial && i != lastBgmSlot_ && !slots_[i].is3d)
            {
                serial = slots_[i].serial;
                slot = i;
            }
        if (slot < 0)
            return -1;
        noteOffAll(slot, true);
        slots_[slot].playing = false;
        return lastBgmSlot_ = slot;
    }
    int allocSeSlot()
    {
        for (int i = 3; i < 16; ++i)
            if (!slots_[i].playing && i != lastSeSlot_)
                return lastSeSlot_ = i;
        int slot = -1;
        uint32_t serial = 0xFFFFFFFFu;
        for (int i = 3; i < 16; ++i)
            if (slots_[i].serial < serial && i != lastSeSlot_ && !slots_[i].is3d)
            {
                serial = slots_[i].serial;
                slot = i;
            }
        if (slot < 0)
            return -1;
        noteOffAll(slot, true);
        slots_[slot].playing = false;
        return lastSeSlot_ = slot;
    }
    void startSlot(int slot, int32_t id, int bank, int vol)
    {
        Slot& s = slots_[slot];
        s.playing = true;
        resetChannels(slot);
        s.volStep = 0;
        s.fadeIn = false;
        s.is3d = false;
        s.location = -1;
        s.id = id;
        s.group = bank;
        s.serial = serialNext_++;
        if (vol >= 128)
        {
            if (vol & 0x100)
            {
                s.volStep = 64;
                vol = 0;
            }
            else
                vol = 127;
        }
        s.vol = vol << 8;
        applyVolume(id, slot, vol);
    }
    // sdfSndBgmPlay
    void bgmPlay(int32_t id, uint32_t location, int vol)
    {
        const int bank = findBankById(id);
        if (bank == 1 || bank == 2)
            bankAltSlot_ = uint8_t(bank);
        for (auto& s : slots_)
            if (s.id == id)
            {
                if (s.volStep < 0)
                    s.volStep = 32;
                return;
            }
        if (slotMuted(id))
            return;
        if (bank < 0)
        {
            NSND_TRACE("bgm %08x: bank not resident\n", unsigned(id));
            postError(18);
            return;
        }
        const int slot = allocBgmSlot();
        if (slot < 0)
            return;
        bgmSwitchReq_ = false;
        bgmSwitchSlot_ = bgmSwitchMidi_ = -1;
        if (slots_[slot].playing)
        {
            noteOffAll(slot, true);
            slots_[slot].playing = false;
        }
        if (!midiSelect(slot, uint16_t(id), bank, location))
            return;
        startSlot(slot, id, bank, vol);
        NSND_TRACE("bgm %08x -> slot %d (bank slot %d, loc %u, vol %d)\n", unsigned(id), slot, bank, location, vol);
    }
    // sdfSndSePlay; returns the slot
    int sePlay(int32_t id, int vol, int pan)
    {
        const int bank = findBankById(id);
        if (slotMuted(id))
            return -1;
        if (bank < 0)
        {
            NSND_TRACE("se %08x: bank not resident\n", unsigned(id));
            postError(18);
            return -1;
        }
        const int slot = allocSeSlot();
        if (slot < 0)
            return -1;
        if (!midiSelect(slot, uint16_t(id), bank, 0))
            return -1;
        startSlot(slot, id, bank, vol);
        slots_[slot].ch[0].pan = uint8_t(pan & 0x7F);
        refreshSlotVoices(slot);
        NSND_TRACE("se %08x -> slot %d (vol %d pan %d)\n", unsigned(id), slot, vol, pan);
        return slot;
    }
    static int atan2Deg(int x, int y)
    {
        if (x == 0 && y == 0)
            return 0;
        int ax = std::abs(x), ay = std::abs(y), deg;
        if (ax == 0)
            deg = 0;
        else if (ay == 0)
            deg = 90;
        else
            deg = int(std::lround(std::atan2(double(ax), double(ay)) * 57.29577951308232));
        if (y < 0)
            deg = 180 - deg;
        if (x < 0)
            deg = -deg;
        return deg;
    }
    static int isqrt(int64_t v) { return v <= 0 ? -1 : int(std::sqrt(double(v))); }
    static int distanceAtten(int dist, int radius)
    {
        int outer = 4 * radius;
        if (radius < 3)
        {
            radius = 3;
            outer = 12;
        }
        const int a = 10 * (2 * (outer + radius) - (dist - 10)) / radius;
        return std::clamp(a, 0, 100);
    }
    static void panLevel(int angle, int dist, int radius, int& pan, int& level)
    {
        int a = angle % 360;
        if (a >= 181)
            a -= 360;
        if (a < -180)
            a += 360;
        int sign = 1;
        if (a < 0)
        {
            sign = -1;
            a = -a;
        }
        level = 100;
        if (a >= 45)
        {
            if (a >= 91)
            {
                pan = 63 - (180 - a) * 70 * sign / 100;
                level = 75 - ((a - 90) >> 2);
            }
            else
            {
                level = 100 - (a - 45) / 2;
                pan = 63 - a * 70 * sign / 100;
            }
        }
        else
            pan = 63 - a * 70 * sign / 100;
        if (dist < 11)
            pan = ((pan - 63) >> 1) + 63;
        else
            level = level * distanceAtten(dist, radius) / 100;
    }
    void sePlay3d(int32_t id, int radius, int x, int y, int z)
    {
        const int dx = x - listenerX_, dy = y - listenerY_, dz = z - listenerZ_;
        const int dist = isqrt(int64_t(dx) * dx + int64_t(dy) * dy + int64_t(dz) * dz);
        int pan, level;
        panLevel(atan2Deg(-dx, dz) - listenerAngle_, dist, radius, pan, level);
        const int slot = sePlay(id, std::min(127, 127 * level / 100), pan);
        if (slot >= 0)
        {
            Slot& s = slots_[slot];
            s.is3d = true;
            s.posX = x;
            s.posY = y;
            s.posZ = z;
            s.radius = radius;
        }
    }
    void update3d(int slot)
    {
        Slot& s = slots_[slot];
        if (!s.is3d)
            return;
        const int dx = s.posX - listenerX_, dy = s.posY - listenerY_, dz = s.posZ - listenerZ_;
        const int dist = isqrt(int64_t(dx) * dx + int64_t(dy) * dy + int64_t(dz) * dz);
        int pan, level;
        panLevel(atan2Deg(-dx, dz) - listenerAngle_, dist, s.radius, pan, level);
        const int vol = std::min(127, 127 * level / 100);
        s.vol = vol << 8;
        s.volStep = 0;
        applyVolume(s.id, slot, vol);
        s.ch[0].pan = uint8_t(pan & 0x7F);
        refreshSlotVoices(slot);
    }
    void stopAll(int which)
    {
        bgmSwitchReq_ = false;
        bgmSwitchSlot_ = bgmSwitchMidi_ = -1;
        const int first = which == 0 ? 0 : which == 1 ? 0 : 3;
        const int last = which == 0 ? 16 : which == 1 ? 3 : 16;
        for (int i = first; i < last; ++i)
            if (slots_[i].playing)
            {
                if (i < 3)
                    saveResume(i);
                stopSlot(i);
                slots_[i].vol = 32512;
            }
    }

    // ---- commands -------------------------------------------------------------
    void command(const std::array<uint8_t, 128>& p)
    {
        const uint32_t opt = rd32(&p[12]);
        const uint8_t op = uint8_t(opt >> 20);
        const int arg4 = int((opt >> 16) & 0xF);
        const uint8_t* d = &p[0x14];
        NSND_TRACE("cmd %02x arg %x data %08x %08x %08x\n", op, arg4, rd32(d), rd32(d + 4), rd32(d + 8));
        switch (op)
        {
        case 0x00: // sdfSndInitSynth: the EE's work area (EE copy at sdfSndMap)
            eeWorkArea_ = kEeWorkArea;
            std::memcpy(wa_.data(), rdram_ + physical(kEeWorkArea), kWorkAreaSize);
            if (const uint32_t ee = rd32(&wa_[kWaEeAddr]); ee && physical(ee) + kWorkAreaSize <= 0x02000000u)
                eeWorkArea_ = ee;
            wr32(&wa_[kWaErrCount], 0);
            wr32(&wa_[kWaErrCode], 0xFFFFFFFFu);
            for (auto& s : slots_)
                s = Slot{};
            for (auto& r : resume_)
                r = {-1, -1};
            seqVolume_ = 127;
            initialized_ = true;
            break;
        case 0x01:
            stopAll(0);
            break;
        case 0x02:
            bgmPlay(int32_t(rd32(d)), rd32(d + 4), rd16(d + 8));
            break;
        case 0x03:
            for (int i = 0; i < 16; ++i)
                if (slots_[i].id == int32_t(rd32(d)) && slots_[i].playing)
                {
                    if (i < 3)
                        saveResume(i);
                    stopSlot(i);
                }
            break;
        case 0x04:
            stopAll(0);
            break;
        case 0x09:
            sePlay(int32_t(rd32(d)), rd16(d + 8), rd16(d + 10) & 0x7F);
            break;
        case 0x0A:
        {
            char path[109] = {};
            std::memcpy(path, d, 108);
            loadSmg(path, false);
            break;
        }
        case 0x0B:
        {
            const int bank = findBankById(int32_t(rd32(d)));
            if (bank >= 0)
            {
                unloadBank(bank);
                pushWorkArea();
            }
            break;
        }
        case 0xC0:
            pushWorkArea();
            break;
        case 0x0D:
            for (auto& s : slots_)
                if (s.id == int32_t(rd32(d)) && s.playing)
                {
                    s.volStep = -32;
                    s.fadeIn = false;
                }
            break;
        case 0x0E:
            if (arg4 - 1 >= 0)
                bgmSwitchMidi_ = arg4 - 1;
            bgmSwitchReq_ = true;
            break;
        case 0x0F:
        {
            int vol = 8 * arg4;
            if (vol & 8)
                vol |= 7;
            streamPlayFile(eeString(kEePcmName), vol);
            break;
        }
        case 0x10:
            streamStop();
            break;
        case 0x11:
        {
            int vol = 8 * arg4;
            if (vol & 8)
                vol |= 7;
            stream_.volume = vol;
            break;
        }
        case 0x12:
            // On-memory stream fed by a guest callback (movie audio); the EE's own path drains it.
            NSND_TRACE("on-memory stream request ignored\n");
            break;
        case 0x13:
        {
            const int32_t id = int32_t(rd32(d));
            bool playing = false;
            for (auto& s : slots_)
                if (s.id == id)
                {
                    if (s.volStep < 0)
                        s.volStep = 32;
                    playing = true;
                }
            if (playing)
                break;
            uint32_t loc = 0;
            for (auto& r : resume_)
                if (r.first == id && r.second >= 0)
                    loc = uint32_t(r.second);
            bgmPlay(id, loc, 0x100);
            break;
        }
        case 0x14:
        {
            uint16_t depth = uint16_t(arg4 << 10);
            if (opt & 0x10000)
                depth |= 0x3FF;
            spu_.setMaster(depth);
            break;
        }
        case 0x15:
        {
            int v = 8 * arg4;
            if (arg4 & 1)
                v |= 7;
            seqVolume_ = v;
            for (int i = 0; i < 3; ++i)
                if (slots_[i].playing)
                    applyVolume(slots_[i].id, i, seqVolume_);
            break;
        }
        case 0x16:
        {
            const int atX = int32_t(rd32(d + 8)), atZ = int32_t(rd32(d + 16));
            const int pX = int32_t(rd32(d + 20)), pY = int32_t(rd32(d + 24)), pZ = int32_t(rd32(d + 28));
            listenerX_ = pX;
            listenerY_ = pY;
            listenerZ_ = pZ;
            listenerAngle_ = atan2Deg(atX - pX, pZ - atZ);
            listenerDirty_ = true;
            break;
        }
        case 0x17:
            sePlay3d(int32_t(rd32(d)), int32_t(rd32(d + 4)), int32_t(rd32(d + 8)), int32_t(rd32(d + 12)),
                     int32_t(rd32(d + 16)));
            break;
        case 0x18:
            for (int i = 0; i < 3; ++i)
                if (slots_[i].id >= 0 && slots_[i].playing && slots_[i].volStep >= 0)
                {
                    slots_[i].volStep = -64;
                    slots_[i].fadeIn = true;
                }
            break;
        case 0x19:
            for (int i = 0; i < 3; ++i)
                if (slots_[i].id >= 0 && slots_[i].playing)
                {
                    slots_[i].volStep = 64;
                    slots_[i].fadeIn = false;
                }
            break;
        case 0x1A:
            stopAll(1);
            break;
        case 0x1B:
            loadSmg("", true);
            break;
        case 0x1C:
            synthMuted_ = arg4 != 0;
            for (int i = 0; i < 16; ++i)
                refreshSlotVoices(i);
            break;
        case 0x1D:
            if (stream_.playing)
                stream_.fadeStep = arg4 + 1;
            break;
        case 0x1F:
            std::memcpy(&wa_[kWaVolTable], rdram_ + physical(eeWorkArea_ + kWaVolTable), 7 * 128);
            for (int k = 0; k < 7; ++k)
                std::memcpy(volTable_[k].data(), &wa_[kWaVolTable + 128 * k], 128);
            break;
        case 0x20:
            for (int k = 0; k < 7; ++k)
                std::memcpy(&wa_[kWaVolTable + 128 * k], volTable_[k].data(), 128);
            pushWorkArea();
            break;
        case 0x21:
            stopAll(2);
            break;
        default:
            NSND_LOG("unknown command %02x (%04x)\n", op, unsigned(opt & 0xFFFF));
            postError(14);
            break;
        }
    }

    // ---- synthesiser ----------------------------------------------------------
    static constexpr int kVoices = 48;
    void voiceRegs(int v, uint32_t& r, uint32_t& a, int& core, int& idx) const
    {
        core = v / 24;
        idx = v % 24;
        r = Spu::C(core) + 0x10 * idx;
        a = Spu::C(core) + 0x1C0 + 12 * idx;
    }
    void keyOn(int v)
    {
        const int core = v / 24, idx = v % 24;
        if (idx < 16)
            spu_.W(0x1A0 + Spu::C(core), uint16_t(1u << idx));
        else
            spu_.W(0x1A2 + Spu::C(core), uint16_t(1u << (idx - 16)));
    }
    void keyOff(int v)
    {
        const int core = v / 24, idx = v % 24;
        if (idx < 16)
            spu_.W(0x1A4 + Spu::C(core), uint16_t(1u << idx));
        else
            spu_.W(0x1A6 + Spu::C(core), uint16_t(1u << (idx - 16)));
    }
    void setEffectSend(int v, bool on)
    {
        const int core = v / 24, idx = v % 24;
        const uint32_t lo = 0x18C + Spu::C(core), hi = 0x18E + Spu::C(core);
        const uint32_t rlo = 0x194 + Spu::C(core), rhi = 0x196 + Spu::C(core);
        uint32_t& mask = effectMask_[core];
        mask = on ? (mask | (1u << idx)) : (mask & ~(1u << idx));
        if (!reverbOn_)
            return;
        spu_.W(lo, uint16_t(mask));
        spu_.W(hi, uint16_t(mask >> 16));
        spu_.W(rlo, uint16_t(mask));
        spu_.W(rhi, uint16_t(mask >> 16));
    }
    // The sample's SPU attribute picks the core (0x10 core 0, 0x20 core 1): the two cores have
    // different reverbs. Free voice first; then the oldest released; then the oldest.
    int allocVoice(uint8_t spuAttr)
    {
        int lo = 0, hi = kVoices;
        if ((spuAttr & 0x30) == 0x10)
            hi = 24;
        else if ((spuAttr & 0x30) == 0x20)
            lo = 24;
        int best = -1;
        for (int v = lo; v < hi; ++v)
            if (!voices_[v].busy)
                return v;
        uint64_t oldest = ~0ull;
        for (int v = lo; v < hi; ++v)
            if (voices_[v].released && voices_[v].serial < oldest)
            {
                oldest = voices_[v].serial;
                best = v;
            }
        if (best >= 0)
            return best;
        for (int v = lo; v < hi; ++v)
            if (voices_[v].serial < oldest)
            {
                oldest = voices_[v].serial;
                best = v;
            }
        return best;
    }
    void writeVolume(int v)
    {
        const Voice& vc = voices_[v];
        const Slot& s = slots_[vc.slot];
        const Channel& c = s.ch[vc.channel];
        float amp = vc.baseAmp * (c.volume / 127.0f) * (c.expression / 127.0f) * (s.masterVol / 127.0f);
        if (synthMuted_)
            amp = 0;
        const int pan = std::clamp(vc.basePan + int(c.pan) - 64, 0, 127);
        const float l = amp * std::min(1.0f, (127 - pan) / 63.5f);
        const float r = amp * std::min(1.0f, pan / 63.5f);
        uint32_t reg, addr;
        int core, idx;
        voiceRegs(v, reg, addr, core, idx);
        spu_.W(reg + 0, uint16_t(std::clamp(int(l * 0x3FFF), 0, 0x3FFF)));
        spu_.W(reg + 2, uint16_t(std::clamp(int(r * 0x3FFF), 0, 0x3FFF)));
    }
    void writePitch(int v)
    {
        const Voice& vc = voices_[v];
        const Channel& c = slots_[vc.slot].ch[vc.channel];
        const double bend = c.bend / 8192.0 * vc.bendRange;
        const double pitch = vc.basePitch * std::pow(2.0, bend / 12.0);
        uint32_t reg, addr;
        int core, idx;
        voiceRegs(v, reg, addr, core, idx);
        spu_.W(reg + 4, uint16_t(std::clamp(int(std::lround(pitch)), 0, 0x3FFF)));
    }
    void refreshSlotVoices(int slot)
    {
        for (int v = 0; v < kVoices; ++v)
            if (voices_[v].busy && voices_[v].slot == slot)
                writeVolume(v);
    }
    const InstrumentSet* instrumentsFor(int slot, int bankSelect) const
    {
        int bank = slots_[slot].bank;
        if (bank < 0)
            return nullptr;
        if (bankSelect == 1)
            bank += bank == 3 ? 1 : 0;   // sdfSndHsynLoad: bank 3 plays with bank 4 as its second header
        const Bank& b = banks_[bank];
        return b.loaded && b.instruments.valid ? &b.instruments : nullptr;
    }
    void noteOn(int slot, int ch, int note, int vel)
    {
        if (silent_)
            return;
        Slot& s = slots_[slot];
        const Channel& c = s.ch[ch];
        int bankSel = c.bank;
        const InstrumentSet* is = instrumentsFor(slot, bankSel);
        int bank = s.bank + ((bankSel == 1 && s.bank == 3) ? 1 : 0);
        if (!is)
            return;
        if (c.program >= is->programs.size() || !is->programs[c.program].valid)
            return;
        const Program& pr = is->programs[c.program];
        for (const Split& sp : pr.splits)
        {
            if (note < sp.lo || note > (sp.hi < sp.lo ? 127 : sp.hi) || sp.sampleSet >= is->sets.size())
                continue;
            const SampleSet& set = is->sets[sp.sampleSet];
            if (vel < set.velLo || vel > set.velHi)
                continue;
            for (uint16_t si : set.samples)
            {
                if (si >= is->samples.size())
                    continue;
                const SampleParam& sm = is->samples[si];
                if (vel < sm.velLo || vel > sm.velHi || sm.vag >= is->vags.size())
                    continue;
                const VagInfo& vag = is->vags[sm.vag];
                const int v = allocVoice(sm.spuAttr);
                if (v < 0)
                    return;
                if (voices_[v].busy)
                    keyOff(v);
                Voice& vc = voices_[v];
                vc = {};
                vc.busy = true;
                vc.slot = slot;
                vc.channel = ch;
                vc.note = note;
                vc.serial = voiceSerial_++;
                vc.keyTick = tickCount_;
                const float velAmp = vel / 127.0f;
                vc.baseAmp = (pr.volume / 127.0f) * (sp.volume / 127.0f) * (sm.volume / 127.0f) * velAmp * velAmp;
                vc.basePan = std::clamp(64 + (int(sm.pan) - 64) + (int(pr.pan) - 64) + (int(sp.pan) - 64), 0, 127);
                const double semis = note - sm.baseNote + pr.transpose + sp.transpose +
                                     (pr.detune + sp.detune + sm.detune) / 128.0;
                vc.basePitch = 4096.0 * vag.rate / 48000.0 * std::pow(2.0, semis / 12.0);
                vc.bendRange = (c.bend >= 0 ? sp.bendHi : sp.bendLo) / 128.0f;
                uint32_t reg, addr;
                int core, idx;
                voiceRegs(v, reg, addr, core, idx);
                writeVolume(v);
                writePitch(v);
                spu_.W(reg + 6, sm.adsr1);
                spu_.W(reg + 8, sm.adsr2);
                const uint32_t start = (kBankSpu[bank] + vag.offset) / 2;
                spu_.W(addr + 0, uint16_t((start >> 16) & 0xF));
                spu_.W(addr + 2, uint16_t(start & 0xFFFF));
                setEffectSend(v, (sm.spuAttr & 0x0C) != 0);
                keyOn(v);
            }
        }
    }
    void noteOff(int slot, int ch, int note)
    {
        if (slots_[slot].ch[ch].sustain)
            return;
        for (int v = 0; v < kVoices; ++v)
        {
            Voice& vc = voices_[v];
            if (vc.busy && !vc.released && vc.slot == slot && vc.channel == ch && vc.note == note)
            {
                keyOff(v);
                vc.released = true;
            }
        }
    }
    void noteOffAll(int slot, bool hard)
    {
        for (int v = 0; v < kVoices; ++v)
        {
            Voice& vc = voices_[v];
            if (vc.busy && vc.slot == slot)
            {
                if (hard)
                {
                    uint32_t reg, addr;
                    int core, idx;
                    voiceRegs(v, reg, addr, core, idx);
                    spu_.W(reg + 0, 0);
                    spu_.W(reg + 2, 0);
                }
                keyOff(v);
                vc.released = true;
            }
        }
    }
    void reapVoices()
    {
        for (int v = 0; v < kVoices; ++v)
        {
            Voice& vc = voices_[v];
            if (!vc.busy)
                continue;
            uint32_t reg, addr;
            int core, idx;
            voiceRegs(v, reg, addr, core, idx);
            // ENVX reaches 0 at the end of the release, or when a one-shot sample ends.
            if (vc.keyTick < tickCount_ && spu_.R(reg + 0xA) == 0)
                vc = {};
        }
        // Voice map for the EE (sdfSndUpdateVoiceMap): 0x80|... active, -1 idle.
        for (int core = 0; core < 2; ++core)
            for (int i = 0; i < 24; ++i)
            {
                const Voice& vc = voices_[core * 24 + i];
                wa_[kWaVoiceMap + 32 * core + i] = vc.busy ? uint8_t((vc.slot << 4) & 0x70) : 0xFF;
            }
        pushVoiceMap();
    }

    // ---- sequencer ------------------------------------------------------------
    uint8_t dataByte(Slot& s)
    {
        if (s.pos >= s.seq.size)
            return 0;
        uint8_t b = s.seq.data[s.pos++];
        if (b & 0x80)
        {
            s.skipDelta = true;
            b &= 0x7F;
        }
        return b;
    }
    uint8_t rawByte(Slot& s) { return s.pos < s.seq.size ? s.seq.data[s.pos++] : 0; }
    void readDelta(Slot& s)
    {
        if (s.skipDelta)
        {
            s.skipDelta = false;
            s.nextDelta = 0;
            return;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4 && s.pos < s.seq.size; ++i)
        {
            const uint8_t b = s.seq.data[s.pos++];
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80))
                break;
        }
        s.nextDelta = v;
    }
    // Executes one event (the delta before it has already elapsed). Returns false at the end.
    bool stepEvent(int slot)
    {
        Slot& s = slots_[slot];
        if (s.pos >= s.seq.size)
            return false;
        s.midiLocation = uint32_t(s.pos);
        uint8_t status = s.seq.data[s.pos];
        if (status < 0x80)
            status = s.running;
        else
        {
            ++s.pos;
            if (status != 0xFF)
                s.running = status;
        }
        const int ch = status & 0x0F;
        Channel& c = s.ch[ch];
        switch (status & 0xF0)
        {
        case 0x80:
            noteOff(slot, ch, dataByte(s));
            break;
        case 0x90:
        {
            const uint8_t key = rawByte(s), vel = dataByte(s);
            if (vel)
                noteOn(slot, ch, key, vel);
            else
                noteOff(slot, ch, key);
            break;
        }
        case 0xA0:
            rawByte(s);
            dataByte(s);
            break;
        case 0xB0:
        {
            const uint8_t ctl = rawByte(s), val = dataByte(s);
            controller(slot, ch, ctl, val);
            break;
        }
        case 0xC0:
            c.program = dataByte(s);
            break;
        case 0xD0:
        {
            const uint8_t val = dataByte(s);
            channelMessage(slot, ch, val);
            break;
        }
        case 0xE0:
        {
            const uint8_t lo = rawByte(s), hi = dataByte(s);
            c.bend = int16_t(((hi << 7) | lo) - 8192);
            if (!silent_)
                for (int v = 0; v < kVoices; ++v)
                    if (voices_[v].busy && voices_[v].slot == slot && voices_[v].channel == ch)
                        writePitch(v);
            break;
        }
        case 0xF0:
            if (status == 0xFF)
            {
                const uint8_t type = rawByte(s);
                if (type == 0x51)
                {
                    rawByte(s);
                    const uint32_t t = (uint32_t(rawByte(s)) << 16) | (uint32_t(rawByte(s)) << 8) | rawByte(s);
                    if (t)
                        s.tempo = t;
                }
                else
                    return false;  // 0x2F and anything else ends the track (VGMTrans does the same)
            }
            else
                return false;
            break;
        }
        readDelta(s);
        return true;
    }
    void controller(int slot, int ch, uint8_t ctl, uint8_t val)
    {
        Slot& s = slots_[slot];
        Channel& c = s.ch[ch];
        switch (ctl)
        {
        case 0:
            c.bank = val;
            break;
        case 7:
            c.volume = val;
            break;
        case 10:
            c.pan = val;
            break;
        case 11:
            c.expression = val;
            break;
        case 64:
            c.sustain = val >= 64;
            if (!c.sustain && !silent_)
                for (int v = 0; v < kVoices; ++v)
                    if (voices_[v].busy && !voices_[v].released && voices_[v].slot == slot && voices_[v].channel == ch &&
                        !noteHeld(slot, ch, voices_[v].note))
                    {
                        keyOff(v);
                        voices_[v].released = true;
                    }
            break;
        case 99: // NRPN MSB: 0 loop start, 1 loop end
            s.nrpn = val;
            break;
        case 6: // data entry: the loop's number
            if (s.nrpn == 0)
            {
                const int id = val & 7;
                s.loopPos[id] = s.pos;
                s.loopSkip[id] = s.skipDelta;
                s.loopLeft[id] = -1;
            }
            else if (s.nrpn == 1)
                s.loopEndId = val & 7;
            s.nrpn = 0x7F;
            break;
        case 38: // data entry LSB after a loop end: repeat count, 0 = forever
            if (s.loopEndId >= 0)
            {
                const int id = s.loopEndId;
                s.loopEndId = -1;
                if (s.loopPos[id] == 0)
                    break;  // no matching start
                if (val == 0)
                {
                    s.pos = s.loopPos[id];
                    s.skipDelta = s.loopSkip[id];
                }
                else
                {
                    if (s.loopLeft[id] < 0)
                        s.loopLeft[id] = val;
                    if (s.loopLeft[id] > 0)
                    {
                        --s.loopLeft[id];
                        s.pos = s.loopPos[id];
                        s.skipDelta = s.loopSkip[id];
                    }
                    else
                        s.loopLeft[id] = -1;
                }
            }
            break;
        case 121: // reset all controllers
            c.volume = 127;
            c.expression = 127;
            c.pan = 64;
            c.bend = 0;
            break;
        case 120:
        case 123:
            for (int v = 0; v < kVoices; ++v)
                if (voices_[v].busy && voices_[v].slot == slot && voices_[v].channel == ch)
                {
                    keyOff(v);
                    voices_[v].released = true;
                }
            break;
        default:
            break;
        }
        if ((ctl == 7 || ctl == 10 || ctl == 11) && !silent_)
            for (int v = 0; v < kVoices; ++v)
                if (voices_[v].busy && voices_[v].slot == slot && voices_[v].channel == ch)
                    writeVolume(v);
    }
    bool noteHeld(int, int, int) const { return false; }
    // sdfSndMidiCbMessage: channel pressure messages are the sequences' markers.
    void channelMessage(int slot, int ch, uint8_t val)
    {
        if (ch == 0)
        {
            if (bgmSwitchReq_)
            {
                if (bgmSwitchMidi_ < 0)
                {
                    if (val == 127)
                    {
                        bgmSwitchReq_ = false;
                        return;
                    }
                    bgmSwitchMidi_ = val;
                }
                bgmSwitchSlot_ = slot;
            }
        }
        else if (ch == 15)
            slots_[slot].location = int32_t(slots_[slot].midiLocation);
    }
    void runSequence(int slot)
    {
        Slot& s = slots_[slot];
        s.pendingTicks += double(1000000.0 / kTickHz) * s.seq.ppqn / double(s.tempo);
        int guard = 0;
        while (s.playing && s.pendingTicks >= s.nextDelta && guard++ < 4096)
        {
            s.pendingTicks -= s.nextDelta;
            if (!stepEvent(slot))
            {
                // End of the sequence: the slot frees itself (flags & 2 clears).
                noteOffAll(slot, false);
                s.playing = false;
                break;
            }
        }
    }

    // ---- the 240 Hz tick (sdfSndSeqThread) -----------------------------------
    void tick()
    {
        ++tickCount_;
        for (int i = 0; i < 16; ++i)
            if (slots_[i].playing)
                runSequence(i);
        reapVoices();
        for (int i = 0; i < 16; ++i)
        {
            Slot& s = slots_[i];
            if (!s.playing)
            {
                s.location = -1;
                s.id = -1;
                s.fadeIn = false;
                s.volStep = 0;
                continue;
            }
            if (listenerDirty_ && i >= 3)
                update3d(i);
            if (bgmSwitchSlot_ == i && bgmSwitchMidi_ >= 0)
                bgmSwitch(i);
            if (!s.volStep)
                continue;
            const int vol = s.vol + s.volStep;
            s.vol = vol;
            if (vol >= 32513)
            {
                s.vol = 32512;
                s.volStep = 0;
                applyVolume(s.id, i, s.vol >> 8);
            }
            else if (vol >= 0)
            {
                if (s.fadeIn && vol < 0x4000)
                {
                    s.vol = 0x3FFF;
                    s.volStep = 0;
                    s.fadeIn = false;
                }
                applyVolume(s.id, i, s.vol >> 8);
            }
            else
            {
                saveResume(i);
                s.vol = 0;
                stopSlot(i);
            }
        }
        listenerDirty_ = false;
        streamFadeTick();
    }
    void bgmSwitch(int i)
    {
        Slot& from = slots_[i];
        const int bank = findBankById(from.id);
        const int slot = allocBgmSlot();
        if (slot < 0 || bank < 0)
            return;
        const int32_t newId = (from.id & int32_t(0xFFFF0000)) | bgmSwitchMidi_;
        if (!midiSelect(slot, bgmSwitchMidi_, bank, 0))
            return;
        Slot& to = slots_[slot];
        to.playing = true;
        resetChannels(slot);
        to.volStep = from.volStep;
        to.fadeIn = from.fadeIn;
        to.vol = from.vol;
        to.group = from.group;
        to.serial = serialNext_++;
        to.location = -1;
        to.id = newId;
        applyVolume(newId, slot, from.vol >> 8);
        bgmSwitchReq_ = false;
        bgmSwitchMidi_ = bgmSwitchSlot_ = -1;
        noteOffAll(i, false);
        from.playing = false;
        from.fadeIn = false;
        from.volStep = 0;
        from.location = -1;
        from.id = -1;
    }

    // ---- 6. APCM streams --------------------------------------------------------
    struct Stream
    {
        bool playing = false;
        std::vector<uint8_t> data;   // ADPCM body
        size_t pos = 0;
        bool stereo = false;
        uint32_t rate = 44100;
        int volume = 127;
        int fadeStep = 0, fadeFrac = 0;
        // decoder state per channel
        int32_t hist[2][2] = {};
        std::vector<int16_t> pcm[2];
        size_t pcmPos = 0;
        double phase = 0;
    } stream_;
    static void decodeBlock(const uint8_t* b, int32_t* hist, int16_t* out)
    {
        static const int f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};
        const int shift = b[0] & 0x0F, filter = std::min(4, (b[0] >> 4) & 0x0F);
        for (int i = 0; i < 28; ++i)
        {
            int n = (b[2 + i / 2] >> ((i & 1) * 4)) & 0x0F;
            if (n & 8)
                n -= 16;
            int32_t s = (n << 12) >> std::min(shift, 12);
            s += (hist[0] * f0[filter] + hist[1] * f1[filter] + 32) >> 6;
            s = std::clamp(s, -32768, 32767);
            hist[1] = hist[0];
            hist[0] = s;
            out[i] = int16_t(s);
        }
    }
    void streamPlayFile(const std::string& name, int vol)
    {
        if (stream_.playing)
            return;  // "ADPCM now playing": the IRX ignores a second request
        auto f = disc_.read(name);
        if (!f || f->size() < 0x20 || (*f)[0] != 0x0B || std::memcmp(&(*f)[8], "APCM", 4) != 0)
        {
            NSND_LOG("stream %s not playable\n", name.c_str());
            eeWrite32(kEePcmState, 0);
            return;
        }
        static const uint32_t rates[6] = {48000, 44100, 32000, 24000, 22050, 16000};
        const uint32_t len = rd32(&(*f)[0x10]);
        stream_ = Stream{};
        stream_.stereo = (*f)[0x1C] != 0;
        stream_.rate = rates[std::min<uint8_t>((*f)[0x1D], 5)];
        stream_.volume = vol;
        stream_.data.assign(f->begin() + 0x20, f->begin() + std::min<size_t>(f->size(), 0x20 + size_t(len) * 2));
        stream_.playing = true;
        eeWrite32(kEePcmState, 1);
        NSND_TRACE("stream %s: %zu bytes %s %u Hz vol %d\n", name.c_str(), stream_.data.size(),
                   stream_.stereo ? "stereo" : "mono", stream_.rate, vol);
    }
    void streamStop()
    {
        if (stream_.playing)
            NSND_TRACE("stream stopped\n");
        stream_.playing = false;
        stream_.data.clear();
        eeWrite32(kEePcmState, 0);
    }
    void streamFadeTick()
    {
        if (!stream_.playing || !stream_.fadeStep)
            return;
        stream_.fadeFrac += stream_.fadeStep;
        if (stream_.fadeFrac >= 16)
        {
            stream_.volume -= (stream_.fadeFrac >> 4) & 0xF;
            stream_.fadeFrac &= 0xF;
        }
        if (stream_.volume <= 0)
            streamStop();
    }
    // Next stream frame pair (one decoded 512-byte chunk at a time: stereo = 256 L + 256 R).
    bool streamFrame(float& l, float& r)
    {
        Stream& st = stream_;
        if (!st.playing)
            return false;
        auto refill = [&]() {
            st.pcm[0].clear();
            st.pcm[1].clear();
            st.pcmPos = 0;
            if (st.pos >= st.data.size())
                return false;
            const size_t chunk = std::min<size_t>(512, st.data.size() - st.pos);
            const uint8_t* p = &st.data[st.pos];
            int16_t out[28];
            if (st.stereo)
            {
                for (int c = 0; c < 2; ++c)
                    for (size_t b = 0; b + 16 <= 256 && c * 256 + b + 16 <= chunk; b += 16)
                    {
                        decodeBlock(p + c * 256 + b, st.hist[c], out);
                        st.pcm[c].insert(st.pcm[c].end(), out, out + 28);
                    }
            }
            else
                for (size_t b = 0; b + 16 <= chunk; b += 16)
                {
                    decodeBlock(p + b, st.hist[0], out);
                    st.pcm[0].insert(st.pcm[0].end(), out, out + 28);
                }
            st.pos += chunk;
            return !st.pcm[0].empty();
        };
        while (st.pcmPos + 1 >= st.pcm[0].size())
        {
            if (!refill())
            {
                streamStop();
                return false;
            }
        }
        const size_t i = st.pcmPos;
        const float fr = float(st.phase);
        const auto at = [&](int c, size_t k) { return st.pcm[c][std::min(k, st.pcm[c].size() - 1)] / 32768.0f; };
        const float gain = (st.volume / 127.0f) * (synthMuted_ ? 0.0f : 1.0f);
        l = (at(0, i) * (1 - fr) + at(0, i + 1) * fr) * gain;
        r = st.stereo ? (at(1, i) * (1 - fr) + at(1, i + 1) * fr) * gain : l;
        st.phase += double(st.rate) / kFrameRate;
        while (st.phase >= 1.0)
        {
            st.phase -= 1.0;
            ++st.pcmPos;
        }
        return true;
    }

    // ---- 7. thread and output ---------------------------------------------------
    static constexpr size_t kRing = 16384;   // stereo frames, power of two
    static constexpr size_t kAhead = 2400;   // 50 ms queued ahead of the device
    std::vector<float> ring_ = std::vector<float>(kRing * 2);
    std::atomic<uint64_t> written_{0}, read_{0};

    static size_t pullThunk(void* user, float* stereo, size_t frames)
    {
        return static_cast<Driver*>(user)->pull(stereo, frames);
    }
    size_t pull(float* stereo, size_t frames)
    {
        const uint64_t w = written_.load(std::memory_order_acquire), r = read_.load(std::memory_order_relaxed);
        const size_t have = size_t(std::min<uint64_t>(w - r, frames));
        for (size_t i = 0; i < have; ++i)
        {
            const size_t k = size_t((r + i) & (kRing - 1));
            stereo[2 * i] = ring_[2 * k];
            stereo[2 * i + 1] = ring_[2 * k + 1];
        }
        for (size_t i = have; i < frames; ++i)
            stereo[2 * i] = stereo[2 * i + 1] = 0;
        read_.store(r + frames <= w ? r + frames : w, std::memory_order_release);
        return have;
    }
    void render() { renderFrames(kFramesPerTick); }
    // Advance the SPU2 by `frames` and queue them (plus any stream) for the device.
    void renderFrames(int frames)
    {
        spu_.advance(uint32_t(frames));
        float buf[kFramesPerTick * 2];
        const size_t got = spu_.pull(buf, size_t(frames));
        for (size_t i = got; i < size_t(frames); ++i)
            buf[2 * i] = buf[2 * i + 1] = 0;
        const uint64_t w = written_.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; ++i)
        {
            float l = 0, r = 0;
            streamFrame(l, r);
            const size_t k = size_t((w + i) & (kRing - 1));
            ring_[2 * k] = buf[2 * i] + l;
            ring_[2 * k + 1] = buf[2 * i + 1] + r;
        }
        written_.store(w + uint64_t(frames), std::memory_order_release);
    }
    void loop()
    {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        uint64_t ticks = 0;
        while (running_)
        {
            std::deque<std::array<uint8_t, 128>> work;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(2), [this] { return !queue_.empty() || !running_; });
                work.swap(queue_);
            }
            for (auto& p : work)
                command(p);
            if (!initialized_)
                continue;
            // Without an output device, consume at wall-clock speed so time still passes.
            if (!audioReady_)
            {
                const uint64_t due = uint64_t(std::chrono::duration<double>(clock::now() - t0).count() * kFrameRate);
                read_.store(std::min<uint64_t>(due, written_.load()), std::memory_order_relaxed);
            }
            int budget = 8;
            while (written_.load() - read_.load() < kAhead && budget-- > 0)
            {
                tick();
                render();
                ++ticks;
            }
            // A long stall (device gone, host asleep): never queue more than the ring holds.
            if (written_.load() - read_.load() > kRing - kFramesPerTick)
                read_.store(written_.load() - kAhead);
        }
    }

    // ---- state ------------------------------------------------------------------
    uint8_t* rdram_ = nullptr;
    DiscFs disc_;
    Spu spu_;
    bool audioReady_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex queueMutex_;
    std::condition_variable wake_;
    std::deque<std::array<uint8_t, 128>> queue_;

    bool initialized_ = false;
    uint32_t eeWorkArea_ = 0;
    std::array<uint8_t, kWorkAreaSize> wa_{};
    std::array<Bank, 7> banks_{};
    std::array<std::array<uint8_t, 128>, 7> volTable_{};
    std::array<Slot, 16> slots_{};
    std::array<std::pair<int32_t, int32_t>, 32> resume_{};
    int resumeIdx_ = 0;
    std::array<Voice, kVoices> voices_{};
    uint64_t voiceSerial_ = 1;
    uint64_t tickCount_ = 0;
    uint32_t effectMask_[2] = {};
    bool reverbOn_ = false;
    bool silent_ = false;
    bool synthMuted_ = false;
    bool bank1Loaded_ = false, bank3Loaded_ = false;
    uint8_t bankAltSlot_ = 0;
    uint32_t serialNext_ = 0;
    int seqVolume_ = 127;
    int lastBgmSlot_ = -1, lastSeSlot_ = -1;
    bool bgmSwitchReq_ = false;
    int bgmSwitchSlot_ = -1, bgmSwitchMidi_ = -1;
    int listenerX_ = 0, listenerY_ = 0, listenerZ_ = 0, listenerAngle_ = 0;
    bool listenerDirty_ = false;
};

Driver* g_driver = nullptr;
} // namespace

bool start(uint8_t* rdram, const char* discRoot)
{
    if (g_driver)
        return true;
    auto* d = new Driver();
    if (!d->start(rdram, discRoot ? discRoot : "."))
    {
        delete d;
        return false;
    }
    g_driver = d;
    return true;
}

bool active() { return g_driver != nullptr; }

void post(const uint8_t* packet, uint32_t size)
{
    if (g_driver)
        g_driver->post(packet, size);
}

void setTrace(bool on) { g_trace.store(on); }
} // namespace magatama::sound
