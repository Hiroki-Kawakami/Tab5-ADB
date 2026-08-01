# 通信仕様 — Tab5 ⇄ tab5adb-agent

Tab5（ESP32-P4。組み込み ADB ホスト）と Android 側の `tab5adb-agent`（app_process で起動する
サーバ）の間で交わす通信の契約。**この文書を単一の情報源 (SoT) とし、Tab5 側（`adb` + app）と
agent 側はこれに従う。** プロトコルを変更するときは、まずこの文書を更新してから
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

> **ステータス。** フレーム層（§3）・HELLO + `MIRROR_START`（§4）・映像（§5）・音声（§6）は
> この版で確定。音声は **Tab5Only / PhoneOnly の2モード**（`MIRROR_START` の `streams` の `AUDIO`
> ビット ON/OFF で選択）、USB は生 PCM、ADB-over-TCP/Wi-Fi は Opus、**ターゲットは Android 12 以降**。
> 実機で詰める数値（`max_payload`/`SPLIT_COUNT` の最適値、音声チャンク長/リング深さなど）は §10。

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
再接続する。HELLO（リンク確立）と `MIRROR_START`（機能開始）は別ステップとして分離している。

> **Tab5 側の運用ノート（ワイヤ契約外）**: Tab5 は adb 接続の確立直後に 1〜5 を実行してリンクを
> eager に確立し、成功なら**通常モード**（mirror / 高速 preview / アプリ情報など agent 依存機能を
> 提供）、失敗なら**機能制限モード**（agent 非依存の機能のみ）で動作する。§2.2 の起点が
> 「ユーザ操作（mirror 開始）」から「adb 接続確立」に変わるだけで、シーケンス自体は同じ。

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
| `0x03` | `EVENT`            | 双方向（非同期通知。ORIENTATION=A→T） | §4 |
| `0x04` | `INPUT`            | Tab5→Android（一方向・応答なし） | §4.7（入力注入） |
| `0x10` | `JPEG`             | Android→Tab5 | §5（映像フレームのブロック） |
| `0x11` | `AUDIO`            | Android→Tab5 | §6（音声。PCM / Opus） |

`0x05..0x0F` は制御系の予約、`0x12..0x1F` は media 系の予約。その他は受信側で破棄（前方互換）。
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
| `0x11` | `MIRROR_STOP`      | T→A | **mirror 停止**（映像＋音声）。`STREAMING`→`READY`（リンクは維持） | 下記 |
| `0x12` | `MIRROR_SET_PARAM` | T→A | **予約**: スケールモード/品質/分割数等のライブ変更 | 未定 |
| `0x20` | `GET_APP_LIST`     | T→A | インストール済みアプリ一覧（pkg / ラベル / flags）。AppManager 用 | 下記 |
| `0x21` | `GET_APP_ICON`     | T→A | 1 アプリのアイコン（raw ARGB8888） | 下記 |
| `0x22` | `GET_MEDIA_INFO`   | T→A | 再生中メディアの状態スナップショット（state / content_token） | 下記 |
| `0x23` | `GET_MEDIA_RENDER` | T→A | アルバムアート（ARGB8888）＋タイトル/アーティストを agent 描画した文字画像（A8） | 下記 |
| `0x24` | `MEDIA_CONTROL`    | T→A | 再生操作（play-pause / next / previous） | 下記 |

予約: `0x02..0x0F` 制御一般 / `0x13..0x1F` mirror 制御 / `0x25..` 拡張。

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
| `0x03` | `ORIENTATION` | ソース端末の論理回転（向き）の通知 | 下記 |
| `0x04` | `MEDIA` | 再生中メディアの状態/トラック変化の通知（state / content_token） | 下記 |

予約: `0x05..0x0F` 一般 / `0x10..` 拡張。

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
 +0   u16     target_width        ビューア面の幅 [px] (LE)。全画面 mirror = 720
 +2   u16     target_height       ビューア面の高さ [px] (LE)。全画面 mirror = 1280
 +4   u8      scale_mode          スケールモード。0=fit（既定） / 1=fill / 2=aspect / 3=adapt（§5.3）
 +5   u8      streams             開始するストリームのビットマスク（§4.6 と同じ bit 割当）。映像のみ=0x01、
                                  Tab5Only 音声つき=0x03（VIDEO|AUDIO。§6）。AUDIO 単独は非対応（mirror は常に映像を伴う）
 +6   u16     reserved            0
 +8   u8      max_fps             フレームレート上限。0 = 無制限（agent 既定の上限のみ）。低レート用途
                                  （DeviceScreen の preview 等）で帯域とエンコード負荷をソースで絞る
 +9   u8      jpeg_quality        JPEG 品質 (1..100)。0 = agent 既定（現行 80）。preview は 60
 +10  u8      split_count         ストリップ分割数（§5.3）。0 = agent 既定（現行 4）。preview は 1
                                  （= フレーム全体を 1 JPEG で送る。転送前の分割をしない）
 +11  u8      reserved            0
 +12  u8      audio_codec         AUDIO の要求コーデック。0=agent既定（PCM） / 1=PCM_S16LE /
                                  2=Opus。USBは1、ADB-over-TCP/Wi-Fiは2を指定する
 +13  u8      reserved            0
 +14  u16     reserved            0
 (= 16 bytes。将来は末尾に append-only。+8 以降を欠く旧要求は 0 = 既定とみなす)
