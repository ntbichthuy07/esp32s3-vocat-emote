#include "vov_radio_service.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "audio_service.h"
#include "audio_codec.h"
#include "board.h"
#include "http.h"
#include "network_interface.h"

#include "esp_aac_dec.h"
#include "esp_audio_types.h"
#include "esp_ae_rate_cvt.h"

namespace {

using ShouldStopFn = std::function<bool()>;

constexpr int kHttpTimeoutMs = 8000;
constexpr uint32_t kRadioTaskStackSize = 10240;
constexpr uint32_t kDecodeTaskStackSize = 10240;
constexpr UBaseType_t kRadioTaskPriority = 3;
constexpr size_t kPlaylistMaxBytes = 32 * 1024;
constexpr size_t kSegmentMaxBytes = 512 * 1024;
constexpr size_t kHttpReadChunk = 2048;
// Cap how many stale segments we play back-to-back after falling behind (slow
// network, a stall, ...) rather than binging the whole backlog with no limit.
constexpr size_t kMaxCatchUpSegments = 4;
constexpr uint32_t kMinRefreshMs = 2000;
constexpr uint32_t kMaxRefreshMs = 15000;
constexpr uint32_t kRetryDelayMs = 3000;
// How many downloaded-but-not-yet-decoded segments to keep queued ahead of
// playback, and how many must be queued before the very first one starts
// decoding. At VOV's ~10s segments this is roughly a 20-30s cushion, enough
// to absorb a slow fetch or a brief stall without an audible dropout.
constexpr size_t kMaxQueuedSegments = 3;
constexpr size_t kPrebufferSegments = 2;
// How much to scale the radio's own PCM samples down to while the device is
// actively listening for a command (see VovRadioService::SetDucked). Quiet
// enough that it stops dominating what the mic picks up, not fully muted so
// there's still an audible "something's still on" cue.
constexpr float kDuckedVolumeScale = 0.12f;

const char* TAG = "VovRadio";

bool StartsWith(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    return s.compare(0, n, prefix) == 0;
}

bool IsSupportedUrl(const std::string& url) {
    return StartsWith(url, "http://") || StartsWith(url, "https://");
}

// Sleeps up to `total_ms`, checking `should_stop` every 100ms so a cancel is
// noticed promptly instead of only after the full delay.
void InterruptibleDelay(uint32_t total_ms, const ShouldStopFn& should_stop) {
    for (uint32_t waited = 0; waited < total_ms && !should_stop(); waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Resolves a playlist-relative URI against the playlist's own URL.
std::string ResolveUrl(const std::string& base, const std::string& ref) {
    if (ref.empty()) return base;
    if (IsSupportedUrl(ref)) return ref;

    size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) return ref;
    size_t host_start = scheme_end + 3;

    if (ref[0] == '/') {
        size_t host_end = base.find('/', host_start);
        std::string origin = (host_end == std::string::npos) ? base : base.substr(0, host_end);
        return origin + ref;
    }

    size_t last_slash = base.find_last_of('/');
    std::string dir = (last_slash == std::string::npos || last_slash < host_start)
                          ? base + "/"
                          : base.substr(0, last_slash + 1);
    return dir + ref;
}

struct MediaPlaylist {
    bool is_master = false;
    std::string master_variant_url;
    uint32_t target_duration_s = 6;
    uint32_t media_sequence = 0;
    std::vector<std::string> segment_urls;  // resolved absolute URLs, in order
    // True when segments are MPEG-TS (.ts) rather than raw ADTS AAC (.aac) --
    // set from the first segment URI seen and assumed to hold for the whole
    // stream. Some VOV stations (e.g. the audio-lss.vov.vn CDN) package as TS.
    bool segments_are_ts = false;
};

// True if `uri`'s path (ignoring any "?query" suffix) ends in ".ts".
bool LooksLikeTsSegment(const std::string& uri) {
    size_t q = uri.find('?');
    size_t path_len = (q == std::string::npos) ? uri.size() : q;
    return path_len >= 3 && uri.compare(path_len - 3, 3, ".ts") == 0;
}

// Parses an HLS playlist (master or media). `base_url` resolves relative URIs.
// A radio stream is audio-only, so a master playlist's first variant is used.
MediaPlaylist ParsePlaylist(const std::string& text, const std::string& base_url) {
    MediaPlaylist out;
    bool next_is_variant = false;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;

        if (StartsWith(line, "#EXT-X-STREAM-INF:")) {
            next_is_variant = true;
            continue;
        }
        if (StartsWith(line, "#")) {
            if (StartsWith(line, "#EXT-X-TARGETDURATION:")) {
                out.target_duration_s = static_cast<uint32_t>(std::strtoul(line.c_str() + 22, nullptr, 10));
            } else if (StartsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) {
                out.media_sequence = static_cast<uint32_t>(std::strtoul(line.c_str() + 22, nullptr, 10));
            }
            continue;
        }

        if (next_is_variant) {
            out.is_master = true;
            out.master_variant_url = ResolveUrl(base_url, line);
            return out;  // audio radio: a single variant is enough, stop here
        }
        if (out.segment_urls.empty()) {
            out.segments_are_ts = LooksLikeTsSegment(line);
        }
        out.segment_urls.push_back(ResolveUrl(base_url, line));
    }
    return out;
}

// Fetches `url` fully as text, capped at `max_bytes`. Used for playlists.
// Reuses `http`'s connection (kept alive across calls by the caller) rather
// than opening a fresh one each time.
bool FetchText(Http* http, const std::string& url, size_t max_bytes, std::string& out_text,
               std::string& out_error) {
    http->SetHeader("Accept", "*/*");
    auto opened = http->Open("GET", url);
    if (!opened) {
        out_error = "connect failed: " + opened.error().ToString();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status < 200 || *status >= 300) {
        int code = status ? *status : -1;
        out_error = "HTTP status " + std::to_string(code);
        return false;
    }

    out_text.clear();
    char buf[1024];
    while (out_text.size() < max_bytes) {
        auto r = http->Read(buf, sizeof(buf));
        if (!r || *r <= 0) break;
        out_text.append(buf, *r);
    }
    return true;
}

// A raw byte buffer allocated in PSRAM. Segments run several hundred KB each
// and a few are held at once (see SegmentQueue) to smooth over network
// jitter; PSRAM keeps that off the much smaller internal DRAM heap the rest
// of the firmware needs (a device on this project has been seen with well
// under 100KB of free internal RAM at times).
struct SegmentBuffer {
    uint8_t* data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    bool is_ts = false;

