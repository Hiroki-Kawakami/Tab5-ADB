# 通信仕様 — Tab5 ⇄ tab5adb-agent

Tab5（組み込み ADB ホスト）と Android 側の `tab5adb-agent`（app_process で起動するサーバ）の
間の通信の契約。Tab5 側（`agent_link` + app）と agent 側はこの文書に従い、プロトコルを
変えるときは先にこの文書を更新する。

- 多バイト値はすべてリトルエンディアン、構造体はすべて packed。
- `proto_version` = 1。完全一致のときだけ通信する。

## ストリーム

Tab5 は ADB の `localabstract:` サービスで agent のソケットへストリームを開く。用途ごとに
ソケットを分ける。`<id>` は Tab5 の識別子で、factory MAC の下位 4 バイトを大文字 16 進にした 8 桁。ADB 鍵の
名前 `Tab5-ADB@<id>` と同じ文字列を使う。

| ソケット | 向き | 運ぶもの |
|---|---|---|
| `tab5adb-agent-<id>` | 双方向 | 制御: 要求・応答・イベント・入力 |
| `tab5adb-agent-<id>-video` | 双方向（設定のみ Tab5→agent） | 映像 |
| `tab5adb-agent-<id>-audio` | 双方向（設定のみ Tab5→agent） | 音声 |

- Tab5 ごとに agent プロセスとソケットが分かれるので、1 台の Android 端末に複数の Tab5 が
  同時に接続できる。
- ストリームはすべて Tab5 が開く。agent は開かない。
- 順序は 1 本のストリームの中でだけ保証される。ストリームをまたいだ順序は無い。
- ADB はストリームごとに流量制御するので、映像が詰まっても制御は止まらない。
- 下位（ADB over USB / TCP）が信頼性と順序を保証するので、同期マーカー・CRC・再送は持たない。

## 起動と終了

1. Tab5 は `pkill -f 'com.tab5adb.agent.Server <id>$'` で、自分の識別子で残っている agent
   だけを止める。他の Tab5 の agent は止めない。
2. jar を `/data/local/tmp/tab5adb-agent-<hash>.jar` に置く。`<hash>` は jar の SHA-256 の
   先頭 16 桁（16 進小文字）。そのパスに同じサイズのファイルがあれば push しない。
3. `shell:` で `CLASSPATH=<jar> app_process / com.tab5adb.agent.Server <id>` を起動し、その
   ストリームを開いたままにする。
4. agent は全ソケットで listen したら、標準出力に `listening` の 1 行を出す。Tab5 はこの行を
   待ってから制御ストリームを開く。行より先に shell ストリームが閉じたら起動失敗。
5. Tab5 は制御ストリームで HELLO を送る。

- agent は 1 プロセスで 1 セッションだけを扱う。制御ストリームは 1 本だけ受け付ける。
- 制御ストリームが閉じたら、agent は映像・音声を止めてプロセスを終了する。
- `listening` から 10 秒以内に制御ストリームが開かれなければ、agent は終了する。

## フレーム

各ストリームはメッセージの列。1 メッセージ = 8 バイトのヘッダ + payload。

```
Header (8 bytes)
  u32 length    payload のバイト数
  u8  type
  u8  reserved  0
  u16 id        要求とその REPLY だけが使う。それ以外は 0
```

- `length` は受信側の `max_message`（HELLO で交換する）以下。超えたら受信側はストリームを閉じる。
- `type` の上位 2 bit で種類が決まる（制御を参照）。映像・音声ストリームも同じ区分に従う。
- 未知の `type` は読み捨てる。ただし未知の要求には `ENOTSUP` の REPLY を返す（制御を参照）。

## 制御ストリーム

| type | 名前 | 向き | id |
|---|---|---|---|
| `0x01..0x7F` | 要求 | Tab5→agent | 1..65535 |
| `0x80` | `REPLY` | agent→Tab5 | 要求の id |
| `0x81..0xBF` | イベント | agent→Tab5 | 0 |
| `0xC0..0xFF` | 入力 | Tab5→agent | 0 |

要求は機能ごとに 16 個ずつの区画に分ける。イベントは同じ機能の区画の値に `0x80` を足した区画を使う。

