#pragma once
//
// xi_trigger_recorder.hpp — record TriggerBus events to disk and replay
// them later. Observer mode (doesn't interfere with live dispatch).
//
// Storage layout (manifest + raw pixels):
//
//   <recording-dir>/
//     manifest.json
//     000001_<source>.raw         (w,h,ch header + pixels)
//     000001_<source2>.raw
//     000002_<source>.raw
//     ...
//
// Raw file header (24 bytes, little-endian):
//     magic    uint32  = 0x58494D47  ('XIMG')
//     version  uint32  = 1
//     width    uint32
//     height   uint32
//     channels uint32
//     reserved uint32  = 0
// then: (width * height * channels) bytes of pixel data.
//
// Replay: reads the manifest in order, emits each event through the bus
// using host_api->emit_trigger so the full pipeline (sink / observer /
// correlation policy) sees the event exactly as the live source would.
//

#include "xi_abi.h"
#include "xi_atomic_io.hpp"
#include "xi_image_pool.hpp"
#include "xi_trigger_bus.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xi {

struct RecordedImage {
    std::string source;
    std::string path;      // file name, relative to recording dir
};

struct RecordedEvent {
    xi_trigger_id id{0, 0};
    int64_t       timestamp_us = 0;
    std::vector<RecordedImage> images;
};

class TriggerRecorder {
public:
    static TriggerRecorder& instance() {
        static TriggerRecorder r;
        return r;
    }

    bool start(const std::string& folder) {
        std::lock_guard<std::mutex> lk(mu_);
        if (recording_.load()) return false;
        std::filesystem::create_directories(folder);
        folder_ = folder;
        events_.clear();
        started_at_us_ = xi::now_us();
        recording_ = true;

        // Install observer on the bus.
        TriggerBus::instance().set_observer([this](TriggerEvent ev) {
            this->on_event(std::move(ev));
        });
        return true;
    }

    bool stop() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!recording_.load()) return false;
        TriggerBus::instance().clear_observer();
        recording_ = false;
        // Write manifest.
        write_manifest();
        return true;
    }

    bool is_recording() const { return recording_.load(); }

    int event_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return (int)events_.size();
    }

    std::string folder() const {
        std::lock_guard<std::mutex> lk(mu_);
        return folder_;
    }

    // Replay a previously recorded session. Opens manifest.json, reads each
    // event in order, waits for the original timestamp cadence (scaled by
    // speed), then emits through the bus so it drives the inspection
    // pipeline exactly as live sources would.
    //
    // Runs in a background thread; returns immediately.
    //
    // speed: 1.0 = real time, 2.0 = double, 0.5 = half, 0 = instant.
    bool start_replay(const std::string& folder, double speed = 1.0) {
        std::lock_guard<std::mutex> lk(mu_);
        if (replaying_.load()) return false;
        auto manifest_path = std::filesystem::path(folder) / "manifest.json";
        if (!std::filesystem::exists(manifest_path)) return false;
        auto events = load_manifest(folder);
        if (events.empty()) return false;
        replaying_ = true;
        replay_thread_ = std::thread([this, folder, events, speed] {
            play_(folder, events, speed);
            replaying_ = false;
        });
        replay_thread_.detach();
        return true;
    }

    bool is_replaying() const { return replaying_.load(); }