    SegmentBuffer() = default;
    SegmentBuffer(const SegmentBuffer&) = delete;
    SegmentBuffer& operator=(const SegmentBuffer&) = delete;
    SegmentBuffer(SegmentBuffer&& other) noexcept { *this = std::move(other); }
    SegmentBuffer& operator=(SegmentBuffer&& other) noexcept {
        if (this != &other) {
            Clear();
            data = other.data;
            size = other.size;
            capacity = other.capacity;
            is_ts = other.is_ts;
            other.data = nullptr;
            other.size = 0;
            other.capacity = 0;
        }
        return *this;
    }
    ~SegmentBuffer() { Clear(); }

    void Clear() {
        if (data) {
            heap_caps_free(data);
            data = nullptr;
        }
        size = 0;
        capacity = 0;
    }

    bool Reserve(size_t new_capacity) {
        if (new_capacity <= capacity) return true;
        uint8_t* new_data = static_cast<uint8_t*>(heap_caps_realloc(data, new_capacity, MALLOC_CAP_SPIRAM));
        if (!new_data) return false;
        data = new_data;
        capacity = new_capacity;
        return true;
    }

    bool Append(const uint8_t* bytes, size_t len) {
        if (size + len > capacity) {
            size_t new_capacity = capacity == 0 ? (64 * 1024) : capacity;
            while (new_capacity < size + len) new_capacity *= 2;
            if (!Reserve(new_capacity)) return false;
        }
        memcpy(data + size, bytes, len);
        size += len;
        return true;
    }
};

// Bounded producer/consumer queue of downloaded (but not yet decoded)
// segments, so downloading can run well ahead of playback instead of the two
// being lockstep -- a slow segment fetch then just eats into the cushion
// instead of causing an audible dropout. Every wait polls `should_stop`
// periodically rather than blocking indefinitely, so a stop is still noticed
// promptly even while full/empty.
class SegmentQueue {
public:
    explicit SegmentQueue(size_t max_depth) : max_depth_(max_depth) {}

    bool Push(SegmentBuffer&& seg, const ShouldStopFn& should_stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.size() >= max_depth_) {
            if (should_stop()) return false;
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (should_stop()) return false;
        queue_.push_back(std::move(seg));
        cv_.notify_all();
        return true;
    }

    bool Pop(SegmentBuffer& out, const ShouldStopFn& should_stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty()) {
            if (should_stop()) return false;
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        cv_.notify_all();
        return true;
    }