```

- `target_width`/`target_height` は **Tab5 パネルとは限らないビューア面のサイズ**（mirror 画面は
  パネル全面 720×1280、DeviceScreen の preview は 360×860 ボックス）。**`split_count` > 1 で使う
  ときは 16 の倍数**で指定する（ストリップの 16px 整列。§5.2）。`split_count` = 1 なら偶数で良い。

**応答 (CONTROL_RESPONSE) result**（agent → Tab5, `status = OK`）:

```
 +0   u16     source_width        ソース画面の物理幅 [px] (LE)。情報用
 +2   u16     source_height       ソース画面の物理高さ [px] (LE)。情報用
 +4   u8      video_codec         0x01=JPEG(YUV420)（現行）。以降は予約
 +5   u8      reserved            0
 +6   u16     reserved            0
 +8   u16     out_width           実際に流すフレームの幅 [px] (LE)。fit/fill = target_width、
                                  aspect = agent が決めたサイズ（§5.3）。受信側はこれでバッファを確保する
 +10  u16     out_height          実際に流すフレームの高さ [px] (LE)
 +12  u8      audio_codec         実際の音声コーデック。0x01=PCM_S16LE / 0x02=Opus（§6）。
                                  AUDIO を開始しないとき（streams に AUDIO 無し / agent が音声非対応）は +12 以降が不在
 +13  u8      audio_channels      音声チャンネル数（2=ステレオ）
 +14  u16     reserved            0
 +16  u32     audio_rate          サンプルレート [Hz] (LE)。48000
 (= 映像のみ 12 bytes / 音声つき 20 bytes。append-only。音声タイル(+12 以降)を欠く応答 = 音声なし)
```

- `streams` に立っているが agent が提供できない（HELLO の `capabilities` に無い）ビットがあれば、agent は
  `status = ENOTSUP` を返す。提供可能なビットだけを開始してもよい（運用は実装で確定）。
- `status = OK` の応答を返した後、agent は §5 の JPEG ストリームを流し始める。
- **`STREAMING` 中に再度 `MIRROR_START` を受けたら、現在のストリームを止めて新パラメータで貼り直す
  （in-place reconfigure）**。`MIRROR_STOP` は不要 — Tab5 はスケール／表示モードの切替を新しい
  `MIRROR_START` を 1 通送るだけで行う（stop+start の連打は agent 側で競合しうるので避ける）。
  例: mirror の **DispMode**（fit↔fill↔解像度調整）切替。
- 将来フィールド（音声パラメータ等）を足すときは **末尾に追記**（append-only）。

#### MIRROR_STOP (cmd = 0x11) — mirror 停止

`STREAMING` 中に **Tab5 が CONTROL_REQUEST として送る**。映像（＋将来は音声）の送出を止めさせ、
**`READY` に戻す**。**ストリーム（agent_link 接続）は閉じない** — Tab5 はこの後も同じリンクで
`MIRROR_START` を送れば mirror を再開でき、別機能の制御メッセージも引き続き使える。mirror 機能を
閉じても agent プロセス／リンクを生かしたまま帯域だけ止めるのが目的（Tab5 側 `AgentClient` の運用）。

**要求 (CONTROL_REQUEST) args**（Tab5 → agent）: **なし**（`cmd` + `req_id` のみ）。

**応答 (CONTROL_RESPONSE) result**（agent → Tab5）: **なし**（`status` のみ）。`status = OK` で停止受理。

- agent は MIRROR_STOP を受けたら送出ループを止め、応答を返して `READY` に戻る。停止後に届く可能性の
  ある「最後の JPEG フレーム」は Tab5 が破棄してよい（最新フレーム優先・§8）。
- 既に `READY`（mirror 未開始）で受けた場合も `status = OK`（冪等）。
- Tab5 は MIRROR_STOP の応答を待たずにローカルの受信を止めてよい（応答は確認用）。
- 映像と並行して制御を処理するため、agent は **送出ループと別に制御フレームを read** し、MIRROR_STOP
  を JPEG の流量にブロックされず受け取る（§4.4 のノート）。

#### GET_APP_LIST (cmd = 0x20) — アプリ一覧

**Tab5 が CONTROL_REQUEST として送る**。agent は `PackageManager` からインストール済みアプリの
一覧（パッケージ名・人間可読ラベル・属性）を返す。HELLO capability の `APPINFO`（§4.6）を
広告した agent のみ対応。状態（§7）は変えない。mirror の映像と同じリンクに乗るが、
要求/応答とも小さい（一覧全体で数十 KB）ので帯域への影響は無視できる。

**要求 (CONTROL_REQUEST) args**: **なし**（`cmd` + `req_id` のみ）。

**応答 (CONTROL_RESPONSE) result**（agent → Tab5, `status = OK`）:

```
 +0   u16     count               エントリ数 (LE)
 +2   ...     count 個の可変長エントリ（連続配置、各エントリは下記）:
   +0  u8     flags               bit0 = system アプリ / bit1 = disabled（ユーザー無効化）
   +1  u8     reserved            0
   +2  u8     pkg_len             パッケージ名のバイト長
   +3  u8     label_len           ラベルのバイト長（0 = ラベル不明、pkg を表示）
   +4  ...    pkg                 パッケージ名 (UTF-8, pkg_len bytes)
   +4+pkg_len ... label           ラベル (UTF-8, label_len bytes。255 バイトに収まるよう切り詰め)
