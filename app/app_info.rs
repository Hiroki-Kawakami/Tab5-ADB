/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Names and icons of apps from the agent, kept in a heap region of their own.
 */

use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::alloc::AllocError;
use core::ffi::CStr;
use core::time::Duration;

use agent_link::{AppEntry, Link};
use devkit_gfx::{ImageBuf, ImageFormat, ImageSource};
use idf::heap::{Caps, Region};
use idf::log::error;
use idf::sync::{Mutex, OnceLock};

use crate::raster_text::RasterText;

const TAG: &CStr = c"app_info";
const REGION_BYTES: usize = 4 * 1024 * 1024;
const TIMEOUT: Duration = Duration::from_secs(5);

pub type Name = RasterText<&'static Region>;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Size {
    pub icon_px: u16,
    pub text_px: u8,
}

/// What the cache holds for an app at one size.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Found {
    pub id: u32,
    pub name: bool,
    pub icon: bool,
}

struct Entry {
    package: String,
    size: Size,
    id: u32,
    used: u32,
    name: Option<Arc<Name>>,
    icon: Option<Arc<dyn ImageSource>>,
}

impl Entry {
    /// Nothing outside the cache holds its pictures.
    fn unshown(&self) -> bool {
        self.name.as_ref().is_none_or(|n| Arc::strong_count(n) == 1) && self.icon.as_ref().is_none_or(|i| Arc::strong_count(i) == 1)
    }
}

struct Cache {
    next_id: u32,
    clock: u32,
    entries: Vec<Entry>,
}

static REGION: OnceLock<Option<Region>> = OnceLock::new();
static CACHE: Mutex<Cache> = Mutex::new(Cache { next_id: 0, clock: 0, entries: Vec::new() });

fn region() -> Option<&'static Region> {
    REGION
        .get_or_init(|| Region::new(REGION_BYTES, Caps::SPIRAM).inspect_err(|err| error!(TAG, "region: {err}")).ok())
        .as_ref()
}

impl Cache {
    fn find(&mut self, package: &str, size: Size) -> Option<&mut Entry> {
        self.entries.iter_mut().find(|e| e.size == size && e.package == package)
    }

    /// Drops the least recently used entry nobody shows; `false` when there is none.
    fn evict(&mut self) -> bool {
        let oldest = self.entries.iter().enumerate().filter(|(_, e)| (e.name.is_some() || e.icon.is_some()) && e.unshown()).min_by_key(|(_, e)| e.used).map(|(i, _)| i);
        match oldest {
            Some(i) => {
                self.entries.swap_remove(i);
                true
            }
            None => false,
        }
    }

    fn alloc<T>(&mut self, mut make: impl FnMut(&'static Region) -> Result<T, AllocError>) -> Option<T> {
        let region = region()?;
        loop {
            if let Ok(value) = make(region) {
                return Some(value);
            }
            if !self.evict() {
                return None;
            }
        }
    }

    fn store(&mut self, package: &str, size: Size, entry: Option<AppEntry<'_>>) {
        let (name, icon) = match entry {
            Some(entry) => {
                let name = self.alloc(|region| RasterText::copy_in(&entry.name, region)).map(Arc::new);
                let icon = (entry.icon_width > 0 && entry.icon_height > 0).then(|| self.alloc(|region| icon(&entry, region))).flatten();
                (name, icon)
            }
            None => (None, None),
        };
        self.clock = self.clock.wrapping_add(1);
        self.next_id = self.next_id.wrapping_add(1);
        let (id, used) = (self.next_id, self.clock);
        if let Some(old) = self.find(package, size) {
            *old = Entry { package: old.package.clone(), size, id, used, name, icon };
            return;
        }
        self.entries.push(Entry { package: package.into(), size, id, used, name, icon });
    }
}

fn icon(entry: &AppEntry<'_>, region: &'static Region) -> Result<Arc<dyn ImageSource>, AllocError> {
    let mut image = unsafe { ImageBuf::new_uninit_in(ImageFormat::Argb8888, entry.icon_width, entry.icon_height, usize::from(entry.icon_width), 4, region)? };
    image.pixels_mut().copy_from_slice(entry.icon);
    Ok(Arc::new(image))
}

/// What the cache has for `package` at `size`; counts as a use.
pub fn find(package: &str, size: Size) -> Option<Found> {
    let mut cache = CACHE.lock();
    cache.clock = cache.clock.wrapping_add(1);
    let clock = cache.clock;
    let entry = cache.find(package, size)?;
    entry.used = clock;
    Some(Found { id: entry.id, name: entry.name.is_some(), icon: entry.icon.is_some() })
}

pub fn name(id: u32) -> Option<Arc<Name>> {
    CACHE.lock().entries.iter().find(|e| e.id == id).and_then(|e| e.name.clone())
}

pub fn icon_of(id: u32) -> Option<Arc<dyn ImageSource>> {
    CACHE.lock().entries.iter().find(|e| e.id == id).and_then(|e| e.icon.clone())
}

/// Forgets every app; what is on screen stays until it leaves.
pub fn clear() {
    CACHE.lock().entries.clear();
}

/// Asks the agent for `packages` at `size` and keeps what comes back.
pub async fn fetch(link: &Link, packages: &[&str], size: Size) -> agent_link::Result<()> {
    let reply = link.app_info(size.icon_px, size.text_px, packages, TIMEOUT).await?;
    let mut cache = CACHE.lock();
    for (package, entry) in packages.iter().zip(reply.entries()) {
        cache.store(package, size, entry);
    }
    Ok(())
}
