# テスト — tab5adb-agent / Tab5⇄agent 通信

android-agent（`tab5adb-agent`）と Tab5 側（`embedded_adb`/`adb` + `agent_link`）の
**通信のテスト方法**をまとめたドキュメント。`protocol.md` が通信の確定仕様（契約）、
本書はその**検証手段**。

## 1. 考え方 — headless C++ ハーネス（GUI なし）

一次検証は、**Tab5（ADB ホスト）の役を PC 上で実機 Android 相手に演じる headless C++
ハーネス**（`components/agent_link/test/`）。`components/adb/test/test_*.cpp` と同じ host
テストパターン。

- **本番の Tab5 側コードをそのままリンク**して回す: `embedded_adb` / `adb` / `agent_link` を
  そのまま使い、host ランタイムは simulator と同じ `idf_compat`（`freertos`=pthread 実装 /
  `nvs`=JSON / `esp_*` shim）+ `transport_libusb`。**SDL/LVGL/`app/` は持ち込まない**。
- libusb で **PC 直結の実機**を叩くので、Tab5 実機を待たずに Tab5 経路を検証できる。受信スタックの
  “描画以外” を実機 agent 相手に丸ごと回す。GUI 操作なし・終了コードで合否判定。

## 2. 前提

- **実機が PC に接続・認証済み**（Android 14 で検証）。未認証なら
  `components/adb/test/test_client` を一度実行して端末の「USB デバッグを許可」をタップ。
- **agent jar をビルド済み**: `nix develop -c android-agent/build.sh` → `build/tab5adb-agent.jar`。
- **`adb kill-server` してから実行**（標準 adb-server が USB を掴むと libusb が claim できない）。
  runner が自動でやる。

## 3. 実行方法 — runner スクリプト

```sh
nix develop -c components/agent_link/test/run.sh [jar-path]
```

`components/agent_link/test/run.sh` が:

- `idf_compat` の `.o`（`nvs`/`esp_err` は gcc、`freertos_*`/`esp_timer` も）+ `embedded_adb` 全
  `.cpp` + `transport_libusb` + `adb` 全 `.cpp` + `agent_link` + テスト本体を **1 コマンドでビルド**、
- `adb kill-server` してから実行。
- 成果物（`.o`・バイナリ）は `test/build/`（gitignore の `build` 規則で無視。`.o` キャッシュで再ビルド高速）。
- jar パスは `build.sh` の出力を既定、argv で上書き可。
- テストを増やしたら `TEST=<name>` で `test/<name>.cpp` を選択。

毎回 g++ を手打ちしない（`embedded_adb`/`adb` の「ヘッダにコマンドを書く」流儀の runner 版）。

## 4. 現在のテスト

### `test_hello` — HELLO ハンドシェイク（`test/test_hello.cpp`）

ハーネスが bring-up を**自己完結**で回す（実ファームの埋め込み dex 展開は不要、既存スライスで代替）:

1. `Client::connect_usb`（CNXN+AUTH）。
2. 古い agent を `exec("pkill -f …Server")` で掃除 → `Sync::push` で jar を `/data/local/tmp` へ。
3. `open_shell("CLASSPATH=… app_process / com.tab5adb.agent.Server")` で起動。**shell セッションを
   開いたまま**にするので agent の stdout を `ShellListener` で観測でき、両側の `HELLO ok` を test が見られる。
4. `agent_link::Link::open` → `open_stream("localabstract:tab5adb-agent")`。agent が listen するまで
   Link を張り直して短間隔リトライ（HELLO 前に閉じたら未起動とみなし再試行）。
5. HELLO 往復: agent が HELLO REQUEST を送り、Tab5 側 `Link` が proto 照合して RESPONSE
   （`max_payload` / 720×1280 / `scale_mode`）を返す。agent が proto 一致を確認し `HELLO ok` を出力。

**合否**: `on_link_hello` 発火（Tab5 側受理）＋ agent stdout に `HELLO ok`（agent 側受理）＋ `Link` の
`on_link_close` が exactly-once。`PASSED`/`FAILED` を終了コードで返す。実機で検証済み。

## 5. カバー範囲（レイヤー別）

| 対象 | headless ハーネスで |
|---|---|
| agent ビルド/起動（`app_process`） | ✅（ハーネスが push + `open_shell` 起動） |
| localabstract 接続経路（`A_OPEN localabstract:`） | ✅ **embedded_adb 実装で**（標準 adb forward ではなく真の経路） |
| フレーム framing（MAGIC/TYPE/FLAGS/SEQ/LENGTH）/ HELLO（protocol.md §3,§4） | ✅ Tab5 側パーサ + HELLO 応答 |
| JPEG ストリップ受信→デコード | ⏳ Phase 2（同ハーネスを拡張。§7） |
| 受信→デコード→**描画** | ⏳ シミュレータ統合（`agent_link` を `app/` の画面へ） |
| E2E mirror | ⏳ 実機 Tab5 + 実機 Android |

## 6. デバッグ用フォールバック — 標準 adb

`embedded_adb` の localabstract が疎通しないとき、**原因が agent 側か Tab5 側かを切り分ける用途のみ**
（合否判定には使わない）。`run.sh`（android-agent）で agent を起動し、別シェルから:

```sh
nix develop -c adb forward tcp:8080 localabstract:tab5adb-agent
nix develop -c python3 -c 'import socket; print(socket.create_connection(("localhost",8080),3).recv(64))'
```

agent は **binary な HELLO フレームを喋り応答を待つ**ので、素の `recv` はテキストにならない（HELLO
REQUEST のバイト列が返り、その後 agent は応答待ちでブロックする）。「agent が正しく listen・送信して
いる」と確認できれば原因は Tab5 側（embedded_adb の open/retry/フレーム解釈）に絞れる（逆も同様）。

## 7. Phase 2（JPEG ストリップ）のテストは別途

ストリップ受信のテストは同じ headless ハーネスを拡張して足す（`agent_link` の reactive パーサが
`FRAME_END` を **デコード + framebuffer seam** に渡す：test は host libjpeg + メモリ FB、`app/` は P4 HW
JPEG + bsp FB）。検証アプローチ（構造アサート / host デコード / 性能ノブ）と実装の参考は **memory
`mirror-phase2-test-plan`** にまとめてある。実装したら本書の §4/§5 に追記する。

## 8. 注意点

- **`adb kill-server` 必須**（runner が実行）。標準 adb-server が USB を掴むと libusb で claim できない。
- コールバックはリーダー/ワーカースレッドで発火するが、**headless には LVGL が無い**ので `lv_async_call`
  marshalling は不要 — そのまま判定スレッドで読んでよい（テストが薄くなる利点）。
- ADB の `max_payload`（device 16KB / libusb 256KB）と protocol の `max_payload`（暫定 256KiB）は**別物**。
  1 プロトコルフレームは複数 `A_WRTE` に分割され byte stream 上で再構成される。
- 端末なしで回せるのは framing の純パーサ単体（canned バイト列を `Transport` モックに流す）まで。HELLO
  以降は実機が要る。
</content>
