# jpeg_decode_enhanced

ESP32-P4 のハードウェア JPEG デコーダを拡張する ESP-IDF コンポーネント。
IDF 標準の `jpeg_decoder_process()` を置き換え、次の 2 つを提供する。

- **Layer 1: ストリップ分割デコード** (`jpeg_decode_enhanced.h`) —
  デコード結果を全画面バッファではなく「ストリップ」(数 MCU 行ぶんの帯) の
  リングバッファへ 2D-DMA で直接書き込み、1 ストリップ完了ごとにコールバックで
  下流へ渡す。中間バッファが小さくなり、Internal SRAM に置けば
  PSRAM 帯域のボトルネックを回避できる。フルレンジ YUV→RGB 変換にも対応。
- **Layer 2: PPA パイプライン** (`jpeg_ppa_pipeline.h`) — Layer 1 の
  ストリップを PPA SRM (scale / rotate / mirror) へ流し込み、デコードと
  並行して出力フレームバッファへ合成する高レベル API。

全 API は C 言語 (opaque handle + `esp_err_t`)。

## なぜ存在するか

JPEG コーデック単体は 1280×720 を 60fps でデコードできるが、IDF 標準の

```
jpeg_decoder_process → PSRAM → PPA SRM → PSRAM フレームバッファ
```

という経路では PSRAM 帯域が律速になり実測 ~20fps で頭打ちになる。
中間ラスタを Internal SRAM のストリップリングに置き、JPEG デコードと
PPA 処理をストリップ単位でパイプライン化すると PSRAM の読み戻しが消え、
カメラ律速の 30fps に戻る (M5Stack Tab5 + UVC カメラで実測)。

```
JPEG codec ──TX: PSRAM 上の JPEG ストリーム──┐
                                             ▼
                            2D-DMA RX (ストリップごとの連結ディスクリプタ)
                                             │
                                             ▼
                       ストリップリング (ring_count 本, SRAM or PSRAM)
                                             │
                                             ▼  on_strip_done (ISR) → queue
                                       PPA worker task
                                             │  ppa_do_scale_rotate_mirror (blocking)
                                             ▼
                                  出力フレームバッファ (PSRAM)
```

## 使い方

### Layer 2: JPEG → 変換 → フレームバッファ (一番よく使う形)

```c
#include "jpeg_ppa_pipeline.h"

jpeg_ppa_pipeline_cfg_t cfg = {
    .max_pic_w        = 1280,                      // 受け付ける最大サイズ
    .max_pic_h        = 720,
    .strip_h_hint     = 16,                        // ストリップ高さの希望値
    .ring_count       = 5,                         // リング段数
    .strip_color_mode = PPA_SRM_COLOR_MODE_RGB888, // 中間フォーマット
    .rgb_order        = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    .conv_std         = JPEG_YUV_RGB_CONV_STD_BT601,
    .yuv_full_range   = true,                      // MJPEG はフルレンジ
    .worker_core      = 0,                         // -1 = affinity なし
};
jpeg_ppa_pipeline_handle_t pipe;
ESP_ERROR_CHECK(jpeg_ppa_pipeline_new(&cfg, &pipe));

// 変換パラメータは毎フレーム変えられる
jpeg_ppa_transform_t t = {
    .rotation = PPA_SRM_ROTATION_ANGLE_90,
    .scale_x  = 1.0f,
    .scale_y  = 1.0f,
    // .mirror_x / .mirror_y / .rgb_swap / .byte_swap
    // .in_crop = {x, y, w, h}   入力空間クロップ (w/h=0 で全体)
    // .out_offset_x / .out_offset_y  出力先の左上座標
};
jpeg_ppa_output_t out = {
    .buffer     = framebuffer,
    .pic_w      = 720,
    .pic_h      = 1280,
    .color_mode = PPA_SRM_COLOR_MODE_RGB565,
};
ESP_ERROR_CHECK(jpeg_ppa_pipeline_process(pipe, jpeg, jpeg_size, &out, &t, NULL));
// 戻った時点で全ストリップの PPA 書き込みまで完了している
```

