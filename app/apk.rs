/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * An APK's package details, from its zip's AndroidManifest.xml and
 * resources.arsc.
 */

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use devkit_compression::{Inflate, Window};
use idf::heap::Caps;

const EOCD: u32 = 0x0605_4b50;
const CENTRAL: u32 = 0x0201_4b50;
const LOCAL: u32 = 0x0403_4b50;
const EOCD_SIZE: usize = 22;
const COMMENT_MAX: usize = 0xffff;
const LOCAL_SIZE: usize = 30;
const STORED: u16 = 0;
const DEFLATED: u16 = 8;
const ENTRY_MAX: u32 = 16 * 1024 * 1024;
const REFERENCE_DEPTH: u8 = 4;

const RES_STRING_POOL: u16 = 0x0001;
const RES_TABLE: u16 = 0x0002;
const RES_XML: u16 = 0x0003;
const RES_XML_RESOURCE_MAP: u16 = 0x0180;
const RES_XML_START_ELEMENT: u16 = 0x0102;
const RES_TABLE_PACKAGE: u16 = 0x0200;
const RES_TABLE_TYPE: u16 = 0x0201;
const UTF8_FLAG: u32 = 0x100;
const NO_ENTRY: u32 = 0xffff_ffff;
const TYPE_FLAG_SPARSE: u8 = 0x01;
const TYPE_FLAG_OFFSET16: u8 = 0x02;
const CONFIG_DENSITY: usize = 34;
const DENSITY_DEFAULT: u16 = 0;
const DENSITY_MEDIUM: u16 = 160;
const DENSITY_ANY: u16 = 0xfffe;
const DENSITY_NONE: u16 = 0xffff;
const ICON_MAX: u32 = 1024 * 1024;
const ENTRY_FLAG_COMPLEX: u16 = 0x0001;
/// The entry holds its value's type in its flags' high byte and the value in
/// place of a key and a `Res_value`.
const ENTRY_FLAG_COMPACT: u16 = 0x0008;

const TYPE_REFERENCE: u8 = 0x01;
const TYPE_STRING: u8 = 0x03;
const TYPE_INT_DEC: u8 = 0x10;
const TYPE_INT_HEX: u8 = 0x11;

const ATTR_LABEL: u32 = 0x0101_0001;
const ATTR_ICON: u32 = 0x0101_0002;
const ATTR_VERSION_CODE: u32 = 0x0101_021b;
const ATTR_VERSION_NAME: u32 = 0x0101_021c;
const ATTR_MIN_SDK: u32 = 0x0101_020c;
const ATTR_TARGET_SDK: u32 = 0x0101_0270;

/// A field the package does not give stays empty.
#[derive(Clone, Default, PartialEq, Eq, Debug)]
pub struct ApkInfo {
    pub package: String,
    pub label: String,
    pub version_name: String,
    pub version_code: Option<u32>,
    pub min_sdk: Option<u32>,
    pub target_sdk: Option<u32>,
    /// The launcher icon as a PNG or WebP file.
    pub icon: Option<Vec<u8>>,
}

/// Random access to the APK's bytes.
pub trait ReadAt {
    fn len(&self) -> u64;
    /// Up to `len` bytes from `offset`.
    async fn read_at(&mut self, offset: u64, len: usize) -> Result<Vec<u8>, String>;
}

pub async fn read<R: ReadAt>(apk: &mut R) -> Result<ApkInfo, String> {
    let entries = directory(apk).await?;
    let manifest = entries.iter().find(|e| e.name == "AndroidManifest.xml").ok_or("No AndroidManifest.xml in the package.")?;
    let manifest = extract(apk, manifest).await?;
    let (mut info, label, icon) = parse_manifest(&manifest)?;
    if let Label::Text(text) = &label {
        info.label = text.clone();
    }
    let needs_table = matches!(label, Label::Reference(_)) || icon.is_some();
    let table = match entries.iter().find(|e| e.name == "resources.arsc") {
        Some(table) if needs_table => extract(apk, table).await?,
        _ => return Ok(info),
    };
    if let Label::Reference(id) = label {
        info.label = resolve_string(&table, id).unwrap_or_default();
    }
    if let Some(path) = icon.and_then(|id| resolve_icon(&table, id))
        && let Some(entry) = entries.iter().find(|e| e.name == path && e.size <= ICON_MAX)
    {
        info.icon = extract(apk, entry).await.ok();
    }
    Ok(info)
}

