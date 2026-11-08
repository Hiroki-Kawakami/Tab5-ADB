/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;
use core::ffi::CStr;
use core::time::Duration;

use adb::link::Connection;
use devkit_ui::widgets::{progress_bar, progress_indicator, segmented, table_header, table_section, top_bar};
use devkit_ui::{Align, Canvas, ColorRole, Component, Cx, Element, Justify, Part, Prop, Rect, Screen, State, StateStyle, This, Ui, id};
use idf::executor;
use idf::log::error;

use crate::device_info::{self, CpuTimes, Hardware, MetricsSample, Status, Wifi};
use crate::fonts::{icon, small_icon};
use crate::resources::{colors, icons};

const TAG: &CStr = c"device_info";

const GAP: i16 = 20;
const MARGIN: i16 = 24;
const RADIUS: u8 = 20;
const CARD_PADDING: i16 = 24;
const DEVICE_CARD_WIDTH: i16 = 340;
const CARD_HEIGHT_PORTRAIT: i16 = 216;
const TABS_WIDTH_LANDSCAPE: i16 = 568;
const TAB_HEIGHT: i16 = 56;
const CHART_HEIGHT_PORTRAIT: i16 = 160;
const CHART_RADIUS: u8 = 12;
const CHART_LINES: i16 = 4;
const CHART_POINTS: usize = 60;
const ROW_HEIGHT: i16 = 64;
const BADGE_SIZE: i16 = 72;
const STATUS_INTERVAL: Duration = Duration::from_secs(10);
const READ_CHUNK: usize = 4096;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Tab {
    Summary,
    Metrics,
    Details,
}

static TAB_TRACK: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Background(colors::PRESSED)],
    states: &[],
};

static TAB: StateStyle = StateStyle {
    font: None,
    base: &[Prop::Height(TAB_HEIGHT), Prop::Foreground(colors::CARD_ICON)],
    states: &[(State::SELECTED, &[Prop::Background(ColorRole::Surface), Prop::Foreground(ColorRole::Text), Prop::Shadow(1, 4, colors::SHADOW)])],
};

const TABS: [(Tab, &str); 3] = [(Tab::Summary, "Summary"), (Tab::Metrics, "Metrics"), (Tab::Details, "Details")];

#[derive(Clone, Copy)]
pub enum Msg {
    Back,
    Tab(Tab),
    CpuChart(i16, i16),
    MemoryChart(i16, i16),
}

#[derive(Default)]
struct Chart {
    size: (i16, i16),
    history: VecDeque<u8>,
    points: Vec<(f32, f32)>,
}

impl Chart {
    fn push(&mut self, value: u8) {
        if self.history.len() == CHART_POINTS {
            self.history.pop_front();
        }
        self.history.push_back(value.min(100));
        self.lay_out();
    }

    fn resize(&mut self, width: i16, height: i16) {
        self.size = (width, height);
        self.lay_out();
    }

    fn lay_out(&mut self) {
        let (width, height) = (f32::from(self.size.0), f32::from(self.size.1));
        let step = width / (CHART_POINTS - 1) as f32;
        let first = CHART_POINTS - self.history.len();
        self.points.clear();
        self.points.extend(self.history.iter().enumerate().map(|(i, &v)| ((first + i) as f32 * step, (height - 2.0) * (1.0 - f32::from(v) / 100.0) + 1.0)));
    }
}

#[derive(Default)]
struct Metrics {
    prev: Option<CpuTimes>,
    cpu: Chart,
    memory: Chart,
    total: Option<u8>,
    cores: Vec<u8>,
    sample: MetricsSample,
}

