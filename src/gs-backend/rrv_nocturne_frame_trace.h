#pragma once

// Diagnostic only: correlate original draw completion with the GS field that
// consumes it. This never selects a frame, changes guest state, or reads MMIO.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>

namespace rrv::nocturne_frame_trace {
struct Receipt {
    uint64_t serial = 0, field = 0;
    uint32_t kernel = 0, request = 0, queue = 0, tag = 0;
    uint32_t mode = 0, buffer = 0, half = 0, count = 0, cadence = 0, slot = 3;
};

class Trace {
public:
    static constexpr uint32_t limit = 32768;
    ~Trace() { if (file_) std::fclose(file_); }
    void open(const char* path) {
        if (!path || !*path) return;
        file_ = std::fopen(path, "w");
        if (!file_) throw std::runtime_error("Could not open original frame trace");
        std::setvbuf(file_, nullptr, _IOFBF, 65536);
        std::fprintf(file_, "# kind host_ns gs_field delivery_serial delivery_field kernel request queue tag mode buffer half draw_count cadence actual_slot\n");
    }
    // Product initialization calls open() before either producer starts.
    bool enabled() const { return file_ != nullptr; }
    void delivered(Receipt value) {
        std::lock_guard lock(mutex_);
        value.serial = latest_.serial + 1;
        latest_ = value;
        row('D', value.field);
    }
    void field(uint64_t value) {
        std::lock_guard lock(mutex_);
        row('V', value);
    }
    Receipt latest() {
        std::lock_guard lock(mutex_);
        return latest_;
    }
    uint32_t rows() const { return rows_; } // after producers join only
private:
    void row(char kind, uint64_t field) {
        if (!file_ || rows_ >= limit) return;
        ++rows_;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto& r = latest_;
        std::fprintf(file_, "%c %lld %llu %llu %llu %u %u %u %08x %u %u %u %u %u %u\n",
            kind, static_cast<long long>(ns), static_cast<unsigned long long>(field),
            static_cast<unsigned long long>(r.serial), static_cast<unsigned long long>(r.field),
            r.kernel, r.request, r.queue, r.tag, r.mode, r.buffer, r.half, r.count, r.cadence, r.slot);
    }
    std::mutex mutex_;
    std::FILE* file_ = nullptr;
    Receipt latest_{};
    uint32_t rows_ = 0;
};
inline Trace& trace() { static Trace value; return value; }
} // namespace rrv::nocturne_frame_trace
