# 通信仕様 — Tab5 ⇄ tab5adb-agent

Tab5（ESP32-P4。組み込み ADB ホスト）と Android 側の `tab5adb-agent`（app_process で起動する
サーバ）の間で交わす通信の契約。**この文書を単一の情報源 (SoT) とし、Tab5 側（`embedded_adb`/
`adb` + app）と agent 側はこれに従う。** プロトコルを変更するときは、まずこの文書を更新してから
実装する。

- すべての多バイト値は **リトルエンディアン (LE)**（ESP32-P4 / Android-ARM とも LE ネイティブ）。
- 文中の `u8` `u16` `u32` は符号なし整数。`i16` 等は符号付き。
- バージョン: **proto_version = 1**。完全一致のみ許可（§4.4）。
- **役割の非対称性**: media（映像/音声）は **Android → Tab5 の単方向**。制御だけが双方向で、
  接続直後の **HELLO は agent（Android）が送り Tab5 が応える**（§4, §7）。mirror の開始は逆に
  **Tab5 が `MIRROR_START` を送り agent が応える**（§4）。
- **HELLO の役割は agent_link レイヤー接続の確立だけ**: proto 版・agent バージョン・capability の
  確認に限る。mirror など個別機能のパラメータは扱わず、それらは各機能の開始メッセージ
  （`MIRROR_START` 等）が運ぶ。agent_link は mirror 専用ではなく Tab5⇄Android の汎用リンク。

> **ステータス。** フレーム層（§3）・HELLO + `MIRROR_START`（§4）・映像（§5）はこの版で確定。
> 音声（§6）は **枠だけ**（`MIRROR_START` の `AUDIO` ビット＋ AUDIO フレームを将来追記）。実機で
> 詰める数値（`max_payload`/`SPLIT_COUNT` の最適値など）は §10。

---

## 1. レイヤー構成

```
 アプリ        | 制御メッセージ / 映像(JPEG) / 音声         ← §4, §5, §6
 パケット層    | MAGIC + 8B ヘッダ + payload               ← §3
 トランスポート | ADB stream (localabstract:tab5adb-agent)  ← §2
```

トランスポートは **ADB のストリーム** = 信頼性・順序保証つきの双方向バイトストリーム。USB 接続では
USB bulk のリンク層 CRC/再送 + ADB のメッセージ整合の上に、WiFi 接続（`adb tcpip`）では TCP の上に
乗る。いずれも信頼性・順序保証があるので、本プロトコルは **アプリレベルの再送(ARQ)も payload CRC も
持たない**。フレーム境界は LENGTH から決定的に復元でき、`MAGIC` は同期確認のサニティマーカとして
残す（§8）。

---

## 2. トランスポート層 — ADB ストリーム

### 2.1 ソケットと接続の向き

- agent は app_process 起動後、**abstract socket `localabstract:tab5adb-agent`** で待ち受ける
  （`android.net.LocalServerSocket`）。
- Tab5 は組み込み ADB ホストから **`localabstract:tab5adb-agent` サービスへストリームを開いて**
  接続する（agent が listen・Tab5 が connect する **forward** 方式）。
- 1 本のストリームを **TYPE フィールドで多重化**する（制御 + 映像 +（将来）音声）。別ストリームは張らない。

### 2.2 起動・接続シーケンス

ユーザ操作（mirror 開始）を起点に:

1. Tab5 が agent の dex を `sync:` で `/data/local/tmp` へ push（ファーム埋め込みの dex を展開して送る）。
2. Tab5 が `shell:`/`exec` で `CLASSPATH=… app_process / com.tab5adb.agent.Server` を起動。
3. Tab5 が `localabstract:tab5adb-agent` へストリームを開く（agent が listen するまで短間隔でリトライ）。
4. 接続確立後、**agent が `HELLO`（CONTROL_REQUEST）を送る**（proto 版・agent バージョン・capability。§4.4）。
5. Tab5 が `HELLO`（CONTROL_RESPONSE）を返す（互換確認、`max_payload`・capability の通知）。
   → ここで **agent_link 接続が確立**（`READY`）。