```

- agent は**ラベルの大文字小文字無視順でソートして返す**（Tab5 側のソート省略）。
- 応答 payload 全体は `max_payload` 以下に収まること（数百アプリ × 数十 B で余裕）。
- この要求の応答待ちは §8 の既定 1000 ms ではなく **3000 ms**（数百アプリのラベル解決に時間がかかる）。

#### GET_APP_ICON (cmd = 0x21) — アプリアイコン

**Tab5 が CONTROL_REQUEST として送る**。agent は指定パッケージのランチャーアイコンを
`size_px`×`size_px` に描画し、**raw ARGB8888** で返す（Tab5 側に PNG デコーダを増やさない選択。
帯域が問題になったら圧縮形式を append する）。`APPINFO` capability が前提。

**要求 (CONTROL_REQUEST) args**（Tab5 → agent）:

```
 +0   u16     size_px             要求アイコン辺長 [px] (LE)。推奨 56〜96
 +2   u16     reserved            0
 +4   ...     package             パッケージ名 (UTF-8, payload の残り全部)
```

**応答 (CONTROL_RESPONSE) result**（agent → Tab5, `status = OK`）:

```
 +0   u16     width               実際の幅 [px] (LE)。= size_px
 +2   u16     height              実際の高さ [px] (LE)
 +4   u8      format              0x01 = ARGB8888（現行唯一）
 +5   u8      reserved            0
 +6   u16     reserved            0
 +8   ...     pixels              width*height 個の u32 (LE) = 0xAARRGGBB
                                  （メモリ順 B,G,R,A。Android の Color int / LVGL の
                                  ARGB8888 ネイティブ順と同一なので両端とも無変換）
```

- 不明パッケージ／描画失敗は `status = EINVAL`。
- 応答待ちは **3000 ms**（GET_APP_LIST と同じ理由）。

#### ORIENTATION (event = 0x03) — ソース端末の向き通知

mirror 中、**agent が EVENT として送る**。ソース端末（ディスプレイ 0）の**論理回転**を Tab5 に
知らせる。Tab5 はこれを使って **overlay UI のレイアウトを縦向き／横向きに切り替える**だけで、
**映像は変えない**（自然向きロック・§5.1 のとおり、映像は常に端末のナチュラル向き framebuffer）。
横長アプリは Tab5 上で横向きのまま表示され、ユーザが Tab5 本体を回して見るので、overlay も
それに合わせて回す。

**data**（agent → Tab5、§4.3 の `event` バイトに続く）:

```
 +0   u8    rotation    Surface.ROTATION_* (0/1/2/3 = 0°/90°/180°/270°)
 +1   u8    reserved    0
 +2   u16   reserved    0
 (= 4 bytes。将来は末尾に append-only)
```

- agent は **ストリーム開始時（最初のフレーム）に 1 回**と、以降 **回転が変わるたび**に送る
  （`ScreenCapture` を作り直すのと同じ契機）。
- `rotation` が **90/270（奇数）なら横向き**、**0/180 なら縦向き**。Tab5 は overlay を縦ストリップ
  ／横ストリップに切り替える。受信前の既定は縦向き。

#### メディア情報（GET_MEDIA_INFO / GET_MEDIA_RENDER / MEDIA_CONTROL / MEDIA イベント）

ADBDeviceScreen の「再生中メディア」カードを agent 経由でリアルタイム更新し、アルバムアートと
**CJK 等を含むタイトル/アーティストを agent 側で描画**して送る（Tab5 に CJK フォントを持たせず
あらゆる文字種を表示するため。アプリアイコンと同じく「agent がビットマップ化、Tab5 は blit のみ」）。
`MEDIA` capability（§4.6 bit 3）が前提 — agent が MediaSession に届かない環境では capability を落とす。
agent は `android.media.session.ISessionManager`（binder）から最優先セッションの `MediaController` を得て
メタデータ・再生状態・アルバムアートを読む（`MEDIA_CONTENT_CONTROL`、shell uid で取得可）。

**設計:** 変化（再生/停止・曲送り）は agent が **`MEDIA` イベントで push**（リアルタイム）。Tab5 は
`content_token`（タイトル＋アーティスト＋アルバム＋アート同一性のハッシュ）が変わったときだけ
`GET_MEDIA_RENDER` でアート＋文字画像を取り直す。`state` だけの変化（play↔pause）は再描画不要。

##### MEDIA イベント (event = 0x04) — 状態/トラック変化の通知

リンク確立後、**agent が EVENT として送る**（接続直後に 1 回＋以降変化のたび）。

```
 +0   u8    state          0=none / 1=playing / 2=paused / 3=buffering / 4=stopped
 +1   u8    has_art        0/1（アルバムアートの有無）
 +2   u16   reserved       0
 +4   u32   content_token  タイトル/アーティスト/アルバム/アート同一性のハッシュ (LE)。
                           0 = 再生中セッションなし。変化時のみ Tab5 は GET_MEDIA_RENDER を呼ぶ
 (= 8 bytes。将来は末尾に append-only)
