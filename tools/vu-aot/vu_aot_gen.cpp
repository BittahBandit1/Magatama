// C++ port of tools/vu-aot/vu_aot_gen.py, the --elf mode: the VU microcode catalogue (RRV_VU_AOT) from the user's
// own game ELF. It writes the same rrv_vu_aot_blocks.inc and manifest.json, byte for byte (checked against the
// Python generator by scripts/check_vu_aot_port.sh). It exists for the hosts where the build runs without Python:
// the Android app compiles the game on the phone (src/app/android). See vu_aot_gen.py for what every rule means.
//
//   vu_aot_gen --elf SLUS_209.11 --output DIR [--no-analysis]
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
// ---------------------------------------------------------------------------------------------- SHA-256
struct Sha256
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t used = 0;
    uint64_t total = 0;
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t *p)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void *data, size_t size)
    {
        const auto *p = static_cast<const uint8_t *>(data);
        total += size;
        while (size)
        {
            const size_t take = std::min(size, sizeof buf - used);
            std::memcpy(buf + used, p, take);
            used += take; p += take; size -= take;
            if (used == sizeof buf) { block(buf); used = 0; }
        }
    }
    std::string hex()
    {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        static const char *digits = "0123456789abcdef";
        std::string out;
        for (uint32_t v : h)
            for (int i = 28; i >= 0; i -= 4) out += digits[(v >> i) & 0xF];
        return out;
    }
};

std::string sha256(const std::string &data)
{
    Sha256 s;
    s.update(data.data(), data.size());
    return s.hex();
}

template <typename T> void put_le(Sha256 &s, T v)
{
    uint8_t b[sizeof(T)];
    for (size_t i = 0; i < sizeof(T); ++i) b[i] = uint8_t(uint64_t(v) >> (8 * i));
    s.update(b, sizeof b);
}

// ---------------------------------------------------------------------------------------------- decode tables
constexpr int MAX_SLOTS = 256;
constexpr int MAYBE_BRANCH = 1, ARM_E = 2, LAST = 4;
constexpr int NO_MAC_WORD = 8, NO_RING_WRITE = 16, NO_VISIBLE = 32, CLEAN_VS = 64, CLEAN_VT = 128, CLEAN_ACC = 256;
constexpr int ALL_LANES = 0xF, XYZ = 0xE, ACC = 32;

bool in(std::initializer_list<int> set, int v) { return std::find(set.begin(), set.end(), v) != set.end(); }
bool branch_op(int op) { return in({0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}, op); }
bool static_target_op(int op) { return in({0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}, op); }
bool call_op(int op) { return op == 0x21 || op == 0x25; }
bool cond_op(int op) { return in({0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}, op); }

enum Dst { D_NONE, D_FD, D_FT, D_ACC };
enum Vt { V_NONE, V_BC, V_FULL, V_OP };
struct Upper
{
    const char *name;
    Dst dst;
    bool mac, flush;
    Vt vt;
    bool acc;
};
constexpr Upper NOP{"NOP", D_NONE, false, false, V_NONE, false};
Upper fmac(const char *n, Dst d, Vt vt, bool acc = false) { return {n, d, true, true, vt, acc}; }
Upper plain(const char *n, Dst d, Vt vt) { return {n, d, false, false, vt, false}; }