6. mirror を始めるとき、**Tab5 が `MIRROR_START`（CONTROL_REQUEST）を送る**（パネル寸法・スケール
   モード・開始ストリーム。§4.4）。
7. agent が `MIRROR_START`（CONTROL_RESPONSE）を返し（ソース寸法・コーデック）、以降 `JPEG`
   フレームを流し始める（§5）。

切断（ストリーム CLSE / agent 終了 / USB 抜け / WiFi 途絶）で `IDLE` に戻り、必要なら本シーケンスで
再接続する。今は mirror がリンクを張る唯一の用途なので 1 と 6 は連続するが、HELLO（リンク確立）と
`MIRROR_START`（機能開始）は別ステップとして分離している。

---

## 3. パケット（フレーム）フォーマット

ストリーム上の 1 パケット = **フレーム**。固定 8 バイトヘッダ + payload。

```
 offset  size  field
 0       1     MAGIC    = 0xA5（同期確認用のサニティマーカ）
 1       1     TYPE     フレーム種別（§3.1）
 2       1     FLAGS    ビットフラグ（§3.2）
 3       1     SEQ      送信方向ごとのフレームカウンタ (mod 256)。診断/欠落検出用
 4       4     LENGTH   payload のバイト数 (LE, u32)。0..max_payload
 8       N     PAYLOAD  N = LENGTH
```

- **オーバーヘッド = 8 バイト/フレーム**。payload 先頭が 4 バイト境界に揃う（P4 の JPEG 読み出しに有利）。
- `max_payload`: Tab5 が受理できる payload 最大長。HELLO 応答（§4.4）で通知する。agent は **JPEG の
  1 ブロック payload をこの値以下に収める**。
- `SEQ` はその方向で 1 フレーム送るごとに +1。信頼ストリームなので欠落は基本起きないが、実装バグ・
  バッファ破壊の検出に使う。要求/応答の対応付けは `SEQ` ではなく payload の `req_id`（§4）で行う。
- **payload CRC は持たない**（§1）。

### 3.1 TYPE

| 値 | 名前 | 方向 | payload |
|---|---|---|---|
| `0x01` | `CONTROL_REQUEST`  | 双方向（HELLO は A→T） | §4 |
| `0x02` | `CONTROL_RESPONSE` | 双方向 | §4 |
| `0x03` | `EVENT`            | 双方向（非同期通知。予約） | §4 |
| `0x10` | `JPEG`             | Android→Tab5 | §5（映像フレームのブロック） |
| `0x11` | `AUDIO`            | Android→Tab5 | §6（音声。**予約・枠のみ**） |

`0x04..0x0F` は制御系の予約、`0x12..0x1F` は media 系の予約。その他は受信側で破棄（前方互換）。
（A=Android/agent, T=Tab5）

### 3.2 FLAGS（ビット）

| bit | 名前 | 適用 TYPE | 意味 |
|---|---|---|---|
| 0 | `FRAME_START` | JPEG/AUDIO | このフレームが 1 映像/音声フレームの先頭ブロック |
| 1 | `FRAME_END`   | JPEG/AUDIO | このフレームが末尾ブロック。**Tab5 はこのブロックのデコード完了後に framebuffer を切り替える**（§5） |
| 2-7 | 予約 | — | 0 を送る／受信側は無視 |

1 ブロックで完結する映像フレームは `FRAME_START|FRAME_END` を両方立てる。

---

## 4. 制御メッセージ（枠組みと共通規約）

要求/応答モデル。**どちらの側からでも要求を出せる**（応答側が `req_id` をエコーして対応付ける）。
v1 で実際に使うのは agent 発の `HELLO`（リンク確立）と Tab5 発の `MIRROR_START`（mirror 開始）。
その他の Tab5→agent 制御（品質変更・スケールモード切替・mirror 停止など）は §4.4 のレジストリへ
追記して定義する。

### 4.1 CONTROL_REQUEST payload（共通規約）

