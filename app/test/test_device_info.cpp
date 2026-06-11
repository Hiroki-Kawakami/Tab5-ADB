// Host unit test for app/device_info — the fixtures are verbatim Pixel 10
// (Android 16) output captured over `adb shell`, plus synthetic edge cases.
// Wi-Fi SSID/BSSID/MAC/IP are replaced with synthetic placeholders.
// No phone, no LVGL: just parsers on strings.

#include <cassert>
#include <cstdio>
#include <string>

#include "device_info.hpp"

using namespace app::devinfo;

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                \
        }                                                            \
    } while (0)

static const char kBattery[] =
    "Current Battery Service state:\n"
    "  AC powered: true\n"
    "  USB powered: false\n"
    "  Wireless powered: false\n"
    "  Dock powered: false\n"
    "  Max charging current: 0\n"
    "  Max charging voltage: 0\n"
    "  Charge counter: 4135000\n"
    "  status: 4\n"
    "  health: 2\n"
    "  present: true\n"
    "  level: 89\n"
    "  scale: 100\n"
    "  voltage: 4206\n"
    "  The last voltage value sent via the battery changed broadcast: 4229\n"
    "  temperature: 303\n"
    "  technology: Li-ion\n"
    "  Charging state: 4\n"
    "  Charging policy: 2\n"
    "  Capacity level: 4\n";

static const char kDf[] =
    "Filesystem       1K-blocks     Used Available Use% Mounted on\n"
    "/dev/block/dm-75 114582612 33489008  80962532  30% /data/user/0\n";

static const char kWifi[] =
    "Wifi is enabled\n"
    "Wifi scanning is only available when wifi is enabled\n"
    "==== Primary ClientModeManager instance ====\n"
    "Wifi is connected to \"example-ap\"\n"
    "WifiInfo: SSID: \"example-ap\", BSSID: 02:00:00:00:00:01, MAC: 02:00:00:00:00:02, "
    "IP: /192.168.1.100, Security type: 4, Supplicant state: COMPLETED, Wi-Fi standard: 11ax, "
    "RSSI: -56, Link speed: 720Mbps, Tx Link speed: 720Mbps, Frequency: 5200MHz, "
    "MLO Information: , AP MLO Affiliated links: [MloLink{6GHz, channel: 37, id: 2, "
    "RSSI: -127}, MloLink{2.4GHz, channel: 6, id: 0, RSSI: -49}]\n";

static const char kRegistry[] =
    "    mServiceState={mVoiceRegState=1(OUT_OF_SERVICE), mDataRegState=1(OUT_OF_SERVICE), "
    "mChannelNumber=100, getRilVoiceRadioTechnology=14(LTE), getRilDataRadioTechnology=18(IWLAN)}\n"
    "    mSignalStrength=SignalStrength:{mCdma=CellSignalStrengthCdma: cdmaDbm=2147483647 "
    "level=0,mGsm=CellSignalStrengthGsm: rssi=2147483647 mLevel=0,mWcdma=CellSignalStrengthWcdma: "
    "ss=2147483647 level=0,mTdscdma=CellSignalStrengthTdscdma: rssi=2147483647 level=0,"
    "mLte=CellSignalStrengthLte: rssi=-71 rsrp=-98 rsrq=-7 rssnr=15 cqiTableIndex=1 cqi=0 ta=0 "
    "level=4 parametersUseForLevel=0,mNr=CellSignalStrengthNr:{ csiRsrp = 2147483647 "
    "ssSinr = 2147483647 level = 0 parametersUseForLevel = 0 },primary=CellSignalStrengthLte}\n";

static const char kMeminfo[] =
    "MemTotal:       11841016 kB\n"
    "MemFree:          452336 kB\n"
    "MemAvailable:    1755460 kB\n"
    "Buffers:            2560 kB\n"
    "Cached:          2483568 kB\n";

static const char kStatA[] =
    "cpu  1000 0 1000 8000 0 0 0 0 0 0\n"
    "cpu0 500 0 500 4000 0 0 0 0 0 0\n"
    "cpu1 500 0 500 4000 0 0 0 0 0 0\n"
    "intr 89551320 0 0 0\n"
    "ctxt 113227368\n";

static const char kStatB[] =
    "cpu  1500 0 1500 8500 500 0 0 0 0 0\n"
    "cpu0 1000 0 1000 3500 500 0 0 0 0 0\n"
    "cpu1 500 0 500 5000 0 0 0 0 0 0\n"
    "intr 89551321 0 0 0\n"
    "ctxt 113227369\n";

static const char kGetprop[] =
    "[ro.build.version.release]: [16]\n"
    "[ro.build.version.sdk]: [36]\n"
    "[ro.soc.manufacturer]: [Google]\n"
    "[ro.soc.model]: [Tensor G5]\n"
    "[gsm.sim.state]: [ABSENT,NOT_READY]\n"
    "[persist.sys.timezone]: [Asia/Tokyo]\n";

