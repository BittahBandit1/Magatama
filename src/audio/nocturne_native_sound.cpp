// Opt-in DQ8 original-IRX audio bridge. All guest work runs under the existing
// guest permit. SDL only consumes PCM from the SPSC ring below.
#include "nocturne_native_sound.h"
#include "lle/iop.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "rrv_sdl_audio.h"
#include "nocturne_pcm_ring.h"
#include "nocturne_audio_stretch.h"
#include "rrv_mac_qos.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

namespace magatama::native_sound {
namespace {
using ps2x::iop::lle::EeRpcRequest;
using ps2x::iop::lle::Iop;
constexpr uint32_t kRam = 0x02000000u, kIopRam = 0x00200000u;
constexpr uint64_t kEePerSample = 8u * Iop::kCyclesPerSample;
uint32_t physical(uint32_t a) { return a & 0x1fffffffu; }
bool fits(uint32_t a, size_t n) {
  uint32_t offset = 0;
  bool scratch = false;
  return ps2ResolveGuestPointer(a, offset, scratch) && !scratch &&
         offset < kRam && n <= kRam - offset;
}
uint32_t word(const void *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
using Ring = PcmRing;
struct Server {
  uint32_t sid = 0, func = 0, buffer = 0, queue = 0, gp = 0;
  uint32_t thread = 0;
  bool sleeping = false;
};
struct State final : ps2x::iop::lle::EeLink {
  PS2Runtime *runtime = nullptr;
  uint8_t *ram = nullptr;
  std::unique_ptr<Iop> iop;
  std::map<uint32_t, PS2Runtime::RecompiledFunction> original;
  std::map<uint32_t, Server> servers;
  std::map<uint32_t, uint32_t> eeAllocations;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> rpcKinds;
  std::set<uint32_t> wakeServers;
  std::deque<std::shared_ptr<EeRpcRequest>> requests;
  std::deque<std::vector<uint8_t>> commands;
  Ring ring;
  std::unique_ptr<PcmStretch> stretch;
  std::chrono::steady_clock::time_point traceEnd{};
  uint64_t traceEvents = 0;
  bool loaded = false, servicing = false;
  uint64_t rendered = 0, epoch = 0, lastService = 0, lastField = ~0ull,
           rpcCount = 0, commandCount = 0, nonzero = 0;
  uint64_t fastForward = 0;
  int peak = 0;
  std::FILE *wav = nullptr;
  bool readEe(uint32_t a, void *dst, uint32_t n) override {
    if (!ram || !fits(a, n))
      return false;
    std::memcpy(dst, ram + physical(a), n);
    return true;
  }
  bool writeEe(uint32_t a, const void *src, uint32_t n) override {
    if (!ram || !fits(a, n))
      return false;
    std::memcpy(ram + physical(a), src, n);
    return true;
  }
  bool eeServer(uint32_t sid) override { return servers.count(sid) != 0; }
  bool submitEeRpc(std::shared_ptr<EeRpcRequest> request) override {
    if (!eeServer(request->sid))
      return false;
    requests.push_back(std::move(request));
    auto &server = servers.at(requests.back()->sid);
    if (server.sleeping)
      wakeServers.insert(server.thread);
    return true;
  }
  bool sendCommand(uint32_t cid, const std::vector<uint8_t> &data) override {
    if (cid != 0x80000001u || data.size() != 24)
      return false;
    commands.push_back(data);
    return true;
  }
  void log(const std::string &s) override {
    std::fprintf(stderr, "[native-sound] %s\n", s.c_str());
  }
};
State &state() {
  static auto *s = new State;
  return *s;
}
void requireHealthy() {
  if (!state().iop->healthy())
    throw std::runtime_error("Native sound IOP fault; see native-sound log");
}
void returnToCaller(R5900Context *ctx, uint32_t value) {
  SET_GPR_U32(ctx, 2, value);
  ctx->pc = GPR_U32(ctx, 31);
}
void bind(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt) {
  auto &s = state();
  s.ram = ram;
  s.runtime = rt;
  if (!s.iop)
    s.iop = std::make_unique<Iop>(s);
}
void finishWav() {
  auto &s = state();
  if (s.wav) {
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0,   0,   0,   0, 'W', 'A', 'V',
                     'E', 'f', 'm', 't', ' ', 16,  0,   0, 0,   1,   0,
                     2,   0,   0,   0,   0,   0,   0,   0, 0,   0,   4,
                     0,   16,  0,   'd', 'a', 't', 'a', 0, 0,   0,   0};
    auto put = [&](size_t at, uint32_t v) {
      for (unsigned j = 0; j < 4; ++j)
        h[at + j] = uint8_t(v >> (8 * j));
    };
    put(4, uint32_t(s.rendered * 4 + 36));
    put(24, 48000);
    put(28, 48000 * 4);
    put(40, uint32_t(s.rendered * 4));
    std::fseek(s.wav, 0, SEEK_SET);
    std::fwrite(h, 1, 44, s.wav);
    std::fclose(s.wav);
    s.wav = nullptr;
  }
  std::fprintf(stderr,
               "[native-sound] final frames=%llu nonzero=%llu peak=%d rpc=%llu "
               "commands=%llu underrun=%llu overrun=%llu\n",
               (unsigned long long)s.rendered, (unsigned long long)s.nonzero,
               s.peak, (unsigned long long)s.rpcCount,
               (unsigned long long)s.commandCount,
               (unsigned long long)s.ring.underrun.load(),
               (unsigned long long)s.ring.overrun.load());
  for (const auto &[key, count] : s.rpcKinds)
    std::fprintf(stderr,
                 "[native-sound] RPC totals sid=%08x fno=%u count=%llu\n",
                 key.first, key.second, (unsigned long long)count);
}
void loadModules() {
  auto &s = state();
  if (s.loaded)
    return;
  if (!s.eeServer(0x66504f49u))
    throw std::runtime_error(
        "Native sound: original EE IOP-file server is not registered");
  const char *root = std::getenv("MAGATAMA_DISC");
  for (const char *name : {"USERIRX/SDFIOPFL.IRX", "SOUNDIRX/LIBSD.IRX",
                           "SOUNDIRX/MODHSYN.IRX", "SOUNDIRX/MODMIDI.IRX",
                           "SOUNDIRX/MODMSIN.IRX", "USERIRX/SDFSDMAN.IRX"}) {
    std::ifstream f(std::filesystem::path(root ? root : ".") / name,
                    std::ios::binary);
    if (!f)
      throw std::runtime_error(
          std::string("Native sound missing local module ") + name);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    if (s.iop->loadModule(bytes, name, {}) < 0)
      throw std::runtime_error(
          std::string("Native sound module start failed: ") + name);
  }
  s.ring.prefill = Ring::prefillFromEnvironment();
  if(PcmStretch::requested()) s.stretch=std::make_unique<PcmStretch>();
  s.loaded = true;
  s.epoch = s.runtime->gate3TemporalV1().now();
  s.lastService = s.epoch;
  const char *ff = std::getenv("RRV_GATE3_FAST_FORWARD");
  s.fastForward = ff ? std::strtoull(ff, nullptr, 10) : 0;
  if (const char *path = std::getenv("MAGATAMA_NATIVE_WAV"); path && *path) {
    s.wav = std::fopen(path, "wb");
    if (!s.wav)
      throw std::runtime_error("Native sound WAV open failed");
    uint8_t h[44]{};
    std::fwrite(h, 1, 44, s.wav);
  }
  std::atexit(finishWav);
  rrv::audio::initialize();
  rrv::audio::setStreamSource(Ring::pull, &s.ring);
  std::fprintf(stderr,"[native-sound] pitch-preserving playback stretch=%s; rate/pitch=1; original guest clock unchanged\n",s.stretch?"on":"off");
  std::fprintf(stderr, "[native-sound] original six-module driver loaded; "
                       "guest-clock stereo 48kHz; prefill=%zu frames (%.1f ms)\n",
                       s.ring.prefill, s.ring.prefill / 48.0);
}
void service(PS2Runtime *rt, R5900Context *ctx) {
  auto &s = state();
  if (!s.loaded || s.servicing)
    return;
  // Optional QoS applies to the EE thread actually producing PCM. The
  // hardware callback keeps CoreAudio's own scheduling policy untouched.
  rrv::macInteractiveQos();
  auto now = rt->gate3TemporalV1().now();
  if (now - s.lastService < 294912u / 2 && s.commands.empty())
    return; // 0.5 ms guest time
  s.servicing = true;
  struct Exit {
    bool &b;
    ~Exit() { b = false; }
  } exit{s.servicing};
  // Opt-in wall-clock evidence only; never advances the original driver or
  // changes the SDL callback. Keep timestamps off the normal service path.
  static const bool trace = [] { const char* v = std::getenv("MAGATAMA_AUDIO_TRACE");
    return v && std::strcmp(v, "1") == 0; }();
  struct Trace {
    State& s; PS2Runtime* rt; bool enabled;
    std::chrono::steady_clock::time_point begin;
    ~Trace() {
      if (!enabled) return;
      const auto end = std::chrono::steady_clock::now();
      const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
      const double gap = s.traceEnd.time_since_epoch().count() ? ms(begin-s.traceEnd) : 0;
      const auto events = s.ring.underrunEvents.load();
      if (gap > 30 || ms(end-begin) > 8 || events != s.traceEvents)
        std::fprintf(stderr, "[native-audio-trace] field=%llu host=%.6f gap_ms=%.3f service_ms=%.3f queued=%llu events=%llu\n",
          (unsigned long long)rt->gate3TemporalV1().starts(),
          std::chrono::duration<double>(begin.time_since_epoch()).count(), gap, ms(end-begin),
          (unsigned long long)(s.ring.write.load()-s.ring.read.load()), (unsigned long long)events);
      s.traceEnd=end; s.traceEvents=events;
    }
  } diagnostic{s, rt, trace, trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
  s.lastService = now;
  uint64_t target = (now - s.epoch) / kEePerSample;
  while (s.rendered < target) {
    int16_t l = 0, r = 0;
    s.iop->step(l, r);
    ++s.rendered;
    if (l || r)
      ++s.nonzero;
    s.peak = std::max({s.peak, std::abs(int(l)), std::abs(int(r))});
    if (s.wav) {
      int16_t p[2] = {l, r};
      std::fwrite(p, 2, 2, s.wav);
    }
    if (rt->gate3TemporalV1().starts() >= s.fastForward) {
      if(s.stretch)s.stretch->push(l,r,s.ring);else s.ring.push(l,r);
    }
    requireHealthy();
  }
  while (!s.commands.empty()) {
    auto packet = std::move(s.commands.front());
    s.commands.pop_front();
    uint32_t code = word(packet.data() + 16), value = word(packet.data() + 20);
    // Original SIF system command 1 is SetSreg. The EE library's own
    // register array is read by generated sceSifGetSreg.
    if (code >= 32)
      throw std::runtime_error("Native sound invalid SIF register index");
    std::memcpy(s.ram + 0x59bbc0u + code * 4, &value, 4);
    R5900Context copy = *ctx;
    SET_GPR_U32(&copy, 4, code);
    SET_GPR_U32(&copy, 5, value);
    ps2_stubs::sceSifSetSreg(s.ram, &copy, rt);
  }
  auto field = rt->gate3TemporalV1().starts();
  if (field / 300 != s.lastField / 300) {
    s.lastField = field;
    std::fprintf(stderr,
                 "[native-sound] field=%llu frames=%llu rpc=%llu pending=%zu "
                 "commands=%llu peak=%d queued=%llu underrun=%llu events=%llu priming=%llu overrun=%llu stretch_tempo=%.3f stretch_min=%.3f stretch_max=%.3f\n",
                 (unsigned long long)field, (unsigned long long)s.rendered,
                 (unsigned long long)s.rpcCount, s.requests.size(),
                 (unsigned long long)s.commandCount, s.peak,
                 (unsigned long long)(s.ring.write.load()-s.ring.read.load()),
                 (unsigned long long)s.ring.underrun.load(),
                 (unsigned long long)s.ring.underrunEvents.load(),
                 (unsigned long long)s.ring.primingFrames.load(),
                 (unsigned long long)s.ring.overrun.load(),
                 s.stretch?s.stretch->tempo:1.0,
                 s.stretch?s.stretch->minTempo:1.0,
                 s.stretch?s.stretch->maxTempo:1.0);
  }
  // The interpreter has returned; wake actual EE server threads now. Never
  // preempt an active R3000A interpreter frame with reentrant EE guest work.
  auto wake = std::move(s.wakeServers);
  s.wakeServers.clear();
  s.servicing = false;
  for (auto thread : wake) {
    R5900Context copy = *ctx;
    SET_GPR_U32(&copy, 4, thread);
    ps2_syscalls::iWakeupThread(s.ram, &copy, rt);
  }
}
bool idle(PS2Runtime *, uint64_t cycle) {
  const auto &s = state();
  return !s.loaded || s.servicing ||
         (s.commands.empty() && cycle - s.lastService < 294912u / 2);
}
void alloc(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt) {
  bind(ram, ctx, rt);
  uint32_t bytes = GPR_U32(ctx, ctx->pc == 0x2e2c08u ? 5 : 4);
  uint32_t address =
      bytes && bytes <= kIopRam ? state().iop->allocate(bytes) : 0;
  if (address)
    state().eeAllocations[address] = bytes;
  returnToCaller(ctx, address);
}
void freeHeap(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt) {
  bind(ram, ctx, rt);
  auto &s = state();
  uint32_t address = physical(GPR_U32(ctx, 4));
  if (!s.eeAllocations.erase(address)) {
    returnToCaller(ctx, uint32_t(-1));
    return;
  }
  s.iop->release(address);
  returnToCaller(ctx, 0);
}
void dma(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt) {
  bind(ram, ctx, rt);
  auto &s = state();
  uint32_t list = GPR_U32(ctx, 4), count = GPR_U32(ctx, 5);
  if (!count || count > 32 || !fits(list, count * 16)) {
    returnToCaller(ctx, 0);
    return;
  }
  // Validate the entire list before making any guest-visible write.
  for (uint32_t n = 0; n < count; ++n) {
    auto *d = ram + physical(list) + 16 * n;
    uint32_t src = word(d), dst = physical(word(d + 4)), size = word(d + 8);
    if (!fits(src, size) || !s.iop->ownsMemory(dst, size)) {
      returnToCaller(ctx, 0);
      return;
    }
  }
  for (uint32_t n = 0; n < count; ++n) {
    auto *d = ram + physical(list) + 16 * n;
    s.iop->writeMemory(physical(word(d + 4)), ram + physical(word(d)),
                       word(d + 8));
  }
  returnToCaller(ctx, 1);
}
void otherData(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt) {
  bind(ram, ctx, rt);
  auto &s = state();
  uint32_t src = physical(GPR_U32(ctx, 5)), dst = GPR_U32(ctx, 6),
           size = GPR_U32(ctx, 7);
  if (!fits(dst, size) || !s.iop->ownsMemory(src, size)) {
    returnToCaller(ctx, uint32_t(-1));
    return;
  }
  s.iop->readMemory(src, ram + physical(dst), size);
  returnToCaller(ctx, 0);
}
void send(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt) {
  bind(ram, ctx, rt);
  auto &s = state();
  uint32_t cid = GPR_U32(ctx, 4), p = GPR_U32(ctx, 5), size = GPR_U32(ctx, 6);
  if (cid != 0) {
    s.original.at(ctx->pc)(ram, ctx, rt);
    return;
  }
  loadModules();
  if (!fits(p, size) || size < 20 || size > 128)
    throw std::runtime_error("Native sound malformed EE command packet");
  const uint32_t copyBytes = ((word(ram + physical(p) + 12) >> 28) + 1) * 16;
  if (copyBytes > 128)
    throw std::runtime_error("Native sound command copy exceeds 128 bytes");
  std::array<uint8_t, 128> packet{};
  std::memcpy(packet.data(), ram + physical(p), size);
  // Start the dispatcher at its registered handler before admitting packets.
  if (!s.iop->hasCommandHandler(0))
    s.iop->settle();
  if (!s.iop->receiveCommand(cid, packet.data(), 128))
    throw std::runtime_error("Native sound command not admitted");
  ++s.commandCount;
  if (s.commandCount < 32)
    std::fprintf(stderr, "[native-sound] send opt=%08x tag=%x bytes=%u\n",
                 word(packet.data() + 12), word(packet.data() + 16), size);
  returnToCaller(ctx, 1);
}
void regServer(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt) {
  bind(ram, ctx, rt);
  auto &s = state();
  auto entry = ctx->pc;
  Server server{GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7),
                GPR_U32(ctx, 10), GPR_U32(ctx, 28)};
  R5900Context current = *ctx;
  ps2_syscalls::GetThreadId(ram, &current, rt);
  server.thread = GPR_U32((&current), 2);
  s.original.at(entry)(ram, ctx, rt);
  if (server.sid == 0x66504f49u || server.sid == 0x54524e53u) {
    s.servers[server.sid] = server;
    std::fprintf(
        stderr,
        "[native-sound] EE server sid=%08x func=%08x buf=%08x queue=%08x\n",
        server.sid, server.func, server.buffer, server.queue);
  }
}
void rpcLoop(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt) {
  bind(ram, ctx, rt);
  auto &s = state();
  const uint32_t queue = GPR_U32(ctx, 4), entry = ctx->pc;
  auto find = [&]() {
    return std::find_if(s.requests.begin(), s.requests.end(), [&](auto &r) {
      return s.servers.at(r->sid).queue == queue;
    });
  };
  bool known = std::any_of(s.servers.begin(), s.servers.end(),
                           [&](auto &kv) { return kv.second.queue == queue; });
  if (!known) {
    s.original.at(entry)(ram, ctx, rt);
    return;
  }
  // This executes on the actual game's RPC-server thread. Blocking file and
  // movie providers can release its permit while every other EE thread runs.
  while (!rt->isStopRequested()) {
    while (find() == s.requests.end() && !rt->isStopRequested()) {
      for (auto &[sid, server] : s.servers)
        if (server.queue == queue)
          server.sleeping = true;
      ps2_syscalls::SleepThread(ram, ctx, rt);
      for (auto &[sid, server] : s.servers)
        if (server.queue == queue)
          server.sleeping = false;
    }
    if (rt->isStopRequested())
      break;
    auto it = find();
    if (it == s.requests.end())
      continue;
    auto request = *it;
    s.requests.erase(it);
    auto server = s.servers.at(request->sid);
    ++s.rpcCount;
    ++s.rpcKinds[{request->sid, request->function}];
    if (request->send.size() > 256 ||
        !fits(server.buffer, request->send.size()))
      throw std::runtime_error("Native sound EE RPC request overflow");
    std::memcpy(ram + physical(server.buffer), request->send.data(),
                request->send.size());
    const uint32_t stack = rt->guestMalloc(0x4000, 16);
    if (!stack)
      throw std::runtime_error("Native sound EE RPC stack allocation failed");
    struct Stack {
      PS2Runtime *r;
      uint32_t p;
      ~Stack() { r->guestFree(p); }
    } lease{rt, stack};
    struct Restore {
      PS2Runtime *r;
      R5900Context *c;
      ~Restore() { r->gate3SetWaitContextV1(c); }
    } restore{rt, ctx};
    R5900Context call = *ctx;
    SET_GPR_U32(&call, 4, request->function);
    SET_GPR_U32(&call, 5, server.buffer);
    SET_GPR_U32(&call, 6, uint32_t(request->send.size()));
    SET_GPR_U32(&call, 28, server.gp);
    SET_GPR_U32(&call, 29, stack + 0x4000 - 16);
    SET_GPR_U32(&call, 31, 0);
    call.pc = server.func;
    while (call.pc && !rt->isStopRequested()) {
      rt->gate3CheckpointV1(&call);
      auto fn = rt->lookupFunction(call.pc);
      if (!fn)
        throw std::runtime_error("Native sound EE RPC missing continuation");
      PS2Runtime::GuestExecutionScope execution(rt);
      fn(ram, &call, rt);
    }
    const uint32_t result = GPR_U32((&call), 2);
    if (!fits(result, request->receiveSize))
      throw std::runtime_error("Native sound EE RPC reply outside RAM");
    request->reply.resize(request->receiveSize);
    std::memcpy(request->reply.data(), ram + physical(result),
                request->receiveSize);
    request->success = true;
    request->done = true;
    if (s.rpcCount < 48)
      std::fprintf(
          stderr, "[native-sound] RPC sid=%08x fno=%u result=%08x reply=%08x\n",
          request->sid, request->function, result,
          request->reply.size() >= 4 ? word(request->reply.data()) : 0);
  }
  returnToCaller(ctx, 0);
}
void init(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt) {
  bind(ram, ctx, rt);
  loadModules();
  state().original.at(ctx->pc)(ram, ctx, rt);
}
} // namespace
bool requested() {
  const char *v = std::getenv("MAGATAMA_SOUND_NATIVE");
  return v && v[0] == '1';
}
bool install(PS2Runtime &runtime) {
  if (!requested())
    return false;
  auto &s = state();
  for (auto [address, fn] : std::initializer_list<
           std::pair<uint32_t, PS2Runtime::RecompiledFunction>>{
           {0x2e2b98, alloc},
           {0x2e2c08, alloc},
           {0x2e2d00, freeHeap},
           {0x2dc260, dma},
           {0x2dc270, dma},
           {0x2de920, otherData},
           {0x2de2b0, send},
           {0x2de2f0, send},
           {0x2df010, regServer},
           {0x2df428, rpcLoop},
           {0x2c0ff8, init}}) {
    if (!runtime.hasFunction(address))
      throw std::runtime_error("Native sound missing SIF entry");
    s.original[address] = runtime.lookupFunction(address);
    runtime.registerFunction(address, fn);
  }
  g_gate8AudioHooksV1.service = service;
  g_gate8AudioHooksV1.idle = idle;
  std::fprintf(stderr,
               "[native-sound] opt-in DQ8 original-driver bridge armed\n");
  return true;
}
} // namespace magatama::native_sound