```
 0  u8   cmd      コマンド（§4.4）
 1  u8   req_id   応答でエコーされ要求と応答を対応付ける (0..255 循環)
 2  ...  args     cmd ごとに定義（§4.4）
```

### 4.2 CONTROL_RESPONSE payload（共通規約）

```
 0  u8   cmd      対応する要求の cmd をエコー
 1  u8   req_id   対応する要求の req_id をエコー
 2  u8   status   ステータス（§4.5）。0 = OK
 3  ...  result   cmd ごとに定義（status=OK のときのみ有効）
```

### 4.3 EVENT payload（共通規約・予約）

```
 0  u8   event    イベント種別（§4.4）
 1  ...  data     event ごとに定義
```

### 4.4 メッセージレジストリ（随時追記）

枠（番号と役割）だけ確保する。payload の具体フィールドは実装時にここへ追記して確定する。

**コマンド**

| cmd | 名前 | 発信 | 役割 | payload 詳細 |
|---|---|---|---|---|
| `0x01` | `HELLO` | A→T | **agent_link 接続確立**。proto 版・agent バージョン・capability の確認のみ | 下記 |
| `0x10` | `MIRROR_START`     | T→A | **mirror 開始**（映像＋音声。v1 は映像のみ）。パネル寸法/スケールモード/開始ストリームを運ぶ | 下記 |
| `0x11` | `MIRROR_STOP`      | T→A | **予約**: mirror 停止（映像＋音声） | 未定 |
| `0x12` | `MIRROR_SET_PARAM` | T→A | **予約**: スケールモード/品質/分割数等のライブ変更 | 未定 |

予約: `0x02..0x0F` 制御一般 / `0x13..0x1F` mirror 制御 / `0x20..` 拡張。

> v1 のフロー: **HELLO でリンクを確立 → Tab5 の `MIRROR_START` で mirror 開始 → agent が JPEG
> ストリームを流す**。`MIRROR_START` は映像と音声の **両方**を開始するメッセージ（運ぶストリームは
> `streams` ビットマスクで選ぶ。v1 は映像のみ）。音声フィールドは実装時に末尾へ append する。
> 「映像と並行して制御を処理できる」よう、Tab5/agent とも受信ループは TYPE で振り分け、JPEG の流量
> で制御がブロックされない実装にする。

**イベント**（予約）

| event | 名前 | 役割 | data 詳細 |
|---|---|---|---|
| `0x01` | `ERROR` | エラー通知 | 未定 |
| `0x02` | `STREAM_STOPPED` | ストリーム停止通知 | 未定 |

予約: `0x03..0x0F` 一般 / `0x10..` 拡張。

#### HELLO (cmd = 0x01) — agent_link 接続確立

接続直後に **agent が CONTROL_REQUEST として送る**。**proto 版・agent バージョン・capability の確認のみ**
で、mirror など個別機能のパラメータは扱わない（それらは各機能の開始メッセージが運ぶ）。状態を変えない。

**要求 (CONTROL_REQUEST) args**（agent → Tab5）:

```
 +0   u8      proto_version       プロトコル版（現行 = 1）
 +1   u8      agent_version_major
 +2   u8      agent_version_minor
 +3   u8      agent_version_patch
 +4   u16     capabilities        agent が提供できる機能のビットマスク (LE)。§4.6
 +6   u16     reserved            0
 (= 8 bytes。将来は末尾に append-only)
```

**応答 (CONTROL_RESPONSE) result**（Tab5 → agent, `status = OK`）:

```
 +0   u8      proto_version       Tab5 のプロトコル版。agent が完全一致を確認
 +1   u8      reserved            0
 +2   u16     capabilities        Tab5 が受理できる機能のビットマスク (LE)。§4.6
 +4   u32     max_payload         Tab5 が受理できる payload 最大長 (LE)。agent はフレーム payload をこれ以下に収める
 (= 8 bytes。将来は末尾に append-only)
```

