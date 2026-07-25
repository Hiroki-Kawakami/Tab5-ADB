// Headless test for the tab5adb-agent AUDIO stream (§6), the Tab5 role played by
// the real adb + agent_link stack over libusb vs a real Android device
// — NO LVGL/SDL. Same bring-up as test_mirror (connect -> Sync::push the jar ->
// open_shell app_process -> Link::open -> HELLO), then:
//
//   - launches the agent with `--test-pattern --test-tone` so BOTH the video frame
//     and the audio are deterministic (a 440 Hz sine), not the live screen / capture;
//   - on_link_hello -> set_audio_listener + set_video_listener -> start_mirror()
//     with streams = VIDEO|AUDIO (MIRROR_START round trip);
//   - validates on_audio_started's format (PCM_S16LE / stereo / 48 kHz from the §6.2
//     response tail) and that AUDIO frames flow concurrently with video, each a
//     4-byte-aligned (stereo s16) PCM chunk whose RMS is well above silence (the
//     test tone), so the demux + framing + format plumbing is exercised end to end.
//
// Pass = HELLO + on_audio_started (correct format) + >= kMinAudioFrames clean,
// non-silent audio frames + video also flowing + the link closes once. With
// TAB5ADB_REAL=1 it uses the live screen + real REMOTE_SUBMIX capture instead (the
// RMS/silence assertion is relaxed, since the device may be silent). Run:
//   nix develop -c sh -c 'TEST=test_audio components/agent_link/test/run.sh'
#include "adb.hpp"
#include "agent_link.hpp"

#include <nvs_flash.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr const char* kRemoteJar = "/data/local/tmp/tab5adb-agent.jar";
constexpr const char* kBaseCmd =
    "CLASSPATH=/data/local/tmp/tab5adb-agent.jar app_process / com.tab5adb.agent.Server";
const bool kReal = std::getenv("TAB5ADB_REAL") != nullptr;
// Default: deterministic test pattern (video) + test tone (audio). TAB5ADB_REAL=1
// smoke-tests the live screen + real REMOTE_SUBMIX capture (RMS check relaxed).
const std::string kLaunchCmd =
    std::string(kBaseCmd) + (kReal ? "" : " --test-pattern --test-tone");

constexpr int kMinAudioFrames = 50;   // ~0.5 s at 100 chunks/s
constexpr int kMinVideoFrames = 2;    // confirm video flows alongside audio
constexpr double kSilenceRms = 200.0; // test tone (amp 8000) is far above this

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