impl Metrics {
    fn push(&mut self, sample: MetricsSample) {
        if let Some(usage) = self.prev.as_ref().and_then(|prev| device_info::cpu_usage(prev, &sample.cpu))
            && let Some((&total, cores)) = usage.split_first()
        {
            self.total = Some(total);
            self.cores = cores.to_vec();
            self.cpu.push(total);
        }
        if let Some(memory) = sample.memory {
            self.memory.push((memory.used_kb() * 100 / memory.total_kb) as u8);
        }
        self.prev = Some(sample.cpu.clone());
        self.sample = sample;
    }
}

pub struct DeviceInfoScreen {
    conn: Connection,
    tab: Tab,
    hardware: Option<Hardware>,
    status: Option<Status>,
    metrics: Metrics,
}

impl DeviceInfoScreen {
    pub fn new(conn: Connection) -> Self {
        Self { conn, tab: Tab::Summary, hardware: None, status: None, metrics: Metrics::default() }
    }

    fn draw_cpu<'a>(&'a self, canvas: &mut Canvas<'a, '_>) {
        draw_chart(canvas, &self.metrics.cpu, canvas.colors().primary);
    }

    fn draw_memory<'a>(&'a self, canvas: &mut Canvas<'a, '_>) {
        let color = canvas.colors().color(colors::CHART_MEMORY);
        draw_chart(canvas, &self.metrics.memory, color);
    }

    fn summary<'a>(&'a self, ui: &'a Ui<Msg>, landscape: bool) -> Element<'a, Msg> {
        if self.hardware.is_none() && self.status.is_none() {
            return loading(ui);
        }
        let cards = [self.soc_card(ui), self.memory_card(ui), self.storage_card(ui), self.battery_card(ui), self.network_card(ui), self.system_card(ui)];
        if landscape {
            let [a, b, c, d, e, f] = cards;
            let row = |cards: [Element<'a, Msg>; 3]| ui.row(cards.map(|card| card.width(0).grow(1))).gap(GAP).grow(1);
            let grid = ui.column([row([a, b, c]), row([d, e, f])]).gap(GAP).grow(1);
            ui.row([self.device_card(ui, true).width(DEVICE_CARD_WIDTH), grid]).gap(GAP).padding_each(MARGIN, 4, MARGIN, MARGIN).grow(1)
        } else {
            let grid = ui.grid(2, cards.map(|card| card.height(CARD_HEIGHT_PORTRAIT))).gap(GAP);
            ui.scroll(ui.column([self.device_card(ui, false), grid]).gap(GAP).padding_each(MARGIN, 0, MARGIN, MARGIN + ui.insets().bottom)).grow(1)
        }
    }

    fn device_card<'a>(&'a self, ui: &'a Ui<Msg>, landscape: bool) -> Element<'a, Msg> {
        let theme = ui.theme();
        let hw = self.hardware.as_ref();
        let prop = |key| hw.map_or("", |hw| hw.prop(key));
        let name = Some(prop("ro.product.model")).filter(|n| !n.is_empty()).unwrap_or("Android device");
        let maker = join(ui, &[prop("ro.product.manufacturer"), prop("ro.product.device")]);
        let version = match (prop("ro.build.version.release"), prop("ro.build.version.sdk")) {
            ("", _) => "",
            (release, "") => ui.format(format_args!("Android {release}")),
            (release, sdk) => ui.format(format_args!("Android {release} (SDK {sdk})")),
        };
        let badge = ui.row([ui.icon(icon(icons::ANDROID)).foreground(colors::ANDROID)]).size(BADGE_SIZE, BADGE_SIZE).radius(16).align(Align::Center).justify(Justify::Center).background(colors::ANDROID_BADGE);
        let title = ui.text(theme.fonts.title, name);
        if landscape {
            card(ui, [badge, title.padding_each(0, 16, 0, 0), sub(ui, maker), sub(ui, version)])
        } else {
            let text = ui.column([title, sub(ui, join(ui, &[maker, version]))]).gap(4).grow(1);
            ui.row([badge, text]).gap(GAP).align(Align::Center).padding(CARD_PADDING).radius(RADIUS).background(ui.colors().surface)
        }
    }

    fn soc_card<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let hw = self.hardware.as_ref();
        let cores = hw.and_then(|hw| hw.cores).map_or("", |c| ui.format(format_args!("{c} cores")));
        let freq = hw.and_then(|hw| hw.max_freq_khz).map_or("", |f| ui.store(&device_info::format_frequency(f)));
        let abi = hw.map_or("", |hw| hw.prop("ro.product.cpu.abilist").split(',').next().unwrap_or(""));
        info_card(ui, icons::CPU, "SoC", [headline(ui, hw.map_or("", Hardware::soc)), sub(ui, join(ui, &[cores, freq])), sub(ui, abi)])
    }

    fn memory_card<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let memory = self.status.as_ref().and_then(|s| s.memory);
        let marketed = memory.and_then(|m| device_info::marketed_ram_gb(m.total_kb)).map_or("", |gb| ui.format(format_args!("{gb} GB")));
        let total = memory.map_or("", |m| ui.format(format_args!("Total {}", device_info::format_kb(m.total_kb))));
        let available = memory.map_or("", |m| ui.format(format_args!("Available {}", device_info::format_kb(m.available_kb))));
        info_card(ui, icons::MEMORY, "Memory", [headline(ui, marketed), sub(ui, total), sub(ui, available)])
    }

    fn storage_card<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let Some(storage) = self.status.as_ref().and_then(|s| s.storage) else {
            return info_card(ui, icons::STORAGE, "Storage", [headline(ui, "")]);
        };
        let used = ui.format(format_args!("{} used", device_info::format_kb(storage.used_kb)));
        let total = ui.format(format_args!("of {}", device_info::format_kb(storage.total_kb)));
        let marketed = device_info::marketed_storage_gb(storage.total_kb).map_or("", |gb| ui.format(format_args!("{gb} GB")));
        let fraction = storage.used_kb as f32 / storage.total_kb as f32;
        info_card(ui, icons::STORAGE, "Storage", [headline(ui, used), progress_bar(ui, Some(fraction.min(1.0))).padding_y(6), sub(ui, join(ui, &[total, marketed]))])
    }

    fn battery_card<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let Some(b) = self.status.as_ref().and_then(|s| s.battery.as_ref()) else {
            return info_card(ui, icons::BATTERY_CHARGING, "Battery", [headline(ui, "")]);
        };
        let level = ui.format(format_args!("{}% \u{2022} {}", b.level, b.status_text()));
        let health = ui.format(format_args!("{} \u{2022} {}", b.health_text(), device_info::format_temperature(b.temperature_dc)));
        let voltage = if b.voltage_mv > 0 { ui.format(format_args!("{} mV", b.voltage_mv)) } else { "" };
        let technology = if b.technology == "Unknown" { "" } else { b.technology.as_str() };
        info_card(ui, icons::BATTERY_CHARGING, "Battery", [headline(ui, level), sub(ui, health), sub(ui, join(ui, &[voltage, technology]))])
    }

    fn network_card<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let Some(status) = self.status.as_ref() else {
            return info_card(ui, icons::WIFI, "Network", [headline(ui, "")]);
        };
        let mut lines = Vec::new();
        match &status.wifi {
            Wifi::Off => lines.push(headline(ui, "Wi-Fi off")),
            Wifi::Disconnected => lines.push(headline(ui, "Wi-Fi not connected")),
            Wifi::Connected { rssi, ip } => {
                lines.push(headline(ui, "Wi-Fi"));
                lines.push(sub(ui, ui.format(format_args!("{rssi} dBm ({}/4)", Wifi::bars(*rssi)))));
                lines.push(sub(ui, ip.as_deref().unwrap_or("")));
            }
        }
        let cell = &status.cellular;
        let cellular = if !cell.sim_present() {
            "Cellular: no SIM"
        } else if !cell.in_service {
            "Cellular: out of service"
        } else {
            ui.format(format_args!("Cellular: signal {}/4", cell.level))
        };
        lines.push(sub(ui, cellular));
        info_card(ui, icons::WIFI, "Network", lines)
    }

    fn system_card<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let hw = self.hardware.as_ref();
        let prop = |key| hw.map_or("", |hw| hw.prop(key));
        let version = match prop("ro.build.version.release") {
            "" => "",
            release => ui.format(format_args!("Android {release}")),
        };
        let patch = match prop("ro.build.version.security_patch") {
            "" => "",
            patch => ui.format(format_args!("Patch {patch}")),
        };
        info_card(ui, icons::INFO, "System", [headline(ui, version), sub(ui, patch), sub(ui, prop("ro.build.id"))])
    }

    fn metrics<'a>(&'a self, ui: &'a Ui<Msg>, landscape: bool) -> Element<'a, Msg> {
        let theme = ui.theme();
        let m = &self.metrics;
        let total = m.total.map_or("-", |t| ui.format(format_args!("{t}%")));
        let cpu_chart = ui.canvas(Self::draw_cpu).on_size(Msg::CpuChart).id(id!("cpu-chart"));
        let cores = m.cores.iter().enumerate().map(|(i, &usage)| {
            let label = ui.text(theme.fonts.caption, ui.format(format_args!("cpu{i}"))).foreground(colors::MUTED).width(64);
            let value = ui.text(theme.fonts.caption, ui.format(format_args!("{usage}%"))).text_end().width(56);
            ui.row([label, progress_bar(ui, Some(f32::from(usage) / 100.0)).grow(1), value]).gap(12).align(Align::Center)
        });
        let cores = ui.grid(2, cores.collect::<Vec<_>>()).gap_xy(48, 16);
        let memory = m.sample.memory.map_or("-", |mem| {
            let gb = |kb: u64| (kb * 10 + 512 * 1024) / (1024 * 1024);
            let (used, total) = (gb(mem.used_kb()), gb(mem.total_kb));
            ui.format(format_args!("{}.{} / {}.{} GB", used / 10, used % 10, total / 10, total % 10))
        });
        let memory_chart = ui.canvas(Self::draw_memory).on_size(Msg::MemoryChart).id(id!("memory-chart"));
        let load = m.sample.load.as_deref().unwrap_or("-");
        let battery = match (m.sample.battery_level, m.sample.temperature_dc) {
            (Some(level), Some(dc)) => ui.format(format_args!("{level}% \u{2022} {}", device_info::format_temperature(dc))),
            (Some(level), None) => ui.format(format_args!("{level}%")),
            _ => "-",
        };
        let stats = table_section(ui, [info_row(ui, "Load average", load), info_row(ui, "Battery", battery)]);
        if landscape {
            let cpu = card(ui, [metric_head(ui, icons::CPU, "CPU", total), cpu_chart.grow(1), cores.padding_each(0, 8, 0, 0)]).grow(1);
            let memory = card(ui, [metric_head(ui, icons::MEMORY, "Memory", memory), memory_chart.grow(1)]).grow(1);
            ui.row([cpu.width(0).grow(3), ui.column([memory, stats]).gap(GAP).width(0).grow(2)]).gap(GAP).padding_each(MARGIN, 4, MARGIN, MARGIN).grow(1)
        } else {
            let cpu = card(ui, [metric_head(ui, icons::CPU, "CPU", total), cpu_chart.height(CHART_HEIGHT_PORTRAIT), cores.padding_each(0, 8, 0, 0)]);
            let memory = card(ui, [metric_head(ui, icons::MEMORY, "Memory", memory), memory_chart.height(CHART_HEIGHT_PORTRAIT)]);
            ui.scroll(ui.column([cpu, memory, stats]).gap(GAP).padding_each(MARGIN, 0, MARGIN, MARGIN + ui.insets().bottom)).grow(1)
        }
    }

    fn details<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let Some(hw) = self.hardware.as_ref() else {
            return loading(ui);
        };
        let prop = |key| hw.prop(key);
        let device = section(ui, "DEVICE", [
            ("Manufacturer", prop("ro.product.manufacturer")),
            ("Brand", prop("ro.product.brand")),
            ("Model", prop("ro.product.model")),
            ("Device", prop("ro.product.device")),
            ("Product", prop("ro.product.name")),
            ("Board", prop("ro.product.board")),
            ("Hardware", prop("ro.hardware")),
            ("Serial", prop("ro.serialno")),
        ]);
        let soc = match (prop("ro.soc.manufacturer"), prop("ro.soc.model")) {
            ("", model) => model,
            (maker, "") => maker,
            (maker, model) => ui.format(format_args!("{maker} {model}")),
        };
        let status = self.status.as_ref();
        let uptime = status.and_then(|s| s.uptime_s).map_or("", |s| ui.store(&device_info::format_uptime(s)));
        let sim = status.map_or(prop("gsm.sim.state"), |s| s.cellular.sim_state.as_str());
        let system = section(ui, "SYSTEM", [
            ("SoC", soc),
            ("ABIs", prop("ro.product.cpu.abilist")),
            ("Display", hw.display.as_deref().unwrap_or("")),
            ("Uptime", uptime),
            ("Locale", prop("persist.sys.locale")),
            ("Timezone", prop("persist.sys.timezone")),
            ("SIM state", sim),
        ]);
        let build = section(ui, "BUILD", [
            ("Build", prop("ro.build.id")),
            ("Build date", ui.store(&device_info::format_build_date(prop("ro.build.date")))),
            ("Bootloader", prop("ro.bootloader")),
            ("Baseband", prop("gsm.version.baseband")),
            ("Kernel", hw.kernel.as_deref().unwrap_or("")),
            ("Fingerprint", prop("ro.build.fingerprint")),
        ]);
        ui.scroll(ui.column([device, system, build]).padding_each(MARGIN, 0, MARGIN, MARGIN + ui.insets().bottom)).grow(1).id(id!("details-scroll"))
    }
}