- 双方とも `proto_version` の **完全一致**を確認してから使う。jar は Tab5 が保持し通信前に転送するため
  不一致は基本起こらないが、念のため不一致なら Tab5 は `status = ENOTSUP` を返しストリームを閉じる。
- `max_payload` はリンク層（§3）の容量なので mirror に限らず全 TYPE に効く。`capabilities` は両者の
  AND が実際に使える機能集合（§4.6）。
- 将来フィールドを足すときは **末尾に追記**（append-only）。

#### MIRROR_START (cmd = 0x10) — mirror 開始

HELLO で agent_link が確立した後、**Tab5 が CONTROL_REQUEST として送る**。映像（＋将来は音声）の
mirror を開始させ、表示パラメータ（パネル寸法・スケールモード）を agent に渡す。agent は応答を返した
**後に** ストリーミングを始める。

**要求 (CONTROL_REQUEST) args**（Tab5 → agent）:

```
 +0   u16     target_width        Tab5 パネル幅 [px] (LE)。= 720
 +2   u16     target_height       Tab5 パネル高さ [px] (LE)。= 1280
 +4   u8      scale_mode          スケールモード。0=fit（既定） / 1=fill（§5.3）
 +5   u8      streams             開始するストリームのビットマスク（§4.6 と同じ bit 割当）。v1 = 0x01（VIDEO のみ）
 +6   u16     reserved            0
 (= 8 bytes。将来は末尾に append-only)
```

**応答 (CONTROL_RESPONSE) result**（agent → Tab5, `status = OK`）:

```
 +0   u16     source_width        ソース画面の物理幅 [px] (LE)。情報用
 +2   u16     source_height       ソース画面の物理高さ [px] (LE)。情報用
 +4   u8      video_codec         0x01=JPEG(YUV420)（現行）。以降は予約
 +5   u8      reserved            0
 +6   u16     reserved            0
 (= 8 bytes。将来は末尾に append-only)
```

- `streams` に立っているが agent が提供できない（HELLO の `capabilities` に無い）ビットがあれば、agent は
  `status = ENOTSUP` を返す。提供可能なビットだけを開始してもよい（運用は実装で確定）。
- `status = OK` の応答を返した後、agent は §5 の JPEG ストリームを流し始める。
- 将来フィールド（音声パラメータ等）を足すときは **末尾に追記**（append-only）。

### 4.6 capability ビット（共通）

機能の有無を表すビットマスク。HELLO で双方が広告し（agent=提供可能 / Tab5=受理可能）、`MIRROR_START`
の `streams` も同じ bit 割当を使う。

| bit | 名前 | 意味 |
|---|---|---|
| 0 | `VIDEO` | 映像 mirror（JPEG ストリップ。§5） |
| 1 | `AUDIO` | 音声 mirror（§6。**予約**） |
| 2-15 | 予約 | 0 |

v1 は agent・Tab5 とも `VIDEO` を立てる（`AUDIO` は将来）。両者の `capabilities` の **AND** が利用可能な
機能集合。

### 4.5 ステータスコード（共通の基準値・拡張可）

| 値 | 名前 | 意味 |
|---|---|---|
| `0x00` | `OK` | 成功 |
| `0x01` | `EINVAL` | 引数不正 |
| `0x02` | `ENOTSUP` | 非対応（proto 不一致含む） |
| `0x03` | `EBUSY` | リソース使用中 |
| `0x04` | `ESTATE` | 現在の状態で不可 |
| `0x05` | `ERANGE` | 値域外 |
| `0xFF` | `EFAIL` | その他の失敗 |

---

## 5. 映像ストリーム（JPEG）

`TYPE = JPEG`(0x10) のフレームで、画面 1 フレームを **横方向のストリップ（水平バンド）に分割**して
Android→Tab5 へ送る。各ブロックは「Tab5 画面内の矩形領域（座標・サイズ）」と「その領域を JPEG 圧縮
したデータ」を持つ。v1 では **毎フレーム画面全体**を送る。agent は **Tab5 の `MIRROR_START`（§4）を
受けてから**このストリームを開始する（`target_width`/`target_height`/`scale_mode` はその要求が運ぶ）。