class Listener : public adb::ClientListener,
                 public adb::SyncListener,
                 public adb::ShellListener,
                 public agent_link::LinkLifecycleListener,
                 public agent_link::VideoListener,
                 public agent_link::AudioListener {
public:
    // --- ClientListener ---
    void on_state(adb::Client*, adb::ConnectionState s) override {
        std::printf(">>> state: %s\n", adb::to_string(s));
        if (s == adb::ConnectionState::Online) online_ = true;
        if (s == adb::ConnectionState::Unauthorized)
            std::printf("    -> authorize the device first (run test_client)\n");
    }
    bool online() const { return online_; }

    // --- SyncListener ---
    void on_sync_close(adb::Sync*, adb::Error) override {}

    // --- ShellListener (agent stdout/stderr) ---
    void on_shell_data(adb::Shell*, const uint8_t* d, size_t n) override {
        std::fwrite("agent| ", 1, 7, stdout);
        std::fwrite(d, 1, n, stdout);
        std::fflush(stdout);
    }
    void on_shell_close(adb::Shell*, adb::Error) override {}

    // --- LinkLifecycleListener ---
    void on_link_hello(agent_link::Link* link, const agent_link::AgentInfo& info) override {
        std::printf(">>> on_link_hello: proto=%u agent=%u.%u.%u caps=0x%04x (AUDIO=%s)\n",
                    info.proto_version, info.version_major, info.version_minor,
                    info.version_patch, info.capabilities,
                    (info.capabilities & agent_link::kCapAudio) ? "Y" : "N");
        agent_caps_ = info.capabilities;
        hello_link_ = link;
        hello_ = true;
    }
    void on_link_close(agent_link::Link* link, adb::Error err) override {
        std::printf(">>> on_link_close: %s\n", adb::to_string(err));
        if (link == hello_link_) ++hello_link_closes_;
        link_closed_ = true;
    }

    // --- VideoListener (just confirm video flows concurrently) ---
    void on_mirror_started(agent_link::Link*, const agent_link::MirrorInfo& info) override {
        std::printf(">>> on_mirror_started: source=%ux%u codec=%u out=%ux%u\n",
                    info.source_width, info.source_height, info.video_codec,
                    info.out_width, info.out_height);
        mirror_started_ = true;
    }
    void on_video_strip(agent_link::Link*, const agent_link::VideoStrip& s) override {
        if (s.frame_end) video_frames_++;
    }

    // --- AudioListener ---
    void on_audio_started(agent_link::Link*, const agent_link::AudioInfo& info) override {
        std::printf(">>> on_audio_started: codec=%u channels=%u rate=%u\n",
                    info.codec, info.channels, info.sample_rate);
        audio_codec_ = info.codec;
        audio_channels_ = info.channels;
        audio_rate_ = info.sample_rate;
        audio_started_ = true;
    }
    void on_audio_data(agent_link::Link*, const uint8_t* pcm, size_t len) override {
        if (len == 0 || (len % 4) != 0) {  // stereo s16 = 4 bytes/frame (§6.3)
            bad_audio_++;
            return;
        }
        if (audio_frames_ == 0) std::printf(">>> first AUDIO frame: %zu bytes\n", len);
        // Accumulate sum-of-squares for the RMS / silence check (integer, atomic).
        const int16_t* s = reinterpret_cast<const int16_t*>(pcm);
        uint64_t sq = 0;
        const size_t n = len / 2;
        for (size_t i = 0; i < n; ++i) sq += static_cast<uint64_t>(s[i] * s[i]);
        sumsq_ += sq;
        nsamp_ += n;
        audio_frames_++;
    }

    bool hello() const { return hello_; }
    bool link_closed() const { return link_closed_; }
    void reset_link() { link_closed_ = false; }
    int hello_link_closes() const { return hello_link_closes_; }
    uint16_t agent_caps() const { return agent_caps_; }
    bool mirror_started() const { return mirror_started_; }
    int video_frames() const { return video_frames_; }
    bool audio_started() const { return audio_started_; }
    int audio_frames() const { return audio_frames_; }
    int bad_audio() const { return bad_audio_; }
    uint8_t audio_codec() const { return audio_codec_; }
    uint8_t audio_channels() const { return audio_channels_; }
    uint32_t audio_rate() const { return audio_rate_; }
    double audio_rms() const {
        uint64_t n = nsamp_.load();
        return n ? std::sqrt(static_cast<double>(sumsq_.load()) / n) : 0.0;
    }

private:
    std::atomic<bool> online_{false};
    std::atomic<bool> hello_{false};
    std::atomic<bool> link_closed_{false};
    std::atomic<int> hello_link_closes_{0};
    std::atomic<agent_link::Link*> hello_link_{nullptr};
    std::atomic<uint16_t> agent_caps_{0};
    std::atomic<bool> mirror_started_{false};
    std::atomic<int> video_frames_{0};
    std::atomic<bool> audio_started_{false};
    std::atomic<int> audio_frames_{0};
    std::atomic<int> bad_audio_{0};
    std::atomic<uint8_t> audio_codec_{0};
    std::atomic<uint8_t> audio_channels_{0};
    std::atomic<uint32_t> audio_rate_{0};
    std::atomic<uint64_t> sumsq_{0};
    std::atomic<uint64_t> nsamp_{0};
};

struct BufSource {
    const std::string* data;
    size_t off = 0;
    int operator()(uint8_t* buf, size_t cap) {
        size_t n = std::min(cap, data->size() - off);
        std::memcpy(buf, data->data() + off, n);
        off += n;
        return static_cast<int>(n);
    }
};