impl Component for DeviceInfoScreen {
    type Msg = Msg;

    fn update(&mut self, msg: Msg, cx: &mut Cx<Self>) {
        match msg {
            Msg::Back => cx.nav().pop(),
            Msg::Tab(tab) => self.tab = tab,
            Msg::CpuChart(width, height) => {
                self.metrics.cpu.resize(width, height);
                cx.redraw(id!("cpu-chart"));
            }
            Msg::MemoryChart(width, height) => {
                self.metrics.memory.resize(width, height);
                cx.redraw(id!("memory-chart"));
            }
        }
        cx.rebuild();
    }

    fn view<'a>(&'a self, ui: &'a Ui<Msg>) -> Element<'a, Msg> {
        let (width, height) = ui.screen_size();
        let landscape = width > height;
        let body = match self.tab {
            Tab::Summary => ui.column([self.summary(ui, landscape)]).task(|this: This<Self>| refresh_status(this, true)).id(id!("summary")),
            Tab::Metrics => ui.column([self.metrics(ui, landscape)]).task(|this: This<Self>| stream_metrics(this)).id(id!("metrics")),
            Tab::Details => ui.column([self.details(ui)]).task(|this: This<Self>| refresh_status(this, false)).id(id!("details")),
        }
        .grow(1);
        let header = if landscape {
            top_bar(ui, Some(Msg::Back), "Device Info", ui.row([tabs(ui, self.tab).width(TABS_WIDTH_LANDSCAPE)]).padding_each(0, 0, MARGIN - ui.theme().widgets.top_bar.padding_x, 0)).scroll_border(id!("details-scroll"))
        } else {
            ui.column([top_bar(ui, Some(Msg::Back), "Device Info", ui.empty()), ui.row([tabs(ui, self.tab).grow(1)]).padding_each(MARGIN, 0, MARGIN, GAP)]).scroll_border(id!("details-scroll"))
        };
        ui.column([header, body]).padding_each(0, ui.insets().top, 0, 0).background(ui.colors().background)
    }
}