struct Tables
{
    std::array<Upper, 64> upper;
    std::array<Upper, 128> special;
    Tables()
    {
        upper.fill(NOP);
        special.fill(NOP);
        for (int i = 0; i < 4; ++i)
        {
            upper[0x00 + i] = fmac("ADDbc", D_FD, V_BC);
            upper[0x04 + i] = fmac("SUBbc", D_FD, V_BC);
            upper[0x08 + i] = fmac("MADDbc", D_FD, V_BC, true);
            upper[0x0C + i] = fmac("MSUBbc", D_FD, V_BC, true);
            upper[0x10 + i] = plain("MAXbc", D_FD, V_BC);
            upper[0x14 + i] = plain("MINIbc", D_FD, V_BC);
            upper[0x18 + i] = fmac("MULbc", D_FD, V_BC);
            special[0x00 + i] = fmac("ADDAbc", D_ACC, V_BC);
            special[0x04 + i] = fmac("SUBAbc", D_ACC, V_BC);
            special[0x08 + i] = fmac("MADDAbc", D_ACC, V_BC, true);
            special[0x0C + i] = fmac("MSUBAbc", D_ACC, V_BC, true);
            special[0x10 + i] = plain("ITOF", D_FT, V_NONE);
            special[0x14 + i] = plain("FTOI", D_FT, V_NONE);
            special[0x18 + i] = fmac("MULAbc", D_ACC, V_BC);
        }
        upper[0x1C] = fmac("MULq", D_FD, V_NONE);
        upper[0x1D] = plain("MAXi", D_FD, V_NONE);
        upper[0x1E] = fmac("MULi", D_FD, V_NONE);
        upper[0x1F] = plain("MINIi", D_FD, V_NONE);
        upper[0x20] = fmac("ADDq", D_FD, V_NONE);
        upper[0x21] = fmac("MADDq", D_FD, V_NONE, true);
        upper[0x22] = fmac("ADDi", D_FD, V_NONE);
        upper[0x23] = fmac("MADDi", D_FD, V_NONE, true);
        upper[0x24] = fmac("SUBq", D_FD, V_NONE);
        upper[0x25] = fmac("MSUBq", D_FD, V_NONE, true);
        upper[0x26] = fmac("SUBi", D_FD, V_NONE);
        upper[0x27] = fmac("MSUBi", D_FD, V_NONE, true);
        upper[0x28] = fmac("ADD", D_FD, V_FULL);
        upper[0x29] = fmac("MADD", D_FD, V_FULL, true);
        upper[0x2A] = fmac("MUL", D_FD, V_FULL);
        upper[0x2B] = plain("MAX", D_FD, V_FULL);
        upper[0x2C] = fmac("SUB", D_FD, V_FULL);
        upper[0x2D] = fmac("MSUB", D_FD, V_FULL, true);
        upper[0x2E] = fmac("OPMSUB", D_FD, V_OP);
        upper[0x2F] = plain("MINI", D_FD, V_FULL);
        special[0x1C] = fmac("MULAq", D_ACC, V_NONE);
        special[0x1D] = plain("ABS", D_FT, V_NONE);
        special[0x1E] = fmac("MULAi", D_ACC, V_NONE);
        special[0x1F] = plain("CLIP", D_NONE, V_NONE);
        special[0x20] = fmac("ADDAq", D_ACC, V_NONE);
        special[0x21] = fmac("MADDAq", D_ACC, V_NONE, true);
        special[0x22] = fmac("ADDAi", D_ACC, V_NONE);
        special[0x23] = fmac("MADDAi", D_ACC, V_NONE, true);
        special[0x24] = fmac("SUBAq", D_ACC, V_NONE);
        special[0x25] = fmac("MSUBAq", D_ACC, V_NONE, true);
        special[0x26] = fmac("SUBAi", D_ACC, V_NONE);
        special[0x27] = fmac("MSUBAi", D_ACC, V_NONE, true);
        special[0x28] = fmac("ADDA", D_ACC, V_FULL);
        special[0x29] = fmac("MADDA", D_ACC, V_FULL, true);
        special[0x2A] = fmac("MULA", D_ACC, V_FULL);
        special[0x2C] = fmac("SUBA", D_ACC, V_FULL);
        special[0x2D] = fmac("MSUBA", D_ACC, V_FULL, true);
        special[0x2E] = Upper{"OPMULA", D_ACC, true, false, V_OP, false};
    }
};
const Tables &tables()
{
    static const Tables t;
    return t;
}

struct Decoded
{
    Upper e;
    int dest, ft, fs, fd, bc;
};
Decoded decode_upper(uint32_t upper)
{
    const int op = upper & 0x3F;
    const Upper e = op >= 0x3C ? tables().special[((upper & 0x3) | ((upper >> 4) & 0x7C)) & 0x7F] : tables().upper[op];
    return {e, int((upper >> 21) & 0xF), int((upper >> 16) & 0x1F), int((upper >> 11) & 0x1F), int((upper >> 6) & 0x1F),
            int(upper & 3)};
}

bool has_lower(uint32_t lower, uint32_t upper)
{
    return !(upper & 0x80000000u) && lower != 0x00000000u && lower != 0x8000033Cu;
}
bool reads_flags(uint32_t lower, uint32_t upper)
{
    return has_lower(lower, upper) && in({0x10, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x1A, 0x1B, 0x1C}, (lower >> 25) & 0x7F);
}

