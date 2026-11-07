/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use core::ffi::CStr;

use bsp_rs::display::{Display, ImageFormat};
use idf::log::error;
use idf::nvs::{Key, Nvs, Raw, Value};
use idf::sync::Mutex;

const TAG: &CStr = c"settings";
const NAMESPACE: &CStr = c"tab5adb";

pub const MIN_BRIGHTNESS: u8 = 1;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum SoundPlayback {
    Tab5,
    Android,
}

impl Value for SoundPlayback {
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
    brightness: Setting<u8>,
    color_depth: Setting<ColorDepth>,
    sound_playback: Setting<SoundPlayback>,
    agent_mode: Setting<AgentMode>,
}

static STORE: Mutex<Store> = Mutex::new(Store {
    nvs: None,
    display: None,
    brightness: Setting::new(c"brightness", 80, |percent| percent.clamp(MIN_BRIGHTNESS, 100)),
    color_depth: Setting::new(c"color_depth", ColorDepth::Bits24, keep),
    sound_playback: Setting::new(c"playback", SoundPlayback::Tab5, keep),
    agent_mode: Setting::new(c"agent_mode", AgentMode::Normal, keep),
});

pub fn init() {
    let mut store = STORE.lock();
    match Nvs::open(NAMESPACE) {
        Ok(nvs) => {
            store.brightness.load(&nvs);
            store.color_depth.load(&nvs);
            store.sound_playback.load(&nvs);
            store.agent_mode.load(&nvs);
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

pub fn commit() {
    let mut guard = STORE.lock();
    let store = &mut *guard;
    let Some(nvs) = &mut store.nvs else {
        return;
    };
    let mut written = store.brightness.commit(nvs);
    written |= store.color_depth.commit(nvs);
    written |= store.sound_playback.commit(nvs);
    written |= store.agent_mode.commit(nvs);
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

pub fn color_depth() -> ColorDepth {
    STORE.lock().color_depth.value
}

pub fn set_color_depth(depth: ColorDepth) {
    STORE.lock().color_depth.set(depth);
}

pub fn sound_playback() -> SoundPlayback {
    STORE.lock().sound_playback.value
}

pub fn set_sound_playback(playback: SoundPlayback) {
    STORE.lock().sound_playback.set(playback);
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