impl Screen for DeviceInfoScreen {
    fn on_load(&mut self, cx: &mut Cx<Self>) {
        let conn = self.conn.clone();
        cx.spawn(|this| load_hardware(this, conn));
    }
}

async fn load_hardware(this: This<DeviceInfoScreen>, conn: Connection) {
    match adb::services::exec(&conn, device_info::HARDWARE_COMMAND).await {
        Ok(out) => {
            let hardware = device_info::parse_hardware(&String::from_utf8_lossy(&out));
            this.with(|s, cx| {
                s.hardware = Some(hardware);
                cx.rebuild();
            });
        }
        Err(err) => error!(TAG, "hardware: {err}"),
    }
}

async fn refresh_status(this: This<DeviceInfoScreen>, repeat: bool) {
    let Some(conn) = this.with(|s, _| s.conn.clone()) else { return };
    loop {
        match adb::services::exec(&conn, device_info::STATUS_COMMAND).await {
            Ok(out) => {
                let status = device_info::parse_status(&String::from_utf8_lossy(&out));
                this.with(|s, cx| {
                    s.status = Some(status);
                    cx.rebuild();
                });
            }
            Err(err) => error!(TAG, "status: {err}"),
        }
        if !repeat {
            return;
        }
        executor::sleep(STATUS_INTERVAL).await;
    }
}