    // Blocks until at least `n` segments are queued -- a real pre-buffer
    // cushion before the very first one is decoded -- or should_stop().
    void WaitForPrebuffer(size_t n, const ShouldStopFn& should_stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.size() < n && !should_stop()) {
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<SegmentBuffer> queue_;
    size_t max_depth_;
};

// Downmixes/upmixes interleaved PCM from `src_channels` to `dst_channels`.
std::vector<int16_t> ConvertChannels(const int16_t* samples, size_t total_samples, int src_channels,
                                     int dst_channels) {
    if (src_channels <= 0 || dst_channels <= 0 || src_channels == dst_channels) {
        return std::vector<int16_t>(samples, samples + total_samples);
    }
    size_t frames = total_samples / static_cast<size_t>(src_channels);
    std::vector<int16_t> out(frames * dst_channels);
    if (src_channels == 2 && dst_channels == 1) {
        for (size_t i = 0; i < frames; i++) {
            int32_t sum = samples[i * 2] + samples[i * 2 + 1];
            out[i] = static_cast<int16_t>(sum / 2);
        }
    } else if (src_channels == 1 && dst_channels == 2) {
        for (size_t i = 0; i < frames; i++) {
            out[i * 2] = samples[i];
            out[i * 2 + 1] = samples[i];
        }
    } else {
        for (size_t i = 0; i < frames; i++) {
            for (int c = 0; c < dst_channels; c++) {
                out[i * dst_channels + c] = samples[i * src_channels + (c % src_channels)];
            }
        }
    }
    return out;
}

// Decodes ADTS AAC bytes fed incrementally and plays the resulting PCM through
// AudioService, resampling/remixing to the board's output format as needed.
class AacPipeline {
public:
    AacPipeline(AudioService& audio_service, int out_rate, int out_channels, ShouldStopFn should_stop,
               const std::atomic<float>* volume_scale)
        : audio_service_(audio_service),
          out_rate_(out_rate),
          out_channels_(out_channels > 0 ? out_channels : 1),
          should_stop_(std::move(should_stop)),
          volume_scale_(volume_scale),
          pcm_buf_(8192) {}

    ~AacPipeline() {
        if (resampler_) esp_ae_rate_cvt_close(resampler_);
        if (decoder_) esp_aac_dec_close(decoder_);
    }

    bool Open() {
        // The vendored decoder logs an ESP_LOGE line for every single failed
        // frame -- normal, expected traffic here since a resync walks one
        // byte at a time through a leading ID3 tag (or a corrupted segment,
        // until we give up on it). A bad network patch can otherwise flood
        // the synchronous console UART with hundreds of lines per second,
        // which is slow enough to make the whole device feel unresponsive
        // (including to a voice "stop" command) for as long as it lasts.
        esp_log_level_set("ESP_AAC_DEC", ESP_LOG_NONE);

        esp_aac_dec_cfg_t cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
        cfg.aac_plus_enable = true;  // tolerate HE-AAC/SBR streams too
        return esp_aac_dec_open(&cfg, sizeof(cfg), &decoder_) == ESP_AUDIO_ERR_OK && decoder_ != nullptr;
    }

    // Each HLS segment here is its own self-contained ADTS stream (it starts
    // with a fresh ID3 tag, not a continuation of the previous one), so any
    // bytes left over from the previous segment are not a partial frame of
    // *this* one -- they're stale garbage that would corrupt this segment's
    // sync alignment. Call this before starting a new segment.
    void ResetForNewSegment() {
        leftover_.clear();
        consecutive_resync_failures_ = 0;
    }

    // Appends `len` bytes of ADTS AAC and decodes/plays every complete frame
    // found. Leftover partial-frame bytes are kept for the next call.
    // Returns false once resync has failed so many times in a row that the
    // remaining data is almost certainly not ADTS AAC at all (a corrupted or
    // truncated download) -- the caller should then abandon this segment.
    bool Feed(const uint8_t* data, size_t len) {
        leftover_.insert(leftover_.end(), data, data + len);
        if (leftover_.empty()) return true;

        esp_audio_dec_in_raw_t raw{};
        raw.buffer = leftover_.data();
        raw.len = static_cast<uint32_t>(leftover_.size());

        while (raw.len > 0) {
            if (should_stop_()) break;

            esp_audio_dec_out_frame_t out{};
            out.buffer = pcm_buf_.data();
            out.len = static_cast<uint32_t>(pcm_buf_.size());
            esp_audio_dec_info_t info{};

            auto ret = esp_aac_dec_decode(decoder_, &raw, &out, &info);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > pcm_buf_.size()) {
                pcm_buf_.resize(out.needed_size);
                continue;  // retry the same input with a bigger output buffer
            }
            if (ret == ESP_AUDIO_ERR_DATA_LACK) {
                break;  // wait for more input bytes on the next Feed() call
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                // Unrecoverable-looking frame: skip a byte to try to resync
                // on the next ADTS sync word rather than aborting the stream.
                size_t skip = raw.consumed > 0 ? raw.consumed : 1;
                raw.buffer += skip;
                raw.len -= static_cast<uint32_t>(std::min<size_t>(skip, raw.len));
                ++consecutive_resync_failures_;
                if (consecutive_resync_failures_ > kMaxConsecutiveResyncFailures) {
                    ESP_LOGW(TAG, "Giving up on this segment after %d consecutive AAC resync "
                                 "failures (likely a corrupted/truncated download)",
                             consecutive_resync_failures_);
                    leftover_.clear();
                    consecutive_resync_failures_ = 0;
                    return false;
                }
                // A failing decoder call still logs on the C library side (out
                // of our control) and a tight resync loop can print hundreds
                // of these back to back. Yield periodically so that flood
                // can't starve the shared log output (and this task's own
                // CPU time) away from higher-priority work.
                if ((consecutive_resync_failures_ % 16) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                continue;
            }

            consecutive_resync_failures_ = 0;
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;

            if (out.decoded_size > 0) {
                ++frames_decoded_;
                PlayPcm(reinterpret_cast<const int16_t*>(out.buffer), out.decoded_size / sizeof(int16_t),
                        info);
            }
        }

        size_t consumed_total = leftover_.size() - raw.len;
        leftover_.erase(leftover_.begin(), leftover_.begin() + consumed_total);
        return true;
    }