| 要求 | イベント | 機能 |
|---|---|---|
| `0x01..0x0F` | `0x81..0x8F` | セッション |
| `0x10..0x1F` | `0x90..0x9F` | 表示 |
| `0x20..0x2F` | `0xA0..0xAF` | アプリ |
| `0x30..0x3F` | `0xB0..0xBF` | メディア |
| `0x40..0x4F` | — | プレビュー |
| `0x50..0x7F` | — | 予約 |

### 要求と REPLY

- Tab5 は id を 1..65535 で巡回させて振る。応答を待っている要求どうしで id は重ならない。
- agent はすべての要求に REPLY をちょうど 1 つ返す。要求は並行に処理してよく、REPLY の順序は
  要求の順序と一致しなくてよい。
- 応答を待つ時間は Tab5 が要求ごとに決める。待ち終わった後に届いた REPLY は捨てる。
- 画像を含む result も REPLY で返す。result を付けると Tab5 の制御の `max_message` を超えるとき、
  agent は result を付けずに `EFAIL` を返す。

```
REPLY
  u8  status
  u8  reserved[3]
  ... result      要求ごとに定義。status = OK のときだけ有効
```

| status | 名前 | 意味 |
|---|---|---|
| `0x00` | `OK` | 成功 |
| `0x01` | `EINVAL` | 引数不正 |
| `0x02` | `ENOTSUP` | 非対応の要求・値・proto 版 |
| `0x03` | `EBUSY` | 端末の資源を他（別の Tab5 や他のアプリ）が使っていて取れない |
| `0x04` | `ESTATE` | 今の状態では受けられない |
| `0xFF` | `EFAIL` | その他の失敗 |

### イベント

- agent は送信が詰まってもイベントで処理を止めない。送れずに溜まったら、同じ type の古い
  ものを新しいもので置き換える。
- Tab5 は制御ストリームを常に読み続ける。

### 入力

応答の無い一方向のメッセージ。Tab5 は送信待ちの間に溜まったメッセージを 1 回の書き込みに
まとめて送ってよい。

### 要求一覧

| type | 名前 | 内容 |
|---|---|---|
| `0x01` | `HELLO` | セッションの確立 |
| `0x10` | `SET_ROTATION` | 端末の画面の向きを固定する・戻す |
| `0x11` | `ADAPT_DISPLAY` | 端末の解像度を縦横比に合わせる・戻す |
| `0x20` | `GET_APP_INFO` | アプリの名前とアイコン |
| `0x30` | `GET_MEDIA` | 再生中のメディアの状態 |
| `0x31` | `GET_MEDIA_RENDER` | 再生中のメディアの文字とアート |
| `0x32` | `MEDIA_CONTROL` | 再生中のメディアの操作 |
| `0x40` | `SCREENSHOT` | 端末の画面を撮って保持し、縮小画像を返す |
| `0x41` | `SCREENSHOT_RENDER` | 保持しているスクリーンショットの縮小画像 |
| `0x42` | `SCREENSHOT_SAVE` | 保持しているスクリーンショットを PNG のファイルに書く |
| `0x43` | `SCREENSHOT_RELEASE` | 保持しているスクリーンショットを捨てる |
| `0x44` | `RENDER_FILE` | 端末上のファイルの縮小画像と属性 |

### イベント一覧

| type | 名前 | 内容 |
|---|---|---|
| `0xB0` | `MEDIA` | 再生中のメディアの状態が変わった |

### HELLO

Tab5 が制御ストリームで最初に送るメッセージ。REPLY が `OK` で返るまで、Tab5 は他の
メッセージを送らず、映像・音声のストリームも開かない。

```
HELLO
  u16 proto_version
  u16 reserved
  u32 capabilities       Tab5 が受け付ける機能
  u32 max_message[3]     Tab5 が受信できる payload の上限。制御, 映像, 音声

REPLY result
  u16 proto_version
  u8  version_major      agent のバージョン
  u8  version_minor
  u8  version_patch
  u8  reserved
  u16 reserved
  u32 capabilities       agent が提供する機能
  u32 max_message        agent が制御ストリームで受信できる payload の上限
```

- `proto_version` が一致しなければ agent は `ENOTSUP` を返し、制御ストリームを閉じて終了する。
- 両者の `capabilities` の AND が使える機能。ビットは機能ごとに割り当てる。

