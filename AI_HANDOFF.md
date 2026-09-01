# AI_HANDOFF

> Contract: `portable-agent-handoff/v1`
> Captured: `2026-09-01T12:00:00+09:00`
> Scope: chrxh/alien フォーク上の生成庭園プロジェクト — CUDA 生態系をヘッドレス実行し、独自ビジュアル(墨インク)と SuperCollider ソニフィケーションをリアルタイム配信する制作環境の引き継ぎ。

## Status

- Overall: `IN_PROGRESS`(基盤・5つの庭・組曲まで完成。次は長期進化観察と楽曲設計)
- Evidence freshness: current(2026-08-31 時点。リモートサーバの走行状態のみ揮発)
- Safe continuation: yes(下記 Safety Boundaries の範囲で)

## Read First

1. `CLAUDE.md` — リポジトリ規約(言語・スタイル・ハード規則)。
2. `AI_HANDOFF.md` — 本書。
3. `V3-README.md` — アーキテクチャ図・起動手順・ツール一覧。運用の入口。
4. `GARDEN-NOTES.md` — 庭=生態系=音響の設計原理(分散・色経済・神経回路・季節)。**新しい庭を作る前に必読**。

Start by checking the real checkout, branch/worktree, and dirty state. Do not assume the path or branch named in an older note is still current.

## Source of Truth

| Area | Authoritative path or artifact | Evidence / conflict note |
| --- | --- | --- |
| ソースコード正本 | この Mac ローカルリポジトリ(`${PROJECT_ROOT}`、branch `v3`) | `[VERIFIED]` ab7b98cf7 で origin/v3 と同期・クリーン |
| リモートミラー | GitHub `daitomanabe/alien-v3`(private、branch `v3`)+ upstream `chrxh/alien` | `[VERIFIED]` push 済み。リモートホスト上のソースは rsync コピー(編集しない) |
| ビルド・実行場 | SSH エイリアス `mmmmm4090-ubuntu`(現行)/ `raytrek4090`(旧・予備) | `[VERIFIED]` 両方 `~/workspaces/alien-v3` にビルド済みバイナリ |
| 育成済みシーン | リモート `~/workspaces/alien-v3/scenes/*-mature.sim` ほか | `[VERIFIED]` mmmmm に wild-mature / rain-mature / islands-mature / colosseum2-mature / vortex4-mature 等 |
| 種ライブラリ | リモート `garden-dump/creature-*.content` + `scenes/apex-predator.content` / `creature-32.content` | `[VERIFIED]` 本家 88 ゲノムの解読済み抽出物 |
| 完成テイク | ローカル `takes/`(git 管理外)、プレビューは `viz/*.mp4` | `[VERIFIED]` 組曲 `takes/suite-20260831-171938.mp4`(173 秒) |

## Current State

- `[VERIFIED]` パイプライン全体が mmmmm4090-ubuntu で稼働: alien_server(ヘッドレス CUDA)→ OSC(統計/系統/捕食イベント)+ ジオメトリ UDP → Mac の SuperCollider + moderngl レンダラ。世界サイズはサーバが通知し受信側が自動追従。
- `[VERIFIED]` ネットワークはサブスクライバ方式(受信側が hello を送り、サーバは送信元へ返信配信)。理由: 両 GPU ホストは共有 tailnet ノードで、ホスト発の新規フローは Mac に届かない。
- `[VERIFIED]` 庭カタログ 5 種(vortex / colosseum2 / rain / islands / wild)。wild が最新の到達点: fBm ノイズ地形(幾何学の見えない自然配置)、45 株 → 49 系統に変異分岐、頂点捕食者による自然捕食が定常化。
- `[VERIFIED]` 収録は完全無音(SC マスターをプライベートバスへ描画し `s.record(bus:)` でタップ。スピーカーには何も出ない)。ブートは現在の出力デバイスに追従(44.1kHz 強制は 48kHz デバイスで scsynth が静かに死ぬため撤廃)。
- `[VERIFIED]` mmmmm 上の tmux `alien-srv` は colosseum2-mature(組曲収録の最終ロード)を 400 TPS で走行中。timestep 約 141 万。
- `[INFERRED]` raytrek4090 のワークスペースは最終同期時点のまま利用可能(サーバは全停止済み)。根拠: 停止コマンドの成功ログ。再利用前に要確認。
- `[STALE]` リモートの走行状態・timestep は本書執筆時点の値。参照前に `scripts/alien-ctl.sh status` で再確認すること。