    // Total AAC frames successfully decoded so far.
    uint32_t FramesDecoded() const { return frames_decoded_; }

private:
    // Generous enough to skip past any realistic leading ID3v2/metadata tag
    // (the largest seen from VOV is 73 bytes), but small enough to abandon a
    // truly corrupted segment quickly instead of scanning hundreds of KB of
    // it one byte at a time, each failed byte logging on the decoder's side.
    static constexpr int kMaxConsecutiveResyncFailures = 128;
    int consecutive_resync_failures_ = 0;

    void PlayPcm(const int16_t* samples, size_t count, const esp_audio_dec_info_t& info) {
        if (count == 0 || info.channel == 0) return;

        std::vector<int16_t> converted = ConvertChannels(samples, count, info.channel, out_channels_);

        int src_rate = info.sample_rate > 0 ? static_cast<int>(info.sample_rate) : out_rate_;
        if (src_rate != out_rate_) {
            if (resampler_ && resampler_rate_ != src_rate) {
                esp_ae_rate_cvt_close(resampler_);
                resampler_ = nullptr;
            }
            if (!resampler_) {
                esp_ae_rate_cvt_cfg_t cfg{};
                cfg.src_rate = static_cast<uint32_t>(src_rate);
                cfg.dest_rate = static_cast<uint32_t>(out_rate_);
                cfg.channel = static_cast<uint8_t>(out_channels_);
                cfg.bits_per_sample = ESP_AUDIO_BIT16;
                cfg.complexity = 2;
                cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                if (esp_ae_rate_cvt_open(&cfg, &resampler_) == ESP_AE_ERR_OK) {
                    resampler_rate_ = src_rate;
                } else {
                    resampler_ = nullptr;
                }
            }
            if (resampler_) {
                uint32_t in_samples = static_cast<uint32_t>(converted.size() / out_channels_);
                uint32_t max_out = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(resampler_, in_samples, &max_out);
                if (max_out > 0) {
                    std::vector<int16_t> resampled(static_cast<size_t>(max_out) * out_channels_);
                    uint32_t actual = max_out;
                    esp_ae_rate_cvt_process(resampler_, (esp_ae_sample_t)converted.data(), in_samples,
                                            (esp_ae_sample_t)resampled.data(), &actual);
                    resampled.resize(static_cast<size_t>(actual) * out_channels_);
                    converted = std::move(resampled);
                }
            }
        }

        if (converted.empty() || should_stop_()) return;

        float scale = volume_scale_ ? volume_scale_->load() : 1.0f;
        if (scale < 0.999f) {
            for (auto& sample : converted) {
                int scaled = static_cast<int>(static_cast<float>(sample) * scale);
                sample = static_cast<int16_t>(std::clamp(scaled, -32768, 32767));
            }
        }
        audio_service_.PushPcmToPlaybackQueue(std::move(converted), true);
    }