| bit | 名前 | 機能 |
|---|---|---|
| 0 | `VIDEO` | 映像ストリーム、`SET_ROTATION`、`ADAPT_DISPLAY` |
| 1 | `AUDIO` | 音声ストリーム |
| 2 | `INPUT` | `TOUCH`、`KEY`、`INPUT_CLEAR` |
| 3 | `MEDIA` | `GET_MEDIA`、`GET_MEDIA_RENDER`、`MEDIA_CONTROL`、`MEDIA` イベント |
| 4 | `PREVIEW` | `SCREENSHOT`、`SCREENSHOT_RENDER`、`SCREENSHOT_SAVE`、`SCREENSHOT_RELEASE`、`RENDER_FILE` |
| 5 | `APPS` | `GET_APP_INFO` |

### SET_ROTATION

```
SET_ROTATION
  u8  rotation           0..3 = 固定する向き（「向き」を参照）。0xFF = 固定をやめて元に戻す
  u8  reserved[3]

REPLY result
  なし
```

- agent は最初に固定したときの端末の状態（自動回転か、固定ならその向き）を覚えておき、
  `0xFF`、制御ストリームが閉じたとき、プロセスの終了時にその状態へ戻す。
- 固定中に別の向きを受けたら、その向きに固定し直す。覚えた状態は変えない。
- 固定していないときの `0xFF` は何もせず `OK`。

### ADAPT_DISPLAY

```
ADAPT_DISPLAY
  u16 width              合わせる縦横比。0×0 = 元に戻す
  u16 height

REPLY result
  なし
```

- agent は端末の解像度を、自然な向きの長辺と短辺の向きを保ったまま、短辺の長さはそのままに、
  長辺を `width`・`height` の長辺と短辺の比に合わせた長さ（偶数に丸める）にする。端末の
  アプリは新しい解像度で描き直す。
- 映像ストリームは開き直さなくてよい。解像度が変わると、agent は次のフレームからその大きさで
  取り込む。
- agent は 0×0、制御ストリームが閉じたとき、プロセスの終了時に元の解像度へ戻す。
- 解像度を変えている間に別の比を受けたら、その比に合わせ直す。
- ユーザーが既に解像度を変えている（`wm size` の上書きがある）端末では、変えずに `ESTATE` を返す。
- 解像度を変えていないときの 0×0 は何もせず `OK`。

### GET_APP_INFO

```
GET_APP_INFO
  u16 icon_px            アイコンの一辺 [px]。0 = アイコンは要らない
  u8  text_px            名前の文字の大きさ（「文字の画像」を参照）
  u8  count              1..255
  count × {
    u8  length           パッケージ名のバイト数
    ... package          パッケージ名（UTF-8）
  }

REPLY result
  count × {              要求と同じ順
    u8  found            0 = そのパッケージは無い。以降を持たない
    u8  reserved[3]
    TEXT_IMAGE name      アプリの名前
    u16 icon_width       0 = アイコンは無い
    u16 icon_height
    ... icon             icon_width × icon_height 個の u32。0xAARRGGBB、乗算済みでない
  }
```

- 名前はランチャーに出る名前。無ければパッケージ名を描く。
- アイコンは `icon_px` × `icon_px` に描く。
- 1 回に載せるパッケージの数は、REPLY が Tab5 の制御の `max_message` に収まるように Tab5 が決める。

### GET_MEDIA

```
GET_MEDIA
  なし

REPLY result
  MEDIA_STATE

MEDIA_STATE
  u8  state              0 = セッションが無い, 1 = 再生中, 2 = 一時停止, 3 = バッファ中, 4 = 停止
  u8  flags              bit 0 ART: アートがある
                         bit 1 SEEK: シークできる
                         bit 2 MUTED: 端末のメディアの音量がミュートされている
  u16 reserved
  u32 token              曲名・アーティスト・アルバム・アプリから作る値。0 = セッションが無い
  u32 position_ms        この状態になったときの再生位置
  u32 duration_ms        曲の長さ。0 = 分からない
  u16 speed              再生速度。1000 = 等速。再生中でなければ 0
  u8  volume             端末のメディアの音量の段階
  u8  volume_max         その最大
```

- agent はメタデータを持つセッションのうち、再生中・バッファ中・一時停止の順に優先して 1 つ選ぶ。
- 文字やアートは `token` が変わったときだけ変わる。ただし、曲が変わった後でアートだけが遅れて
  届くことがあり、そのときは `token` を変えずに `ART` が立つ。