```

##### GET_MEDIA_INFO (cmd = 0x22) — 状態スナップショット

**Tab5 が CONTROL_REQUEST として送る**（カード初期表示用。イベントは変化時のみ発火するため）。

**要求 args**: **なし**（`cmd` + `req_id` のみ）。

**応答 result**（agent → Tab5, `status = OK`）: MEDIA イベントの data と同じ 8 バイト（state /
has_art / reserved / content_token）。

##### GET_MEDIA_RENDER (cmd = 0x23) — アート＋文字画像

**Tab5 が CONTROL_REQUEST として送る**。agent はアルバムアートを `art_px`×`art_px` の **raw ARGB8888**、
タイトル/アーティストを各 1 行の **A8（アルファマスク）** に描画して返す（文字色は Tab5 が recolor で付ける）。
文字は `width_px` を超える分を agent 側で省略記号（…）に丸める。

**要求 args**（Tab5 → agent）:

```
 +0   u16   width_px    文字行の最大幅 [px] (LE)。超過分は agent が省略(…)
 +2   u16   art_px      アルバムアート辺長 [px] (LE)。0 = アート不要
 +4   u8    title_px    タイトル文字サイズ [px]
 +5   u8    artist_px   アーティスト文字サイズ [px]
 +6   u16   reserved    0
 (= 8 bytes)
```

**応答 result**（agent → Tab5, `status = OK`）: セクション列（固定で art→title→artist の順）。

```
 +0   u8    section_count   セクション数（= 3 固定）
 +1   u8    reserved        0
 +2   u16   reserved        0
 +4   ...   section_count 個のセクション（連続配置、各セクションは下記）:
   +0   u8    kind        0=ALBUM_ART / 1=TITLE / 2=ARTIST
   +1   u8    format      0=absent（空。アート無し / 文字空） / 1=ARGB8888 / 2=A8
   +2   u16   width  (LE)
   +4   u16   height (LE)
   +6   u32   data_len (LE) = width*height*bpp（ARGB8888 = 4, A8 = 1, absent = 0）
   +10  ...   pixels (data_len bytes。ARGB8888 はメモリ順 B,G,R,A = LVGL ネイティブ、A8 は α 1B/px)
```

- 再生中セッションが無い／メタデータ欠落は `status = OK` で各セクション `format = absent`（または
  `content_token = 0` のみ）。不正な要求は `status = EINVAL`。
- 応答待ちは **2000 ms**（アートのスケール＋文字描画の余裕。既定 1000 ms より長め）。

##### MEDIA_CONTROL (cmd = 0x24) — 再生操作

**Tab5 が CONTROL_REQUEST として送る**。agent は `MediaController.getTransportControls()` で操作する
（対象セッションが確実。`cmd media_session dispatch` の exec より低遅延）。

**要求 args**（Tab5 → agent）:

```
 +0   u8    action     0=play_pause / 1=next / 2=previous / 3=play / 4=pause
 +1   u8    reserved   0
 +2   u16   reserved   0
 (= 4 bytes)
```

**応答 result**: **なし**（`status` のみ）。`status = OK` で受理。再生中セッションが無ければ `EFAIL`。

### 4.6 capability ビット（共通）

機能の有無を表すビットマスク。HELLO で双方が広告し（agent=提供可能 / Tab5=受理可能）、`MIRROR_START`
の `streams` も同じ bit 割当を使う。

| bit | 名前 | 意味 |
|---|---|---|
| 0 | `VIDEO` | 映像 mirror（JPEG ストリップ。§5） |
| 1 | `AUDIO` | 音声 mirror（§6。Android 12+） |
| 2 | `APPINFO` | アプリ情報（`GET_APP_LIST` / `GET_APP_ICON`。§4.4） |
| 3 | `MEDIA` | 再生中メディア情報（`GET_MEDIA_INFO` / `GET_MEDIA_RENDER` / `MEDIA_CONTROL` ＋ `MEDIA` イベント。§4.4） |
| 4-15 | 予約 | 0 |

agent・Tab5 とも `VIDEO` / `AUDIO`（§6） / `APPINFO` / `MEDIA` を立てる（agent は PackageManager や
MediaSession に届かない環境ではそれぞれ落とす）。両者の `capabilities` の **AND** が利用可能な機能集合。

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

### 4.7 入力注入（INPUT）— Tab5→Android・一方向

`TYPE = INPUT`(0x04) のフレームで、Tab5 がソース端末への入力イベント（キー／将来はタッチ・テキスト）を
送る。mirror の overlay UI（電源・音量・ナビゲーションボタン）と、今後のタッチパススルー／キーボード
入力の **共通チャネル**。agent は scrcpy と同じ手法で **hidden API `InputManager.injectInputEvent()`**
に流す（app_process は shell uid なので INJECT_EVENTS を持ち、許可ダイアログなしで注入できる）。

- **一方向・fire-and-forget**: `CONTROL_REQUEST` のような `req_id`／応答は持たない（高頻度のタッチを
  応答待ちにしないため）。失敗は agent 側ログのみ。状態（§7）も変えない。
- payload 先頭の `input_type` で種別を分ける。

```
 +0   u8   input_type   0x00=KEY / 0x01=TOUCH_SNAPSHOT / 0x02=TEXT（予約） /
                         0x03=TOUCH_SNAPSHOT_BATCH
 +1   ...               input_type ごとに定義（下記）