void lower_transfer(uint32_t lower, std::array<int, 33> &clean)
{
    const int op = (lower >> 25) & 0x7F, dest = (lower >> 21) & 0xF, it = (lower >> 16) & 0x1F, fs = (lower >> 11) & 0x1F;
    if (op == 0x00)
    {
        clean[it] &= ~dest;
        return;
    }
    if (in({0x01, 0x04, 0x05, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x1A, 0x1B, 0x1C, 0x20,
            0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F},
           op))
        return;
    if (op == 0x40)
    {
        const int funct = lower & 0x3F;
        if (in({0x30, 0x31, 0x32, 0x34, 0x35}, funct))
            return;
        if (funct >= 0x3C)
        {
            const int f2 = (lower & 0x3) | ((lower >> 4) & 0x7C);
            if (in({0x3C, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x35, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x70, 0x71, 0x72,
                    0x73, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x68, 0x69, 0x6C},
                   f2))
                return;
            if (in({0x3D, 0x34, 0x36, 0x64}, f2))
            {
                clean[it] &= ~dest;
                return;
            }
            if (f2 == 0x30)
            {
                clean[it] = (clean[it] & ~dest) | (clean[fs] & dest);
                return;
            }
            if (f2 == 0x31)
            {
                const int src = clean[fs] == ALL_LANES ? dest : 0;
                clean[it] = (clean[it] & ~dest) | src;
                return;
            }
        }
    }
    clean.fill(0);
}

struct Slot
{
    int64_t pc;
    uint32_t lower, upper;
    int64_t next;
    int flags;
};

struct Stats
{
    long slots = 0, fmac = 0, no_mac_word = 0, no_ring_write = 0, no_visible = 0, clamps = 0, clamps_skipped = 0;
};

void analyse(std::vector<Slot> &slots, Stats &stats)
{
    const int n = int(slots.size());
    std::vector<bool> reader(n), ring_write(n);
    for (int k = 0; k < n; ++k) reader[k] = reads_flags(slots[k].lower, slots[k].upper);
    for (int k = 0; k < n; ++k) ring_write[k] = k + 4 >= n || reader[k + 4];
    std::vector<int> fmacs;
    for (int k = 0; k < n; ++k)
        if (decode_upper(slots[k].upper).e.mac) fmacs.push_back(k);
    std::set<int> mac_word;
    for (size_t i = 0; i < fmacs.size(); ++i)
    {
        const int k = fmacs[i], until = i + 1 < fmacs.size() ? fmacs[i + 1] : n;
        bool any = false;
        for (int j = k; j < until; ++j) any = any || ring_write[j];
        if (until == n || any) mac_word.insert(k);
    }
    stats.slots += n;
    stats.fmac += long(fmacs.size());
    std::array<int, 33> clean{};
    for (int k = 0; k < n; ++k)
    {
        Slot &sl = slots[k];
        const Decoded d = decode_upper(sl.upper);
        int flags = 0;
        if (!reader[k]) { flags |= NO_VISIBLE; ++stats.no_visible; }
        if (!ring_write[k]) { flags |= NO_RING_WRITE; ++stats.no_ring_write; }
        if (d.e.mac && !mac_word.count(k)) { flags |= NO_MAC_WORD; ++stats.no_mac_word; }
        if (d.e.flush)
        {
            const int vs_need = d.e.vt == V_OP ? XYZ : d.dest;
            const int vt_need = d.e.vt == V_BC ? (8 >> d.bc) : d.e.vt == V_FULL ? d.dest : d.e.vt == V_OP ? XYZ : -1;
            const int acc_need = d.e.acc ? d.dest : -1;
            const int needs[3] = {vs_need, vt_need, acc_need}, regs[3] = {d.fs, d.ft, ACC},
                      bits[3] = {CLEAN_VS, CLEAN_VT, CLEAN_ACC};
            for (int o = 0; o < 3; ++o)
            {
                if (needs[o] < 0) { flags |= bits[o]; continue; }
                ++stats.clamps;
                if ((clean[regs[o]] & needs[o]) == needs[o]) { flags |= bits[o]; ++stats.clamps_skipped; }
            }
        }
        sl.flags |= flags;
        if (d.e.dst != D_NONE)
        {
            const int target = d.e.dst == D_FD ? d.fd : d.e.dst == D_FT ? d.ft : ACC;
            const std::string name = d.e.name;
            int value;
            if (d.e.mac || name == "ITOF") value = d.dest;
            else if (name == "MAXbc" || name == "MINIbc") value = (clean[d.ft] & (8 >> d.bc)) ? (clean[d.fs] & d.dest) : 0;
            else if (name == "ABS") value = clean[d.fs] & d.dest;
            else value = 0;
            clean[target] = (clean[target] & ~d.dest) | value;
        }
        if (has_lower(sl.lower, sl.upper)) lower_transfer(sl.lower, clean);
        clean[0] = ALL_LANES;
    }
    if (slots.back().flags & NO_RING_WRITE)
    {
        std::fprintf(stderr, "vu_aot_gen: the last slot of a block drops its ring write\n");
        std::exit(1);
    }
}

