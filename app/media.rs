/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * The device's playing media: the agent's MEDIA events, or `dumpsys
 * media_session` without the agent.
 */

use alloc::string::{String, ToString};
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::time::Duration;

use adb::link::Connection;
use agent_link::protocol::{ESTATE, MEDIA};
use agent_link::{Link, MediaAction, MediaState, PlayState};
use devkit_gfx::ImageSource;
use devkit_ui::{Component, Cx, This};
use idf::executor::{self, Receiver};
use idf::heap::Caps;
use idf::log::error;
use idf::time::Instant;

use crate::preview;
use crate::raster_text::RasterText;

const TAG: &CStr = c"media";
const TIMEOUT: Duration = Duration::from_secs(3);
const POLL: Duration = Duration::from_secs(1);
const AFTER_CONTROL: Duration = Duration::from_millis(300);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Action {
    PlayPause,
    Next,
    Previous,
}

impl Action {
    fn media(self) -> MediaAction {
        match self {
            Action::PlayPause => MediaAction::PlayPause,
            Action::Next => MediaAction::Next,
            Action::Previous => MediaAction::Previous,
        }
    }

    fn dispatch(self) -> &'static str {
        match self {
            Action::PlayPause => "play-pause",
            Action::Next => "next",
            Action::Previous => "previous",
        }
    }
}

/// Where playback stood when a state came.
#[derive(Clone, Copy, Debug)]
pub struct Progress {
    pub state: MediaState,
    pub at: Instant,
}

impl Progress {
    pub fn position_ms(&self) -> u32 {
        let moved = self.at.elapsed().as_millis() as u64 * u64::from(self.state.speed) / 1000;
        let position = u64::from(self.state.position_ms) + moved;
        match self.state.duration_ms {
            0 => position.min(u64::from(u32::MAX)) as u32,
            duration => position.min(u64::from(duration)) as u32,
        }
    }
}

/// The track as the agent drew it.
pub struct Drawn {
    pub title: Option<RasterText<Caps>>,
    pub subtitle: Option<RasterText<Caps>>,
    pub art: Option<Arc<dyn ImageSource>>,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct DrawSize {
    pub title_px: u8,
    pub subtitle_px: u8,
    pub art_px: u16,
    /// `agent_link::protocol::SUBTITLE_*`.
    pub subtitle: u8,
}

pub enum Update {
    State(Progress),
    Drawn(Drawn),
}

async fn draw(link: &Link, size: DrawSize) -> Result<Drawn, String> {
    let reply = link.media_render(size.title_px, size.subtitle_px, size.art_px, size.subtitle, TIMEOUT).await.map_err(|e| e.to_string())?;
    let render = reply.render().ok_or("bad render")?;
    let text = |image| RasterText::copy_in(image, Caps::SPIRAM).map_err(|_| "out of memory");
    let art = if render.art.is_empty() { None } else { Some(Arc::new(preview::decode_jpeg(render.art).await?) as Arc<dyn ImageSource>) };
    Ok(Drawn { title: Some(text(&render.title)?), subtitle: Some(text(&render.subtitle)?), art })
}

/// Follows the agent's MEDIA events into `apply`, drawing each new track at
/// `size`; ends when the component or the link goes.
pub async fn follow<C: Component>(this: This<C>, link: Link, size: DrawSize, apply: fn(&mut C, Update, &mut Cx<C>)) {
    let mut events = link.watch(MEDIA);
    let mut drawn: Option<(u32, bool)> = None;
    loop {
        let payload = match events.next().await {
            Ok(payload) => payload,
            Err(err) => {
                error!(TAG, "events: {err}");
                return;
            }
        };
        let Some(state) = MediaState::parse(&payload) else {
            error!(TAG, "bad state");
            continue;
        };
        if this.with(|c, cx| apply(c, Update::State(Progress { state, at: Instant::now() }), cx)).is_none() {
            return;
        }
        let stale = drawn.is_none_or(|(token, art)| token != state.token || (state.has_art && !art));
        if state.token == 0 || !stale {
            continue;
        }
        match draw(&link, size).await {
            Ok(track) => {
                drawn = Some((state.token, state.has_art));
                if this.with(|c, cx| apply(c, Update::Drawn(track), cx)).is_none() {
                    return;
                }
            }
            Err(err) => error!(TAG, "render: {err}"),
        }
    }
}

/// Without a session, play-pause goes through `dispatch`, which reaches the
/// app that played last.
pub async fn control(link: &Link, conn: &Connection, action: Action) {
    match link.media_control(action.media(), TIMEOUT).await {
        Ok(()) => {}
        Err(agent_link::Error::Status(ESTATE)) if action == Action::PlayPause => dispatch(conn, action).await,
        Err(err) => error!(TAG, "{action:?}: {err}"),
    }
}

pub async fn set_mute(link: &Link, mute: bool) {
    if let Err(err) = link.media_control(MediaAction::Mute(mute), TIMEOUT).await {
        error!(TAG, "mute: {err}");
    }
}

pub async fn set_volume(link: &Link, step: u8) {
    if let Err(err) = link.media_control(MediaAction::Volume(step), TIMEOUT).await {
        error!(TAG, "volume: {err}");
    }
}

pub async fn seek(link: &Link, position_ms: u32) {
    if let Err(err) = link.media_control(MediaAction::Seek(position_ms), TIMEOUT).await {
        error!(TAG, "seek: {err}");
    }
}

/// What `dumpsys media_session` tells of the session it would pick.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Session {
    pub state: PlayState,
    pub title: String,
    pub artist: String,
}