```

#### INPUT_KEY (input_type = 0x00) — キーイベント

```
 +1   u8    action     0=DOWN, 1=UP（KeyEvent.ACTION_DOWN/UP）
 +2   u32   keycode    Android KeyEvent.KEYCODE_* (LE)
 +6   u32   repeat     キーリピート回数 (LE)。単発押下は 0
 +10  u32   meta       KeyEvent メタ状態 (LE)。修飾なしは 0
 (= 入力ペイロード計 14 bytes。将来は末尾に append-only)
```

- agent は `new KeyEvent(now, now, action, keycode, repeat, meta, VIRTUAL_KEYBOARD, 0, 0,
  SOURCE_KEYBOARD)` を `INJECT_INPUT_EVENT_MODE_ASYNC` で注入する（ディスプレイ 0）。
- **1 タップ = Tab5 が DOWN→UP の 2 フレームを送る**（agent は down/up をそのまま注入し、状態を持たない。
  物理キーボード passthrough も同じ down/up 意味論）。
- overlay のボタンが使う keycode: Home=3, Back=4, VolumeUp=24, VolumeDown=25, Power=26, AppSwitch=187。
  Tab5 側が keycode を決め、agent は透過する。

#### INPUT_TOUCH_SNAPSHOT (input_type = 0x01) — タッチ状態

mirror 表示中の Tab5 パネル上でパススルー対象になっている全ポインタを、サンプル時刻とともに送る。
Tab5 は DOWN/MOVE/UP へ分解しない。agent が前回の状態との差分から複数指の `MotionEvent` を構成する。

```
 +1   u32   sample_time_ms  Tab5 の単調増加時刻 [ms] の下位32 bit (LE)
 +5   u8    point_count     この時点で接触中のポインタ数（0..10）
 then point_count × point（各 5 bytes）:
   +0  u8    pointer_id     Tab5 タッチコントローラの track ID
   +1  u16   x              Tab5 パネル座標 X [px] (LE)
   +3  u16   y              Tab5 パネル座標 Y [px] (LE)
 (= 入力ペイロード計 6 + 5×point_count bytes)
```

- snapshot は完全状態であり、前回存在しなかった ID は DOWN、両方に存在して座標が変わった ID は MOVE、
  前回存在して今回ない ID は UP になる。`point_count=0` は全ポインタの解放を表す。2 本目以降の追加・
  最後以外の削除は agent が `ACTION_POINTER_DOWN/UP` にする。
- 同一 ID・同一座標の snapshot は `MotionEvent` を発生させない。タッチコントローラの定周期ポーリングが
  同じ座標を繰り返しても、Android の速度推定へ停止サンプルを追加しないためである。
- `sample_time_ms` は転送時刻ではなく BSP が状態を採取した時刻。agent はジェスチャー先頭で Android の
  `uptimeMillis` へ対応付け、以降の相対時間を `MotionEvent.eventTime` に使う。32 bit wrap は差分計算で扱う。
- 座標は Tab5 パネル座標で送り、source logical 座標への逆変換は agent が行う。レターボックス内の新規
  ポインタは注入しない。注入済みポインタが一時的に映像外へ出ても snapshot に ID がある間は保持し、
  ID が消えた時だけ解放する。
- pointer の配列順は意味を持たず、snapshot 内の ID は一意でなければならない。
- KEY 同様、一方向・fire-and-forget（`req_id`／応答なし、リンク状態を変えない）。

#### INPUT_TOUCH_SNAPSHOT_BATCH (input_type = 0x03) — タッチ状態（まとめ送り）

`INPUT_TOUCH_SNAPSHOT` を1フレームに複数件まとめる。各 snapshot は完全状態かつ固有の時刻を持つため、
agent は順番に差分を取りながら、まとめ前と同じ `MotionEvent.eventTime` 間隔で再生できる。

```
 +1   u8    snapshot_count  後続 snapshot 数（1..255）
 +2   ...   snapshot        INPUT_TOUCH_SNAPSHOT の +1 以降と同じ可変長レコード
 ...        snapshot        snapshot_count 件を採取順に連結
