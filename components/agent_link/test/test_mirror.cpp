// Headless test for the tab5adb-agent JPEG strip mirror (Phase 2), the Tab5 role
// played by the real adb + agent_link stack over libusb vs a real
// Android device — NO LVGL/SDL. Continues test_hello's bring-up (connect ->
// Sync::push the jar -> open_shell app_process -> Link::open -> HELLO) and then:
//
//   - launches the agent with `--test-pattern` so the source frame is the
//     deterministic TestPattern (grid + up-arrow), not the live screen — the
//     pipeline / framing / receive / decode are what we verify here, not capture;
//   - on_link_hello -> Link::start_mirror() (MIRROR_START round trip);
//   - validates each JPEG strip structurally (16-alignment, in-bounds, decodes,
//     decoded dims == subheader w x h) and the per-frame tiling (SPLIT_COUNT
//     strips, x/w constant, y contiguous, Σh == image height, FRAME_START/END);
//   - composites strips into a 720x1280 memory framebuffer (the app's bsp FB +
//     P4 HW JPEG seam, here host libjpeg + memory) and writes one frame to
//     build/mirror_frame.ppm (relative to the runner's cwd, i.e. the repo root)
//     for eyeballing rotation / fit placement.
//
// Pass = MIRROR_START accepted + >= kMinFrames clean frames + the link closes
// once. Build & run via the runner (TEST selects the source):
//   nix develop -c sh -c 'TEST=test_mirror components/agent_link/test/run.sh'
#include "adb.hpp"
#include "agent_link.hpp"

#include <jpeglib.h>
#include <nvs_flash.h>

#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kRemoteJar = "/data/local/tmp/tab5adb-agent.jar";
constexpr const char* kBaseCmd =
    "CLASSPATH=/data/local/tmp/tab5adb-agent.jar app_process / com.tab5adb.agent.Server";
// Default: the deterministic test pattern (verifies pipeline + framing + decode).
// Set TAB5ADB_REAL=1 to smoke-test the real SurfaceControl screen capture instead
// (structural asserts still apply; the PPM shows the actual device screen).
const std::string kLaunchCmd =
    std::string(kBaseCmd) + (std::getenv("TAB5ADB_REAL") ? "" : " --test-pattern");

constexpr int kPanelW = 720;
constexpr int kPanelH = 1280;
constexpr int kSplitCount = 4;     // agent SPLIT_COUNT (§5.1)
constexpr int kMinFrames = 3;      // clean frames required to pass

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// --- libjpeg decode (host side; device uses the P4 HW JPEG decoder) ---
struct JpegErr {
    jpeg_error_mgr base;
    jmp_buf jmp;
};
void jpeg_on_error(j_common_ptr c) { std::longjmp(reinterpret_cast<JpegErr*>(c->err)->jmp, 1); }

struct Decoded {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;  // w*h*3
};