## Completed and Verified

- `[VERIFIED]` mmmmm4090-ubuntu への移行(ビルド 93/93、GCC 13 ネイティブ、専有 800 TPS)。
  - Evidence: リモート `build.log` の `BUILD_EXIT=0`; Mac から world announce + static 37,353 点の受信。
- `[VERIFIED]` 自然捕食の達成(colosseum2 / wild)。裸の Attacker ノードは NN 発火信号が無く機能しない — 本家の頂点捕食者(進化済み神経回路)を植えるのが正解。
  - Evidence: サーバログ `attacks 4` 初出 timestep 39,781(カタクリズムなし); 42 分の全期間グラフ `sound/colosseum2-evolution.png`。
- `[VERIFIED]` 庭ごとのサウンドプロファイル(default/colosseum/rain/islands/wild — クリック/水滴/木/土)と 4 楽章の組曲。
  - Evidence: `takes/suite-20260831-171938.mp4`(ffprobe 173.3 秒); sclang ログ `garden profile: wild`。
- `[VERIFIED]` 無音収録での正常テイク。
  - Evidence: 直近テイク EXIT=0、AIFF 60 秒、SC ブートログ `sample rate = 48000`。
- `[NOT_RUN]` EngineTests / EngineInterfaceTests 等の本家テストスイート。
  - Reason: エンジン改造は描画抽出経路の追加のみで本家カーネルは非改変のため優先度を下げた; required before: upstream への貢献や engine 挙動に触れる変更。

## Pending Work

1. `[VERIFIED->収穫済]` wild 長期観察: 31.7M steps / 11 時間の完全ログを回収、系統 45..58 の動的平衡を `sound/wild-lifetime.png` に可視化。最終形 = `scenes/wild-ancient.sim`。現在のメインは **Meridian v2**(巡る太陽の庭、`scenes/meridian2.sim`、`logs/meridian2.log`)— 生態の太陽追従を重心自己相関で実証済み(`sound/meridian2-centroid.png`)。keeper は `scripts/keeper.sh`(tmux クォート壊れを修正)。
2. `[UNKNOWN]` 組曲 v3 のユーザー試聴と調整 — first check: `takes/suite-20260831-214722.mp4`(rain→islands→wild→colosseum、loudnorm 済み)を聴き、尺・順序・カタクリズム量の好みを確認する。構成は `scripts/suite.sh` の GARDENS 配列で編集可能。
3. `[UNKNOWN]` aizuri ルックの採否と調整 — first check: `viz/aizuri-rain.png` を見る。藍が読めるのは fluid リッチ配信(サーバ `--geom-fluid 6000` 以上)とセット。採用ならプロファイル/庭との対応表を決める。
4. `[UNKNOWN]` take.sh をパイプ経由で実行すると exit 144 で失敗する事象(ファイルリダイレクトでは常に成功)— first check: 再現するか `bash scripts/take.sh 20 t 0 | tail` を試し、再現すればハーネスのプロセスグループ挙動を疑う。回避策(`> log 2>&1` 実行)は確立済み。

## Blockers and Decisions Needed

- `[UNKNOWN]` 美学の最終判断(どの庭・どのテイクを「作品」とするか、色彩・音の好み)— do not choose silently because 作品の署名者はユーザーであり、これまでも「too obvious」等の方向修正はユーザー発だった。
- `[UNKNOWN]` mmmmm 上の tmux セッション `0`(2026-08-30 作成、本プロジェクト外)— 触らない。所有者不明のため。