async fn stream_metrics(this: This<DeviceInfoScreen>) {
    let Some(conn) = this.with(|s, _| {
        s.metrics.prev = None;
        s.conn.clone()
    }) else {
        return;
    };
    let mut stream = match adb::services::shell(&conn, device_info::METRICS_COMMAND).await {
        Ok(stream) => stream,
        Err(err) => {
            error!(TAG, "metrics: {err}");
            return;
        }
    };
    let mut buf = vec![0; READ_CHUNK];
    let (mut pending, mut frame, mut started) = (String::new(), String::new(), false);
    loop {
        let n = match stream.read(&mut buf).await {
            Ok(0) => return,
            Ok(n) => n,
            Err(err) => {
                error!(TAG, "metrics: {err}");
                return;
            }
        };
        pending.push_str(&String::from_utf8_lossy(&buf[..n]));
        while let Some(end) = pending.find('\n') {
            let line: String = pending.drain(..=end).collect();
            if line.trim() != device_info::METRICS_MARKER {
                frame.push_str(&line);
                continue;
            }
            if started {
                let sample = device_info::parse_metrics(&frame);
                let alive = this.with(|s, cx| {
                    s.metrics.push(sample);
                    cx.rebuild();
                    redraw_charts(cx);
                });
                if alive.is_none() {
                    return;
                }
            }
            started = true;
            frame.clear();
        }
    }
}