```

ADB-over-TCP では小さい `A_WRTE` ごとの stop-and-wait が映像と競合するため、Tab5 はリンク待ちの間に
増えた snapshot を最大24件まで保持し、リンクが空いた時またはポインタ集合が変わった時に送る。上限超過時は
最古を落としてよい。各 snapshot が完全状態なので、受信側は残った先頭から正しい接触状態を再構成できる。
USB は `INPUT_TOUCH_SNAPSHOT` を単発送信する。

> **テキスト（input_type=0x02）は予約**。同じ INPUT チャネル・同じ注入経路に乗るので、
> フレーム層（§3）は変えない。

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
2. **スケール（§5.3 のモード）**: 回転後の画像をビューア面（`target_width`×`target_height`。
   全画面 mirror = 720×1280）へスケールする。**スケールは agent 側で行う。**
   `max_fps` > 0 のときは送出をその FPS 以下に壁時計ペーシングする（静止画面はそもそも
   新フレームが出ないので送られない）。
3. **ストリップ分割**: スケール後の（必ず縦長の）画像を **`split_count` 本の水平バンド**に分割する
   （`MIRROR_START` の `split_count`。0 = 既定 4）。分割時（>1）は**各ストリップの高さ・出力寸法とも
   16 の倍数**。`split_count` = 1 はフレーム全体が 1 JPEG（preview 用 — 分割しない）。
4. **JPEG 符号化**: 各ストリップを **YUV420（4:2:0）**で JPEG 圧縮し、1 フレーム（§3）
   として順に送る（先頭 `FRAME_START`、末尾 `FRAME_END`）。品質は `MIRROR_START` の
   `jpeg_quality`（0 = 既定 80。preview は 60）。

- **色形式 = YUV420（4:2:0）**。

> **実装メモ（ワイヤ契約は不変）**: 上の 1〜2（回転・スケール・レターボックス）は**実画面キャプチャでは
> GPU にオフロード**する。`SurfaceControl.setDisplayProjection`（回転コード + パネルサイズ
> `ImageReader` 内の中央寄せ矩形）で SurfaceFlinger が回転・スケール・**黒レターボックス**まで一括で
> コンポジットするので、agent は CPU でフルフレームの読み戻し/回転/スケール/合成コピーを行わない
> （`Projection`/`ScreenCapture`）。よって出力は常に **`out_width`×`out_height` フル**（全画面
> mirror = 720×1280）で、各ストリップは `x=0, w=out_width`（§5.2 のとおり）。`--test-pattern` だけは SurfaceFlinger が無いので 1〜4 を CPU（`FramePipeline`）
> で行う。生成手段の差であり、送出されるフレーム/ストリップの形は同じ。
>
> **物理向き固定の実装メモ**: §5.1 の「物理向き基準・論理回転は考慮しない」は、**端末の自然
> （ナチュラル）向きの physical framebuffer を常に映す**ことを意味する（縦長端末なら常に縦長）。
> ところが主経路（Android 14/15 の `DisplayManager.createVirtualDisplay` ミラー）はディスプレイ 0 の
> **現在の論理回転に追従**してしまう（端末を回すと Tab5 まで回って縮小レターボックスする）。これを
> 打ち消すため、`ScreenCapture` を**その時点の端末回転（`Surface.ROTATION_*`）向けに構築**し、
> (1) reader を回転に合わせた向き（ROTATION_0/180=縦長 `target_w×target_h`、90/270=横長
> `target_h×target_w`）にして論理画面をレターボックスなくフィットさせ（スケールは GPU）、
> (2) `acquire()` で**端末回転の逆だけ回転**して自然向きの `target_w×target_h` フレームに戻す。
> 端末を横にしたアプリは Tab5 上で横向きのまま全面表示され（見るときは Tab5 本体を回す）、回転して
> 縮小されることはない。ROTATION_0（通常時）は逆回転 0°＝従来どおり GPU のみの経路で、回転時だけ
> パネルサイズ 1 枚の `Bitmap` 回転コストが乗る。端末回転は `Server` が毎フレーム
> `DisplayManagerGlobal.getRealDisplay(0).getRotation()` で見て、変化したら capture を作り直す。
> 自然向きロックは主経路のみ。レガシーのフォールバック（pre-Android-12）は `Projection` の
> ソースアスペクト幾何のまま（現行ターゲットではない）。逆回転は `counterDeg = (rotation & 3) * 90`、
> 回転方向は実機（Tab5＋実機）で確認済み。
> `--test-pattern` だけは SurfaceFlinger が無いので 1〜4 を CPU（`FramePipeline`）で行う。

### 5.2 JPEG payload

```
 +0   u16   x        ブロック左上 X [px] (LE)。分割時は 16 の倍数
 +2   u16   y        ブロック左上 Y [px] (LE)。分割時は 16 の倍数
 +4   u16   w        ブロック幅 [px] (LE)。分割時は 16 の倍数（split_count=1 は偶数で可）
 +6   u16   h        ブロック高さ [px] (LE)。分割時は 16 の倍数（split_count=1 は偶数で可）
 +8   ...   jpeg     この w×h 領域を JPEG 圧縮（YUV420, jpeg_quality）したデータ
