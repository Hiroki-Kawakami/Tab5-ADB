// Pure wire format for the tab5adb-agent protocol — the contract in
// android-agent/docs/protocol.md, with NO I/O (the agent_link counterpart of
// embedded_adb's adb_protocol.hpp). Both the Tab5 side (this component) and the
// agent (Java) follow protocol.md; keep this header in sync with §3/§4 there.
//
// All multi-byte values are little-endian (ESP32-P4 and Android-ARM are both LE).
#pragma once

#include <cstddef>
#include <cstdint>

namespace agent_link {

// proto_version: exact match required (protocol.md §4.4).
constexpr uint8_t kProtoVersion = 1;

// Frame header (protocol.md §3): MAGIC + 7 bytes, payload follows.
constexpr uint8_t kMagic = 0xA5;
constexpr size_t kFrameHeaderSize = 8;

// TYPE (§3.1).
enum Type : uint8_t {
    kTypeControlRequest = 0x01,
    kTypeControlResponse = 0x02,
    kTypeEvent = 0x03,
    kTypeInput = 0x04,  // T->A: input injection (key/touch/text), fire-and-forget
    kTypeJpeg = 0x10,
    kTypeAudio = 0x11,
};

// FLAGS bits (§3.2).
enum Flag : uint8_t {
    kFlagFrameStart = 0x01,
    kFlagFrameEnd = 0x02,
};

// Control commands (§4.4).
enum Cmd : uint8_t {
    kCmdHello = 0x01,         // A->T: agent_link link establishment (link-only)
    kCmdMirrorStart = 0x10,   // T->A: start mirror (video + future audio)
    kCmdMirrorStop = 0x11,    // T->A: stop mirror -> READY (link kept), §4.4
    kCmdMirrorSetParam = 0x12,  // T->A: live param change (reserved)
    kCmdGetAppList = 0x20,    // T->A: installed apps (pkg/label/flags), §4.4
    kCmdGetAppIcon = 0x21,    // T->A: one app icon as raw ARGB8888, §4.4
};

// GET_APP_LIST entry flags (§4.4).
enum AppFlag : uint8_t {
    kAppFlagSystem = 0x01,
    kAppFlagDisabled = 0x02,
};

// GET_APP_ICON result: fixed fields before the pixels (§4.4).
constexpr size_t kAppIconHeaderLen = 8;
constexpr uint8_t kAppIconFormatArgb8888 = 0x01;

// EVENT types (§4.4 event registry). Agent->Tab5 async notifications (TYPE=EVENT).
enum Event : uint8_t {
    kEventError = 0x01,
    kEventStreamStopped = 0x02,
    kEventOrientation = 0x03,  // source device logical rotation changed (§4.4)
};

// ORIENTATION event data (after the event byte, §4.4): rotation + reserved.
//  +0 u8  rotation   Surface.ROTATION_* (0/1/2/3 = 0/90/180/270)
//  +1 u8  reserved
//  +2 u16 reserved
constexpr size_t kOrientationDataLen = 4;

// A rotation code is "landscape" when it is 90 or 270 (odd). The Tab5 keeps
// showing the device's natural-orientation framebuffer either way (§5.1); this
// only drives the overlay layout (portrait strip vs landscape strip).
inline bool rotation_is_landscape(uint8_t rotation) { return (rotation & 1) != 0; }

// Capability bits (§4.6) — also the MIRROR_START `streams` bit assignment.
enum Cap : uint16_t {
    kCapVideo = 0x0001,
    kCapAudio = 0x0002,
    kCapAppInfo = 0x0004,  // GET_APP_LIST / GET_APP_ICON (§4.4)
};

// scale_mode (MIRROR_START args, §5.3).
enum ScaleMode : uint8_t {
    kScaleFit = 0,     // aspect-preserve inscribe, letterbox (default)
    kScaleFill = 1,    // aspect-preserve cover, crop (always target-sized)
    kScaleAspect = 2,  // agent sizes the output to the source aspect within the
                       // target box (16-aligned, no letterbox/crop); the chosen
                       // size comes back as out_width/out_height
};

// Status codes (§4.5).
enum Status : uint8_t {
    kStatusOk = 0x00,
    kStatusEinval = 0x01,
    kStatusEnotsup = 0x02,  // proto mismatch
    kStatusEbusy = 0x03,
    kStatusEstate = 0x04,
    kStatusErange = 0x05,
    kStatusEfail = 0xFF,
};

// video_codec (MIRROR_START response, §4.4).
constexpr uint8_t kVideoCodecJpeg = 0x01;

// audio_codec (MIRROR_START response audio tail, §6). One AUDIO frame carries one
// codec unit; the codec is fixed here so the frame body needs no per-frame header.
enum AudioCodec : uint8_t {
    kAudioCodecPcmS16le = 0x01,  // interleaved 16-bit LE PCM (v1)
    kAudioCodecOpus = 0x02,      // reserved (frame body = one Opus packet)
};

// --- INPUT channel (§4.7) — TYPE=INPUT, Tab5->agent, fire-and-forget ---
//
// The agent injects these via the hidden InputManager.injectInputEvent (the
// scrcpy technique); this is the shared input foundation that key (overlay
// power/volume/nav), touch passthrough and keyboard all ride. The payload's
// first byte is the input_type; the rest is type-specific.
enum InputType : uint8_t {
    kInputKey = 0x00,   // a single key event (down or up)
    kInputTouch = 0x01,  // touch passthrough (reserved)
    kInputText = 0x02,   // text/keyboard (reserved)
};

// INPUT_KEY args after the input_type byte (§4.7):
//  +1 u8  action   kKeyActionDown / kKeyActionUp
//  +2 u32 keycode  Android KeyEvent.KEYCODE_* (LE)
//  +6 u32 repeat   key-repeat count (LE; 0 for a discrete press)
//  +10 u32 meta    KeyEvent meta state (LE; 0 for no modifiers)
constexpr size_t kInputKeyArgsLen = 13;  // after the input_type byte

enum KeyAction : uint8_t {
    kKeyActionDown = 0,  // KeyEvent.ACTION_DOWN
    kKeyActionUp = 1,    // KeyEvent.ACTION_UP
};

// INPUT_TOUCH args after the input_type byte (§4.7):
//  +1 u8  action      kTouchDown / kTouchMove / kTouchUp (per-pointer)
//  +2 u8  pointer_id  source touch-controller track id (forwarded verbatim)
//  +3 u8  reserved    0
//  +4 u16 x           Tab5 panel x [px] (LE; 0..target_width-1)
//  +6 u16 y           Tab5 panel y [px] (LE; 0..target_height-1)
constexpr size_t kInputTouchArgsLen = 7;  // after the input_type byte

// Per-pointer touch action (§4.7). The agent maps these to the composite
// MotionEvent.ACTION_* (DOWN / POINTER_DOWN / MOVE / POINTER_UP / UP) using its
// own active-pointer set, so the wire only carries the per-pointer transition.
// Coordinates are Tab5 panel coords; the agent inverts the mirror geometry to the
// source device's logical display coords before injecting.
enum TouchAction : uint8_t {
    kTouchDown = 0,
    kTouchMove = 1,
    kTouchUp = 2,
};

// Android KeyEvent.KEYCODE_* values for the keys the overlay drives. The agent
// passes these straight to KeyEvent, so the Tab5 side owns the mapping.
enum Keycode : uint16_t {
    kKeyHome = 3,         // KEYCODE_HOME
    kKeyBack = 4,         // KEYCODE_BACK
    kKeyVolumeUp = 24,    // KEYCODE_VOLUME_UP
    kKeyVolumeDown = 25,  // KEYCODE_VOLUME_DOWN
    kKeyPower = 26,       // KEYCODE_POWER
    kKeyAppSwitch = 187,  // KEYCODE_APP_SWITCH (Recents)
};

// HELLO request args / response result lengths (§4.4) — link-only now.
constexpr size_t kHelloArgsLen = 8;    // after cmd+req_id
constexpr size_t kHelloResultLen = 8;  // after cmd+req_id+status

// MIRROR_START request args / response result lengths (§4.4). The args carry
// max_fps at +8 and the result carries out_width/out_height at +8; both tails
// are append-only, so a peer may omit them (max_fps=0 / out=target assumed) —
// the *Base lengths are the minimum a parser requires.
constexpr size_t kMirrorStartArgsLen = 12;       // after cmd+req_id (what we send)
constexpr size_t kMirrorStartResultBaseLen = 8;  // after cmd+req_id+status (minimum)
constexpr size_t kMirrorStartResultLen = 12;     // with out_width/out_height (video only)

// MIRROR_START response with the §6 audio tail appended (audio_codec, audio_channels,
// reserved u16, audio_rate u32 at +12..+19, within the result after cmd+req_id+status).
// Present only when AUDIO was started; a video-only response is kMirrorStartResultLen.
constexpr size_t kMirrorStartResultAudioLen = 20;
constexpr size_t kMirrorAudioCodecOff = 12;     // u8
constexpr size_t kMirrorAudioChannelsOff = 13;  // u8
constexpr size_t kMirrorAudioRateOff = 16;      // u32 (LE)

// JPEG block subheader (§5.2): x, y, w, h (all u16), then the JPEG bytes.
constexpr size_t kJpegSubheaderSize = 8;

// --- little-endian field access ---
inline uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline void wr_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// Decoded frame header (§3).
struct FrameHeader {
    uint8_t magic;
    uint8_t type;
    uint8_t flags;
    uint8_t seq;
    uint32_t length;
};

// Parse the 8-byte header from `p` (caller guarantees >= kFrameHeaderSize bytes).
// Returns false if MAGIC is wrong (a sync-loss / corruption sentinel, §8).
inline bool parse_header(const uint8_t* p, FrameHeader& h) {
    h.magic = p[0];
    h.type = p[1];
    h.flags = p[2];
    h.seq = p[3];
    h.length = rd_u32(p + 4);
    return h.magic == kMagic;
}

// Write an 8-byte header into `p` (caller guarantees >= kFrameHeaderSize bytes).
inline void write_header(uint8_t* p, uint8_t type, uint8_t flags, uint8_t seq,
                         uint32_t length) {
    p[0] = kMagic;
    p[1] = type;
    p[2] = flags;
    p[3] = seq;
    wr_u32(p + 4, length);
}

// JPEG block subheader (§5.2): the rectangle (Tab5 device coords) the JPEG bytes
// decode into. All fields are 16px multiples; the JPEG bytes follow at +8.
struct JpegSubheader {
    uint16_t x, y, w, h;
};

// Parse the JPEG subheader from a JPEG frame's payload (caller guarantees
// len >= kJpegSubheaderSize).
inline void parse_jpeg_subheader(const uint8_t* p, JpegSubheader& s) {
    s.x = rd_u16(p + 0);
    s.y = rd_u16(p + 2);
    s.w = rd_u16(p + 4);
    s.h = rd_u16(p + 6);
}

// Write a JPEG subheader into `p` (caller guarantees >= kJpegSubheaderSize bytes).
inline void write_jpeg_subheader(uint8_t* p, uint16_t x, uint16_t y, uint16_t w,
                                 uint16_t h) {
    wr_u16(p + 0, x);
    wr_u16(p + 2, y);
    wr_u16(p + 4, w);
    wr_u16(p + 6, h);
}

}  // namespace agent_link