bool push_jar(const std::shared_ptr<adb::Client>& client,
              const std::shared_ptr<Listener>& listener, const std::string& jar) {
    std::ifstream f(jar, std::ios::binary);
    if (!f) {
        std::printf("FAIL: cannot open jar '%s'\n", jar.c_str());
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::printf("push %s (%zu bytes) -> %s\n", jar.c_str(), bytes.size(), kRemoteJar);

    auto sync = client->open_sync(listener);
    if (!sync) {
        std::printf("FAIL: open_sync returned nullptr\n");
        return false;
    }
    auto src = std::make_shared<BufSource>();
    src->data = &bytes;
    std::atomic<bool> done{false};
    std::atomic<adb::Error> err{adb::Error::Ok};
    sync->push(kRemoteJar, 0644, /*mtime=*/0,
               [src](uint8_t* b, size_t c) { return (*src)(b, c); },
               [&](adb::Error e) { err = e; done = true; });
    for (int i = 0; i < 100 && !done; ++i) sleep_ms(100);
    sync->close();
    bool ok = done && err.load() == adb::Error::Ok;
    if (!ok) std::printf("FAIL: push (%s)\n", adb::to_string(err.load()));
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    nvs_flash_sim_set_path("/tmp/adb_test_nvs.json");
    const std::string jar =
        argc > 1 ? argv[1] : "../../android-agent/build/tab5adb-agent.jar";

    auto listener = std::make_shared<Listener>();
    auto client = adb::Client::connect_usb(listener);

    for (int i = 0; i < 200 && !listener->online(); ++i) {
        if (client->state() == adb::ConnectionState::Closed) break;
        sleep_ms(100);
    }
    if (!listener->online()) {
        std::printf("FAIL: did not reach Online\n");
        return 1;
    }

    std::atomic<bool> kill_done{false};
    client->exec("pkill -f com.tab5adb.agent.Server; true",
                 [&](adb::Error, const std::string&) { kill_done = true; });
    for (int i = 0; i < 30 && !kill_done; ++i) sleep_ms(100);

    if (!push_jar(client, listener, jar)) {
        client->close();
        return 1;
    }

    std::printf("launch: %s\n", kLaunchCmd.c_str());
    auto shell = client->open_shell(listener, kLaunchCmd);
    if (!shell) {
        std::printf("FAIL: open_shell returned nullptr\n");
        client->close();
        return 1;
    }

    // Retry localabstract until the agent is listening (protocol.md §2.2-3).
    std::shared_ptr<agent_link::Link> link;
    for (int attempt = 0; attempt < 30 && !listener->hello(); ++attempt) {
        listener->reset_link();
        link = agent_link::Link::open(client, listener);
        if (!link) {
            sleep_ms(200);
            continue;
        }
        for (int i = 0; i < 10 && !listener->hello() && !listener->link_closed(); ++i)
            sleep_ms(100);
        if (listener->hello()) break;
        link.reset();
        sleep_ms(300);
    }

    // READY: register both channel listeners, then start the mirror with audio.
    if (listener->hello() && link) {
        link->set_video_listener(listener);
        link->set_audio_listener(listener);
        agent_link::MirrorConfig cfg;
        cfg.streams = agent_link::kCapVideo | agent_link::kCapAudio;  // §6.1 Tab5Only
        adb::Error e = link->start_mirror(cfg);
        std::printf(">>> start_mirror(VIDEO|AUDIO): %s\n", adb::to_string(e));
    }

    // Collect audio (and confirm video is flowing too).
    for (int i = 0; i < 100 && listener->audio_frames() < kMinAudioFrames; ++i)
        sleep_ms(100);

    bool hello = listener->hello();
    bool audio_ok = listener->audio_started() &&
                    listener->audio_codec() == agent_link::kAudioCodecPcmS16le &&
                    listener->audio_channels() == 2 && listener->audio_rate() == 48000;
    int aframes = listener->audio_frames();
    int vframes = listener->video_frames();
    double rms = listener->audio_rms();
    bool not_silent = kReal || rms > kSilenceRms;
    std::printf("hello=%s audio_started=%s fmt(codec=%u ch=%u rate=%u) "
                "audio_frames=%d bad_audio=%d rms=%.1f video_frames=%d\n",
                hello ? "ok" : "FAILED", listener->audio_started() ? "ok" : "FAILED",
                listener->audio_codec(), listener->audio_channels(),
                listener->audio_rate(), aframes, listener->bad_audio(), rms, vframes);

    // Teardown: close the link, then the shell (kills the agent), then the client.
    if (link) link->close();
    sleep_ms(200);
    shell->close();
    shell.reset();
    link.reset();
    client->close();

    bool one_close = listener->hello_link_closes() == 1;
    bool ok = hello && audio_ok && aframes >= kMinAudioFrames &&
              listener->bad_audio() == 0 && not_silent &&
              vframes >= kMinVideoFrames && one_close;
    std::printf("%s (audio_frames=%d video_frames=%d rms=%.1f hello_link_closes=%d)\n",
                ok ? "PASSED: AUDIO streams clean PCM alongside video, link closes once"
                   : "FAILED",
                aframes, vframes, rms, listener->hello_link_closes());
    return ok ? 0 : 1;
}
