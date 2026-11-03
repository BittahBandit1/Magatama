// Runs original local Nocturne IRX modules through the pinned DQ8 IOP core.
// No EE game, card, GUI or SDL device is opened by this diagnostic.
#include "lle/iop.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
struct Host final : ps2x::iop::lle::EeLink {
  unsigned unsupported = 0, faults = 0, acks = 0, completed = 0, writes = 0;
  std::vector<uint32_t> completedTags;
  std::array<uint8_t, 0x2000> ee{};
  ps2x::iop::lle::Iop *iop = nullptr;
  std::vector<uint8_t> bank;
  size_t filePos = 0;
  unsigned rpcCalls = 0, rpcErrors = 0;
  bool fileOpen = false;
  std::vector<std::shared_ptr<ps2x::iop::lle::EeRpcRequest>> pending;
  static uint32_t word(const std::vector<uint8_t> &v, size_t n) {
    uint32_t w = 0;
    if (n + 4 <= v.size())
      std::memcpy(&w, v.data() + n, 4);
    return w;
  }
  bool submitEeRpc(std::shared_ptr<ps2x::iop::lle::EeRpcRequest> r) override {
    pending.push_back(std::move(r));
    return true;
  }
  void service() {
    auto work = std::move(pending);
    pending.clear();
    for (auto &r : work) {
      ++rpcCalls;
      int32_t result = -1;
      bool valid = r->sid == 0x66504f49u;
      if (valid)
        switch (r->function) {
        case 32: {
          const std::string path(r->send.begin(),
                                 std::find(r->send.begin(), r->send.end(), 0));
          valid = path == "PROBE.SMG" && !bank.empty() && !fileOpen;
          if (valid) {
            fileOpen = true;
            filePos = 0;
            result = 1;
          }
          break;
        }
        case 33:
          valid = fileOpen && word(r->send, 0) == 1;
          if (valid)
            result = int32_t(bank.size());
          break;
        case 34: {
          auto dst = word(r->send, 4), size = word(r->send, 8);
          valid = fileOpen && r->send.size() >= 12 && word(r->send, 0) == 1 &&
                  dst < 0x200000u && size <= 0x200000u - dst;
          if (valid) {
            size = uint32_t(std::min(size_t(size), bank.size() - filePos));
            iop->writeMemory(dst, bank.data() + filePos, size);
            filePos += size;
            result = int32_t(size);
          }
          break;
        }
        case 35:
          valid = fileOpen && word(r->send, 0) == 1;
          if (valid) {
            fileOpen = false;
            result = 0;
          }
          break;
        default:
          valid = false;
          break;
        }
      r->reply.resize(r->receiveSize);
      if (r->reply.size() >= 4)
        std::memcpy(r->reply.data(), &result, 4);
      r->success = valid;
      r->done = true;
      if (!valid)
        ++rpcErrors;
    }
  }
  bool readEe(uint32_t a, void *p, uint32_t n) override {
    if (a > ee.size() || n > ee.size() - a)
      return false;
    std::memcpy(p, ee.data() + a, n);
    return true;
  }
  bool writeEe(uint32_t a, const void *p, uint32_t n) override {
    if (a > ee.size() || n > ee.size() - a)
      return false;
    std::memcpy(ee.data() + a, p, n);
    ++writes;
    return true;
  }
  // Only advertise the two original EE services. Requests are not faked.
  bool eeServer(uint32_t sid) override {
    return sid == 0x66504F49u || sid == 0x54524E53u;
  }
  bool sendCommand(uint32_t cid, const std::vector<uint8_t> &p) override {
    if (cid == 0x80000001u && p.size() == 24) {
      uint32_t code;
      std::memcpy(&code, p.data() + 16, 4);
      if (code == 31)
        ++acks;
      else if (code == 30) {
        ++completed;
        completedTags.push_back(word(p, 20));
      } else
        return false;
      return true;
    }
    return false;
  }
  void log(const std::string &s) override {
    std::fprintf(stderr, "%s\n", s.c_str());
    if (s.find("no kernel service") != std::string::npos)
      ++unsupported;
    if (s.find("unsupported instruction") != std::string::npos)
      ++faults;
  }
};
int main(int argc, char **argv) {
  if (argc < 2 || argc > 4) {
    std::fprintf(stderr,
                 "usage: nocturne-iop-probe <local extracted disc directory> "
                 "[private local SMG file [PCM WAV output]]\n");
    return 2;
  }
  Host host;
  ps2x::iop::lle::Iop iop(host);
  host.iop = &iop;
  unsigned loaded = 0;
  if (argc >= 3) {
    std::ifstream f(argv[2], std::ios::binary);
    if (!f)
      return 2;
    host.bank.assign(std::istreambuf_iterator<char>(f), {});
    if (host.bank.size() < 176)
      return 2;
  }
  for (const char *name : {"USERIRX/SDFIOPFL.IRX", "SOUNDIRX/LIBSD.IRX",
                           "SOUNDIRX/MODHSYN.IRX", "SOUNDIRX/MODMIDI.IRX",
                           "SOUNDIRX/MODMSIN.IRX", "USERIRX/SDFSDMAN.IRX"}) {
    std::ifstream f(std::filesystem::path(argv[1]) / name, std::ios::binary);
    if (!f) {
      std::fprintf(stderr, "Missing local module %s\n", name);
      return 2;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    auto id = iop.loadModule(bytes, name, {});
    std::printf("IRX %s bytes=%zu module_id=%d\n", name, bytes.size(), id);
    if (id > 0)
      ++loaded;
    else
      return 1;
  }
  for (unsigned n = 0; n < 4800; ++n) {
    int16_t l, r;
    host.service();
    iop.step(l, r);
  }
  std::printf(
      "BRINGUP loaded=%u/6 unsupported_imports=%u instruction_faults=%u "
      "ready_acks=%u cmd_handler=%d healthy=%d; no audible-output claim\n",
      loaded, host.unsupported, host.faults, host.acks,
      iop.hasCommandHandler(0), iop.healthy());
  if (loaded != 6 || host.unsupported || host.faults || host.acks != 1 ||
      !iop.hasCommandHandler(0) || !iop.healthy())
    return 1;
  // Minimal synthesizer work area, following the USA EE sdfSndInit layout.
  // This tests original IRX execution, not host-synthesized sound.
  std::array<uint32_t, 0x5a0 / 4> work{};
  work[0] = 4;
  work[1] = 4;
  work[2] = iop.allocate(0x4000);
  work[3] = 0x4000;
  for (unsigned n = 0; n < 16; ++n)
    work[4 + n * 4] = n | 0x2000;
  work[0x18 / 4] = iop.allocate(0x24e00);
  // The USA ELF caller's four program-buffer sizes at 0x342fc0.
  const uint32_t sizes[4] = {2624, 1856, 10304, 4736};
  for (unsigned n = 0; n < 4; ++n) {
    work[(0x110 + n * 8) / 4] = sizes[n];
    work[(0x114 + n * 8) / 4] = iop.allocate(sizes[n]);
  }
  work[0x1c8 / 4] = 0x1000;
  work[0x1cc / 4] = 0x5a0;
  for (unsigned n = 0; n < 7; ++n)
    work[(0x190 + n * 8) / 4] = 0xffffffff;
  const uint32_t address = iop.allocate(sizeof(work));
  iop.writeMemory(address, work.data(), sizeof(work));
  std::array<uint32_t, 8> packet{32, 0, 0, 0x10000000, 0x1234, address, 0, 0};
  if (!iop.receiveCommand(0, packet.data(), sizeof(packet)))
    return 1;
  for (unsigned n = 0; n < 24000 && iop.healthy(); ++n) {
    int16_t l, r;
    host.service();
    iop.step(l, r);
  }
  iop.readMemory(address, work.data(), sizeof(work));
  std::printf("SYNTH_INIT completed_acks=%u ee_dma_writes=%u errors=%u "
              "last_error=%08x healthy=%d; no audible-output claim\n",
              host.completed, host.writes, work[0x1d8 / 4], work[0x1dc / 4],
              iop.healthy());
  if (host.completed != 1 || work[0x1d8 / 4] != 0 || !iop.healthy())
    return 1;
  if (!host.bank.empty()) {
    // Exercise the original driver's normal filename bank load and actual
    // SDFIOPFL -> EE RPC -> IOP transfer, then original MIDI/synth playback.
    std::array<uint32_t, 32> load{128, 0, 0, 0x70a00000, 0x1235};
    std::memcpy(reinterpret_cast<uint8_t *>(load.data()) + 20, "PROBE.SMG", 10);
    if (!iop.receiveCommand(0, load.data(), sizeof(load)))
      return 1;
    for (unsigned n = 0; n < 96000 && iop.healthy() && host.completed < 2;
         ++n) {
      int16_t l, r;
      host.service();
      iop.step(l, r);
    }
    iop.readMemory(address, work.data(), sizeof(work));
    const uint32_t id = Host::word(host.bank, 0x28);
    bool found = false;
    for (unsigned n = 0; n < 7; ++n)
      found |= work[(0x190 + n * 8) / 4] == (id >> 16);
    std::printf("BANK_LOAD completed_acks=%u loaded=%d rpc_calls=%u "
                "rpc_errors=%u bytes_read=%zu errors=%u healthy=%d\n",
                host.completed, found, host.rpcCalls, host.rpcErrors,
                host.filePos, work[0x1d8 / 4], iop.healthy());
    if (!found || host.completed != 2 || host.rpcErrors || work[0x1d8 / 4] ||
        !iop.healthy())
      return 1;
    packet = {32, 0, 0, 0x10200000, 0x1236, id | 1u, 0, 127};
    if (!iop.receiveCommand(0, packet.data(), sizeof(packet)))
      return 1;
    uint64_t nonzero = 0;
    int peak = 0;
    std::vector<int16_t> pcm;
    pcm.reserve(480000);
    for (unsigned n = 0; n < 240000 && iop.healthy(); ++n) {
      int16_t l, r;
      host.service();
      iop.step(l, r);
      pcm.push_back(l);
      pcm.push_back(r);
      if (l || r)
        ++nonzero;
      peak = std::max({peak, std::abs(int(l)), std::abs(int(r))});
    }
    iop.readMemory(address, work.data(), sizeof(work));
    std::printf(
        "PCM_RENDER frames=240000 nonzero_frames=%llu peak=%d errors=%u "
        "healthy=%d; headless test, listening quality unverified\n",
        static_cast<unsigned long long>(nonzero), peak, work[0x1d8 / 4],
        iop.healthy());
    if (argc == 4) {
      std::ofstream out(argv[3], std::ios::binary);
      auto u16 = [&](uint16_t v) {
        out.put(char(v));
        out.put(char(v >> 8));
      };
      auto u32 = [&](uint32_t v) {
        u16(uint16_t(v));
        u16(uint16_t(v >> 16));
      };
      const uint32_t bytes = uint32_t(pcm.size() * 2);
      out.write("RIFF", 4);
      u32(bytes + 36);
      out.write("WAVEfmt ", 8);
      u32(16);
      u16(1);
      u16(2);
      u32(48000);
      u32(48000 * 4);
      u16(4);
      u16(16);
      out.write("data", 4);
      u32(bytes);
      for (auto v : pcm)
        u16(uint16_t(v));
      out.close();
      if (!out)
        return 2;
    }
    return nonzero > 4800 && peak > 128 &&
                   host.completedTags ==
                       std::vector<uint32_t>({0x1234, 0x1235, 0x1236}) &&
                   work[0x1d8 / 4] == 0 && iop.healthy()
               ? 0
               : 1;
  }
  return host.completed == 1 && work[0x1d8 / 4] == 0 && iop.healthy() &&
                 loaded == 6 && host.unsupported == 0 && host.faults == 0 &&
                 host.acks == 1 && iop.hasCommandHandler(0) && iop.healthy()
             ? 0
             : 1;
}
