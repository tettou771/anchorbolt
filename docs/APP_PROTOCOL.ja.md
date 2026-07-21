# アプリプロトコル — あらゆるアプリを監視対象にする

[English](APP_PROTOCOL.md) ・ [← README](../README.ja.md) ・ [Architecture](ARCHITECTURE.ja.md)

TrussC アプリは何もしなくても全部入り — health、サムネイル、ステータス、リモート操作 — が手に入ります。フレームワークが AnchorBolt の期待する MCP エンドポイントを標準搭載しているからです。しかしこの契約自体は TrussC を一切要求しません。このページはその契約そのものです: Unity でも Electron でも Processing でも openFrameworks でも、何であれアプリが何を実装すればネイティブ同様に監視されるかを定めます。

> **`tc_` プレフィックスについて:** このプロトコルは
> [TrussC](https://github.com/TrussC-org/TrussC) 発祥で、LSP のメソッドが
> `textDocument/` を保つのと同じようにツール名はそのプレフィックスを保っています。
> 実装に TrussC のコードは一切不要です。

- [段階的な対応レベル](#段階的な対応レベル)
- [supervisor がアプリに渡すもの](#supervisor-がアプリに渡すもの)
- [トランスポート](#トランスポート)
- [ツールの契約](#ツールの契約)
- [タイミング](#タイミング)
- [実装のテスト](#実装のテスト)

---

## 段階的な対応レベル

各レベルは独立した費用対効果です — 割に合わなくなったところで止めて構いません。Level 0 はコストゼロで既に有用です。

| level | 実装するもの | 得られるもの |
|---|---|---|
| **0 — 起動できる** | 何も（`--watchdog-timeout 0` で実行） | クラッシュ検知 + 自動再起動、ダッシュボードの supervisor イベント、リモート再起動/更新 |
| **1 — ログ** | `TRUSSC_LOG_FILE` が指すファイルへログを追記 | ログ収集、サーバーへの送信、`search_logs` / ダッシュボードのログビュー |
| **2 — health** | HTTP JSON-RPC エンドポイント + `tc_get_health` | ハング検知（クラッシュだけでなく）、fps/メモリのグラフ、wall の緑/赤 |
| **3 — スクリーンショット** | `tc_get_screenshot` | wall のライブサムネイル、ライブビュー |
| **4 — 拡張** | `tc_get_status`、`tc_get_alerts`、入力系ツールの任意の組み合わせ | カスタムグラフとステータス値、スマホまで届くアラート、ブラウザからのリモート操作 |

Level 0 は強調しておく価値があります: `anchorbolt start -p anything --watchdog-timeout 0`
だけで、クラッシュしたバイナリの再起動とイベント記録は既に動きます。以下はすべてその上のレベルの話です。

---

## supervisor がアプリに渡すもの

`anchorbolt start` はアプリを子プロセスとして起動し、環境変数を3つ設定します:

| 変数 | 意味 |
|---|---|
| `TRUSSC_MCP=1` | 「監視されている — MCP エンドポイントを立てよ」 |
| `TRUSSC_MCP_PORT` | それを提供するローカル TCP ポート（デフォルト 47777、複数の supervisor が同居できるよう上方向にスキャン） |
| `TRUSSC_LOG_FILE` | ログ行を**追記**するファイル |

つまずきやすい点が2つ:

- **stdout はキャプチャされません。** supervisor がログを収集するのは
  `TRUSSC_LOG_FILE` からだけです。stdout にログを出すアプリは、自分でその行を
  ファイルにも書いてください（追記モードで開くこと — 同日内の再起動では同じ
  ファイルが再利用されます）。
- **プラットフォームの丁寧なシグナルで終了すること。** 再起動や停止の際、
  supervisor は SIGTERM（POSIX）/ WM_CLOSE（Windows）を送って **5 秒**待ち、
  それから強制終了します。この猶予内にクリーンに終了してください。

---

## トランスポート

エンドポイントは `localhost:$TRUSSC_MCP_PORT` のプレーンな HTTP サーバーで、
`POST /mcp` に JSON-RPC 2.0 で応答します。これは本物の
[MCP](https://modelcontextprotocol.io) サーバーです — どの MCP クライアントからも
直接アプリに繋げます（デバッグもそうやります）— が、supervisor が使うのは固定の
サブセットだけで、契約もそのサブセットにピン留めされています。MCP 仕様の進化が
あなたの実装を壊すことはありません:

| メソッド | 必須 | 用途 |
|---|---|---|
| `tools/list` | はい | どのオプション規約を話すかの発見 |
| `tools/call` | はい | それ以外すべて |
| `initialize`, `ping` | 推奨 | supervisor は使わないが、MCP クライアント（Claude、インスペクタ）に必要 |

1 POST につき 1 リクエスト、`Content-Type: application/json`、レスポンスは
JSON-RPC の返信。セッションなし、ストリーミングなし、notification なし。

### 結果の形式

`tools/call` の返信は `result.content` に MCP の**コンテントブロック**を載せます:

- **データ系ツール**（health、status、alerts）は `text` ブロックを1つ返し、
  その `text` は JSON **オブジェクト**のシリアライズです:

```json
{"jsonrpc": "2.0", "id": 1,
 "result": {"content": [
   {"type": "text", "text": "{\"status\":\"ok\",\"fps\":60.0,\"pid\":4242}"}
 ]}}
```

- **画像系ツール**（screenshot、status image）は `image` ブロック — base64 データと
  `mimeType` — を1つ返し、任意で JSON メタデータの `text` ブロックを続けられます
  （supervisor が両者をマージします）:

```json
{"jsonrpc": "2.0", "id": 1,
 "result": {"content": [
   {"type": "image", "data": "<base64 jpeg>", "mimeType": "image/jpeg"},
   {"type": "text", "text": "{\"width\":512,\"height\":288}"}
 ]}}
```

トランスポート障害、非 200、パースできない返信はすべて同じ扱い:
watchdog の時計上の **miss** です。

---

## ツールの契約

### `tc_get_health` — level 2 の必須ツール

health のポーリング周期（[タイミング](#タイミング)参照）で呼ばれます。
安価であること — カウンタを読むだけ、重いものには触らないこと。

引数なし。JSON オブジェクトを返します。ルールは2つ、あとは自由です:

- **`pid` は必須** — 自プロセスの id を数値で。supervisor は自分の子プロセスと
  照合し、不一致は miss と数えます。共有ポート上の古い/他人のアプリが
  死んだ子プロセスを覆い隠すのを防ぐ仕組みです。
- オブジェクト全体がハートビートとしてフリートサーバーへ転送されます。
  ダッシュボードが既にグラフ化を知っているフィールド: `fps`、`rssBytes`
  （プロセスメモリ。リーク監視で見るべき数値）、`uptimeSec`。`version`、
  `width`、`height` は関連箇所に表示されます。余分なフィールドは無害です。

```json
{"status": "ok", "fps": 60.0, "frameCount": 86400, "uptimeSec": 1440.5,
 "width": 1920, "height": 1080, "version": "myapp 2.1", "pid": 4242,
 "rssBytes": 268435456}
```

アプリが*生きている*ことを証明するスレッドから応答してください。ソケットが
開いているだけでは駄目です — レンダーループがハングしても HTTP スレッドが
応答し続けるなら、それは何も見ていない watchdog です。（メインループに
カウンタを進めさせ、止まったら応答を拒否するか stale を返すこと。）

### `tc_get_screenshot` — level 3 の必須ツール

| 引数 | 意味 |
|---|---|
| `format` | `"png"`（デフォルト）または `"jpg"` |
| `width` | 目標幅。アスペクト維持、拡大はしない。省略 = フル解像度 |
| `quality` | JPEG 品質 1–100（デフォルト 75） |

`image` コンテントブロックを返します。supervisor の呼び方は3通り:

- **サムネイル:** `{format:"jpg", width:512, quality:75}`（`--thumb-*` フラグ由来）を
  `--thumb-interval`（デフォルト 30 秒）ごと。
- **ライブビュー:** 誰かが見ている間、同じ JPEG 呼び出しを 1–15 fps で。
- **詳細ビュー:** `{format:"png"}` — フル解像度、オンデマンド。

ピクセルの取得はフレームループで、エンコードはループの外で。ライブビューの
レートでループ内エンコードをすると、インスタレーションに見えるカクつきになります。

### オプション規約 — level 4

supervisor はアプリの実行ごとに一度 `tools/list` を呼び、名前が存在するものだけを
有効化します。無くても問題ありません。他に登録の仕組みはありません。

**`tc_get_status`** — アプリが公開する運用値。引数なし。返すもの:

```json
{"values": [
   {"name": "scene",    "value": "idle",  "mode": "status"},
   {"name": "visitors", "value": 132,     "mode": "graph"}
 ],
 "images": ["entranceCam"]}
```

`mode:"graph"` は「時系列でプロットして」、`"status"` は「現在値を表示して」。
`images` は `tc_get_status_image` で取得できる名前の一覧です。ペイロードは
ハートビートに載ってダッシュボードへ届きます。

**`tc_get_status_image`** — 引数 `{name, width?, quality?}`。screenshot ツールと
同様に JPEG の `image` ブロックを返します。`tc_get_status` が列挙した各名前に
ついてサムネイル周期でポーリングされます — ウェブカメラでもデバッグビューでも、
ダッシュボードに載せたいピクセルなら何でも。

**`tc_get_alerts`** — 「人間に知らせるべきこと」。引数なし。溜まっている
アラートを返し、**同時にクリアします**（drain 方式 — 各アラートは正確に
1つの消費者に届く）:

```json
{"alerts": [{"at": "2026-07-22T03:12:44", "text": "IR camera disconnected!"}]}
```

health の周期で drain され、各エントリは通知シンク（Slack / Discord / ntfy /
…）を発火します。文字通り誰かのスマホまで届き得ます。メッセージは自分の
ログにも書いてください — supervisor が居なくてもローカルに残るべきものです。

**入力系ツール — リモート操作へのオプトイン。** `tools/list` に
`tc_mouse_move` か `tc_key_press` があれば、そのアプリはリモート操作に
オプトインしたことになり、ダッシュボードに操作 UI が現れます（サーバー側では
operator ロールが必要）。慣例のセット:

| ツール | 引数 |
|---|---|
| `tc_mouse_move` | `x`, `y`（ピクセル、ウィンドウ座標） |
| `tc_mouse_click` | `x`, `y`, `button`（`"left"`/`"right"`/`"middle"`） |
| `tc_key_press` | `key`（文字、または `"enter"`, `"space"`, `"left"` などの名前） |

本気でないなら公開しないこと: 列挙すること自体が同意スイッチです。

---

## タイミング

実装が守るべき数値（デフォルト。理由は
[Architecture — タイムアウト](ARCHITECTURE.ja.md)を参照）:

| 項目 | デフォルト | アプリ側の契約 |
|---|---|---|
| health ポーリング | `--watchdog-timeout` / 3 ≈ 3 秒ごと | 毎回、速く安価に応答する |
| ハング watchdog | healthy な応答なしが実時間 10 秒 → 再起動 | miss ~3 回で終わり。health が本当の生存を反映するように |
| 起動猶予 | 120 秒 | 最初の healthy 応答はこの中に。重い起動でも大丈夫 |
| サムネイルポーリング | 30 秒ごと | ライブビュー中は 1–15 fps のバーストが加わる |
| 終了 | SIGTERM / WM_CLOSE → 5 秒 → 強制終了 | 5 秒以内にクリーンに終了 |

---

## 実装のテスト

まず AnchorBolt 抜きで、エンドポイントに直接話しかけます:

```bash
TRUSSC_MCP=1 TRUSSC_MCP_PORT=47777 TRUSSC_LOG_FILE=/tmp/app.log ./myapp &

curl -s localhost:47777/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'

curl -s localhost:47777/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
       "params":{"name":"tc_get_health","arguments":{}}}'
```

確認事項: health の返信が JSON の `text` ブロックであること、`pid` が自プロセスと
一致すること、screenshot 呼び出しが実際の画像にデコードできる `image` ブロックを
返すこと。`initialize` を実装したなら任意の MCP クライアントでも試せます:
`claude mcp add --transport http myapp http://localhost:47777/mcp`。

次に本番:

```bash
anchorbolt start -p ./myapp
```

猶予時間内に supervisor の出力へ `app healthy (fps ...)` が出るはずです。
レンダーループだけ殺して（プロセスは生かしたまま）、watchdog が再起動させる
ことを確認してください — それが本当に意味のあるテストです。`--server` を
足せばアプリは wall に、サムネイル付きで現れます。

---

## 関連ページ

- [Architecture](ARCHITECTURE.ja.md) — 監視・スプール・フリートサーバーの
  組み立てと、各タイムアウトの理由。
- [Get started](GET_STARTED.ja.md) — 実運用への展開: トークン、グループ、
  通知、トンネル。
- TrussC の [AI_AUTOMATION.md](https://github.com/TrussC-org/TrussC/blob/main/docs/AI_AUTOMATION.md)
  — このプロトコルの参照実装。アプリ側 API（`mcp::status`、`mcp::alert`、
  カスタムツール）を含む。