uint32_t word(const std::string &b, int64_t at)
{
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v; // little-endian hosts (arm64, x86-64)
}

bool lower_is_branch(uint32_t lower, uint32_t upper)
{
    if (upper & 0x80000000u) return false;
    if (lower == 0x00000000u || lower == 0x8000033Cu) return false;
    return branch_op((lower >> 25) & 0x7F);
}
int64_t imm11(uint32_t lower)
{
    const int64_t v = lower & 0x7FF;
    return v & 0x400 ? v - 0x800 : v;
}

std::vector<Slot> plan(const std::string &image, int64_t region_end, int64_t entry, bool &stop)
{
    std::vector<Slot> slots;
    bool armed_branch = false, armed_e = false;
    stop = false;
    int64_t pc = entry;
    while (int(slots.size()) < MAX_SLOTS)
    {
        if (pc + 8 > region_end) break;
        const uint32_t lower = word(image, pc), upper = word(image, pc + 4);
        const bool maybe_branch = armed_branch, has_e = upper & 0x40000000u, is_branch = lower_is_branch(lower, upper);
        slots.push_back({pc, lower, upper, pc + 8, (maybe_branch ? MAYBE_BRANCH : 0) | (has_e ? ARM_E : 0)});
        if (armed_e) { stop = true; break; }
        armed_e = has_e;
        armed_branch = is_branch;
        if (maybe_branch) break;
        pc += 8;
    }
    if (!slots.empty()) slots.back().flags |= LAST;
    return slots;
}

std::vector<int64_t> successors(const std::vector<Slot> &slots, bool stop)
{
    if (stop) return {slots.back().next};
    const Slot &last = slots.back();
    if ((last.flags & MAYBE_BRANCH) && slots.size() >= 2)
    {
        std::vector<int64_t> out;
        const Slot &br = slots[slots.size() - 2];
        const int op = (br.lower >> 25) & 0x7F;
        if (static_target_op(op)) out.push_back(br.pc + 8 + imm11(br.lower) * 8);
        if (cond_op(op) || call_op(op)) out.push_back(last.next);
        return out;
    }
    return {last.next};
}

std::set<int64_t> seeds(const std::string &image, int64_t region_end)
{
    std::set<int64_t> s{0};
    for (int64_t pc = 0; pc < region_end - 7; pc += 8)
    {
        const uint32_t lower = word(image, pc), upper = word(image, pc + 4);
        if (upper & 0x40000000u) s.insert(pc + 16);
        if (lower_is_branch(lower, upper))
        {
            const int op = (lower >> 25) & 0x7F;
            if (static_target_op(op)) s.insert(pc + 8 + imm11(lower) * 8);
            if (call_op(op) || cond_op(op)) s.insert(pc + 16);
        }
    }
    return s;
}

struct Block
{
    std::vector<Slot> slots;
    bool stop;
};
using Raw = std::vector<uint64_t>;

// Keys in the Python generator's order: (first word, -length, words).
struct RawLess
{
    bool operator()(const Raw &a, const Raw &b) const
    {
        if (a[0] != b[0]) return a[0] < b[0];
        if (a.size() != b.size()) return a.size() > b.size();
        return a < b;
    }
};