    AudioService& audio_service_;
    int out_rate_;
    int out_channels_;
    ShouldStopFn should_stop_;
    const std::atomic<float>* volume_scale_;
    void* decoder_ = nullptr;
    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    int resampler_rate_ = 0;
    std::vector<uint8_t> leftover_;
    std::vector<uint8_t> pcm_buf_;
    uint32_t frames_decoded_ = 0;
};

// VOV's segments (Wowza "cupertino" audio-only packaging) each start with a
// leading ID3v2 tag -- Apple-HLS-style timed metadata -- before the actual
// ADTS AAC data. Returns the tag's total byte length (header + body), or 0 if
// `data` doesn't start with one, so the caller can skip past it before
// feeding the rest to the AAC decoder (which chokes on the ID3 bytes).
size_t Id3v2TagSize(const uint8_t* data, size_t len) {
    if (len < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3') {
        return 0;
    }
    // Bytes 6-9: body size as a 28-bit "synchsafe" integer (7 significant
    // bits per byte, high bit always 0), not counting the 10-byte header.
    uint32_t body_size = (static_cast<uint32_t>(data[6] & 0x7F) << 21) |
                          (static_cast<uint32_t>(data[7] & 0x7F) << 14) |
                          (static_cast<uint32_t>(data[8] & 0x7F) << 7) |
                          static_cast<uint32_t>(data[9] & 0x7F);
    return 10 + body_size;
}

constexpr size_t kTsPacketSize = 188;

// Extracts the raw ADTS AAC elementary stream out of an MPEG-TS-packaged HLS
// segment (some VOV stations, e.g. audio-lss.vov.vn, package this way instead
// of plain .aac files) and feeds it to an AacPipeline. Finds the audio track
// itself from the TS's PAT/PMT rather than assuming a fixed PID, since that
// varies per stream. Persists across segments within one station session:
// the audio PID, once found, does not need rediscovering every segment.
class TsDemuxer {
public:
    // Appends `len` bytes of MPEG-TS, demuxes complete 188-byte packets, and
    // feeds the audio track's ES bytes to `pipeline`. Returns whatever the
    // last `pipeline.Feed()` call returned (false = pipeline gave up on this
    // segment; see AacPipeline::Feed), or true if nothing was fed.
    bool Feed(const uint8_t* data, size_t len, AacPipeline& pipeline) {
        buf_.insert(buf_.end(), data, data + len);

        bool ok = true;
        size_t pos = 0;
        while (pos + kTsPacketSize <= buf_.size()) {
            if (buf_[pos] != 0x47) {
                ++pos;  // lost sync -- scan forward for the next packet start
                continue;
            }
            ok = ProcessPacket(buf_.data() + pos, pipeline);
            pos += kTsPacketSize;
            if (!ok) break;
        }
        buf_.erase(buf_.begin(), buf_.begin() + pos);
        return ok;
    }

    // Each segment is its own independent TS stream (a fresh PAT/PMT at the
    // start), so any leftover partial-packet bytes from the previous one are
    // stale. The discovered PIDs stay valid across segments, though -- no
    // need to rediscover them, and re-parsing a repeated PAT/PMT is a no-op.
    void ResetForNewSegment() { buf_.clear(); }

private:
    bool ProcessPacket(const uint8_t* pkt, AacPipeline& pipeline) {
        bool payload_start = (pkt[1] & 0x40) != 0;
        int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
        int adaptation_field_control = (pkt[3] >> 4) & 0x3;

        if (adaptation_field_control == 0 || adaptation_field_control == 2) {
            return true;  // reserved, or adaptation-field-only: no payload
        }
        size_t offset = 4;
        if (adaptation_field_control == 3) {
            uint8_t af_len = pkt[offset];
            offset += 1 + af_len;
            if (offset > kTsPacketSize) return true;
        }
        if (offset >= kTsPacketSize) return true;

        const uint8_t* payload = pkt + offset;
        size_t payload_len = kTsPacketSize - offset;

        if (pid == 0) {
            if (payload_start) ParsePsiSection(payload, payload_len, /*is_pat=*/true);
            return true;
        }
        if (pmt_pid_ >= 0 && pid == pmt_pid_) {
            if (payload_start) ParsePsiSection(payload, payload_len, /*is_pat=*/false);
            return true;
        }
        if (audio_pid_ >= 0 && pid == audio_pid_) {
            if (payload_start) {
                size_t es_offset = PesHeaderSize(payload, payload_len);
                if (es_offset >= payload_len) return true;
                return pipeline.Feed(payload + es_offset, payload_len - es_offset);
            }
            return pipeline.Feed(payload, payload_len);
        }
        return true;
    }

    // Parses a PAT or PMT section. Assumes it fits within this one TS packet
    // (true for the small, single-program tables a live audio-only HLS
    // stream uses) -- a section spanning multiple packets is simply clipped,
    // which just means a slower first PID discovery, not a decode error.
    void ParsePsiSection(const uint8_t* payload, size_t len, bool is_pat) {
        if (len < 1) return;
        uint8_t pointer_field = payload[0];
        size_t off = 1 + pointer_field;
        if (off + 3 > len) return;

        uint16_t section_length = ((payload[off + 1] & 0x0F) << 8) | payload[off + 2];
        size_t section_end = off + 3 + section_length;
        if (section_end > len) section_end = len;
        size_t entries_end = (section_end >= 4) ? section_end - 4 : 0;  // exclude trailing CRC32

        size_t p = off + 3 + 5;  // past table_id+section_length and the 5-byte common section header
        if (is_pat) {
            while (p + 4 <= entries_end) {
                uint16_t program_number = (payload[p] << 8) | payload[p + 1];
                uint16_t pid = ((payload[p + 2] & 0x1F) << 8) | payload[p + 3];
                if (program_number != 0 && pmt_pid_ < 0) {
                    pmt_pid_ = pid;
                }
                p += 4;
            }
        } else {
            if (p + 4 > section_end) return;
            p += 2;  // PCR_PID
            uint16_t program_info_length = ((payload[p] & 0x0F) << 8) | payload[p + 1];
            p += 2 + program_info_length;
            while (p + 5 <= entries_end) {
                uint8_t stream_type = payload[p];
                uint16_t pid = ((payload[p + 1] & 0x1F) << 8) | payload[p + 2];
                uint16_t es_info_length = ((payload[p + 3] & 0x0F) << 8) | payload[p + 4];
                if (stream_type == 0x0F && audio_pid_ < 0) {  // ADTS AAC (required by the HLS spec)
                    audio_pid_ = pid;
                }
                p += 5 + es_info_length;
            }
        }
    }