- Tab5 は `speed` に従って再生位置を進めて表示する。

### MEDIA

`MEDIA_STATE` を運ぶイベント。agent はセッション・状態・曲・アートの有無・シークの可否・音量が
変わったとき、再生位置が飛んだとき（シークなど）に送る。再生が進むだけでは送らない。

### GET_MEDIA_RENDER

```
GET_MEDIA_RENDER
  u8  title_px           曲名の文字の大きさ（「文字の画像」を参照）
  u8  subtitle_px        2 行目の文字の大きさ
  u16 art_px             アートの一辺 [px]。0 = アートは要らない
  u8  subtitle           2 行目に並べるもの。bit 0 = アーティスト, bit 1 = アルバム, bit 2 = アプリの名前
  u8  reserved[3]

REPLY result
  TEXT_IMAGE title
  TEXT_IMAGE subtitle    `subtitle` のうち空でないものを " · " でつないだもの
  u16 art_width          0 = アートは無い
  u16 art_height
  u32 art_length
  ... art                JPEG。baseline、YUV 4:2:0
```

- アートは縦横比を保ったまま、短い辺が `art_px` になるように縮め、中央を `art_px` × `art_px` で
  切り取る。
- セッションが無ければ、文字は空、アートは無し。

### MEDIA_CONTROL

```
MEDIA_CONTROL
  u8  action             0 = 再生・一時停止, 1 = 次, 2 = 前, 3 = シーク, 4 = 音量, 5 = ミュート
  u8  reserved[3]
  u32 value              シークは位置 [ms]、音量は段階、ミュートは 1 = する・0 = 解除。ほかでは使わない

REPLY result
  なし
```

- セッションが無ければ `ESTATE`。シークできないセッションへのシークも `ESTATE`。音量とミュートは
  セッションが無くても変えられる。音量を変えるとミュートは解除される。

### 縮小画像と属性

プレビューの要求は、縮小した画像と属性の集まりを `RENDERED` で返す。

```
RENDERED
  u16 image_width        0 = 画像は無い
  u16 image_height
  u32 jpeg_length
  ... jpeg               baseline、YUV 4:2:0
  u16 attr_count
  attr_count × ATTR

ATTR
  u16 key
  u8  type               0 = u32, 1 = i32, 2 = 分数 (u32 分子, u32 分母), 3 = 符号付き分数 (i32, i32),
                         4 = UTF-8 文字列
  u8  reserved
  u16 length             value のバイト数
  ... value
```

- 画像は正立させ、縦横比を保って枠に収まる最大の大きさにする。元より大きくはしない。幅と高さは
  それぞれ 4 の倍数に切り下げる（映像ストリームと同じ）。
- REPLY が Tab5 の制御の `max_message` を超えるなら、agent は画像の品質を下げて符号化し直す。
- Tab5 は知らない `key` を読み飛ばす。agent は値の無い属性を入れない。
- 分数の分母は 0 にしない。

| key | type | 属性 |
|---|---|---|
| `0x0001` | u32 | 幅 [px]。正立させた大きさ |
| `0x0002` | u32 | 高さ [px] |
| `0x0003` | 文字列 | MIME タイプ |
| `0x0100` | 文字列 | EXIF Make |
| `0x0101` | 文字列 | EXIF Model |
| `0x0102` | 文字列 | EXIF LensModel |
| `0x0103` | 文字列 | EXIF Software |
| `0x0104` | 文字列 | 撮影日時。EXIF の書式 `YYYY:MM:DD HH:MM:SS` |
| `0x0105` | u32 | ISO 感度 |
| `0x0106` | 分数 | 露出時間 [s] |
| `0x0107` | 分数 | F 値 |
| `0x0108` | 分数 | 焦点距離 [mm] |
| `0x0109` | u32 | 35mm 換算の焦点距離 [mm] |
| `0x010A` | 符号付き分数 | 露出補正 [EV] |
| `0x010B` | u32 | EXIF Flash |

key の上位 8 bit は属性の区分: `0x00` 共通、`0x01` EXIF。

### SCREENSHOT

```
SCREENSHOT
  u16 box_width          画像を収める枠 [px]
  u16 box_height
  u8  quality            JPEG 品質 1..100。0 = 80
  u8  reserved[3]

REPLY result
  RENDERED               属性は幅と高さ
```

