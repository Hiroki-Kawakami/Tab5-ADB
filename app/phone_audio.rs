/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * The device's sound on the Tab5's speaker: an agent audio stream, decoded,
 * buffered and played with the device's media volume as the Tab5's.
 */

use alloc::vec::Vec;
use core::ffi::CStr;
use core::time::Duration;

use agent_link::protocol::{EBUSY, VOLUME_SCALE};
use agent_link::{AudioCodec, AudioMessage, AudioSource, AudioVolume, Error, Link};
use bsp_rs::audio::{Audio, Format, Pcm, SampleFormat};
use devkit_audio::aac::{AacConfig, AacDecoder, HeMode};
use devkit_audio::drift::{Drift, DriftConfig};
use devkit_audio::fifo::{self, FifoConfig, Overrun, Underrun};
use devkit_audio::graph::{GraphBuilder, GraphConfig};
use devkit_audio::opus::{OpusConfig, OpusDecoder};
use devkit_audio::resampler::{Resampler, ResamplerConfig, ResamplerKind};
use idf::executor::JoinHandle;
use idf::heap::Caps;
use idf::log::error;
use idf::sync::{self, OnceLock};
use idf::thread::{self, Builder};

use crate::adb_link::{self, Route};
use crate::settings;

const TAG: &CStr = c"phone_audio";
const RATE: u32 = 48000;
const CHANNELS: u8 = 2;
const OPEN_TIMEOUT: Duration = Duration::from_secs(3);
const BUFFER_FRAMES: usize = RATE as usize;
/// Prefill, which the drift also holds, and the skip threshold.
const PCM_BUFFERING: (usize, usize) = (RATE as usize / 10, RATE as usize * 80 / 1000);
/// The mirror player outranks the decoder on core 1, so this has to outlast a
/// decode waiting on it while the overlay is redrawn.
const DECODED_BUFFERING: (usize, usize) = (RATE as usize / 4, RATE as usize / 4);
const SKIP_FADE_FRAMES: usize = RATE as usize / 100;
const WRITE_FRAMES: usize = RATE as usize / 100;
const PACKETS: usize = 32;
const UNITY_VOLUME: u16 = 100;
const OPUS_FRAMES: usize = 5760;
const STACK: usize = 16 * 1024;
/// Above the UI's strip lanes, which take both cores at 10 while drawing.
const DECODE_PRIORITY: u32 = 12;
const PLAY_PRIORITY: u32 = 15;
const CORE: u32 = 1;

static OUTPUT: OnceLock<&'static Audio> = OnceLock::new();

pub fn set_output(audio: &'static Audio) {
    let _ = OUTPUT.set(audio);
}

enum Packet {
    Data(Vec<u8>),
    Stop,
}

/// Plays until dropped; the device plays its sound itself again then, at its own volume.
pub struct PhoneAudio {
    receive: Option<JoinHandle<()>>,
    packets: sync::Sender<Packet>,
    decode: Option<thread::JoinHandle<()>>,
    play: Option<thread::JoinHandle<()>>,
}

impl PhoneAudio {
    /// `busy` runs from another task when another Tab5 takes the device's sound.
    pub fn start(link: Link, source: AudioSource, busy: impl FnOnce() + Send + 'static) -> Option<Self> {
        let Some(&output) = OUTPUT.get() else {
            error!(TAG, "no audio output");
            return None;
        };
        let Some(spawner) = adb_link::spawner() else {
            error!(TAG, "adb not started");
            return None;
        };
        let codec = match adb_link::route() {
            Some(Route::Tcp(_)) => AudioCodec::Aac,
            _ => AudioCodec::Pcm,
        };
        let buffering = match codec {
            AudioCodec::Pcm => PCM_BUFFERING,
            AudioCodec::Opus | AudioCodec::Aac => DECODED_BUFFERING,
        };
        let config = FifoConfig { channels: CHANNELS, capacity: BUFFER_FRAMES, prefill: buffering.0, underrun: Underrun::Silence, overrun: Overrun::DropOldest, caps: Caps::SPIRAM };
        let (writer, reader) = fifo::new::<i16>(&config).inspect_err(|err| error!(TAG, "fifo: {err:?}")).ok()?;
        let (packets, received) = sync::channel(PACKETS);
        let decode = Builder::new().name("phone-decode").stack_size(STACK).priority(DECODE_PRIORITY).core(Some(CORE)).spawn(move || decode(codec, received, writer)).inspect_err(|err| error!(TAG, "decode: {err}")).ok()?;
        let play = match Builder::new().name("phone-play").stack_size(STACK).priority(PLAY_PRIORITY).core(Some(CORE)).spawn(move || play(output, reader, buffering)) {
            Ok(play) => play,
            Err(err) => {
                error!(TAG, "play: {err}");
                packets.send(Packet::Stop);
                decode.join();
                return None;
            }
        };
        let sender = packets.clone();
        let receive = spawner.spawn(async move { receive(link, codec, source, sender, output, busy).await });
        Some(Self { receive: Some(receive), packets, decode: Some(decode), play: Some(play) })
    }
}