リソース (リングバッファ・JPEG エンジン・PPA クライアント・worker task) は
`new` 時に固定。**回転・スケール・ミラー・クロップ・出力位置は
`process()` ごとに自由に変えられる**。入力 JPEG のサイズも
`max_pic_w/h` 以内なら毎フレーム異なってよい (ヘッダから自動検出)。

### Layer 1: ストリップを自分で消費する

```c
#include "jpeg_decode_enhanced.h"

static bool on_strip(const jpeg_enh_strip_event_t *e, void *ctx) {
    // ISR コンテキスト。e->buffer に e->rows 行ぶんの画素が入っている。
    // queue 等で task に渡し、処理が終わったら必ず
    // jpeg_enh_strip_decoder_release_strip(dec, e->strip_idx) を呼ぶこと。
    return false;
}

jpeg_enh_strip_decoder_cfg_t cfg = {
    .decode = {
        .output_format  = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order      = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std       = JPEG_YUV_RGB_CONV_STD_BT601,
        .yuv_full_range = true,
    },
    .max_pic_w     = 1280,
    .max_pic_h     = 720,
    .strip_h_hint  = 16,
    .ring_count    = 4,
    .on_strip_done = on_strip,
};
jpeg_enh_strip_decoder_handle_t dec;
ESP_ERROR_CHECK(jpeg_enh_strip_decoder_new(&cfg, &dec));

jpeg_enh_frame_info_t info;
ESP_ERROR_CHECK(jpeg_enh_strip_decoder_process(dec, jpeg, jpeg_size, &info));
```

- `on_frame_start` (任意) はヘッダ解析直後・DMA 開始前に呼び出し元 task の
  コンテキストで呼ばれる。フレームごとの前準備や検証に使える
  (ESP_OK 以外を返すとそのフレームを中止)。
- ストリップの消費が終わるたびに `release_strip()` を呼ぶこと。これが
  リング再利用のバックプレッシャーそのもの (下の内部構造を参照)。

### 全画面一発デコード (PPA なし、フルレンジだけ欲しいとき)

```c
size_t buf_size;
void *buf = jpeg_alloc_decoder_mem(1280 * 720 * 2,
        &(jpeg_decode_memory_alloc_cfg_t){ .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER },
        &buf_size);

jpeg_enh_frame_info_t info;
ESP_ERROR_CHECK(jpeg_enh_decoder_process(dec, jpeg, jpeg_size, buf, buf_size, &info));
// IDF の jpeg_decoder_process() 相当 + full range 対応。
// ring_count = 0 で作った handle でも使える (ストリップ資源を確保しない)。
```

出力バッファは IDF の `jpeg_decoder_process()` と同じアライメント規則
(`jpeg_alloc_decoder_mem()` を使うのが確実)。PSRAM バッファのキャッシュ
無効化はコンポーネント側で行う。

## 設定リファレンス

### サイズ・ストリップ関連

| フィールド | 意味 |
|---|---|
| `max_pic_w/h` | 受け付ける最大画像サイズ。リングバッファとディスクリプタ配列のサイズを決める。実フレームはこれ以内なら毎回違ってよい |
| `strip_h_hint` | ストリップ高さの希望値。フレームの MCU 高 (下表) の倍数に**切り下げ**られる (最低 1 MCU 行)。0 = 全画面 1 ストリップ |
| `ring_count` | リング段数。パイプライン効果には 2 以上、実用は 4–5 程度 |
| `strip_alloc_caps` | リングの heap caps。0 = `MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL` (高速)。`MALLOC_CAP_SPIRAM` で PSRAM 配置 (低速だが SRAM を食わない) |

MCU 高はサンプリングで決まる: YUV444 / YUV422 / GRAY = 8、YUV420 = 16。
`strip_h_hint = 16` ならどのサンプリングでもそのまま 16 行になる。