private:
    mutable std::mutex         mu_;
    std::string                folder_;
    std::atomic<bool>          recording_{false};
    std::atomic<bool>          replaying_{false};
    std::vector<RecordedEvent> events_;
    int64_t                    started_at_us_ = 0;
    std::thread                replay_thread_;

    void on_event(TriggerEvent ev) {
        if (!recording_.load()) {
            for (auto& [n, h] : ev.images) ImagePool::instance().release(h);
            return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        int seq = (int)events_.size();
        RecordedEvent rec;
        rec.id           = ev.id;
        rec.timestamp_us = ev.timestamp_us;
        for (auto& [src, handle] : ev.images) {
            // Build a filename-safe version of the source (replace '/' with '_')
            std::string safe = src;
            for (auto& c : safe) if (c == '/' || c == '\\') c = '_';
            char namebuf[48];
            std::snprintf(namebuf, sizeof(namebuf), "%06d_%s.raw", seq, safe.c_str());
            std::string rel = namebuf;
            auto full = std::filesystem::path(folder_) / rel;

            int32_t w = ImagePool::instance().width(handle);
            int32_t h = ImagePool::instance().height(handle);
            int32_t ch = ImagePool::instance().channels(handle);
            const uint8_t* data = ImagePool::instance().data(handle);
            if (data && w > 0 && h > 0) {
                std::ofstream f(full.string(), std::ios::binary);
                uint32_t hdr[6] = { 0x58494D47u, 1u, (uint32_t)w, (uint32_t)h, (uint32_t)ch, 0u };
                f.write((const char*)hdr, sizeof(hdr));
                f.write((const char*)data, (size_t)w * h * ch);
            }
            rec.images.push_back({ src, rel });
            ImagePool::instance().release(handle);
        }
        events_.push_back(std::move(rec));
    }

    void write_manifest() {
        auto p = std::filesystem::path(folder_) / "manifest.json";
        std::string out = "{\n  \"version\": 1,\n";
        if (!events_.empty()) {
            out += "  \"first_ts_us\": " + std::to_string(events_.front().timestamp_us) + ",\n";
            out += "  \"last_ts_us\": "  + std::to_string(events_.back().timestamp_us)  + ",\n";
        }
        out += "  \"events\": [\n";
        for (size_t i = 0; i < events_.size(); ++i) {
            auto& e = events_[i];
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                          (unsigned long long)e.id.hi, (unsigned long long)e.id.lo);
            out += "    { \"tid\": \"";
            out += buf;
            out += "\", \"ts\": " + std::to_string(e.timestamp_us) + ", \"frames\": { ";
            for (size_t j = 0; j < e.images.size(); ++j) {
                if (j) out += ", ";
                out += "\""; out += e.images[j].source; out += "\": \"";
                out += e.images[j].path; out += "\"";
            }
            out += " } }";
            if (i + 1 < events_.size()) out += ",";
            out += "\n";
        }
        out += "  ]\n}\n";
        xi::atomic_write(p, out);
    }

    std::vector<RecordedEvent> load_manifest(const std::string& folder) {
        // Minimal line-scanning parser — good enough for our own writer.
        std::vector<RecordedEvent> out;
        std::ifstream f((std::filesystem::path(folder) / "manifest.json").string());
        std::string line;
        while (std::getline(f, line)) {
            auto tp = line.find("\"tid\":");
            if (tp == std::string::npos) continue;
            RecordedEvent ev;
            // tid
            auto q1 = line.find('"', tp + 6);
            auto q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            std::string tid_str = line.substr(q1 + 1, q2 - q1 - 1);
            if (tid_str.size() == 32) {
                ev.id.hi = std::stoull(tid_str.substr(0, 16),  nullptr, 16);
                ev.id.lo = std::stoull(tid_str.substr(16, 16), nullptr, 16);
            }
            // ts
            auto tsp = line.find("\"ts\":");
            if (tsp != std::string::npos) {
                try { ev.timestamp_us = std::stoll(line.substr(tsp + 5)); } catch (...) {}
            }
            // frames
            auto fp = line.find("\"frames\":");
            if (fp != std::string::npos) {
                auto bs = line.find('{', fp);
                auto be = (bs == std::string::npos) ? std::string::npos : line.find('}', bs);
                if (bs != std::string::npos && be != std::string::npos) {
                    std::string inner = line.substr(bs + 1, be - bs - 1);
                    size_t pos = 0;
                    while (pos < inner.size()) {
                        auto s1 = inner.find('"', pos); if (s1 == std::string::npos) break;
                        auto s2 = inner.find('"', s1 + 1); if (s2 == std::string::npos) break;
                        auto v1 = inner.find('"', s2 + 1); if (v1 == std::string::npos) break;
                        auto v2 = inner.find('"', v1 + 1); if (v2 == std::string::npos) break;
                        ev.images.push_back({ inner.substr(s1 + 1, s2 - s1 - 1),
                                              inner.substr(v1 + 1, v2 - v1 - 1) });
                        pos = v2 + 1;
                    }
                }
            }
            out.push_back(std::move(ev));
        }
        return out;
    }

    void play_(const std::string& folder, std::vector<RecordedEvent> events, double speed) {
        int64_t prev_ts = events.front().timestamp_us;
        for (auto& e : events) {
            // Wait for the original cadence (if speed > 0)
            if (speed > 0 && e.timestamp_us > prev_ts) {
                int64_t delta_us = (int64_t)((e.timestamp_us - prev_ts) / speed);
                std::this_thread::sleep_for(std::chrono::microseconds(delta_us));
            }
            prev_ts = e.timestamp_us;

            // Load all frames + group by source. Each recorded image's
            // "source" field is the FULL stored name ("source" or
            // "source/key"); split on the first '/' to recover the pair.
            struct PerSource {
                std::vector<xi_image_handle>  handles;
                std::vector<std::string>      keys;     // owned strings (we'll point xi_record_image::key at these)
            };
            std::unordered_map<std::string, PerSource> by_source;
            std::vector<xi_image_handle> all_allocs;

            for (auto& img : e.images) {
                auto path = std::filesystem::path(folder) / img.path;
                std::ifstream f(path.string(), std::ios::binary);
                uint32_t hdr[6];
                f.read((char*)hdr, sizeof(hdr));
                if (f.gcount() != (std::streamsize)sizeof(hdr) || hdr[0] != 0x58494D47u) continue;
                int32_t w = (int32_t)hdr[2];
                int32_t h = (int32_t)hdr[3];
                int32_t ch = (int32_t)hdr[4];
                xi_image_handle handle = ImagePool::instance().create(w, h, ch);
                f.read((char*)ImagePool::instance().data(handle), (std::streamsize)w * h * ch);
                all_allocs.push_back(handle);

                auto slash = img.source.find('/');
                std::string source_name, key;
                if (slash == std::string::npos) {
                    source_name = img.source;
                    key = "frame";
                } else {
                    source_name = img.source.substr(0, slash);
                    key = img.source.substr(slash + 1);
                }
                auto& ps = by_source[source_name];
                ps.handles.push_back(handle);
                ps.keys.push_back(std::move(key));
            }

            // Emit one bus event per source-group, sharing the recorded tid.
            for (auto& [source, ps] : by_source) {
                std::vector<xi_record_image> entries;
                entries.reserve(ps.handles.size());
                for (size_t i = 0; i < ps.handles.size(); ++i) {
                    entries.push_back({ ps.keys[i].c_str(), ps.handles[i] });
                }
                TriggerBus::instance().emit(source, e.id, e.timestamp_us,
                                             entries.data(), (int)entries.size());
            }
            // Bus addref'd internally for each emit; release our refs.
            for (auto h : all_allocs) ImagePool::instance().release(h);

            if (!replaying_.load()) break;   // cancelled
        }
    }
};

} // namespace xi