impl Drop for PhoneAudio {
    fn drop(&mut self) {
        if let Some(receive) = self.receive.take() {
            receive.abort();
        }
        self.packets.send(Packet::Stop);
        if let Some(decode) = self.decode.take() {
            decode.join();
        }
        if let Some(play) = self.play.take() {
            play.join();
        }
        settings::commit();
    }
}

async fn receive(link: Link, codec: AudioCodec, source: AudioSource, packets: sync::Sender<Packet>, output: &'static Audio, busy: impl FnOnce()) {
    let mut audio = match link.open_audio(codec, source, settings::phone_volume(), OPEN_TIMEOUT).await {
        Ok(audio) => audio,
        Err(Error::Status(EBUSY)) => {
            error!(TAG, "open: another Tab5 takes the sound");
            busy();
            return;
        }
        Err(err) => {
            error!(TAG, "open: {err}");
            return;
        }
    };
    let format = audio.format();
    if format.channels != CHANNELS || format.rate != RATE {
        error!(TAG, "{} channels at {} Hz", format.channels, format.rate);
        return;
    }
    loop {
        match audio.next().await {
            Ok(AudioMessage::Data(data)) => {
                if packets.try_send(Packet::Data(data)).is_err() {
                    error!(TAG, "dropped a packet");
                }
            }
            Ok(AudioMessage::Volume { volume, max, muted }) => {
                if let Err(err) = output.set_volume(if muted { 0 } else { tab5_volume(volume, max) }) {
                    error!(TAG, "volume: {err}");
                }
                remember(volume, max, muted);
            }
            Err(err) => {
                error!(TAG, "stream: {err}");
                return;
            }
        }
    }
}

/// The device's step in proportion up to the Tab5's unity, along the Tab5's own curve.
fn tab5_volume(step: u8, max: u8) -> u8 {
    if max == 0 {
        return 0;
    }
    ((u16::from(step.min(max)) * UNITY_VOLUME + u16::from(max) / 2) / u16::from(max)) as u8
}

/// Keeps the level unless the step differs from the one it rounds to, so it does not drift.
fn remember(step: u8, max: u8, muted: bool) {
    if max == 0 {
        return;
    }
    let mut volume = settings::phone_volume();
    let (step, max) = (u32::from(step.min(max)), u32::from(max));
    let scale = u32::from(VOLUME_SCALE);
    if (u32::from(volume.level) * max + scale / 2) / scale != step {
        volume.level = ((step * scale + max / 2) / max) as u16;
    }
    settings::set_phone_volume(AudioVolume { muted, ..volume });
}

/// Until `Packet::Stop`; aborting the FIFO on the way out ends `play`.
fn decode(codec: AudioCodec, packets: sync::Receiver<Packet>, mut fifo: fifo::Writer<i16>) {
    decode_into(codec, &packets, &mut fifo);
    fifo.abort();
}