pub struct ZipEntry {
    name: String,
    method: u16,
    compressed: u32,
    size: u32,
    local: u32,
}

fn u16_at(b: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(at..at + 2)?.try_into().ok()?))
}

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

async fn directory<R: ReadAt>(apk: &mut R) -> Result<Vec<ZipEntry>, String> {
    const NOT_ZIP: &str = "Not a valid package.";
    let len = apk.len();
    let tail_len = len.min((EOCD_SIZE + COMMENT_MAX) as u64);
    let tail = apk.read_at(len - tail_len, tail_len as usize).await?;
    let at = (0..tail.len().saturating_sub(EOCD_SIZE - 1)).rev().find(|&i| u32_at(&tail, i) == Some(EOCD)).ok_or(NOT_ZIP)?;
    let size = u32_at(&tail, at + 12).ok_or(NOT_ZIP)?;
    let offset = u32_at(&tail, at + 16).ok_or(NOT_ZIP)?;
    if u64::from(offset) + u64::from(size) > len || size > ENTRY_MAX {
        return Err(NOT_ZIP.into());
    }
    let cd = apk.read_at(u64::from(offset), size as usize).await?;
    parse_directory(&cd).ok_or_else(|| NOT_ZIP.into())
}

pub fn parse_directory(cd: &[u8]) -> Option<Vec<ZipEntry>> {
    let mut entries = Vec::new();
    let mut at = 0;
    while at + 46 <= cd.len() {
        if u32_at(cd, at)? != CENTRAL {
            return None;
        }
        let name_len = usize::from(u16_at(cd, at + 28)?);
        let extra = usize::from(u16_at(cd, at + 30)?);
        let comment = usize::from(u16_at(cd, at + 32)?);
        let name = cd.get(at + 46..at + 46 + name_len)?;
        entries.push(ZipEntry {
            name: String::from_utf8_lossy(name).into_owned(),
            method: u16_at(cd, at + 10)?,
            compressed: u32_at(cd, at + 20)?,
            size: u32_at(cd, at + 24)?,
            local: u32_at(cd, at + 42)?,
        });
        at += 46 + name_len + extra + comment;
    }
    Some(entries)
}

async fn extract<R: ReadAt>(apk: &mut R, entry: &ZipEntry) -> Result<Vec<u8>, String> {
    let broken = || alloc::format!("{} is damaged.", entry.name);
    if entry.size > ENTRY_MAX || entry.compressed > ENTRY_MAX {
        return Err(alloc::format!("{} is too large.", entry.name));
    }
    let header = apk.read_at(u64::from(entry.local), LOCAL_SIZE).await?;
    if u32_at(&header, 0) != Some(LOCAL) {
        return Err(broken());
    }
    let skip = LOCAL_SIZE + usize::from(u16_at(&header, 26).ok_or_else(broken)?) + usize::from(u16_at(&header, 28).ok_or_else(broken)?);
    let data = apk.read_at(u64::from(entry.local) + skip as u64, entry.compressed as usize).await?;
    if data.len() != entry.compressed as usize {
        return Err(broken());
    }
    match entry.method {
        STORED => Ok(data),
        DEFLATED => inflate(&data, entry.size as usize).ok_or_else(broken),
        _ => Err(alloc::format!("{} is compressed in an unknown way.", entry.name)),
    }
}

