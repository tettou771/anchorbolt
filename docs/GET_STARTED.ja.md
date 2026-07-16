# Get Started

[English](GET_STARTED.md) ・ [← README](../README.ja.md) ・ [Architecture →](ARCHITECTURE.ja.md)

このガイドは、30秒のローカルお試しから実運用デプロイまでを、一歩ずつ進めていきます。すべてのコマンドはコピー＆ペーストで実行でき、必要が満たされたところで止めて構いません。

- [1. ローカルで試す（30秒）](#1-ローカルで試す30秒)
- [2. 別のマシンから監視する](#2-別のマシンから監視する)
- [3. 公開する前に鍵をかける](#3-公開する前に鍵をかける)
- [4. 会場をグループ化し、クライアントの表示範囲を絞る](#4-会場をグループ化しクライアントの表示範囲を絞る)
- [5. 通知を受け取る（Slack、ntfy など）](#5-通知を受け取るslackntfy-など)
- [6. トンネル越しのリモートコントロール](#6-トンネル越しのリモートコントロール)
- [7. 6桁のコードで会場をペアリングする](#7-6桁のコードで会場をペアリングする)
- [8. サービスとして常駐させる](#8-サービスとして常駐させる)

以降、`myApp` はあなたの TrussC アプリを指します——プロジェクトディレクトリ、`.app` バンドル、あるいは素のバイナリのいずれでも構いません。

---

## 1. ローカルで試す（30秒）

1台のマシンで2つのターミナルを使います。まずダッシュボード：

```bash
anchorbolt serve
# → fleet server on http://localhost:54722
```

続いて、アプリとそのダッシュボードを指定してスーパーバイザーを起動します：

```bash
anchorbolt start -p myApp --server http://localhost:54722
```

**http://localhost:54722/** を開くと、あなたのアプリがライブサムネイル付きでウォールに現れます。アプリを落としてみましょう（ウィンドウを閉じるか `kill -9` で）。数秒のうちに AnchorBolt がアプリを再起動し、ウォールの稼働時間がリセットされます。これがコアループのすべてで、しかもあなたのアプリは一行も変わっていません。

![会場が1つだけのウォール](images/wall-single.png)

> **ローカルだけで使いたい？** 会場マシンでの自動再起動だけが目的なら、`--server` を外してください——ダッシュボードは不要です。ログはそれでもローカルに収集されます。

---

## 2. 別のマシンから監視する

サーバー（自宅のマシンや VPS）で `anchorbolt serve` を動かし、各会場の `start` をそこへ向けます：

```bash
# on the server
anchorbolt serve --data ./anchorbolt-data

# on each venue machine
anchorbolt start -p myApp --server http://192.168.1.10:54722 --id osaka-entrance
```

`--id` はウォールに表示される名前です（省略するとバイナリ名になります）。各会場はハートビート、30秒ごとのサムネイル、そしてログを送信します。カードをクリックすると**詳細ビュー**が開きます：より大きなライブサムネイル、fps／メモリのグラフ、イベント履歴、そして検索可能なログパネル。

![詳細ビュー — グラフとログ](images/detail.png)

---

## 3. 公開する前に鍵をかける

**ダッシュボードをインターネットに公開する前に、これを必ず行ってください。** オペレーターが1人も登録されていない状態では、ダッシュボードは開放されています——AI エンドポイントも同様で、つまり URL を知っている人なら誰でも会場を再起動できてしまいます。まず自分用の admin を作成しましょう：

```bash
# on the server
anchorbolt token operator new toru --role admin --data ./anchorbolt-data
# → prints op-... once. Paste it into the dashboard login.
```

これ以降、ダッシュボードはログインを求めます。その `op-...` トークン（または6桁のログインコード——ステップ7を参照）を貼り付ければログインできます。

![ログイン画面](images/login.png)

必要に応じて、会場側にも認証を求めることができます。そうすれば、偽のデータを POST されることを防げます：

```bash
anchorbolt token agent new osaka-entrance --data ./anchorbolt-data
# → prints tc-... once. Give it to the venue:
anchorbolt start -p myApp --server https://ops.example.com \
    --id osaka-entrance --token tc-...
```

ロールは3種類：**viewer**（読み取り専用）、**operator**（＋再起動／アップデート／コントロール）、そして **admin**（＋設定ページのすべて）。いずれも歯車アイコン →**Operators** タブから管理できます。

![設定 — Operators タブ](images/settings-operators.png)

---

## 4. 会場をグループ化し、クライアントの表示範囲を絞る

会場が数個を超えたら、グループにまとめましょう。歯車アイコン →**Apps** タブを開き、各会場の隣にグループ名を入力して Save します。

![設定 — グループ付きの Apps タブ](images/settings-apps.png)

ウォールにはグループごとのタブが増えていくので、「osaka」だけ、あるいは「tokyo」だけを見ることができます。

![グループタブ付きのウォール](images/wall.png)

クライアントに、**そのクライアントの**会場だけが見えるログインを渡すには、**スコープ**を指定してオペレーターを作成します——グループ名または `app:<id>` エントリをカンマ区切りで並べたものです：

```bash
anchorbolt token operator new gallery-client --role viewer --scope tokyo
```

これで `gallery-client` には tokyo グループだけが見え、それ以外の会場はすべて（AI エンドポイント経由も含めて）404 になります。スコープを持たないオペレーターはすべてを見られます。

---

## 5. 通知を受け取る（Slack、ntfy など）

会場の設定ファイルに `sinks` 配列を追加します。プリセットがテンプレートを埋めてくれます：

```jsonc
// anchorbolt.json on the venue machine
{
  "app": "./bin/myApp.app/Contents/MacOS/myApp",
  "id": "osaka-entrance",
  "server": "https://ops.example.com",
  "sinks": [
    { "preset": "slack", "urlFile": "slack.url" },
    { "preset": "ntfy",  "url": "https://ntfy.sh/my-venue-alerts" }
  ]
}
```

これで、クラッシュ、ハング、アップデートの失敗、あるいはアプリが上げたアラートがチャンネルに届きます：

> `[osaka-entrance] restart: app killed by signal 9; restarting`
> `[osaka-entrance] up: app healthy again (restart #1)`

Webhook の URL は秘密情報なので、gitignore したファイル（`urlFile`）か環境変数（`urlEnv`）に保管し、コミットされうる設定にインラインで書かないでください。`uptime-kuma` は特別なプリセットで、*正常な間だけ ping を送る*ため、無音になったときに Kuma が知らせてくれます。

アプリは、たった1行で自前のアラートを上げられます——センサーが抜けた、ヘルプボタンが押された、など：

```cpp
mcp::alert("IR camera disconnected!");
```

このメッセージは同じ sinks と、ダッシュボードのイベント一覧の両方へ流れます。

---

## 6. トンネル越しのリモートコントロール

どこからでも会場に到達できるようにするには、`serve` をリバースプロキシか Cloudflare トンネルの背後に置きます。ダッシュボードは素の HTTP（ポート 54722）ですが、インタラクティブな機能（Restart／Update ボタン、ライブビュー、リモートコントロール）は**別の WebSocket ハブ**（ポート 54723）を使います——なので、そこへのパスをルーティングしてください。

**cloudflared ingress**（同じホスト名をパスで振り分け）：

```yaml
ingress:
  - hostname: ops.example.com
    path: /ws
    service: ws://localhost:54723
  - hostname: ops.example.com
    service: http://localhost:54722
```

**会場側** — WS パスの場所をエージェントに伝え、コントロールを有効化します：

```bash
anchorbolt start -p myApp --server https://ops.example.com \
    --ws-url wss://ops.example.com/ws --allow-control
```

これで詳細ビューに **Live** ボタンが現れます。クリックすると画面を見られ、**control** をオンにすればアプリを操作できます——クリック、ドラッグ、キー入力がそのままアプリへ届きます。

![リモートコントロール付きのライブビュー](images/live.png)

リモートコントロールには、operator ロール（サーバー側）と `--allow-control`（会場側）の*両方*が必要で、さらにアプリが `mcp::registerDebuggerTools()` でオプトインしていなければなりません。監視だけなら HTTP のルートだけで足ります——`--ws-url` はインタラクティブな機能のためだけのものです。

> リモートアップデートも同じ形です：`--allow-update` を追加すると、詳細ビューの **Update** ボタンが、アプリを動かしたまま会場マシン上で `git pull` ＋リビルドを実行し、成功したときだけ切り替えます。

---

## 7. 6桁のコードで会場をペアリングする

`tc-...` の文字列を会場へコピーするのは間違いのもとです。代わりに、設定ページ（**Apps** タブ →*Pairing code*）で**ペアリングコード**を発行し、その6桁を現地にいる人へ読み上げます：

```bash
anchorbolt start -p myApp --server https://ops.example.com --pair 483201
```

このコード（有効期限10分、使い切り）は会場の本物のトークンと引き換えられ、そのトークンはマシン上に非公開で保存されます——だから次回以降の実行では `--pair` も `--token` も要りません。同じ仕組みが**ログインコード**にもなります：オペレーター用に1つ発行すれば、長いトークンを貼り付ける代わりに、6桁を入力するだけでサインインできます。

---

## 8. サービスとして常駐させる

恒久的なインストールでは、すべてを設定ファイルにまとめ、OS に起動させます。`anchorbolt.json` の隣で `anchorbolt start` を実行すると、自動でそれを読み込みます：

```jsonc
{
  "app": "./bin/myApp.app/Contents/MacOS/myApp",
  "args": ["--fullscreen"],
  "id": "osaka-entrance",
  "server": "https://ops.example.com",
  "wsUrl": "wss://ops.example.com/ws",
  "tokenFile": "osaka.token",
  "allowControl": true,
  "watchdogTimeout": 10,
  "sinks": [ { "preset": "slack", "urlFile": "slack.url" } ]
}
```

ログはデフォルトでプラットフォーム慣例の場所に出力されます（macOS では `~/Library/Logs/anchorbolt/<id>/`、Linux では `$XDG_STATE_HOME/anchorbolt/<id>/`、Windows では `%LOCALAPPDATA%\anchorbolt\<id>\`）——これは launchd/systemd 下で作業ディレクトリが `/` になっても問題ありません。`anchorbolt start` を launchd の plist か systemd のユニットで包めば完成です。AnchorBolt がアプリの面倒を見て、launchd/systemd が AnchorBolt の面倒を見ます。

すべてのフラグについては `anchorbolt --help` を実行してください。この裏側の設計については、[Architecture ドキュメント](ARCHITECTURE.ja.md)を読んでください。