### 5.1 agent 側のパイプライン（回転 → スケール → ストリップ → JPEG）

agent は Android 画面を取り込み、Tab5 の 720×1280 パネルへ表示する向き・大きさに整えてから送る。

1. **回転（物理向き基準）**: ソース画面の **物理的な向き**で決める（論理的な画面回転は考慮しない。
   Android 端末の物理画面方向と Tab5 の物理描画方向は固定）:
   - **縦長**（`source_height ≥ source_width`）: 回転しない。
   - **横長**（`source_width > source_height`）: **270° 回転**して表示する。
   - 回転後の画像は **常に縦長**になる（Tab5 の縦長パネルにできるだけ大きく表示する向き）。
2. **スケール（§5.3 のモード）**: 回転後の画像を Tab5 パネル（`target_width`×`target_height` =
   720×1280）へスケールする。**スケールは agent 側で行う。** 出力寸法は 16 の倍数に丸める。
3. **ストリップ分割**: スケール後の（必ず縦長の）画像を **`SPLIT_COUNT` 本の水平バンド**に分割する。
   **各ストリップの高さは 16 の倍数**。
4. **JPEG 符号化**: 各ストリップを **YUV420（4:2:0）・品質 60 固定**で JPEG 圧縮し、1 フレーム（§3）
   として順に送る（先頭 `FRAME_START`、末尾 `FRAME_END`）。

- **`SPLIT_COUNT` = 4**（既定。調整可能。§5.3）。
- **JPEG 品質 = 60 固定**（当面）。
- **色形式 = YUV420（4:2:0）**。

> **実装メモ（ワイヤ契約は不変）**: 上の 1〜2（回転・スケール・レターボックス）は**実画面キャプチャでは
> GPU にオフロード**する。`SurfaceControl.setDisplayProjection`（回転コード + パネルサイズ
> `ImageReader` 内の中央寄せ矩形）で SurfaceFlinger が回転・スケール・**黒レターボックス**まで一括で
> コンポジットするので、agent は CPU でフルフレームの読み戻し/回転/スケール/合成コピーを行わない
> （`Projection`/`ScreenCapture`）。よって出力は常に **720×1280 フル**で、各ストリップは `x=0, w=720`
> （§5.2 のとおり）。`--test-pattern` だけは SurfaceFlinger が無いので 1〜4 を CPU（`FramePipeline`）
> で行う。生成手段の差であり、送出されるフレーム/ストリップの形は同じ。

### 5.2 JPEG payload

```
 +0   u16   x        ブロック左上 X [px] (LE)。16 の倍数
 +2   u16   y        ブロック左上 Y [px] (LE)。16 の倍数
 +4   u16   w        ブロック幅 [px] (LE)。16 の倍数
 +6   u16   h        ブロック高さ [px] (LE)。16 の倍数
 +8   ...   jpeg     この w×h 領域を JPEG 圧縮（YUV420, 品質 60）したデータ
```

- 座標は Tab5 パネル（720×1280）上のデバイス座標。横ストリップなので 1 フレーム内で `x`・`w` は一定、
  `y`/`h` がバンド位置（fit モードでは画像が中央寄せされ `x`>0・`w`<720 になり得る。§5.3）。
- **座標・サイズは 16px の倍数**（YUV420 の MCU 整列、かつ HW JPEG デコード/AXI バーストの都合）。
- 1 ブロックの **payload 全体（8B サブヘッダ + jpeg）は `max_payload` 以下**（§3, §4.4）。
- **末尾ブロックは FLAGS の `FRAME_END`、先頭ブロックは `FRAME_START`**（最終フラグはフレーム層に一元化）。

### 5.3 スケールモードと分割の方針