void add(std::map<Raw, Block, RawLess> &catalogue, const std::string &image, int64_t region_end, std::set<int64_t> entries)
{
    for (int64_t s : seeds(image, region_end)) entries.insert(s);
    std::vector<int64_t> todo(entries.begin(), entries.end()); // sorted ascending; pop() takes the largest
    std::set<int64_t> done;
    while (!todo.empty())
    {
        const int64_t pc = todo.back();
        todo.pop_back();
        if (done.count(pc) || pc % 8 || pc < 0 || pc + 8 > region_end) continue;
        done.insert(pc);
        bool stop;
        std::vector<Slot> slots = plan(image, region_end, pc, stop);
        if (slots.empty()) continue;
        Raw raw;
        for (const Slot &sl : slots) raw.push_back(uint64_t(sl.upper) << 32 | sl.lower);
        const std::vector<int64_t> next = successors(slots, stop);
        catalogue.emplace(raw, Block{std::move(slots), stop}); // first one wins, like dict.setdefault
        for (int64_t n : next)
            if (!done.count(n)) todo.push_back(n);
    }
}

std::vector<std::string> elf_chains(const std::string &elf)
{
    std::vector<std::pair<int64_t, int64_t>> packets;
    const int64_t size = int64_t(elf.size());
    for (int64_t o = 4; o < size - 8; o += 8)
    {
        const uint32_t w = word(elf, o);
        if (((w >> 24) & 0x7F) != 0x4A || word(elf, o - 4) != 0) continue;
        const int64_t num = ((w >> 16) & 0xFF) ? ((w >> 16) & 0xFF) : 256, addr = w & 0xFFFF;
        if (addr + num <= 2048 && o + 4 + num * 8 <= size) packets.push_back({o, num});
    }
    std::vector<std::vector<std::pair<int64_t, int64_t>>> chains;
    std::vector<std::pair<int64_t, int64_t>> cur;
    for (const auto &p : packets)
    {
        if (!cur.empty() && cur.back().first + 4 + cur.back().second * 8 + 4 == p.first) cur.push_back(p);
        else
        {
            if (!cur.empty()) chains.push_back(cur);
            cur = {p};
        }
    }
    if (!cur.empty()) chains.push_back(cur);
    std::vector<std::string> out;
    for (const auto &chain : chains)
    {
        std::string code;
        for (const auto &[o, num] : chain) code += elf.substr(size_t(o + 4), size_t(num * 8));
        const int64_t n = int64_t(code.size()) / 8;
        int64_t nops = 0, e_bits = 0;
        for (int64_t k = 0; k < n; ++k)
        {
            const uint32_t lower = word(code, k * 8), upper = word(code, k * 8 + 4);
            nops += (upper == 0x000002FFu || lower == 0x8000033Cu);
            e_bits += bool(upper & 0x40000000u);
        }
        if (e_bits && nops * 20 >= n) out.push_back(code);
    }
    return out;
}

std::string hexw(uint64_t v, int width)
{
    char b[32];
    std::snprintf(b, sizeof b, "%0*llx", width, static_cast<unsigned long long>(v));
    return b;
}
} // namespace