fn decode_into(codec: AudioCodec, packets: &sync::Receiver<Packet>, fifo: &mut fifo::Writer<i16>) {
    let mut decoder = match codec {
        AudioCodec::Opus => {
            let head = opus_head();
            match OpusDecoder::new(&OpusConfig { head: &head, caps: Caps::SPIRAM }) {
                Ok(decoder) => Compressed::Opus(decoder),
                Err(err) => {
                    error!(TAG, "opus: {err:?}");
                    return;
                }
            }
        }
        AudioCodec::Aac => {
            let asc = aac_asc();
            match AacDecoder::new(&AacConfig { asc: &asc, adts: false, he: HeMode::Off, caps: Caps::SPIRAM, scratch_caps: Some(Caps::INTERNAL), clock: None }) {
                Ok(decoder) => Compressed::Aac(decoder),
                Err(err) => {
                    error!(TAG, "aac: {err:?}");
                    return;
                }
            }
        }
        AudioCodec::Pcm => Compressed::None,
    };
    let mut pcm = alloc::vec![0i16; OPUS_FRAMES * usize::from(CHANNELS)];
    loop {
        let Packet::Data(data) = packets.recv() else { return };
        let decoded = match &mut decoder {
            Compressed::Opus(decoder) => decoder.decode(&data, &mut pcm).map_err(|err| error!(TAG, "opus: {err:?}")),
            Compressed::Aac(decoder) => decoder.decode(&data, &mut pcm).map_err(|err| error!(TAG, "aac: {err:?}")),
            Compressed::None => {
                let n = (data.len() / 2).min(pcm.len());
                for (sample, bytes) in pcm[..n].iter_mut().zip(data.chunks_exact(2)) {
                    *sample = i16::from_le_bytes([bytes[0], bytes[1]]);
                }
                Ok(n / usize::from(CHANNELS))
            }
        };
        let Ok(frames) = decoded else { continue };
        let samples = &pcm[..frames * usize::from(CHANNELS)];
        if fifo.write(samples).is_err() {
            return;
        }
    }
}

enum Compressed {
    None,
    Opus(OpusDecoder),
    Aac(AacDecoder),
}

// The stream sends raw packets without an OpusHead; this one says 48 kHz stereo.
fn opus_head() -> [u8; 19] {
    let mut head = [0u8; 19];
    head[..8].copy_from_slice(b"OpusHead");
    head[8] = 1;
    head[9] = CHANNELS;
    head[12..16].copy_from_slice(&RATE.to_le_bytes());
    head
}

// AAC-LC, 48 kHz, stereo.
fn aac_asc() -> [u8; 2] {
    let (aot, rate_index) = (2u16, 3u16);
    ((aot << 11) | (rate_index << 7) | (u16::from(CHANNELS) << 3)).to_be_bytes()
}

fn play(output: &'static Audio, fifo: fifo::Reader<i16>, (prefill, skip_threshold): (usize, usize)) {
    let resampler = ResamplerConfig { kind: ResamplerKind::Cubic, channels: CHANNELS, in_rate: RATE, out_rate: RATE, max_in_frames: 0, taps: 0, phases: 0, caps: Caps::DEFAULT };
    let (resampler, _) = match Resampler::<i16>::new(&resampler) {
        Ok(resampler) => resampler,
        Err(err) => {
            error!(TAG, "resampler: {err:?}");
            return;
        }
    };
    let drift = Drift::new(&DriftConfig { sample_rate: RATE, target_frames: prefill, skip_threshold_frames: skip_threshold, skip_fade_frames: SKIP_FADE_FRAMES, ..Default::default() });
    let mut graph = GraphBuilder::new(&GraphConfig { max_frames: WRITE_FRAMES, caps: Caps::DEFAULT });
    let source = graph.add_fifo(fifo);
    let synced = graph.add_resampler(source, resampler, Some(drift));
    let mut graph = match graph.build() {
        Ok(graph) => graph,
        Err(err) => {
            error!(TAG, "graph: {err:?}");
            return;
        }
    };
    let mut stream = match output.open(Format { sample_rate: RATE, sample: SampleFormat::S16, channels: CHANNELS }) {
        Ok(stream) => stream,
        Err(err) => {
            error!(TAG, "open output: {err}");
            return;
        }
    };
    let mut pcm = alloc::vec![0i16; WRITE_FRAMES * usize::from(CHANNELS)];
    while graph.read(synced, &mut pcm).is_ok() {
        if let Err(err) = stream.write(Pcm::S16(&pcm)) {
            error!(TAG, "write: {err}");
            break;
        }
    }
    if let Err(err) = stream.close() {
        error!(TAG, "close output: {err}");
    }
}