```

- 座標はビューア面（`target_width`×`target_height`。全画面 mirror = Tab5 パネル 720×1280）上の
  デバイス座標。横ストリップなので 1 フレーム内で `x`・`w` は一定、`y`/`h` がバンド位置（fit モード
  では画像が中央寄せされ `x`>0・`w`<target_width になり得る。§5.3）。
- **分割時（`split_count` > 1）は座標・サイズとも 16px の倍数**（YUV420 の MCU 整列 — 受信側は各
  バンドを行帯へタイトデコードで「配置」するため、バンド境界が MCU 行境界に乗る必要がある）。
  **`split_count` = 1（フレーム全体が 1 JPEG）なら任意の偶数サイズで良い** — JPEG 内部の MCU
  パディングはデコーダが処理する。ただし P4 HW デコーダの出力ラスタは **MCU パディング幅
  （ceil16(w)）で連続格納**されるので、受信側は出力バッファをパディング寸法で確保し、表示は
  実寸 w × ストライド ceil16(w) で行う（`jpeg_enh_frame_info_t.pic_w` がストライドの真値）。
- 1 ブロックの **payload 全体（8B サブヘッダ + jpeg）は `max_payload` 以下**（§3, §4.4）。
- **末尾ブロックは FLAGS の `FRAME_END`、先頭ブロックは `FRAME_START`**（最終フラグはフレーム層に一元化）。

### 5.3 スケールモードと分割の方針

- **分割の主目的 = HW JPEG コーデック負荷の時間分散**: 大きな JPEG を 1 回で HW JPEG コーデックに
  かけると **AXI バスを長時間占有**し、音声処理など一部のリアルタイム処理に影響が出る。これを避ける
  ため、1 フレームを複数ストリップに分けて **デコードを時間方向に分散**させる。`split_count`
  （`MIRROR_START` +10。0 = 既定 4）はこの分散粒度のノブで、負荷分散と転送効率（`max_payload`）の
  兼ね合いで実機調整する。**小フレームの preview は 1**（全画面 mirror と違い分散の必要がなく、
  分割しない方が単純）。Tab5 はバンド数を事前に知る必要はない（各ブロックの座標とフレーム層の
  `FRAME_END` で境界が決まる）。
- **スケールモード**（初期値は `MIRROR_START` の `scale_mode`。将来 `MIRROR_SET_PARAM` でライブ切替）:
  - **fit（既定）**: 画面全体が収まるよう **アスペクト比を保って** `target_width`×`target_height` に
    内接させる。縦横比がターゲットと異なると上下または左右に余白（レターボックス）が出る＝**送られる
    画像はターゲットより小さくなり得る**。中央寄せの分だけ `x`/`y` にオフセットが乗る。余白は静的な
    黒で、Tab5 が初期化時/モード変更時に塗る（毎フレーム画像領域は全面書き換わるのでダブルバッファの
    まま整合する）。
  - **fill**: 縦横で同じ倍率を保ったまま **ターゲット全体を埋める**ようスケールし、はみ出しは
    クロップする。**fill モードの画像は必ず `target_width`×`target_height`**（余白なし）。
  - **aspect**: agent が**出力サイズ自体をソースのアスペクト比に合わせて決める** — 自然向きソースを
    `target_width`×`target_height` のボックスに fit したサイズ（**偶数へ丸め**、ボックス以下。
    幅律速のソースなら幅 = `target_width` ちょうど）を出力フレームとし、レターボックスもクロップも
    なく全画面を流す。決めたサイズは `MIRROR_START` 応答の `out_width`/`out_height` が運ぶ。
    受信側がフレームをそのまま表示する小窓ビューア（DeviceScreen preview の
    「幅 360 固定・高さはソース比率に追従」）向けで、**`split_count` = 1 と組で使う**
    （16px 整列が不要になるので任意の偶数サイズが許される。§5.2）。実装上は
    「アスペクト一致ボックスへの fit」と等価なので、agent はサイズ決定後は fit と同じ経路で流す
    （偶数丸めの誤差 ≤ 1px は許容）。
  - **adapt**: agent が**ソース端末の解像度そのもの**を `wm size`（`IWindowManager.
    setForcedDisplaySize`）でビューア面のアスペクト比に変更する — 端末の短辺を保ったまま長辺を
    `target` のアスペクトに合わせて伸ばすので、端末アプリ側が**リフロー**し、以後は素の fit が
    レターボックスもクロップもなくパネル全面を埋める（agent は内部的に fit 経路で流す。`out_*` =
    `target`）。**aspect（2）が出力フレームの寸法だけを決めて実機解像度に触れないのと決定的に異なり、
    adapt（3）は実機を物理的に変える。** よって **復元は agent が所有する**: `MIRROR_STOP`・別モードへの
    再 `MIRROR_START`・切断・JVM 終了（shutdown hook、SIGTERM/SIGHUP）で `clearForcedDisplaySize`
    （= `wm size reset`）する。Tab5 は `wm size` を一切送らない（**ケーブル切断で reset が届かず
    端末が override に取り残される問題を、最後まで端末側に居る agent が自己復旧で解消する**）。
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

## 6. 音声ストリーム（AUDIO）

`TYPE = AUDIO`(0x11) のフレームで音声を Android→Tab5 の単方向で運ぶ。**ターゲットは Android 12
以降**（それ以前では音声機能を動かさない）。agent は `AudioRecord` の hidden ソース
`REMOTE_SUBMIX`（値 8）で端末の出力ミックスを取り込む — app_process の shell uid が持つ
`CAPTURE_AUDIO_OUTPUT` のおかげで **MediaProjection もパーミッションダイアログも不要**（scrcpy と
同じ手法。実機 Android 14 でキャプチャ成立と消音挙動を確認済み）。

### 6.1 モードと開始

音声は **2 モード**のみ。`MIRROR_START`（§4.4）の `streams` の `AUDIO`(0x02) ビットで選択する
（mirror は常に映像を伴うので音声つきは `streams = VIDEO|AUDIO = 0x03`）:

- **Tab5Only（既定）= `AUDIO` ビット ON**: agent が `REMOTE_SUBMIX` でキャプチャして Tab5 へ流す。
  REMOTE_SUBMIX は出力をサブミックスへリルートするので、**キャプチャ中はスマホ本体が消音**される
  （開始/終了に一瞬のトーン漏れがある）。= 「Tab5 からのみ再生」。
- **PhoneOnly = `AUDIO` ビット OFF**: agent は音声を取り込まない。スマホがそのまま鳴る。
  = 「スマホからのみ再生（音声転送なし）」。

「両方から再生」は REMOTE_SUBMIX が端末を消すため別機構（scrcpy `--audio-dup` 相当の
`AudioPolicy` + loopback-render `AudioMix`）が要り、**現状は非対応**。将来やるなら `MIRROR_START`
引数末尾に `audio_flags`（append-only）を足して選ぶ余地を残す。

### 6.2 フォーマット（agent が決め、MIRROR_START 応答が運ぶ）

Tab5 が接続種別に応じたコーデックを要求し、agent が実際の音声フォーマットを
`MIRROR_START` 応答（§4.4）の末尾フィールド（`audio_codec` /
`audio_channels` / `audio_rate`）で Tab5 へ伝える。Tab5 はこれで音声出力（`bsp_audio_open`）を
構成する:

- USB: `audio_codec` = `0x01` (PCM_S16LE)。低遅延を優先する。
- ADB-over-TCP/Wi-Fi: `audio_codec` = `0x02` (raw Opus)。96 kbps VBR、20 ms/packet。
- `audio_channels` = 2（ステレオ。Tab5 の BSP がスピーカー用にモノミックス、HP はステレオ）。
- `audio_rate` = 48000。

Opus を要求されたのに agent のエンコーダが利用できない場合はPCMへ暗黙フォールバックせず、音声なしで
映像だけを開始する。Wi-Fiへ1.536 MbpsのPCMを流して元の不安定さを再発させないためである。

### 6.3 AUDIO フレーム payload

**1 AUDIO フレーム = 1 コーデック単位**。コーデックは MIRROR_START 応答の `audio_codec` で確定し、
フレームごとには重複して持たせない（Opus へ無改造で差し替えられる構成）:

- **PCM (codec=0x01)**: payload = インターリーブ 16bit LE PCM チャンク（任意長。低レイテンシと
  JPEG とのバースト回避のため **~10ms 刻み**で送る運用）。
- **Opus (codec=0x02)**: payload = 1 raw Opus パケット、48 kHz/stereo/20 ms。フレーム層の
  `LENGTH` がパケット境界を与えるのでOggコンテナや追加サブヘッダは持たない。

`FRAME_START`/`FRAME_END` は両方立てる（各 AUDIO フレームが自己完結）。タイムスタンプ/通し番号が
要るようになったら §6 にサブヘッダを後付けする（フレーム層 §3 は不変）。

映像と同じ 1 本のストリーム上を `TYPE` で多重化するので、音声追加でフレーム層（§3）は変えない。
agent は **音声送出を JPEG 送出と別スレッド**で行い、`Conn.writeFrame` の直列化（§3）でワイヤ整合を
保つ。Tab5 側は受信スレッドでは PCM をリング、Opus をパケット境界つきキューへコピーするだけにして、
別の音声タスクがデコードして `bsp_audio_write`（I2S DMA で自然ペーシング）へ吐き出す。Opus は5 packet
= 100 msを貯めてから開始・アンダーラン復帰し、最大50 packet = 1秒を保持する。上限超過時は古い
パケットを100 ms分までまとめて落とし、decoderをresetして遅延の固定化を避ける。

---

## 7. 状態遷移

```
            connect + HELLO(ok)        MIRROR_START(ok)
  IDLE ──────────────────────────▶ READY ─────────────────▶ STREAMING(JPEG[, AUDIO])
   ▲                                 │  ▲                           │
   │                                 │  └────── MIRROR_STOP ────────┘
   └─────────────────────────────────┴──────────────────────────────┘
              ストリーム切断 / agent 終了 / USB 抜け / WiFi 途絶
```

- `IDLE`: 未接続、または接続直後 HELLO 前。
- `READY`: HELLO 確立後（agent_link 接続済み、mirror 未開始）。制御メッセージのやりとりは可能。
- `STREAMING`: `MIRROR_START` 後。agent が JPEG（要求時は AUDIO も）を流す。
- `STREAMING` で `MIRROR_STOP`（§4.4）を受けると **`READY` に戻る**（リンクは維持。再度 `MIRROR_START`
  で `STREAMING` へ）。
- 切断で `IDLE` に戻り、§2.2 のシーケンスで再接続する。

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
- **音声（§6）の実機調整**: Wi-Fi Opus の開始・復帰バッファは100 ms。実環境で不足する場合だけ
  増やす。HW JPEG デコードの AXI 占有が音声 realtime に与える影響（`SPLIT_COUNT` との兼ね合い）も
  継続して確認する。
