#ifndef VOV_RADIO_SERVICE_H
#define VOV_RADIO_SERVICE_H

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class AudioService;

// One entry in the built-in VOV station list.
struct RadioStation {
    std::string id;    // stable identifier used by the MCP tool, e.g. "vov1"
    std::string name;  // human-readable name, e.g. "VOV1"
    std::string url;   // HLS (.m3u8) playlist URL
};

// Streams a live internet-radio HLS station and plays it through the device
// speaker, alongside the assistant's own audio path.
//
// Each media-playlist segment is expected to be a raw ADTS AAC stream (no
// MPEG-TS wrapper) -- this matches VOV's Wowza "cupertino" audio-only HLS
// packaging (the `.sdp_aac` path segment in their URLs). A station packaged
// as MPEG-TS (.ts) segments is not supported and will fail to decode.
//
// Decoded PCM is resampled to the board's output sample rate/channel count
// and pushed directly into AudioService's playback queue (the same queue the
// Opus decoder feeds), so it shares the existing output task, volume, and
// idle/drain tracking used by the rest of the app, and keeps streaming
// continuously in the background across conversation turns. It does not
// actively pause for the assistant's own speech, so the two can briefly
// interleave in that shared 2-slot queue while the assistant is speaking a
// response (a short blip, not a hang) -- call Stop() explicitly (e.g. the
// self.radio.stop MCP tool) to end a station.
class VovRadioService {
public:
    // Returns the built-in VOV station list.
    static const std::vector<RadioStation>& Stations();
    // Looks up a station by id (case-insensitive). Returns nullptr if unknown.
    static const RadioStation* FindStation(const std::string& id);

    explicit VovRadioService(AudioService& audio_service);
    ~VovRadioService();

    VovRadioService(const VovRadioService&) = delete;
    VovRadioService& operator=(const VovRadioService&) = delete;

    // Starts streaming `url` on a background task and returns immediately,
    // even if a station is already playing -- the new task supersedes it
    // without waiting for it to unwind (see the `generation_` comment below).
    // Safe to call from the main application event loop.
    bool Play(const std::string& url, const std::string& display_name);

    // Stops the current stream, if any. `wait = false` (the default use from
    // the main application event loop) just signals the stop and returns
    // immediately; the worker cleans up on its own. `wait = true` blocks
    // until the background task has fully exited -- only call this from a
    // task that can afford to block, since the worker may be mid-read on a
    // slow HTTP connection and can take up to the HTTP timeout to notice.
    void Stop(bool wait = false);

    bool IsPlaying() const;
    std::string CurrentStationName() const;

    // Scales the radio's own PCM output without touching the shared/global
    // speaker volume, so it can be made quiet without affecting anything
    // else (TTS, notifications) using that volume. This board's AEC alone
    // isn't reliable enough to fully cancel the radio's own audio out of the
    // mic input -- it's been observed picking up VOV's own speech as if it
    // were a user command -- so the caller should duck (true) while actively
    // listening for user speech and restore (false) otherwise.
    void SetDucked(bool ducked);

private:
    AudioService& audio_service_;
    mutable std::mutex mutex_;
    std::string display_name_;
    TaskHandle_t task_handle_ = nullptr;
    bool playing_ = false;
    // Default matches kNormalVolumeScale in vov_radio_service.cc (1.7x boost);
    // kept as a literal here since that constant is file-local to the .cc.
    std::atomic<float> volume_scale_{1.7f};
    // Bumped by every Play()/Stop(); a worker task keeps running only while
    // this still equals the value it was handed at creation. This both
    // signals an explicit Stop() and lets a new Play() supersede whatever
    // task is currently running without having to wait for it to unwind
    // first (two tasks briefly overlapping just means the older one's
    // generation check fails on its very next iteration and it exits quietly
    // without pushing any more audio).
    std::atomic<uint32_t> generation_{0};

    void WorkerTask(std::string url, uint32_t my_generation);
    bool ShouldStop(uint32_t my_generation) const { return generation_.load() != my_generation; }
    // Clears task_handle_/playing_ only if this call is running on the task
    // currently recorded there -- a superseded task must not clobber a newer
    // one's state if it happens to unwind after the newer task has started.
    void FinishWorkerIfCurrent();
};

#endif  // VOV_RADIO_SERVICE_H