int main(int argc, char **argv)
{
    std::string elf_path, output;
    bool no_analysis = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--elf" && i + 1 < argc) elf_path = argv[++i];
        else if (a == "--output" && i + 1 < argc) output = argv[++i];
        else if (a == "--no-analysis") no_analysis = true;
        else
        {
            std::fprintf(stderr, "usage: vu_aot_gen --elf ELF --output DIR [--no-analysis] (record directories: use vu_aot_gen.py)\n");
            return 2;
        }
    }
    if (output.empty())
    {
        std::fprintf(stderr, "vu_aot_gen: --output is required\n");
        return 2;
    }
    std::string elf;
    if (!elf_path.empty())
    {
        std::ifstream in(elf_path, std::ios::binary);
        if (!in) { std::fprintf(stderr, "vu_aot_gen: cannot read %s\n", elf_path.c_str()); return 1; }
        elf.assign(std::istreambuf_iterator<char>(in), {});
    }

    std::map<Raw, Block, RawLess> catalogue;
    long elf_regions = 0;
    if (!elf_path.empty())
        for (const std::string &code : elf_chains(elf))
        {
            ++elf_regions;
            std::set<int64_t> entries;
            for (int64_t pc = 0; pc < int64_t(code.size()); pc += 8) entries.insert(pc);
            add(catalogue, code, int64_t(code.size()), entries);
        }

    Sha256 digest;
    long total_slots = 0;
    for (const auto &[k, b] : catalogue)
    {
        put_le<uint32_t>(digest, uint32_t(k.size()));
        for (uint64_t w : k) put_le<uint64_t>(digest, w);
        put_le<uint8_t>(digest, b.stop ? 1 : 0);
        total_slots += long(k.size());
    }
    Stats stats;
    if (!no_analysis)
        for (auto &[k, b] : catalogue)
        {
            analyse(b.slots, stats);
            for (const Slot &sl : b.slots) put_le<uint16_t>(digest, uint16_t(sl.flags));
        }
    const std::string cat_id = digest.hex().substr(0, 16);

    std::ostringstream out;
    out << "// GENERATED by tools/vu-aot/vu_aot_gen.py \xe2\x80\x94 do not edit.\n";
    out << "// Statically recompiled VU microcode for RRV_VU_AOT (Ridge Racer V).\n";
    out << "// Derivative of the game's microcode: private repository only.\n";
    out << "// catalogue " << cat_id << ": " << elf_regions << " ELF upload chains, 0 recorded images, "
        << catalogue.size() << " blocks, " << total_slots << " slots.\n\n";
    out << "using VuAot = VU1Interpreter::Aot;\n\n";
    std::ostringstream entries;
    size_t i = 0;
    for (const auto &[k, b] : catalogue)
    {
        char name[32];
        std::snprintf(name, sizeof name, "vuaot_%05zu", i++);
        out << "static void " << name << "(VU1Interpreter &vu, uint32_t pc)\n{\n    const int p = VuAot::begin(vu);\n";
        for (size_t n = 0; n < b.slots.size(); ++n)
            out << "    VuAot::slot<0x" << hexw(b.slots[n].lower, 8) << "u, 0x" << hexw(b.slots[n].upper, 8) << "u, " << n
                << "u, " << b.slots[n].flags << "u>(vu, p, pc);\n";
        out << "    VuAot::end<" << b.slots.size() << "u>(vu, p);\n}\n";
        out << "static const uint64_t " << name << "_raw[] = {";
        for (size_t w = 0; w < k.size(); ++w) out << (w ? ", " : "") << "0x" << hexw(k[w], 16) << "ull";
        out << "};\n\n";
        entries << "    {0x" << hexw(k[0], 16) << "ull, " << k.size() << "u, " << (b.stop ? 1 : 0) << "u, " << name
                << "_raw, &" << name << "},\n";
    }
    out << "const VU1Interpreter::AotBlock kVuAotBlocks[] = {\n" << entries.str() << "};\n";
    out << "constexpr size_t kVuAotBlockCount = sizeof(kVuAotBlocks) / sizeof(kVuAotBlocks[0]);\n";
    out << "constexpr const char kVuAotCatalogId[] = \"" << cat_id << "\";\n";
    const std::string inc = out.str();

    std::filesystem::create_directories(output);
    std::ofstream(std::filesystem::path(output) / "rrv_vu_aot_blocks.inc", std::ios::binary) << inc;
    std::ostringstream m;
    m << "{\n  \"schema\": \"rrv-vu-aot-catalogue-v2\",\n  \"catalogue_id\": \"" << cat_id << "\",\n"
      << "  \"generator\": \"tools/vu-aot/vu_aot_gen.py\",\n  \"elf_chains\": " << elf_regions << ",\n"
      << "  \"recorded_images\": 0,\n  \"blocks\": " << catalogue.size() << ",\n  \"slots\": " << total_slots << ",\n"
      << "  \"max_block_slots\": " << MAX_SLOTS << ",\n  \"position_independent\": true,\n  \"block_analysis\": ";
    if (no_analysis) m << "null";
    else
        m << "{\n    \"slots\": " << stats.slots << ",\n    \"fmac\": " << stats.fmac << ",\n    \"no_mac_word\": "
          << stats.no_mac_word << ",\n    \"no_ring_write\": " << stats.no_ring_write << ",\n    \"no_visible\": "
          << stats.no_visible << ",\n    \"clamps\": " << stats.clamps << ",\n    \"clamps_skipped\": "
          << stats.clamps_skipped << "\n  }";
    m << ",\n  \"input_elf_sha256\": ";
    if (elf_path.empty()) m << "null";
    else m << "\"" << sha256(elf) << "\"";
    m << ",\n  \"record_dirs\": [],\n  \"blocks_inc_sha256\": \"" << sha256(inc) << "\"\n}\n";
    std::ofstream(std::filesystem::path(output) / "manifest.json", std::ios::binary) << m.str();
    std::printf("vu_aot_gen: catalogue %s, %zu blocks, %ld slots\n", cat_id.c_str(), catalogue.size(), total_slots);
    return 0;
}