    // Byte length of the PES header at the start of `payload` (a new PES
    // packet), to skip before the raw ADTS AAC elementary-stream data
    // begins. Returns `len` (skip everything) if it doesn't look like a
    // valid PES header.
    static size_t PesHeaderSize(const uint8_t* payload, size_t len) {
        if (len < 9 || payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
            return len;
        }
        size_t size = 9 + payload[8];  // 6-byte start code/stream_id/length + 3 + PES_header_data_length
        return size <= len ? size : len;
    }

    int pmt_pid_ = -1;
    int audio_pid_ = -1;
    std::vector<uint8_t> buf_;
};

// Downloads one full HLS segment into `out` (a PSRAM buffer), reusing `http`'s
// connection (kept alive by the caller) rather than reconnecting every time --
// a fresh TLS handshake per segment (700ms+ seen in practice against VOV's
// servers) was long enough on its own to starve the tiny downstream playback
// queue and cause audible dropouts every ~10s. Returns true only once the
// segment was read to a natural EOF; a partial read (error, stopped, or the
// size cap hit first) is discarded by the caller rather than fed to the
// decoder half-formed.
bool DownloadSegment(Http* http, const std::string& url, SegmentBuffer& out,
                    const ShouldStopFn& should_stop) {
    out.Clear();
    // Reserve the full cap in one shot rather than letting Append() grow it
    // in several reallocs (64K -> 128K -> ... -> 512K) across the download.
    // Segment buffers churn through PSRAM constantly here (one per ~10s,
    // freed and reallocated at whatever size each download happened to
    // need); a single fixed-size allocation per segment is much kinder to
    // the allocator than repeated variable-sized reallocs, which fragment it
    // over a long session -- a fragmentation-induced allocation failure here
    // has been observed to bring down the whole device, not just the radio.
    if (!out.Reserve(kSegmentMaxBytes)) {
        ESP_LOGE(TAG, "Out of PSRAM to reserve segment buffer");
        return false;
    }
    http->SetHeader("Accept", "*/*");
    auto opened = http->Open("GET", url);
    if (!opened) {
        ESP_LOGW(TAG, "Segment connect failed: %s", opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status < 200 || *status >= 300) {
        ESP_LOGW(TAG, "Segment HTTP status %d", status ? *status : -1);
        return false;
    }

    char buf[kHttpReadChunk];
    while (!should_stop() && out.size < kSegmentMaxBytes) {
        auto r = http->Read(buf, sizeof(buf));
        if (!r) return false;
        if (*r == 0) return out.size > 0;  // natural EOF: a complete download
        if (!out.Append(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(*r))) {
            ESP_LOGE(TAG, "Out of PSRAM for segment buffer");
            return false;
        }
    }
    return false;  // stopped, or hit the size cap before EOF
}

// Feeds one fully-downloaded segment through the AAC pipeline (via a
// TsDemuxer first when it's MPEG-TS-packaged).
void DecodeSegment(const SegmentBuffer& seg, AacPipeline& pipeline, TsDemuxer& ts_demuxer) {
    pipeline.ResetForNewSegment();
    if (seg.is_ts) {
        ts_demuxer.ResetForNewSegment();
        ts_demuxer.Feed(seg.data, seg.size, pipeline);
        return;
    }
    size_t tag_size = Id3v2TagSize(seg.data, seg.size);
    if (tag_size < seg.size) {
        pipeline.Feed(seg.data + tag_size, seg.size - tag_size);
    }
}

struct DecodeTaskArgs {
    SegmentQueue* queue;
    AudioService* audio_service;
    int out_rate;
    int out_channels;
    ShouldStopFn should_stop;
    const std::atomic<float>* volume_scale;
    std::atomic<bool>* done;
};

// The consumer half of the pipeline: waits for a pre-buffer cushion, then
// steadily pops downloaded segments and decodes them, independent of however
// bursty the download side's network timing is. Runs as its own task so a
// slow/blocked HTTP read on the download side never itself blocks decoding
// (or vice versa).
void DecodeTaskEntry(void* raw) {
    std::unique_ptr<DecodeTaskArgs> args(static_cast<DecodeTaskArgs*>(raw));

    AacPipeline pipeline(*args->audio_service, args->out_rate, args->out_channels, args->should_stop,
                         args->volume_scale);
    TsDemuxer ts_demuxer;
    if (!pipeline.Open()) {
        ESP_LOGE(TAG, "Failed to open AAC decoder");
        args->done->store(true);
        vTaskDelete(nullptr);
        return;
    }

    args->queue->WaitForPrebuffer(kPrebufferSegments, args->should_stop);

    SegmentBuffer seg;
    while (!args->should_stop()) {
        if (!args->queue->Pop(seg, args->should_stop)) break;
        DecodeSegment(seg, pipeline, ts_demuxer);
    }

    args->done->store(true);
    vTaskDelete(nullptr);
}

const std::vector<RadioStation>& BuiltInStations() {
    static const std::vector<RadioStation> kStations = {
        {"vov1", "VOV1", "https://str.vov.gov.vn/vovlive/vov1vov5Vietnamese.sdp_aac/playlist.m3u8"},
        {"vov2", "VOV2", "https://str.vov.gov.vn/vovlive/vov2.sdp_aac/playlist.m3u8"},
        {"vov3", "VOV3", "https://str.vov.gov.vn/vovlive/vov3.sdp_aac/playlist.m3u8"},
        {"vov_giao_thong_ha_noi", "VOV Giao Thong Ha Noi",
         "https://str.vov.gov.vn/vovlive/vovGTHN.sdp_aac/chunklist_w601606653.m3u8"},
        {"vov_giao_thong_tphcm", "VOV Giao Thong TPHCM",
         "https://str.vov.gov.vn/vovlive/vovGTHCM.sdp_aac/chunklist_w1213978008.m3u8"},
    };
    return kStations;
}

}  // namespace

const std::vector<RadioStation>& VovRadioService::Stations() { return BuiltInStations(); }

const RadioStation* VovRadioService::FindStation(const std::string& id) {
    std::string needle = id;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const auto& station : BuiltInStations()) {
        if (station.id == needle) return &station;
    }
    return nullptr;
}

VovRadioService::VovRadioService(AudioService& audio_service) : audio_service_(audio_service) {}

VovRadioService::~VovRadioService() {
    Stop(true);  // must block: the worker task holds a raw `this` and can't outlive it
}

void VovRadioService::FinishWorkerIfCurrent() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_handle_ == xTaskGetCurrentTaskHandle()) {
        task_handle_ = nullptr;
        playing_ = false;
    }
}