pub fn inflate(data: &[u8], size: usize) -> Option<Vec<u8>> {
    let mut inflate = idf::heap::zeroed_box::<Inflate>(Caps::DEFAULT).ok()?;
    let mut window = idf::heap::zeroed_box::<Window>(Caps::DEFAULT).ok()?;
    let mut out = alloc::vec![0u8; size];
    let mut src = data;
    inflate.start_raw();
    let n = inflate.read(&mut src, &mut window, &mut out).ok()?;
    (n == size).then_some(out)
}

struct Pool<'a> {
    data: &'a [u8],
    count: usize,
    strings: usize,
    utf8: bool,
}

impl<'a> Pool<'a> {
    fn new(chunk: &'a [u8]) -> Option<Self> {
        Some(Self { data: chunk, count: u32_at(chunk, 8)? as usize, strings: u32_at(chunk, 20)? as usize, utf8: u32_at(chunk, 16)? & UTF8_FLAG != 0 })
    }

    fn get(&self, index: u32) -> Option<String> {
        let index = index as usize;
        if index >= self.count {
            return None;
        }
        let offset = u32_at(self.data, 28 + index * 4)? as usize;
        let at = self.strings + offset;
        if self.utf8 {
            let (_, at) = utf8_len(self.data, at)?;
            let (bytes, at) = utf8_len(self.data, at)?;
            Some(String::from_utf8_lossy(self.data.get(at..at + bytes)?).into_owned())
        } else {
            let mut units = usize::from(u16_at(self.data, at)?);
            let mut at = at + 2;
            if units & 0x8000 != 0 {
                units = ((units & 0x7fff) << 16) | usize::from(u16_at(self.data, at)?);
                at += 2;
            }
            let raw = self.data.get(at..at + units * 2)?;
            Some(char::decode_utf16(raw.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]]))).map(|c| c.unwrap_or('\u{fffd}')).collect())
        }
    }
}

fn utf8_len(b: &[u8], at: usize) -> Option<(usize, usize)> {
    let first = usize::from(*b.get(at)?);
    if first & 0x80 == 0 {
        return Some((first, at + 1));
    }
    Some((((first & 0x7f) << 8) | usize::from(*b.get(at + 1)?), at + 2))
}