## Reproduction / Verification

Working directory: `${PROJECT_ROOT}`

```sh
cd "${PROJECT_ROOT}"
git status --short --branch
# リモート稼働確認(デフォルトは mmmmm4090-ubuntu。ALIEN_HOST/ALIEN_IP で切替)
scripts/alien-ctl.sh status
# 庭のロード(例: wild の成熟状態、TPS はテンポ)
scripts/alien-ctl.sh start wild-mature.sim 500
# 見る(world 自動追従・静的構造キャッシュ受信をログで確認)
viz/venv/bin/python viz/alien_viz.py --look ink
# 無音で 30 秒テイク(プロファイル wild)。パイプせずリダイレクトで実行すること
bash scripts/take.sh 30 check 0 12000 12001 wild > /tmp/take-out.log 2>&1; tail -2 /tmp/take-out.log
```

Expected result: status が RUNNING と直近ログ行を表示。viz が `world from server: ...` と `static structure: N points` を出力。take が `take written: takes/check-*.mp4` で終了しスピーカーは無音。

Checks not run: `[NOT_RUN]` 本家テストスイート(EngineTests 等)— 上記のとおり。`[NOT_RUN]` raytrek4090 での再稼働確認 — 移行後未検証。

## Safety Boundaries

- `[VERIFIED]` Preserve: リモート両ホストの `scenes/*.sim`(育成済み資産・再取得不能)、`garden-dump/` 種ライブラリ、ローカル `takes/`(完成テイク、git 管理外)。
- `[VERIFIED]` Preserve: 他プロジェクトのプロセス — mmmmm の tmux `0`、raytrek の `hf-gpu-queue` / `hf-model-tester-comfyui`。kill しない。
- `[VERIFIED]` `external/vcpkg` はピン済みサブモジュール — 変更・コミット禁止(CLAUDE.md ハード規則)。動画・AIFF の大物は git に入れない(.gitignore 済み。71MB 誤コミットの前科あり)。
- `[BLOCKED]` Approval required before: スピーカーから音を出す収録(ユーザーが無音を要求済み。QUIET=0 はユーザー明示時のみ)、GitHub リポジトリの公開設定変更、upstream への push/PR。
- `[UNKNOWN]` Do not assume: リモートホストの sudo 権限(ユーザー実行が必要)、Tailscale 共有ノードの逆方向フロー(ホスト発は Mac に届かない — サブスクライバ方式を崩さない)。

## Next Agent

Use this sequence:

1. Read the instruction files and the source-of-truth paths above.
2. Recheck the current checkout and dirty state.
3. Run the first focused verification command.
4. Continue item 1 under **Pending Work** only if its acceptance condition remains true.
5. Update this handoff with new evidence before ending the task.

Do not treat the conversation summary as a substitute for this document. Do not claim the work is complete until the stated acceptance evidence exists.

## Change Log

| Timestamp | Agent / session label | Change | Evidence |
| --- | --- | --- | --- |
| `2026-08-31` | `Claude Code` | initial capture(移行・5庭・組曲完成時点) | git ab7b98cf7 / 本書の VERIFIED 項目 |
| `2026-08-31` | `Claude Code` | wild 長期観察を稼働(自動セーブ付き)、組曲 v3(楽曲構成・loudnorm)、aizuri ルック、rain/islands シーン取り違え修正(両ホスト)、ctl のセッション/ポート env 化 | git afd774b69; suite `takes/suite-20260831-214722.mp4` |
| `2026-09-01` | `Claude Code` | 6時間セッション: Meridian(巡る太陽+膜迷宮、動くゾーン初使用)v1→v2、太陽追従を重心自己相関で実証、種分化 64 系統→46 の一往復を記録、wild 11時間の生涯回収(wild-ancient)、--offscreen 録画(スロットル根絶)、meridian 鐘プロファイル | git a3c40aa40; `sound/meridian2-centroid.png` / `sound/wild-lifetime.png`; saves: meridian2-mature / meridian1-young / wild-ancient |