bool decode_jpeg(const uint8_t* data, size_t len, Decoded& out) {
    jpeg_decompress_struct cinfo;
    JpegErr err;
    cinfo.err = jpeg_std_error(&err.base);
    err.base.error_exit = jpeg_on_error;
    if (setjmp(err.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    out.w = static_cast<int>(cinfo.output_width);
    out.h = static_cast<int>(cinfo.output_height);
    const int stride = out.w * 3;
    out.rgb.resize(static_cast<size_t>(stride) * out.h);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* row = out.rgb.data() + static_cast<size_t>(cinfo.output_scanline) * stride;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

void write_ppm(const char* path, const std::vector<uint8_t>& rgb, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    f << "P6\n" << w << " " << h << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}

// One object plays every delegate role (single device), like the other harnesses.
// All callbacks fire on reader/worker threads; the per-frame validation state is
// touched only on the reader thread (a single thread), so it needs no lock.
class Listener : public adb::ClientListener,
                 public adb::SyncListener,
                 public adb::ShellListener,
                 public agent_link::LinkLifecycleListener,
                 public agent_link::VideoListener {
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

    // --- ShellListener (agent stdout/stderr over shell:) ---
    void on_shell_data(adb::Shell*, const uint8_t* d, size_t n) override {
        std::fwrite("agent| ", 1, 7, stdout);
        std::fwrite(d, 1, n, stdout);
        std::fflush(stdout);
    }
    void on_shell_close(adb::Shell*, adb::Error) override {}

    // --- LinkLifecycleListener ---
    void on_link_hello(agent_link::Link* link, const agent_link::AgentInfo& info) override {
        std::printf(">>> on_link_hello: proto=%u agent=%u.%u.%u caps=0x%04x\n",
                    info.proto_version, info.version_major, info.version_minor,
                    info.version_patch, info.capabilities);
        hello_link_ = link;
        hello_ = true;  // the main flow registers the video listener + start_mirror
    }

    // --- VideoListener ---
    void on_mirror_started(agent_link::Link*, const agent_link::MirrorInfo& info) override {
        std::printf(">>> on_mirror_started: source=%ux%u codec=%u\n",
                    info.source_width, info.source_height, info.video_codec);
        mirror_started_ = true;
    }

    void on_video_strip(agent_link::Link*, const agent_link::VideoStrip& s) override {
        if (!validate_strip(s)) bad_strips_++;
    }

    void on_link_close(agent_link::Link* link, adb::Error err) override {
        std::printf(">>> on_link_close: %s\n", adb::to_string(err));
        if (link == hello_link_) ++hello_link_closes_;
        link_closed_ = true;
    }

    bool hello() const { return hello_; }
    bool mirror_started() const { return mirror_started_; }
    int frames_done() const { return frames_done_; }
    int bad_strips() const { return bad_strips_; }
    bool link_closed() const { return link_closed_; }
    int hello_link_closes() const { return hello_link_closes_; }
    void reset_link() { link_closed_ = false; }

private:
    // Validate one strip + accumulate the frame; returns false on any violation.
    bool validate_strip(const agent_link::VideoStrip& s) {
        bool ok = true;
        auto fail = [&](const char* why) {
            std::printf("    BAD strip (%u,%u %ux%u): %s\n", s.x, s.y, s.w, s.h, why);
            ok = false;
        };
        if ((s.x % 16) || (s.y % 16) || (s.w % 16) || (s.h % 16)) fail("not 16-aligned");
        if (s.x + s.w > kPanelW || s.y + s.h > kPanelH) fail("out of panel bounds");
        if (s.w == 0 || s.h == 0) fail("empty");

        Decoded dec;
        if (!decode_jpeg(s.jpeg, s.jpeg_len, dec)) {
            fail("JPEG decode failed");
        } else if (dec.w != s.w || dec.h != s.h) {
            std::printf("    BAD strip: decoded %dx%d != subheader %ux%u\n",
                        dec.w, dec.h, s.w, s.h);
            ok = false;
        } else {
            blit(dec, s.x, s.y);  // composite into the memory framebuffer
        }

        // --- per-frame tiling (FRAME_START..FRAME_END) ---
        if (s.frame_start) {
            cur_ = Frame{};
            cur_.x0 = s.x;
            cur_.w0 = s.w;
            cur_.y_cursor = s.y;
            cur_.off_y = s.y;
            cur_.active = true;
        }
        if (!cur_.active) {  // strip outside a frame envelope
            fail("strip outside FRAME_START/END");
            return ok;
        }
        if (s.x != cur_.x0 || s.w != cur_.w0) fail("x/w not constant across the frame");
        if (s.y != cur_.y_cursor) fail("y not contiguous (gap/overlap)");
        cur_.y_cursor = s.y + s.h;
        cur_.count++;

        if (s.frame_end) {
            cur_.active = false;
            if (cur_.count != kSplitCount) {
                std::printf("    BAD frame: %d strips (expected %d)\n", cur_.count, kSplitCount);
                ok = false;
            }
            if (ok) {
                if (frames_done_ == 0) {
                    write_ppm("build/mirror_frame.ppm", fb_, kPanelW, kPanelH);
                    std::printf("    wrote build/mirror_frame.ppm (image %dx%d at y=%d..%d)\n",
                                cur_.w0, cur_.y_cursor - cur_.off_y, cur_.off_y, cur_.y_cursor);
                }
                frames_done_++;
            }
        }
        return ok;
    }

    void blit(const Decoded& d, int x, int y) {
        for (int row = 0; row < d.h; row++) {
            const uint8_t* src = d.rgb.data() + static_cast<size_t>(row) * d.w * 3;
            uint8_t* dst = fb_.data() + (static_cast<size_t>(y + row) * kPanelW + x) * 3;
            std::memcpy(dst, src, static_cast<size_t>(d.w) * 3);
        }
    }

    struct Frame {
        bool active = false;
        int x0 = 0, w0 = 0, y_cursor = 0, off_y = 0, count = 0;
    };

    std::atomic<bool> online_{false};
    std::atomic<bool> hello_{false};
    std::atomic<bool> mirror_started_{false};
    std::atomic<bool> link_closed_{false};
    std::atomic<int> frames_done_{0};
    std::atomic<int> bad_strips_{0};
    std::atomic<agent_link::Link*> hello_link_{nullptr};
    std::atomic<int> hello_link_closes_{0};

    Frame cur_;  // reader thread only
    std::vector<uint8_t> fb_ = std::vector<uint8_t>(static_cast<size_t>(kPanelW) * kPanelH * 3, 0);
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

    // READY: register the video channel listener, then start the mirror — the app
    // flow (the feature attaches + starts after on_link_hello, not from inside it).
    if (listener->hello() && link) {
        link->set_video_listener(listener);
        adb::Error e = link->start_mirror();  // default cfg = 720x1280 fit, video
        std::printf(">>> start_mirror: %s\n", adb::to_string(e));
    }

    // Collect a few clean frames.
    for (int i = 0; i < 100 && listener->frames_done() < kMinFrames; ++i) sleep_ms(100);

    // Exercise MIRROR_STOP -> READY -> MIRROR_START on the SAME link: the stream
    // must pause then resume without re-establishing the link (the AgentClient
    // "keep the link, stop the feature" flow).
    bool cycle_ok = true;
    if (link && listener->mirror_started()) {
        link->stop_mirror();
        sleep_ms(700);                              // let the stream wind down
        int after_stop = listener->frames_done();
        adb::Error e = link->start_mirror();        // resume on the same link
        std::printf(">>> stop/restart: start_mirror=%s (frames at stop=%d)\n",
                    adb::to_string(e), after_stop);
        for (int i = 0; i < 100 && listener->frames_done() < after_stop + kMinFrames; ++i)
            sleep_ms(100);
        cycle_ok = listener->frames_done() >= after_stop + kMinFrames;
        std::printf(">>> stop/restart: resumed to %d frames (%s)\n",
                    listener->frames_done(), cycle_ok ? "ok" : "FAILED");
    }

    bool hello = listener->hello();
    bool started = listener->mirror_started();
    int frames = listener->frames_done();
    int bad = listener->bad_strips();
    std::printf("hello=%s mirror_started=%s frames=%d bad_strips=%d cycle=%s\n",
                hello ? "ok" : "FAILED", started ? "ok" : "FAILED", frames, bad,
                cycle_ok ? "ok" : "FAILED");

    // Teardown: close the link, then the shell (kills the agent), then the client.
    if (link) link->close();
    sleep_ms(200);
    shell->close();
    shell.reset();
    link.reset();
    client->close();

    bool one_close = listener->hello_link_closes() == 1;
    bool ok = hello && started && frames >= kMinFrames && bad == 0 && one_close && cycle_ok;
    std::printf("%s (frames=%d bad_strips=%d hello_link_closes=%d)\n",
                ok ? "PASSED: mirror streams clean JPEG strips, link closes once"
                   : "FAILED",
                frames, bad, listener->hello_link_closes());
    return ok ? 0 : 1;
}