fn redraw_charts(cx: &mut Cx<DeviceInfoScreen>) {
    cx.redraw(id!("cpu-chart"));
    cx.redraw(id!("memory-chart"));
}

fn draw_chart<'a>(canvas: &mut Canvas<'a, '_>, chart: &'a Chart, line: devkit_gfx::Color) {
    let (width, height) = canvas.size();
    let colors = canvas.colors();
    canvas.round_rect(Rect::new(0, 0, width, height), CHART_RADIUS, colors.color(colors::CHART_BACKGROUND));
    for i in 1..CHART_LINES {
        canvas.fill(Rect::new(0, height * i / CHART_LINES, width, 1), colors.divider);
    }
    if chart.points.len() > 1 {
        canvas.polyline(&chart.points, 3.0, line);
    }
}

fn tabs<'a>(ui: &'a Ui<Msg>, selected: Tab) -> Element<'a, Msg> {
    let options = TABS.map(|(tab, label)| (label, Msg::Tab(tab)));
    let index = TABS.iter().position(|&(tab, _)| tab == selected).unwrap_or(0);
    let body = ui.theme().fonts.body;
    segmented(ui, &options, index).style(&TAB_TRACK).part(Part::Item, |item| item.style(&TAB).width(0).grow(1)).part(Part::Label, |label| label.font(body))
}

