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
- 未知の `type` は読み捨てる。ただし未知の要求には `ENOTSUP` の REPLY を返す（制御を参照）。

## 制御ストリーム

| type | 名前 | 向き | id |
|---|---|---|---|
| `0x01..0x3F` | 要求 | Tab5→agent | 1..65535 |
| `0x40` | `REPLY` | agent→Tab5 | 要求の id |
| `0x41..0x5F` | イベント | agent→Tab5 | 0 |
| `0x60..0x7F` | 入力 | Tab5→agent | 0 |

### 要求と REPLY

- Tab5 は id を 1..65535 で巡回させて振る。応答を待っている要求どうしで id は重ならない。
- agent はすべての要求に REPLY をちょうど 1 つ返す。要求は並行に処理してよく、REPLY の順序は
  要求の順序と一致しなくてよい。
- 応答を待つ時間は Tab5 が要求ごとに決める。待ち終わった後に届いた REPLY は捨てる。

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

## 映像・音声ストリーム

映像と音声は同じ規則で開く。

- Tab5 はストリームを開いたら、最初に設定のメッセージを書く。agent は同じストリームで
  設定への応答を返し、その後にデータを流す。応答より後のデータはすべてその設定に従う。
- 設定を変えるときは、Tab5 はストリームを閉じて新しい設定で開き直す。
- agent は種類ごとに同時に 1 本だけ扱う。新しいストリームを受け付けたら、古い方を止めて閉じる。
- HELLO の前に開かれたストリームは、agent が閉じる。
- 端末の資源（音声キャプチャなど）が他に使われていて取れなければ、agent は設定への応答で
  `EBUSY` を返し、ストリームを閉じる。

メッセージは機能ごとに定義する。