/// The chunks inside `data`, as (type, header size, whole chunk).
fn chunks(data: &[u8]) -> impl Iterator<Item = (u16, usize, &[u8])> {
    let mut at = 0;
    core::iter::from_fn(move || {
        let kind = u16_at(data, at)?;
        let header = usize::from(u16_at(data, at + 2)?);
        let size = u32_at(data, at + 4)? as usize;
        let chunk = data.get(at..at.checked_add(size)?)?;
        if size < 8 {
            return None;
        }
        at += size;
        Some((kind, header, chunk))
    })
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Label {
    None,
    Text(String),
    Reference(u32),
}

/// What the binary XML manifest gives, with the label and the icon resource.
pub fn parse_manifest(xml: &[u8]) -> Result<(ApkInfo, Label, Option<u32>), String> {
    const BROKEN: &str = "The package's manifest is damaged.";
    let (kind, header, _) = chunks(xml).next().ok_or(BROKEN)?;
    if kind != RES_XML {
        return Err(BROKEN.into());
    }
    let mut pool = None;
    let mut ids: Vec<u32> = Vec::new();
    let mut info = ApkInfo::default();
    let mut label = Label::None;
    let mut icon = None;
    for (kind, _, chunk) in chunks(&xml[header..]) {
        match kind {
            RES_STRING_POOL => pool = Pool::new(chunk),
            RES_XML_RESOURCE_MAP => ids = chunk[8..].chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect(),
            RES_XML_START_ELEMENT => {
                let pool = pool.as_ref().ok_or(BROKEN)?;
                let element = pool.get(u32_at(chunk, 20).ok_or(BROKEN)?).unwrap_or_default();
                let attr_start = usize::from(u16_at(chunk, 24).ok_or(BROKEN)?);
                let attr_size = usize::from(u16_at(chunk, 26).ok_or(BROKEN)?);
                let count = usize::from(u16_at(chunk, 28).ok_or(BROKEN)?);
                for i in 0..count {
                    let at = 16 + attr_start + i * attr_size;
                    let (Some(name), Some(raw), Some(kind), Some(data)) = (u32_at(chunk, at + 4), u32_at(chunk, at + 8), chunk.get(at + 15), u32_at(chunk, at + 16)) else {
                        return Err(BROKEN.into());
                    };
                    let attr = pool.get(name).unwrap_or_default();
                    let key = match ids.get(name as usize).copied() {
                        Some(ATTR_LABEL) => "label",
                        Some(ATTR_ICON) => "icon",
                        Some(ATTR_VERSION_CODE) => "versionCode",
                        Some(ATTR_VERSION_NAME) => "versionName",
                        Some(ATTR_MIN_SDK) => "minSdkVersion",
                        Some(ATTR_TARGET_SDK) => "targetSdkVersion",
                        _ => attr.as_str(),
                    };
                    let text = || if *kind == TYPE_STRING { pool.get(data) } else if raw != NO_ENTRY { pool.get(raw) } else { None };
                    let number = || match *kind {
                        TYPE_INT_DEC | TYPE_INT_HEX => Some(data),
                        TYPE_STRING => text()?.parse().ok(),
                        _ => None,
                    };
                    match (element.as_str(), key) {
                        ("manifest", "package") => info.package = text().unwrap_or_default(),
                        ("manifest", "versionCode") => info.version_code = number(),
                        ("manifest", "versionName") => info.version_name = text().unwrap_or_default(),
                        ("uses-sdk", "minSdkVersion") => info.min_sdk = number(),
                        ("uses-sdk", "targetSdkVersion") => info.target_sdk = number(),
                        ("application", "icon") if *kind == TYPE_REFERENCE => icon = Some(data),
                        ("application", "label") => {
                            label = match *kind {
                                TYPE_REFERENCE => Label::Reference(data),
                                _ => text().map_or(Label::None, Label::Text),
                            }
                        }
                        _ => {}
                    }
                }
            }
            _ => {}
        }
    }
    Ok((info, label, icon))
}

/// The string resource `id` names, in the configuration without a language
/// when there is one, else the first.
pub fn resolve_string(table: &[u8], id: u32) -> Option<String> {
    resolve_string_in(table, id, REFERENCE_DEPTH)
}

fn resolve_string_in(table: &[u8], id: u32, depth: u8) -> Option<String> {
    let (pool, values) = values(table, id)?;
    let value = values.iter().find(|v| v.default).or(values.first())?;
    match value.kind {
        TYPE_STRING => pool?.get(value.data),
        TYPE_REFERENCE if depth > 0 => resolve_string_in(table, value.data, depth - 1),
        _ => None,
    }
}

/// The path in the package of the densest bitmap resource `id` names; a
/// drawable described in XML has none.
pub fn resolve_icon(table: &[u8], id: u32) -> Option<String> {
    resolve_icon_in(table, id, REFERENCE_DEPTH)
}

fn resolve_icon_in(table: &[u8], id: u32, depth: u8) -> Option<String> {
    let (pool, values) = values(table, id)?;
    let bitmap = |path: &str| {
        let path = path.to_ascii_lowercase();
        path.ends_with(".png") || path.ends_with(".webp")
    };
    let best = values
        .iter()
        .filter(|v| v.kind == TYPE_STRING && v.density != DENSITY_ANY)
        .filter_map(|v| Some((v.density, pool.as_ref()?.get(v.data)?)))
        .filter(|(_, path)| bitmap(path))
        .max_by_key(|&(density, _)| match density {
            DENSITY_NONE => 0,
            DENSITY_DEFAULT => DENSITY_MEDIUM,
            density => density,
        });
    if let Some((_, path)) = best {
        return Some(path);
    }
    let reference = values.iter().find(|v| v.kind == TYPE_REFERENCE)?;
    if depth == 0 {
        return None;
    }
    resolve_icon_in(table, reference.data, depth - 1)
}

struct Value {
    default: bool,
    density: u16,
    kind: u8,
    data: u32,
}

/// Every configuration's value of resource `id`, with the table's strings.
fn values(table: &[u8], id: u32) -> Option<(Option<Pool<'_>>, Vec<Value>)> {
    let (kind, header, _) = chunks(table).next()?;
    if kind != RES_TABLE {
        return None;
    }
    let (package_id, type_id, entry) = ((id >> 24) as u8, ((id >> 16) & 0xff) as u8, (id & 0xffff) as usize);
    let mut pool = None;
    let mut values = Vec::new();
    for (kind, _, chunk) in chunks(&table[header..]) {
        match kind {
            RES_STRING_POOL => pool = Pool::new(chunk),
            RES_TABLE_PACKAGE if u32_at(chunk, 8)? == u32::from(package_id) => {
                let package_header = usize::from(u16_at(chunk, 2)?);
                for (kind, _, chunk) in chunks(&chunk[package_header..]) {
                    if kind != RES_TABLE_TYPE || *chunk.get(8)? != type_id {
                        continue;
                    }
                    let Some((kind, data)) = type_entry(chunk, entry) else { continue };
                    values.push(Value { default: default_config(chunk), density: u16_at(chunk, CONFIG_DENSITY).unwrap_or(0), kind, data });
                }
            }
            _ => {}
        }
    }
    Some((pool, values))
}

fn default_config(chunk: &[u8]) -> bool {
    u16_at(chunk, 28).is_some_and(|language| language == 0)
}

/// The (data type, data) of entry `index` in a type chunk.
fn type_entry(chunk: &[u8], index: usize) -> Option<(u8, u32)> {
    let flags = *chunk.get(9)?;
    let count = u32_at(chunk, 12)? as usize;
    let entries = u32_at(chunk, 16)? as usize;
    let offsets = usize::from(u16_at(chunk, 2)?);
    let offset = if flags & TYPE_FLAG_SPARSE != 0 {
        (0..count).find_map(|i| (usize::from(u16_at(chunk, offsets + i * 4)?) == index).then(|| u16_at(chunk, offsets + i * 4 + 2)).flatten()).map(|o| usize::from(o) * 4)?
    } else if index >= count {
        return None;
    } else if flags & TYPE_FLAG_OFFSET16 != 0 {
        let o = u16_at(chunk, offsets + index * 2)?;
        if o == 0xffff {
            return None;
        }
        usize::from(o) * 4
    } else {
        let o = u32_at(chunk, offsets + index * 4)?;
        if o == NO_ENTRY {
            return None;
        }
        o as usize
    };
    let at = entries + offset;
    let entry_flags = u16_at(chunk, at + 2)?;
    if entry_flags & ENTRY_FLAG_COMPLEX != 0 {
        return None;
    }
    if entry_flags & ENTRY_FLAG_COMPACT != 0 {
        return Some(((entry_flags >> 8) as u8, u32_at(chunk, at + 4)?));
    }
    let size = usize::from(u16_at(chunk, at)?);
    Some((*chunk.get(at + size + 3)?, u32_at(chunk, at + size + 4)?))
}

impl ApkInfo {
    pub fn version(&self) -> String {
        match (self.version_name.as_str(), self.version_code) {
            ("", None) => String::new(),
            ("", Some(code)) => code.to_string(),
            (name, None) => name.to_string(),
            (name, Some(code)) => alloc::format!("{name} ({code})"),
        }
    }

    pub fn sdk(&self) -> String {
        match (self.min_sdk, self.target_sdk) {
            (None, None) => String::new(),
            (Some(min), None) => alloc::format!("min {min}"),
            (None, Some(target)) => alloc::format!("target {target}"),
            (Some(min), Some(target)) => alloc::format!("min {min} / target {target}"),
        }
    }
}