fn loading<'a>(ui: &'a Ui<Msg>) -> Element<'a, Msg> {
    ui.column([progress_indicator(ui, None)]).align(Align::Center).justify(Justify::Center).grow(1)
}

fn join<'a>(ui: &'a Ui<Msg>, parts: &[&str]) -> &'a str {
    let mut text = String::new();
    for part in parts.iter().filter(|p| !p.is_empty()) {
        if !text.is_empty() {
            text.push_str(" \u{2022} ");
        }
        text.push_str(part);
    }
    ui.store(&text)
}

fn card<'a>(ui: &'a Ui<Msg>, children: impl devkit_ui::Children<'a, Msg>) -> Element<'a, Msg> {
    ui.column(children).gap(4).padding(CARD_PADDING).radius(RADIUS).background(ui.colors().surface)
}

fn card_head<'a>(ui: &'a Ui<Msg>, glyph: char, title: &'a str) -> Element<'a, Msg> {
    ui.row([ui.icon(small_icon(glyph)).foreground(colors::CARD_ICON), ui.text(ui.theme().fonts.caption, title).foreground(colors::MUTED).grow(1)]).gap(12).align(Align::Center)
}

fn info_card<'a>(ui: &'a Ui<Msg>, glyph: char, title: &'a str, lines: impl IntoIterator<Item = Element<'a, Msg>>) -> Element<'a, Msg> {
    let mut children = vec![card_head(ui, glyph, title).padding_each(0, 0, 0, 12)];
    children.extend(lines);
    card(ui, children)
}

fn metric_head<'a>(ui: &'a Ui<Msg>, glyph: char, title: &'a str, value: &'a str) -> Element<'a, Msg> {
    ui.row([card_head(ui, glyph, title).grow(1), ui.text(ui.theme().fonts.title, value)]).align(Align::Center).padding_each(0, 0, 0, 12)
}

fn headline<'a>(ui: &'a Ui<Msg>, text: &'a str) -> Element<'a, Msg> {
    ui.text(ui.theme().fonts.body, if text.is_empty() { "-" } else { text }).ellipsis(devkit_ui::Elide::End)
}

fn sub<'a>(ui: &'a Ui<Msg>, text: &'a str) -> Element<'a, Msg> {
    if text.is_empty() {
        return ui.empty();
    }
    ui.text(ui.theme().fonts.caption, text).foreground(colors::MUTED).ellipsis(devkit_ui::Elide::End)
}

fn info_row<'a>(ui: &'a Ui<Msg>, label: &'a str, value: &'a str) -> Element<'a, Msg> {
    let theme = ui.theme();
    ui.row([ui.text(theme.fonts.caption, label).foreground(colors::MUTED).shrink(0), ui.text(theme.fonts.body, value).text_end().wrap().grow(1)])
        .min_height(ROW_HEIGHT)
        .padding_x(theme.widgets.unit.padding)
        .padding_y(10)
        .gap(theme.widgets.unit.padding)
        .align(Align::Center)
}

fn section<'a, const N: usize>(ui: &'a Ui<Msg>, title: &'a str, rows: [(&'a str, &'a str); N]) -> Element<'a, Msg> {
    let rows: Vec<_> = rows.into_iter().filter(|(_, value)| !value.is_empty()).map(|(label, value)| info_row(ui, label, value)).collect();
    if rows.is_empty() {
        return ui.empty();
    }
    ui.column([table_header(ui, title), table_section(ui, rows)])
}