- **分割の主目的 = HW JPEG コーデック負荷の時間分散**: 大きな JPEG を 1 回で HW JPEG コーデックに
  かけると **AXI バスを長時間占有**し、音声処理など一部のリアルタイム処理に影響が出る。これを避ける
  ため、1 フレームを複数ストリップに分けて **デコードを時間方向に分散**させる。`SPLIT_COUNT` はこの
  分散粒度のノブで、**既定 4・調整可能**。負荷分散と転送効率（`max_payload`）の兼ね合いで実機調整する。
  Tab5 はバンド数を事前に知る必要はない（各ブロックの座標とフレーム層の `FRAME_END` で境界が決まる）。
  当面は agent 側の設定値。将来 Tab5 主導で変えるなら `MIRROR_SET_PARAM`（§4.4 予約）。
- **スケールモード**（初期値は `MIRROR_START` の `scale_mode`。将来 `MIRROR_SET_PARAM` でライブ切替）:
  - **fit（既定）**: 画面全体が収まるよう **アスペクト比を保って** 720×1280 に内接させる。縦横比が
    パネルと異なると上下または左右に余白（レターボックス）が出る＝**送られる画像は 720×1280 より
    小さくなり得る**。中央寄せの分だけ `x`/`y` にオフセットが乗る。余白は静的な黒で、Tab5 が初期化時
    /モード変更時に塗る（毎フレーム画像領域は全面書き換わるのでダブルバッファのまま整合する）。
  - **fill**: 縦横で同じ倍率を保ったまま **パネル全体を埋める**ようスケールし、はみ出しはクロップする。
    **fill モードの画像は必ず 720×1280**（余白なし）。
- **更新モデル = 毎フレーム全画面**: 画像領域は毎フレーム全面が書き換わるので **前フレームの
  front→back コピーは不要**。**ブロック単位のレンダリング（領域指定デコード）は実装する**。
- **部分更新（dirty-rect）= 今後**: 変化バンドだけ送れば帯域削減できるが、ダブルバッファだと swap 後の
  バックバッファが 2 フレーム前になるため front→back コピー（またはトリプルバッファ）が要る。v1 では
  採用しない。サブヘッダの座標・サイズは両モード共通なので、ワイヤ形式を変えずに後付けできる。

### 5.4 受信側（Tab5）の処理

1. ブロックが届き次第、その JPEG を **バックバッファの (x,y,w,h) 領域へデコード**する（P4 のハード
   JPEG デコーダ）。次のブロックは並行受信できる。
2. **`FRAME_END` のブロックのデコード完了後に framebuffer を切り替える**（バック⇄フロント）。
3. fit モードの余白は静的な黒として保持する（§5.3）。

---

## 6. 音声ストリーム（AUDIO）— 枠のみ・予約

`TYPE = AUDIO`(0x11) のフレームで音声（PCM またはエンコード済み）を Android→Tab5 へ運ぶ予定。
**本版では構造を確定しない**（拡張の余地だけ確保）。実装時に決めるもの:

- パラメータ（sample_rate / channels / フォーマット / 圧縮）は **`MIRROR_START`（§4.4）の末尾に
  append したフィールドが運ぶ**（`streams` の `AUDIO` ビットで開始を選択）。
- AUDIO payload のサブヘッダ（タイムスタンプ/通し番号など）と本体形式。
- ストリーム境界は §3.2 の `FRAME_START`/`FRAME_END` を流用。

映像と同じ 1 本のストリーム上を `TYPE` で多重化するので、音声追加でフレーム層（§3）は変えない。

---

## 7. 状態遷移

```
            connect + HELLO(ok)        MIRROR_START(ok)
  IDLE ──────────────────────────▶ READY ─────────────────▶ STREAMING(JPEG[, AUDIO])
   ▲                                 │                              │
   └─────────────────────────────────┴──────────────────────────────┘
              ストリーム切断 / agent 終了 / USB 抜け / WiFi 途絶
```

- `IDLE`: 未接続、または接続直後 HELLO 前。
- `READY`: HELLO 確立後（agent_link 接続済み、mirror 未開始）。制御メッセージのやりとりは可能。
- `STREAMING`: `MIRROR_START` 後。agent が JPEG（将来は AUDIO も）を流す。
- 切断で `IDLE` に戻り、§2.2 のシーケンスで再接続する。`MIRROR_STOP`（§4.4 予約）で `READY` に戻る
  経路は将来追加する。