ストリップ境界が MCU 行境界に乗る制約は**ハードウェア由来で解消不可**
(2D-DMA の reorder がサンプリング依存のマクロブロック単位で動き、
1 MCU 行を 2 つのバッファに跨がせられない)。それ以外は柔軟:

- 画像高がストリップ高で割り切れなくてよい (最終ストリップが短くなる)
- 画像高が MCU 整列でなくてよい (パディング込みでデコードされ、
  strip event の `rows` (有効行) / `padded_rows` (実書き込み行) で区別。
  Layer 2 は有効行だけを PPA に渡すので自動でクロップされる)

### 色変換

| フィールド | 意味 |
|---|---|
| `conv_std` | BT601 / BT709 (YUV→RGB 変換規格) |
| `yuv_full_range` | **false** = IDF 標準の limited range 行列 (互換)。**true** = JFIF フルレンジ行列に差し替え。JPEG/MJPEG は事実上すべてフルレンジなので、画が眠い場合はまずこれを疑う |

フルレンジ行列は BT601 / BT709 両方を内蔵。実装は IDF の CSC 設定
(mux/scramble) をそのまま使い、2D-DMA の行列レジスタだけを上書きする方式。

### Layer 2 transform の制約

- PPA のスケールは **1/16 刻みに量子化**される (YUV420 出力時は 1/8 刻み)。
- 各ストリップは独立した PPA ブロックとして処理されるため、
  **ストリップ内部境界 × 量子化後 scale_y が整数**になる必要がある。
  `process()` がフレームごとに検証し、満たさない場合は
  `ESP_ERR_INVALID_ARG` で拒否する。`strip_h_hint` が 16 の倍数なら
  任意のスケールで自動的に満たされる (16 × n/16 = 整数)。
- 量子化の副作用として、スケール端数によっては出力の端に数 px の
  未描画帯が出る (例: 600 行 × 1.2 → 量子化 1.1875 → 712 行 ≠ 720)。
- `mirror_x/y` の配置計算は「PPA のミラーは出力空間で作用する」前提。
  実機未検証 (要確認) なので、初使用時は向きを確認すること。

## PSRAM リングとキャッシュ

`strip_alloc_caps = MALLOC_CAP_SPIRAM` にした場合のキャッシュ整合性は
コンポーネント側で処理する:

- 確保直後に C2M + invalidate で calloc のダーティラインをパージ
  (これを怠ると PPA ドライバの入力 writeback が DMA 書き込みを上書きする)
- CPU でストリップを読む消費者は、読む前に
  `jpeg_enh_strip_decoder_sync_strip_for_cpu()` を呼ぶ (M2C invalidate)。
  Internal RAM リングでは no-op (ESP32-P4 の内部 SRAM はキャッシュ非対象)
- PPA など DMA 消費者はそのままでよい

なお PSRAM リングでは「PSRAM 書き→PSRAM 読み」の往復が復活するので
高速化効果はない。中間バッファ削減 (全画面 → リング数本ぶん) が目的の
ときに使う。

## エラーと診断

- **リングアンダーラン**: 消費側が `ring_count - 1` ストリップ以上遅れると
  DMA チェーンが途切れ、そのフレームは復旧不能 (チェーン終端は pause では
  なく transaction 終了)。`ESP_ERR_TIMEOUT` (約 200ms 後) になり、ログに
  `strip ring underrun after N/M strips` と出る。対処は `ring_count` を
  増やすか消費側を速くする。
- 壊れた JPEG (ヘッダ破損・サイズ超過など) は DMA 開始前に
  `ESP_ERR_INVALID_STATE` / `ESP_ERR_INVALID_SIZE` で弾く。
- Layer 2 はデコードエラー時に worker の残作業を精算してから返るので、
  次の `process()` はクリーンな状態から始まる。PPA 個別ストリップの
  エラーも `process()` の戻り値に伝播する。