static const char kVersion[] =
    "Linux version 6.6.102-android15-8-g6eb5b2a8c46b-ab14739656-4k (kleaf@build-host) "
    "(Android (11368308, +pgo, +bolt, +lto, +mlgo, based on r510928) clang version 18.0.0 "
    "(https://android.googlesource.com/toolchain/llvm-project "
    "477610d4d0d988e69dbc3fae4fe86bff3f07f2b5), LLD 18.0.0) #1 SMP PREEMPT "
    "Mon Jan 19 02:06:09 UTC 2026\n";

int main() {
    // ---- battery ----
    Battery b = parse_dumpsys_battery(kBattery);
    CHECK(b.level == 89);
    CHECK(b.powered);
    CHECK(b.status == 4 && !b.charging());
    CHECK(std::string(b.status_str()) == "Not charging");
    CHECK(std::string(b.health_str()) == "Good");
    CHECK(b.voltage_mV == 4206);
    CHECK(b.temp_dC == 303);
    CHECK(b.technology == "Li-ion");
    CHECK(parse_dumpsys_battery("garbage").level == -1);

    // ---- df ----
    Storage st = parse_df(kDf);
    CHECK(st.total_kb == 114582612);
    CHECK(st.used_kb == 33489008);
    CHECK(st.avail_kb == 80962532);
    CHECK(marketed_storage_gb(st.total_kb) == 128);
    CHECK(parse_df("").total_kb == 0);

    // ---- wifi ----
    Wifi w = parse_wifi_status(kWifi);
    CHECK(w.state == Wifi::State::Connected);
    CHECK(w.rssi == -56);  // the active WifiInfo RSSI, not the MLO links' -127/-49
    CHECK(w.ip == "192.168.1.100");
    CHECK(w.bars() == 3);
    CHECK(parse_wifi_status("Wifi is disabled\n").state == Wifi::State::Off);
    CHECK(parse_wifi_status("Wifi is enabled\n").state == Wifi::State::Disconnected);
    CHECK(parse_wifi_status("Wifi is enabled\n").bars() == 0);

    // ---- cellular ----
    Cellular c = parse_cellular("ABSENT", kRegistry);
    CHECK(!c.sim_present);  // SIM absent gates the icon even though LTE level=4
    CHECK(!c.in_service);
    CHECK(c.level == 4);  // max of the per-RAT levels, INT_MAX sentinels ignored
    Cellular c2 = parse_cellular("LOADED", "mVoiceRegState=0 mSignalStrength: level=4");
    CHECK(c2.sim_present && c2.in_service && c2.level == 4);

    // ---- meminfo ----
    Mem m = parse_meminfo(kMeminfo);
    CHECK(m.total_kb == 11841016);
    CHECK(m.avail_kb == 1755460);
    CHECK(marketed_ram_gb(m.total_kb) == 12);

    // ---- /proc/stat ----
    CpuTimes a = parse_proc_stat(kStatA), bb = parse_proc_stat(kStatB);
    CHECK(a.cores.size() == 3);  // aggregate + 2 cores
    auto usage = cpu_usage(a, bb);
    CHECK(usage.size() == 3);
    // aggregate: busy delta 1000 of total delta 2000 (iowait counts as idle) -> 50%
    CHECK(usage[0] == 50);
    CHECK(usage[1] == 100);  // cpu0: every non-idle/iowait jiffy is busy
    CHECK(usage[2] == 0);    // cpu1: idle-only growth
    CHECK(cpu_usage(CpuTimes{}, bb).empty());

    // ---- getprop ----
    PropList props = parse_getprop(kGetprop);
    CHECK(props.size() == 6);
    CHECK(prop(props, "ro.soc.model") == "Tensor G5");
    CHECK(prop(props, "gsm.sim.state") == "ABSENT,NOT_READY");
    CHECK(prop(props, "no.such.key") == "");

    // ---- sections / formatters ----
    auto secs = split_sections("a\nb\n---SEP---\nc\n---SEP---\n---SEP---\nd");
    CHECK(secs.size() == 4);
    CHECK(secs[0] == "a\nb");
    CHECK(secs[1] == "c");
    CHECK(secs[2] == "");
    CHECK(secs[3] == "d");

    CHECK(format_kb(114582612) == "109.3 GB");
    CHECK(format_kb(524288) == "512 MB");
    CHECK(format_temp_dC(303) == "30.3°C");
    CHECK(format_uptime("160807.27 1244234.65") == "1d 20h 40m");
    CHECK(format_uptime("3672.5 100.0") == "1h 1m");
    CHECK(format_uptime("") == "");
    CHECK(short_kernel(kVersion) == "6.6.102-android15-8-g6eb5b2a8c46b-ab14739656-4k");
    CHECK(first_line("  Pixel 10 \nrest") == "Pixel 10");

    std::printf("OK (%d checks)\n", g_checks);
    return 0;
}