- agent は端末の画面を撮り、次の `SCREENSHOT` か `SCREENSHOT_RELEASE` まで、またはプロセスが
  終わるまで元の画素を保持する。撮る前に、保持していたものと `SCREENSHOT_SAVE` のファイルを捨てる。
- 保持するスクリーンショットは 1 枚だけ。

### SCREENSHOT_RENDER

`SCREENSHOT` と同じ引数と result で、撮り直さずに保持しているものを枠に合わせて返す。保持して
いなければ `ESTATE`。

### SCREENSHOT_SAVE

```
SCREENSHOT_SAVE
  なし

REPLY result
  u32 size               ファイルのバイト数
  ... path               ファイルのパス（UTF-8）
```

- agent は保持しているスクリーンショットを PNG で `/data/local/tmp/` の下のファイルに書く。
  Tab5 はそれを sync で受け取る。保持していなければ `ESTATE`。
- ファイルは、スクリーンショットを捨てるときに agent が消す。

### SCREENSHOT_RELEASE

```
SCREENSHOT_RELEASE
  なし

REPLY result
  なし
```

保持しているスクリーンショットと `SCREENSHOT_SAVE` のファイルを捨てる。保持していなくても `OK`。

### RENDER_FILE

```
RENDER_FILE
  u16 box_width          画像を収める枠 [px]
  u16 box_height
  u8  quality            JPEG 品質 1..100。0 = 80
  u8  flags              bit 0 IMAGE: 画像が要る
                         bit 1 ATTRS: 属性が要る
  u16 reserved
  ... path               ファイルのパス（UTF-8）

REPLY result
  RENDERED
```

- ファイルの種類は agent が中身から決める。扱えない種類なら `ENOTSUP`、読めなければ `EFAIL`。
- 画像ファイルは EXIF の向きに従って正立させる。属性は幅、高さ、MIME タイプと EXIF。
- `IMAGE` が無ければ画像は無し、`ATTRS` が無ければ属性は 0 個。

### 向き

`rotation` は Android の `Surface.ROTATION_*` と同じ値で、端末の自然な向きに対して描画内容が
時計回りに `rotation × 90°` 回っていることを表す。端末を反時計回りに 90° 傾けて横長に
使っている状態が 1。

### 文字の画像

アプリ名や曲名など端末上の文字列は、agent が端末のフォントで描いた画像として result に入れる。
文字の大きさは要求ごとに `u8 text_px`（em の大きさ [px]）で指定する。

```
TEXT_IMAGE
  u16 width              画像の幅 [px]。空の文字列は 0
  u16 height             行の高さ [px]。text_px だけで決まり、文字列によらない
  u16 ascent             上端からベースラインまで [px]
  u16 breaks             区切りの数
  breaks × u16           区切りの x 座標 [px]。昇順
  ... alpha              width × height バイトの A8。行の間に詰め物は無い。0 = 透明
```

- 文字列は省略せずに 1 行で描く。幅が 2048 px を超えるなら、2048 px に収まる最後の区切りで切る。
- 区切りは各書記素クラスタの右端の x 座標。Tab5 はこの位置で切って省略記号・フェード・マーキーを
  付ける。
- 色は Tab5 が付ける。

### 入力一覧

| type | 名前 | 内容 |
|---|---|---|
| `0xC0` | `TOUCH` | 映像に触れている指 |
| `0xC1` | `KEY` | キーを押した・離した |
| `0xC2` | `INPUT_CLEAR` | 押したままの指とキーをすべて離す |

### TOUCH

```
TOUCH
  u32 time_ms            Tab5 が指を採取した時刻 [ms]。単調増加の下位 32 bit
  u16 width              座標の基準にしたフレームの幅と高さ [px]
  u16 height
  u8  rotation           そのフレームの rotation
  u8  count              0..10
  u16 reserved
  count × {
    u8  id               指の識別子。メッセージの中で一意
    u8  reserved
    u16 x                フレームの画素座標
    u16 y
  }
```

- その時点で端末へ渡す指をすべて載せる。DOWN・MOVE・UP には分けない。前のメッセージに
  無かった `id` は触れた指、両方にある `id` は動いた指、前にあって今回無い `id` は離れた指。
  `count` = 0 はすべての指が離れたことを表す。