struct Block<'a> {
    state: PlayState,
    description: Option<&'a str>,
}

/// The session with metadata, playing before buffering before paused.
pub fn parse_sessions(out: &str) -> Option<Session> {
    let mut blocks: Vec<Block<'_>> = Vec::new();
    for line in out.lines().map(str::trim) {
        if line.starts_with("package=") {
            blocks.push(Block { state: PlayState::None, description: None });
            continue;
        }
        let Some(block) = blocks.last_mut() else { continue };
        if line.starts_with("state=")
            && let Some(at) = line.find("{state=")
        {
            let name = &line[at + "{state=".len()..];
            block.state = match name.split('(').next().unwrap_or("") {
                "PLAYING" => PlayState::Playing,
                "PAUSED" => PlayState::Paused,
                "BUFFERING" => PlayState::Buffering,
                "STOPPED" => PlayState::Stopped,
                _ => PlayState::None,
            };
        } else if line.starts_with("metadata:")
            && let Some(at) = line.find("description=")
        {
            let description = line[at + "description=".len()..].trim();
            if !description.is_empty() && description != "null" {
                block.description = Some(description);
            }
        }
    }
    let rank = |block: &Block<'_>| match (block.description, block.state) {
        (None, _) => 0,
        (_, PlayState::Playing) => 3,
        (_, PlayState::Buffering) => 2,
        (_, PlayState::Paused) => 1,
        _ => 0,
    };
    let best = blocks.iter().filter(|b| rank(b) > 0).max_by_key(|b| rank(b))?;
    let mut parts = best.description.unwrap_or("").splitn(3, ", ");
    Some(Session {
        state: best.state,
        title: parts.next().unwrap_or("").to_string(),
        artist: parts.next().unwrap_or("").to_string(),
    })
}

/// Polls `dumpsys media_session` into `apply` a second after each answer, and
/// a moment after each poke; ends when the component goes.
pub async fn poll<C: Component>(this: This<C>, conn: Connection, mut poke: Receiver<()>, apply: fn(&mut C, Option<Session>, &mut Cx<C>)) {
    loop {
        match adb::services::exec(&conn, "dumpsys media_session").await {
            Ok(out) => {
                let session = parse_sessions(&String::from_utf8_lossy(&out));
                if this.with(|c, cx| apply(c, session, cx)).is_none() {
                    return;
                }
            }
            Err(err) => error!(TAG, "dumpsys: {err}"),
        }
        if executor::timeout(POLL, poke.recv()).await.is_ok() {
            executor::sleep(AFTER_CONTROL).await;
        }
    }
}

pub async fn dispatch(conn: &Connection, action: Action) {
    if let Err(err) = adb::services::exec(conn, &alloc::format!("cmd media_session dispatch {}", action.dispatch())).await {
        error!(TAG, "dispatch {}: {err}", action.dispatch());
    }
}

#[cfg(test)]
mod tests {
    use super::{PlayState, Session, parse_sessions};

    const DUMP: &str = "\
MEDIA SESSION SERVICE (dumpsys media_session)

Sessions Stack - have 2 sessions:
  Example Player com.example.player/Example (userId=0)
    ownerPid=1000, ownerUid=10100, userId=0
    package=com.example.player
    active=true
    state=PlaybackState {state=PAUSED(2), position=1000, buffered position=0, speed=0.0}
    metadata: size=3, description=Old Song, Some Artist, Some Album
  Other com.example.other/Other (userId=0)
    package=com.example.other
    active=true
    state=PlaybackState {state=PLAYING(3), position=5000, buffered position=0, speed=1.0}
    metadata: size=2, description=Song Title, The Artist, null
  Idle com.example.idle/Idle (userId=0)
    package=com.example.idle
    state=PlaybackState {state=PLAYING(3), position=0}
    metadata: size=0, description=null
";

    #[test]
    fn picks_the_playing_session_with_metadata() {
        let session = parse_sessions(DUMP).unwrap();
        assert_eq!(session, Session { state: PlayState::Playing, title: "Song Title".into(), artist: "The Artist".into() });
    }

    #[test]
    fn nothing_without_metadata() {
        assert_eq!(parse_sessions("package=com.example.idle\nstate=PlaybackState {state=PLAYING(3)}\nmetadata: size=0, description=null\n"), None);
        assert_eq!(parse_sessions(""), None);
    }
}