## チューニング指針 (Tab5 1280×720 30fps での実測)

- `strip_h_hint = 16`, `ring_count = 5`, RGB888 中間で 30fps。
  リング使用量 = 5 × 16 行 × 1280 px × 3 B ≒ 300 KiB
  (Tab5 では `0x4FF40000` の 384 KiB SRAM プールにぎりぎり収まる)。
- **ストリップを小さくするのはタダではない**。高さ半減 = フレームあたりの
  PPA 呼び出し回数倍増で、呼び出しごとの検証 + cache msync オーバーヘッドに
  より実測 30fps → 24fps に落ちた (16 → 8, RGB888)。
- 必要リング段数の目安: `ring_count - 1 ≥ PPA ストリップ処理時間 / JPEG
  ストリップデコード時間`。

## 内部構造 (変更する人向け)

苦労して学んだ 2D-DMA の事実。これを知らずに触ると壊れる:

1. **owner ビットによるバックプレッシャーは JPEG-RX では機能しない**。
   `owner=CPU` + owner チェック有効は `RX_DSCR_ERROR` で transaction が
   終了する (pause しない)。
2. **チェーン終端 (`next=NULL`) も pause ではない**。`IN_DSCR_EMPTY` が立ち
   transaction が終了する。
3. 唯一機能するバックプレッシャーは **`dma2d_append()` による動的チェーン
   延長**。最初に `ring_count` 本だけリンクし、消費者が strip `i` を
   `release_strip()` したタイミングで descriptor `i + ring_count` を
   スプライスして append する。
4. **全ディスクリプタ `suc_eof=0`**。EOF は JPEG ハードのフレーム完了信号から
   ブリッジが生成する。途中の descriptor に `suc_eof=1` を立てると偽 EOF で
   dma2d ISR の `dma2d_ll_rx_is_fsm_idle` assert を踏む。
5. `on_recv_eof` では **engine の `evt_queue` に `JPEG_DMA2D_RX_EOF` を
   post する** (IDF の `jpeg_rx_eof` と同じ)。セマフォだけでは wait ループが
   解けず decode timeout になる。
6. `on_desc_done` の event data には descriptor アドレスが入らない。
   ストリップ番号は順序保証を前提にカウンタで追跡する。
7. **`on_desc_empty` を登録してはいけない**。登録すると DESC_EMPTY 割り込みが
   新規に有効化され、dma2d ドライバはこのビットでチャネル解放 + FSM idle
   assert まで行う。正常フレーム終端で raw ビットが立つかは未検証のため、
   有効化は全フレームを壊すリスクがある。アンダーラン検出はタイムアウト時に
   `isr_next_strip == chain_tail + 1` (リンク済み descriptor を全消費して
   未スプライスの所で停止) というシグネチャで事後診断している。
8. `release_strip()` のスプライス + append は `frame_active` フラグで
   ゲートしたスピンロック内で行う。エラーパスでは `dma2d_force_end` の前に
   ゲートを閉じるので、遅れてきた release が解放済み (他用途に再割当て
   されたかもしれない) チャネルを叩くことはない。正常フレームでは EOF 時点で
   全スプライトが完了済みなのでゲートの影響はない。

### IDF バージョン依存

`jpeg_private.h` (engine 構造体・ヘッダパーサ・ディスクリプタテーブル) と
`esp_private/dma2d.h` という **ESP-IDF の非公開ヘッダに依存**する。
構造体レイアウトが変わると黙って壊れるため、`jpeg_decode_enhanced.c` に
`ESP_IDF_VERSION` ガードがあり、検証済みは **v5.4.x のみ**。別バージョンへ
移行する際は private 構造体のレイアウトを照合してから
`JPEG_DECODE_ENHANCED_SKIP_IDF_VERSION_CHECK` を定義する。

`esp_driver_jpeg` をコンポーネントごと差し替える (override) 方式は
意図的に採っていない。