bool VovRadioService::Play(const std::string& url, const std::string& display_name) {
    if (!IsSupportedUrl(url)) {
        ESP_LOGE(TAG, "Unsupported radio URL: %s", url.c_str());
        return false;
    }

    struct TaskArgs {
        VovRadioService* self;
        std::string url;
        uint32_t generation;
    };

    // Held across the create so IsPlaying()/CurrentStationName() never see a
    // torn state, and so task_handle_ is updated atomically with them.
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t my_generation = generation_.fetch_add(1) + 1;
    display_name_ = display_name;
    playing_ = true;

    auto* args = new TaskArgs{this, url, my_generation};
    BaseType_t created = xTaskCreate(
        [](void* raw) {
            std::unique_ptr<TaskArgs> args(static_cast<TaskArgs*>(raw));
            args->self->WorkerTask(std::move(args->url), args->generation);
            vTaskDelete(nullptr);
        },
        "radio_stream", kRadioTaskStackSize, args, kRadioTaskPriority, &task_handle_);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create radio streaming task");
        delete args;
        playing_ = false;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void VovRadioService::Stop(bool wait) {
    // Bumping the generation invalidates whatever task is currently running
    // (if any): its next should-stop check fails and it unwinds on its own.
    generation_.fetch_add(1);
    if (!wait) {
        return;
    }
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (task_handle_ == nullptr) {
                playing_ = false;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool VovRadioService::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
}

std::string VovRadioService::CurrentStationName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_ ? display_name_ : std::string();
}

void VovRadioService::SetDucked(bool ducked) {
    volume_scale_.store(ducked ? kDuckedVolumeScale : 1.0f);
}

// The producer/download half of the pipeline (see DecodeTaskEntry for the
// consumer half). Owns the HLS playlist polling and one kept-alive HTTP
// connection shared across every playlist refresh and segment fetch in this
// session, downloads each new segment fully into a SegmentBuffer, and hands
// it to the decode task via a SegmentQueue -- never decodes anything itself.
void VovRadioService::WorkerTask(std::string url, uint32_t my_generation) {
    ESP_LOGI(TAG, "Connecting: %s", url.c_str());

    ShouldStopFn should_stop = [this, my_generation]() { return ShouldStop(my_generation); };

    auto* network = Board::GetInstance().GetNetwork();
    auto* codec = Board::GetInstance().GetAudioCodec();

    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP connection");
        FinishWorkerIfCurrent();
        return;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetKeepAlive(true);

    std::string playlist_url = url;
    {
        std::string text, error;
        if (!FetchText(http.get(), playlist_url, kPlaylistMaxBytes, text, error)) {
            ESP_LOGE(TAG, "Failed to fetch playlist: %s", error.c_str());
            FinishWorkerIfCurrent();
            return;
        }
        MediaPlaylist pl = ParsePlaylist(text, playlist_url);
        if (pl.is_master) {
            if (pl.master_variant_url.empty()) {
                ESP_LOGE(TAG, "Master playlist has no variants");
                FinishWorkerIfCurrent();
                return;
            }
            playlist_url = pl.master_variant_url;
        }
    }

    SegmentQueue queue(kMaxQueuedSegments);
    std::atomic<bool> decode_done{false};
    auto* decode_args =
        new DecodeTaskArgs{&queue,          &audio_service_,  codec->output_sample_rate(),
                          codec->output_channels(), should_stop,      &volume_scale_,
                          &decode_done};
    if (xTaskCreate(DecodeTaskEntry, "radio_decode", kDecodeTaskStackSize, decode_args,
                    kRadioTaskPriority, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create radio decode task");
        delete decode_args;
        FinishWorkerIfCurrent();
        return;
    }

    bool have_last_seq = false;
    uint32_t last_seq = 0;

    while (!should_stop() && !decode_done.load()) {
        std::string text, error;
        if (!FetchText(http.get(), playlist_url, kPlaylistMaxBytes, text, error)) {
            ESP_LOGW(TAG, "Playlist refresh failed: %s", error.c_str());
            InterruptibleDelay(kRetryDelayMs, should_stop);
            continue;
        }

        MediaPlaylist pl = ParsePlaylist(text, playlist_url);
        if (pl.segment_urls.empty()) {
            uint32_t wait_ms = std::clamp<uint32_t>(pl.target_duration_s * 1000, kMinRefreshMs, kMaxRefreshMs);
            InterruptibleDelay(wait_ms, should_stop);
            continue;
        }

        std::vector<std::string> to_play;
        std::vector<uint32_t> to_play_seq;
        if (!have_last_seq) {
            // Join near the live edge instead of racing through the whole list.
            size_t idx = pl.segment_urls.size() - 1;
            to_play.push_back(pl.segment_urls[idx]);
            to_play_seq.push_back(pl.media_sequence + static_cast<uint32_t>(idx));
        } else {
            for (size_t i = 0; i < pl.segment_urls.size(); i++) {
                uint32_t seq = pl.media_sequence + static_cast<uint32_t>(i);
                if (seq > last_seq) {
                    to_play.push_back(pl.segment_urls[i]);
                    to_play_seq.push_back(seq);
                }
            }
            if (to_play.size() > kMaxCatchUpSegments) {
                size_t drop = to_play.size() - kMaxCatchUpSegments;
                ESP_LOGW(TAG, "Falling behind live edge, skipping %u stale segment(s)",
                        static_cast<unsigned>(drop));
                to_play.erase(to_play.begin(), to_play.begin() + drop);
                to_play_seq.erase(to_play_seq.begin(), to_play_seq.begin() + drop);
            }
        }

        int64_t cycle_start_ms = esp_timer_get_time() / 1000;
        bool any_downloaded = false;

        for (size_t i = 0; i < to_play.size() && !should_stop(); i++) {
            last_seq = to_play_seq[i];
            have_last_seq = true;

            SegmentBuffer seg;
            seg.is_ts = pl.segments_are_ts;
            if (DownloadSegment(http.get(), to_play[i], seg, should_stop)) {
                any_downloaded = true;
                if (!queue.Push(std::move(seg), should_stop)) break;
            }
        }

        if (!should_stop() && !any_downloaded) {
            // Every segment in this cycle failed outright -- a sustained
            // network problem, not one bad segment. Back off instead of
            // immediately hammering the server (and the logs) again.
            ESP_LOGW(TAG, "No segments downloaded this cycle, backing off before retrying");
            InterruptibleDelay(kRetryDelayMs, should_stop);
            continue;
        }

        int64_t elapsed_ms = esp_timer_get_time() / 1000 - cycle_start_ms;
        uint32_t target_ms = std::clamp<uint32_t>(pl.target_duration_s * 1000, kMinRefreshMs, kMaxRefreshMs);
        if (!should_stop() && elapsed_ms < static_cast<int64_t>(target_ms)) {
            InterruptibleDelay(target_ms - static_cast<uint32_t>(elapsed_ms), should_stop);
        }
    }

    // Wait for the decode task to notice the same should_stop and exit before
    // this task's stack (which the queue lives on) unwinds out from under it.
    while (!decode_done.load()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "Stream stopped");
    FinishWorkerIfCurrent();
}