---

## 8. エラー処理 / 信頼性

- **トランスポートは信頼性・順序保証つき**（USB は ADB stream over bulk、WiFi は TCP）。よって
  アプリ層の再送・payload CRC は持たない（§1）。
- **MAGIC 不一致**: 信頼ストリームでは本来起きない＝実装バグ/破壊の兆候。受信側はストリームを
  **異常終了して再接続**する（読み飛ばし再同期はしない）。
- **制御のタイムアウト**: 要求側は応答を既定 **1000 ms** 待つ。超過は冪等コマンドのみ再送可
  （`HELLO` は冪等）。
- **未知の TYPE / cmd / event**: 受信側は無視（前方互換）。
- **映像が間に合わない**: 受信側（Tab5）が捌けない分はフレーム単位で捨てる（最新フレーム優先）。
  agent は壁時計ペースで送る。

---

## 9. 例（フレーミングのみ）

HELLO 要求フレーム（`TYPE=CONTROL_REQUEST`, `SEQ=0`, payload = `cmd=0x01, req_id=0x01` + HELLO args）:

```
A5 01 00 00 0A 00 00 00  01 01  01 01 00 00  01 00  00 00
│  │  │  │  └──────────┴ LENGTH=10 (LE u32)
│  │  │  └ SEQ=0
│  │  └ FLAGS=0
│  └ TYPE=CONTROL_REQUEST(0x01)
└ MAGIC
  payload: cmd=01 req_id=01 | proto=01 ver=1.0.0 | caps=0x0001(VIDEO) | rsv=0
```

MIRROR_START 要求フレーム（`TYPE=CONTROL_REQUEST`, `SEQ=1`, payload = `cmd=0x10, req_id=0x02` + args）:

```
A5 01 00 01 0A 00 00 00  10 02  D0 02  00 05  00 01  00 00
│  │  │  │  └──────────┴ LENGTH=10 (LE u32)
│  │  │  └ SEQ=1
│  │  └ FLAGS=0
│  └ TYPE=CONTROL_REQUEST(0x01)
└ MAGIC
  payload: cmd=10 req_id=02 | target_w=0x02D0(720) target_h=0x0500(1280) | scale=0(fit) streams=0x01(VIDEO) | rsv=0
```

JPEG ストリップフレーム（先頭バンド = `FRAME_START`, `SEQ=7`, jpeg=N バイト, fill モード例）:

```
A5 10 01 07 <LEN u32 LE> 00 00 00 00 D0 02 40 01 <jpeg…>
│  │  │  │               └ x=0 y=0 w=0x02D0(720) h=0x0140(320)
│  │  │  └ SEQ=7
│  │  └ FLAGS=0x01 (FRAME_START)
│  └ TYPE=JPEG(0x10)
└ MAGIC
  LENGTH = 8(サブヘッダ) + N(jpeg)。末尾バンドは FLAGS=0x02 (FRAME_END)
```

---

## 10. 確定待ち (Open questions)

- **`max_payload` 既定値**（§3, §4.4）: **暫定 256 KiB（262144）**。`SPLIT_COUNT=4`・720×1280・
  YUV420・品質 60 なら 1 ストリップ（720×320）はこれに十分収まる。JPEG はなるべく速く送りたいので
  大きめに取る方針。Tab5 のストリップ受信バッファ（PSRAM）と速度で実機調整する。
- **`SPLIT_COUNT` の最適値**（§5.3）: 既定 4。負荷分散と転送効率の兼ね合いで実機調整。
- **JPEG の細部**: HW JPEG デコーダの制約（最小タイルサイズ・整列。16 整列は §5.2 で前提化）と
  YUV420 サブサンプルの相性を実機確認。
- 音声（§6）の全フィールド（`MIRROR_START` の `AUDIO` 拡張定義時）。