- 座標は Tab5 が回転・配置を戻した、フレームの画素座標。agent はフレームの大きさと端末の
  論理解像度の比で端末の座標にする。`FILL` のフレームでは切り取った位置の分を足す。
- `width`・`height`・`rotation` が agent が今送っているフレームと異なるメッセージは、
  agent は捨てる。注入中の指があれば取り消す（`ACTION_CANCEL`）。
- Tab5 はフレームの外で触れた指を、フレームに入るまで載せない。入った最初の位置で触れた
  ことになる。フレームの中で触れた指が外へ出たら、座標をフレームの縁に寄せて載せ続ける。
- agent は `time_ms` に、届いたときの `SystemClock.uptimeMillis()` との差のこれまでの最小値
  を足して `MotionEvent` の時刻にする。時計のずれを追うため、最小値は 10 秒ごとに 1 ms 緩める。
- 前と同じ内容のメッセージは Tab5 は送らない。

### KEY

```
KEY
  u16 keycode            Android の KeyEvent.KEYCODE_*
  u8  action             0 = 押した, 1 = 離した
  u8  reserved
```

- 押している間の長押しやキーリピートは Android に任せる。Tab5 は押した時と離した時だけ送る。
- 押したままのキーの「押した」は agent が無視する。押していないキーの「離した」も無視する。

### INPUT_CLEAR

```
INPUT_CLEAR
  なし
```

- agent は注入中の指を取り消し（`ACTION_CANCEL`）、押したままのキーを `FLAG_CANCELED` 付きで
  離す。Tab5 は入力を送る画面を離れるときに送る。
- 制御ストリームが閉じたときも agent は同じことをしてから終了する。

## 映像・音声ストリーム

映像と音声は同じ規則で開く。

- Tab5 はストリームを開いたら、最初に設定のメッセージを書く。agent は同じストリームで
  設定への応答を返し、その後にデータを流す。応答より後のデータはすべてその設定に従う。
- 設定を変えるときは、Tab5 はストリームを閉じて新しい設定で開き直す。
- agent は種類ごとに同時に 1 本だけ扱う。新しいストリームを受け付けたら、古い方を止めて閉じる。
- HELLO の前に開かれたストリームは、agent が閉じる。
- 端末の資源（音声キャプチャなど）が他に使われていて取れなければ、agent は設定への応答で
  `EBUSY` を返し、ストリームを閉じる。

メッセージは機能ごとに定義する。どちらのストリームでも `id` は 0、設定への応答は `REPLY`
（`0x80`）を使う。設定は要求、agent からのデータはイベント、Tab5 からのデータは入力の区分の type を使う。

### 映像ストリーム

| type | 名前 | 向き |
|---|---|---|
| `0x01` | `VIDEO_START` | Tab5→agent |
| `0xC0` | `FRAME_ACK` | Tab5→agent |
| `0x80` | `REPLY` | agent→Tab5 |
| `0x81` | `FRAME` | agent→Tab5 |

```
VIDEO_START
  u16 box_width          フレームを収める枠 [px]
  u16 box_height
  u8  flags              bit 0 SWAP: 端末の rotation が 1 か 3 のとき枠の幅と高さを入れ替える
                         bit 1 FILL: 枠を覆う大きさにして、はみ出しを中央で切り取る
                         bit 2 ADAPT: 回線が運べる量に合わせて品質、次にフレームレートを下げる
  u8  quality            JPEG 品質 1..100。0 = 80
  u8  max_fps            0 = 上限なし
  u8  strips             1 フレームを分ける横帯の数。0 と 1 は分けない
  u32 max_rate           映像の上限 [bytes/s]。品質を下げて収める。0 = 上限なし

REPLY result
  u8  codec              1 = JPEG
  u8  reserved[3]

FRAME
  u16 width              フレーム全体の幅と高さ [px]
  u16 height
  u16 y                  この帯の先頭の行
  u16 rows               この帯の行数。JPEG の大きさは width × rows
  u8  rotation           このフレームを取り込んだときの端末の向き
  u8  flags              bit 0 LAST: フレームの最後の帯
  u16 reserved
  ... jpeg               baseline、YUV 4:2:0

FRAME_ACK
  u32 received           読み終えた FRAME メッセージ（ヘッダ込み）の累計バイト数の下位 32 bit
```

