/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use core::ffi::CStr;

use agent_link::AudioVolume;
use agent_link::protocol::VOLUME_SCALE;
use bsp_rs::audio::Audio;
use bsp_rs::display::{Display, ImageFormat};
use idf::log::error;
use idf::nvs::{Key, Nvs, Raw, Value};
use idf::sync::Mutex;

const TAG: &CStr = c"settings";
const NAMESPACE: &CStr = c"tab5adb";

pub const MIN_BRIGHTNESS: u8 = 1;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum AudioPlayback {
    Tab5,
    Android,
}

impl Value for AudioPlayback {
    type Raw = u8;
    type Arg<'a> = Self;

    fn from_raw(raw: u8) -> Option<Self> {
        match raw {
            0 => Some(Self::Tab5),
            1 => Some(Self::Android),
            _ => None,
        }
    }

    fn to_raw<'a>(value: Self) -> <u8 as Raw>::Arg<'a> {
        value as u8
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum AgentMode {
    Normal,
    Limited,
}

impl Value for AgentMode {
    type Raw = u8;
    type Arg<'a> = Self;

    fn from_raw(raw: u8) -> Option<Self> {
        match raw {
            0 => Some(Self::Normal),
            1 => Some(Self::Limited),
            _ => None,
        }
    }

    fn to_raw<'a>(value: Self) -> <u8 as Raw>::Arg<'a> {
        value as u8
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ColorDepth {
    Bits16,
    Bits24,
}

impl ColorDepth {
    pub fn pixel_format(self) -> ImageFormat {
        match self {
            Self::Bits16 => ImageFormat::Rgb565,
            Self::Bits24 => ImageFormat::Bgr888,
        }
    }
}

impl Value for ColorDepth {
    type Raw = u8;
    type Arg<'a> = Self;

    fn from_raw(raw: u8) -> Option<Self> {
        match raw {
            0 => Some(Self::Bits16),
            1 => Some(Self::Bits24),
            _ => None,
        }
    }

    fn to_raw<'a>(value: Self) -> <u8 as Raw>::Arg<'a> {
        value as u8
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum MirrorOrientation {
    Device,
    Fit,
    Rotate,
}

impl Value for MirrorOrientation {
    type Raw = u8;
    type Arg<'a> = Self;

    fn from_raw(raw: u8) -> Option<Self> {
        match raw {
            0 => Some(Self::Device),
            1 => Some(Self::Fit),
            2 => Some(Self::Rotate),
            _ => None,
        }
    }

    fn to_raw<'a>(value: Self) -> <u8 as Raw>::Arg<'a> {
        value as u8
    }
}

struct Setting<T> {
    key: Key<T>,
    value: T,
    sanitize: fn(T) -> T,
    modified: bool,
}

impl<T> Setting<T>
where
    T: for<'a> Value<Arg<'a> = T> + Copy + PartialEq,
{
    const fn new(name: &'static CStr, value: T, sanitize: fn(T) -> T) -> Self {
        Self { key: Key::new(name), value, sanitize, modified: false }
    }

    fn set(&mut self, value: T) -> bool {
        let value = (self.sanitize)(value);
        if value == self.value {
            return false;
        }
        self.value = value;
        self.modified = true;
        true
    }

    fn load(&mut self, nvs: &Nvs) {
        match nvs.get(self.key) {
            Ok(Some(value)) => self.value = (self.sanitize)(value),
            Ok(None) => {}
            Err(err) => error!(TAG, "get {:?}: {err}", self.key.name()),
        }
    }

    fn commit(&mut self, nvs: &mut Nvs) -> bool {
        if !self.modified {
            return false;
        }
        if let Err(err) = nvs.set(self.key, self.value) {
            error!(TAG, "set {:?}: {err}", self.key.name());
            return false;
        }
        self.modified = false;
        true
    }
}

const fn keep<T>(value: T) -> T {
    value
}

struct Store {
    nvs: Option<Nvs>,
    display: Option<&'static Display>,
    audio: Option<&'static Audio>,
    brightness: Setting<u8>,
    equalizer: Setting<bool>,
    color_depth: Setting<ColorDepth>,
    mirror_playback: Setting<AudioPlayback>,
    now_playing_playback: Setting<AudioPlayback>,
    phone_volume: Setting<u16>,
    phone_muted: Setting<bool>,
    agent_mode: Setting<AgentMode>,
    mirror_orientation: Setting<MirrorOrientation>,
    reveal_learned: Setting<bool>,
    utc_offset: Setting<i16>,
}

static STORE: Mutex<Store> = Mutex::new(Store {
    nvs: None,
    display: None,
    audio: None,
    brightness: Setting::new(c"brightness", 80, |percent| percent.clamp(MIN_BRIGHTNESS, 100)),
    equalizer: Setting::new(c"equalizer", true, keep),
    color_depth: Setting::new(c"color_depth", ColorDepth::Bits16, keep),
    mirror_playback: Setting::new(c"playback", AudioPlayback::Tab5, keep),
    now_playing_playback: Setting::new(c"np_playback", AudioPlayback::Tab5, keep),
    phone_volume: Setting::new(c"phone_volume", VOLUME_SCALE / 2, |level| level.min(VOLUME_SCALE)),
    phone_muted: Setting::new(c"phone_muted", false, keep),
    agent_mode: Setting::new(c"agent_mode", AgentMode::Normal, keep),
    mirror_orientation: Setting::new(c"mirror_orient", MirrorOrientation::Device, keep),
    reveal_learned: Setting::new(c"reveal_learned", false, keep),
    utc_offset: Setting::new(c"utc_offset", 0, |minutes| minutes.clamp(-14 * 60, 14 * 60)),
});

pub fn init() {
    let mut store = STORE.lock();
    match Nvs::open(NAMESPACE) {
        Ok(nvs) => {
            store.brightness.load(&nvs);
            store.equalizer.load(&nvs);
            store.color_depth.load(&nvs);
            store.mirror_playback.load(&nvs);
            store.now_playing_playback.load(&nvs);
            store.phone_volume.load(&nvs);
            store.phone_muted.load(&nvs);
            store.agent_mode.load(&nvs);
            store.mirror_orientation.load(&nvs);
            store.reveal_learned.load(&nvs);
            store.utc_offset.load(&nvs);
            store.nvs = Some(nvs);
        }
        Err(err) => error!(TAG, "open: {err}"),
    }
}

pub fn set_display(display: &'static Display) {
    let mut store = STORE.lock();
    store.display = Some(display);
    apply_brightness(&store);
}

pub fn set_audio(audio: &'static Audio) {
    let mut store = STORE.lock();
    store.audio = Some(audio);
    apply_equalizer(&store);
}

pub fn commit() {
    let mut guard = STORE.lock();
    let store = &mut *guard;
    let Some(nvs) = &mut store.nvs else {
        return;
    };
    let mut written = store.brightness.commit(nvs);
    written |= store.equalizer.commit(nvs);
    written |= store.color_depth.commit(nvs);
    written |= store.mirror_playback.commit(nvs);
    written |= store.now_playing_playback.commit(nvs);
    written |= store.phone_volume.commit(nvs);
    written |= store.phone_muted.commit(nvs);
    written |= store.agent_mode.commit(nvs);
    written |= store.mirror_orientation.commit(nvs);
    written |= store.reveal_learned.commit(nvs);
    written |= store.utc_offset.commit(nvs);
    if written && let Err(err) = nvs.commit() {
        error!(TAG, "commit: {err}");
    }
}

pub fn brightness() -> u8 {
    STORE.lock().brightness.value
}

pub fn set_brightness(percent: u8) {
    let mut store = STORE.lock();
    if store.brightness.set(percent) {
        apply_brightness(&store);
    }
}

pub fn equalizer() -> bool {
    STORE.lock().equalizer.value
}

pub fn set_equalizer(on: bool) {
    let mut store = STORE.lock();
    if store.equalizer.set(on) {
        apply_equalizer(&store);
    }
}

pub fn color_depth() -> ColorDepth {
    STORE.lock().color_depth.value
}

pub fn set_color_depth(depth: ColorDepth) {
    STORE.lock().color_depth.set(depth);
}

pub fn mirror_playback() -> AudioPlayback {
    STORE.lock().mirror_playback.value
}

pub fn set_mirror_playback(playback: AudioPlayback) {
    STORE.lock().mirror_playback.set(playback);
}

pub fn now_playing_playback() -> AudioPlayback {
    STORE.lock().now_playing_playback.value
}

pub fn set_now_playing_playback(playback: AudioPlayback) {
    STORE.lock().now_playing_playback.set(playback);
}

/// The Tab5's own volume, which the device plays at while its sound comes to the Tab5.
pub fn phone_volume() -> AudioVolume {
    let store = STORE.lock();
    AudioVolume { level: store.phone_volume.value, muted: store.phone_muted.value }
}

pub fn set_phone_volume(volume: AudioVolume) {
    let mut store = STORE.lock();
    store.phone_volume.set(volume.level);
    store.phone_muted.set(volume.muted);
}

pub fn agent_mode() -> AgentMode {
    STORE.lock().agent_mode.value
}

pub fn set_agent_mode(mode: AgentMode) {
    STORE.lock().agent_mode.set(mode);
}

fn apply_brightness(store: &Store) {
    if let Some(display) = store.display
        && let Err(err) = display.set_brightness(store.brightness.value)
    {
        error!(TAG, "set_brightness: {err}");
    }
}

fn apply_equalizer(store: &Store) {
    if let Some(audio) = store.audio
        && let Err(err) = audio.set_eq_enabled(store.equalizer.value)
    {
        error!(TAG, "set_eq_enabled: {err}");
    }
}

pub fn mirror_orientation() -> MirrorOrientation {
    STORE.lock().mirror_orientation.value
}

pub fn set_mirror_orientation(orientation: MirrorOrientation) {
    STORE.lock().mirror_orientation.set(orientation);
}

/// The user has shown the mirror controls by swiping from their corner.
pub fn reveal_learned() -> bool {
    STORE.lock().reveal_learned.value
}

pub fn set_reveal_learned() {
    STORE.lock().reveal_learned.set(true);
}

/// Minutes east of UTC, as the last device connected had it.
pub fn utc_offset() -> i16 {
    STORE.lock().utc_offset.value
}

pub fn set_utc_offset(minutes: i16) {
    STORE.lock().utc_offset.set(minutes);
}