- agent は端末の画面を今の向き（論理的な向き）のまま、縦横比を保って枠に収まる最大の大きさに
  拡大・縮小して送る。幅と高さはそれぞれ 4 の倍数に切り下げる（Tab5 の HW JPEG デコーダは
  画素数が 8 の倍数の画像しか扱えない）。余白（レターボックス）は付けない。
- `FILL` のときは、縦横比を保って枠を覆う最小の大きさにし、中央の枠の大きさだけを送る。
  フレームの大きさは枠（4 の倍数に切り下げたもの）と同じになる。
- 回転・配置・余白の塗りつぶしは Tab5 が行う。
- 1 フレームは `strips` 本の帯に上から順に分けて、帯ごとに `FRAME` を送る。帯の行数は 16 の
  倍数で、最後の帯が残りを持つ。agent は符号化した帯から順に送り、Tab5 は届いた帯から描く。
  Tab5 は `LAST` の帯を描き終えてから表示を切り替える。
- 端末の向きが変わると、agent は次のフレームからその向きで取り込み、`width`・`height`・
  `rotation` が変わる。ストリームは開き直さない。
- 画面が変わらない間はフレームを送らない。開いた直後は変化が無くても 1 枚送る。
- `FRAME` 1 つが HELLO の映像の `max_message` を超えるなら、agent はその帯の品質を下げて
  符号化し直す。

#### 流量

- Tab5 は `FRAME` を 1 つ読み終えるたびに `FRAME_ACK` を送る。書き込みが詰まっている間の分は
  まとめて最新の値を 1 つ送ればよい。
- agent が抱えるフレームは、送信中の 1 枚と送信待ちの 1 枚まで。送信待ちのフレームは、
  送り始めるまで新しいフレームで置き換える。
- agent は、送信中のフレームの未着分が「回線の速さ × ACK の戻りにかかる時間」以下になったら
  次のフレームを送り始める。回線の速さは直近 1 秒に ACK された量を未着分があった時間で割った値、
  ACK の戻りにかかる時間は各帯の ACK までの時間から先に並んでいた分とその帯自身を送る時間を
  引いた値の、直近 2 秒の最小値。どちらも測れるまでは、未着分が無くなってから送る。
- 品質は `quality` を上限として、直前のフレームの大きさと直近 1 秒のフレーム数から求めた
  流量が `max_rate`（`ADAPT` のときは回線の速さも）を超えれば下げ、8 割を下回れば戻す。
  下限は 20（`ADAPT` のときは 40）。
- `ADAPT` のとき、品質が下限に達してもなお超えるなら、フレームレートを `max_fps` から 10 まで
  下げる。戻すときはフレームレートを先に戻す。


### 音声ストリーム

| type | 名前 | 向き |
|---|---|---|
| `0x01` | `AUDIO_START` | Tab5→agent |
| `0x80` | `REPLY` | agent→Tab5 |
| `0x81` | `AUDIO` | agent→Tab5 |
| `0x82` | `VOLUME` | agent→Tab5 |

```
AUDIO_START
  u8  codec              1 = PCM（S16LE）, 2 = Opus
  u8  source             0 = メディアの再生だけ, 1 = 通話以外のすべての再生
  u8  reserved[2]

REPLY result
  u8  codec              codec と同じ
  u8  channels           2
  u16 reserved
  u32 rate               48000

AUDIO
  ... data               PCM はインターリーブした約 10 ms 分。Opus は 1 パケット（20 ms）

VOLUME
  u8  volume             端末のメディアの音量の段階
  u8  volume_max         その最大
```

- agent は端末で再生される音を、端末の音量をかける前の大きさで取り込んで送る。取り込んでいる間、
  その音は端末から鳴らない。
- `source` = 0 は用途がメディアの再生だけ、1 はメディア・ゲーム・通知・システム音・案内・
  アシスタント・アラーム・着信音などの再生を取り込む。通話は取り込まない。再生するアプリが
  取り込みを許していない音は取れない。
- `VOLUME` は応答の直後に 1 つ送り、以降は変わるたびに送る。ミュート中は `volume` = 0。Tab5 は
  鳴らす音量をこれで決める。
- Android 12 より前の端末、Opus を作れない端末で Opus を求められたときは `ENOTSUP`。Opus を
  求められて PCM で答えることはしない。
- Opus のパケットは 48 kHz・ステレオ・20 ms で、OpusHead は送らない。
