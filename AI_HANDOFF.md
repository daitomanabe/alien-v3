# AI_HANDOFF

> Contract: `portable-agent-handoff/v1`
> Captured: `2026-09-04T10:30:00+09:00`
> Scope: chrxh/alien フォーク上の生成庭園プロジェクト — CUDA 生態系をヘッドレス実行し、独自ビジュアル(墨インク)と SuperCollider ソニフィケーションをリアルタイム配信する制作環境の引き継ぎ。

## Status

- Overall: `VERIFIED`(**完成版凍結済み** — Meridian を自作庭の完成版とし、タグ `gardens-v1.0` で確定。機材は全停止、正典ファイルはリポジトリ内)
- Evidence freshness: current(2026-09-04 に実地確認: git クリーン @ a072c60ed、リモートに ALIEN プロセスなし、archive 31 スナップショット)
- Safe continuation: yes(残タスクは美学判断とライブ上演のみ。下記 Safety Boundaries の範囲で)

## Read First

1. `CLAUDE.md` — リポジトリ規約(言語・スタイル・ハード規則)。
2. `AI_HANDOFF.md` — 本書。
3. `V3-README.md` — アーキテクチャ図・起動手順・ツール一覧。運用の入口。
4. `GARDEN-NOTES.md` — 庭=生態系=音響の設計原理と**完成版宣言**・日長実験の結果。蘇生手順もここ。

Start by checking the real checkout, branch/worktree, and dirty state. Do not assume the path or branch named in an older note is still current.

## Source of Truth

| Area | Authoritative path or artifact | Evidence / conflict note |
| --- | --- | --- |
| ソースコード正本 | この Mac ローカルリポジトリ(`${PROJECT_ROOT}`、branch `v3`) | `[VERIFIED]` a072c60ed で origin/v3 と同期・クリーン(2026-09-04 確認) |
| リモートミラー | GitHub `daitomanabe/alien-v3`(private、branch `v3`、tag `gardens-v1.0`)+ upstream `chrxh/alien` | `[VERIFIED]` push 済み。リモートホスト上のソースは rsync コピー(編集しない) |
| **完成版の正典** | ローカル `scenes/meridian2.sim`(種)+ `meridian2-params.json` + `meridian2-ancient.sim`(最終形 47.45M steps)+ `meridian3-params.json` / `meridian3-final.sim`(日長実験対照) | `[VERIFIED]` git LFS でコミット済み(9bc9646df)。**リモートに依存しない** |
| 実験一次データ | ローカル `logs/meridian{2,3}-centroid.csv`(重心 1Hz) | `[VERIFIED]` コミット済み。日長実験の図の再生成が可能 |
| ビルド・実行場 | SSH エイリアス `mmmmm4090-ubuntu`(現行)/ `raytrek4090`(旧・予備) | `[VERIFIED]` 両方 `~/workspaces/alien-v3` にビルド済みバイナリ。mmmmm の alien_server は `--params` 対応版 |
| その他の育成シーン | リモート mmmmm `~/workspaces/alien-v3/scenes/`(wild-ancient / rain / islands / colosseum2 / vortex4 の各 mature 等)+ `archive/`(毎時スナップショット 31 件) | `[VERIFIED]` 2026-09-04 に存在確認。Meridian 以外はリモートのみ(消失リスク受容済み) |
| 種ライブラリ | リモート `garden-dump/creature-*.content` + `scenes/apex-predator.content` / `creature-32.content` | `[VERIFIED]` 本家 88 ゲノムの解読済み抽出物 |
| 完成テイク | ローカル `takes/`(git 管理外)、送付済みプレビューは `viz/*.mp4` | `[VERIFIED]` 組曲 v4 = `takes/suite-20260901-201738.mp4`(298.4 秒・5 楽章) |

## Current State

- `[VERIFIED]` **凍結状態(2026-09-04 実地確認)**: mmmmm に tmux セッションなし(ALIEN サーバ・keeper とも停止)、Mac のロガーも停止。ローカル git はクリーンで origin と同期。
- `[VERIFIED]` パイプラインは**全段リアルタイム**: alien_server(CUDA、TPS は `--tps` で任意)→ OSC 20Hz + ジオメトリ UDP 20Hz(サブスクライバ方式)→ Mac で moderngl 描画 + SuperCollider ライブ合成。公開済みテイクは全てライブキャプチャ。
- `[VERIFIED]` **解像度上限を実測済み**(M5 Max、30fps 収録込み): 3840² で 98fps、5120² でクリーン 62fps、6400² で劣化(19fps)、7680² 不可。表示のみなら任意の実在ディスプレイで vsync 律速。筆致はキャンバス追従に正規化済みで、どの解像度でも完成版 1716² と同じ見た目(a072c60ed)。
- `[VERIFIED]` ストリームはワールド座標なので解像度非依存(高解像度化してもリモート負荷ゼロ)。細部密度はサーバ `--geom-cells` / `--geom-fluid` で別途可変。
- `[VERIFIED]` 収録は完全無音(SC マスターをプライベートバスへ描画し `s.record(bus:)` でタップ)。録画は `--offscreen`(FBO・スロットル無縁)+ ultrafast エンコーダで pipe 詰まり根治済み。
- `[VERIFIED]` タイムラプスのフル解像度マスター(/tmp/m3-lapse.mp4、1.8GB)は**消滅済み**(/tmp 清掃)。送付済みの 60 倍圧縮版 `viz/meridian3-timelapse-60x.mp4` が現存する唯一の版。
- `[INFERRED]` raytrek4090 のワークスペースは最終同期時点のまま(旧環境)。再利用前に要確認。

## Completed and Verified

- `[VERIFIED]` **Meridian 完成版凍結(2026-09-02、タグ `gardens-v1.0`)** — 膜迷宮 + 巡る太陽/影 + パトロール嵐の庭。正典一式をローカルにコミットし、全機材を停止。
  - Evidence: git 9bc9646df(LFS 11MB)、tag `gardens-v1.0` がリモート反映済み、GARDEN-NOTES「完成版宣言」節。
- `[VERIFIED]` **4 つの科学的発見**(重心自己相関による検証、図は `sound/`):
  1. 生態は太陽を追う(v2 若年期 r(day)=0.30、半日反相関 -0.48)
  2. 長い日は追従を強める(日長 2 倍で r(day)=0.41)
  3. 追従はライフステージ現象(古代・飽和状態では移動追従が消え、その場の脈動 r=0.38 のみ。振幅 1/2.4)
  4. 位相遅れは一様に約 1/4 日(3 測定すべて 23〜26% 遅れ — 熱慣性の構図)
  - Evidence: `sound/meridian3-centroid.png` / `meridian2-ancient-centroid.png` / `meridian-age-compare.png`; 一次データ `logs/*.csv`; 再生成は `tools/centroid_analyze.py`。
- `[VERIFIED]` 日長実験は厳密対照(同一世界バイト、server `--params` オーバーライドで太陽・影の速度 4 項のみ差分を JSON diff で検証)。
- `[VERIFIED]` 解像度非依存レンダリング(オフスクリーンのデスクトップクランプ解除・筆致正規化・take.sh 基準固定)と上限実測。
  - Evidence: git a072c60ed のコミットメッセージに実測値; 5120² フレームの目視比較。
- `[VERIFIED]` 組曲 v4(雨→群島→wild→闘技場→晩鐘コーダ、全楽章 -18 LUFS、4:58)と aizuri 判断素材(fluid 6000)。
  - Evidence: `takes/suite-20260901-201738.mp4`(ffprobe 298.4s); 送付済み `viz/suite-v4-preview.mp4` / `viz/meridian2-aizuri-preview.mp4`。
- `[VERIFIED]` 移行・捕食・無音収録などの基盤(詳細は Change Log と git 履歴): mmmmm 移行(93/93)、自然捕食(NN 発火する頂点捕食者)、庭別サウンドプロファイル、サブスクライバ UDP、wild 11 時間観察(`sound/wild-lifetime.png`)。
- `[NOT_RUN]` EngineTests / EngineInterfaceTests 等の本家テストスイート。
  - Reason: 本家カーネル非改変(追加は描画抽出と `--params` ロードのみ)のため優先度を下げた; required before: upstream への貢献や engine 挙動に触れる変更。

## Pending Work

1. `[UNKNOWN]` 組曲 v4 のユーザー試聴と調整 — first check: 送付済み `viz/suite-v4-preview.mp4`(5 楽章・晩鐘コーダ、4:58)への反応。構成は `scripts/suite.sh` の GARDENS 配列で編集可能。
2. `[UNKNOWN]` aizuri ルックの採否 — first check: 送付済み `viz/meridian2-aizuri-preview.mp4`(fluid 6000・51 秒)への反応。採用ならサーバ `--geom-fluid 6000` 以上とセットで、プロファイル/庭の対応表を決める。
3. `[UNKNOWN]` ライブ上演セットアップ(未依頼・能力確認済み) — 蘇生 1 コマンド + viz ウィンドウ表示 + QUIET=0(**ユーザー明示時のみ**)。カタクリズムは OSC で演奏中に打てる。first check: ユーザーがライブ実演を求めているか。
4. `[UNKNOWN]` take.sh をパイプ経由で実行すると exit 144 で失敗する事象(ファイルリダイレクトでは常に成功)— first check: `bash scripts/take.sh 20 t 0 | tail` で再現するか。回避策(`> log 2>&1`)は確立済み・全スクリプト適用済み。

## Blockers and Decisions Needed

- `[UNKNOWN]` 美学の最終判断(組曲 v4 の尺・順序、aizuri 採否、ライブ上演の要否)— do not choose silently because 作品の署名者はユーザーであり、これまでの方向修正(「too obvious」等)は全てユーザー発だった。

## Reproduction / Verification

Working directory: `${PROJECT_ROOT}`

```sh
cd "${PROJECT_ROOT}"
git status --short --branch          # クリーン、v3 = origin/v3 のはず
git tag -l 'gardens*'                # gardens-v1.0
ls -la scenes/                       # meridian2.sim / *-params.json / *-ancient / *-final(LFS 実体)
# 凍結確認(何も走っていないこと)
scripts/alien-ctl.sh status          # STOPPED が期待値
```

庭の蘇生(必要になったときのみ):

```sh
bash scripts/alien-ctl.sh start meridian2-ancient.sim 800
viz/venv/bin/python viz/alien_viz.py --look ink        # ライブ表示(ウィンドウ)
# 無音テイク(パイプせずリダイレクトで)
bash scripts/take.sh 30 check 0 12000 12001 meridian > /tmp/take-out.log 2>&1; tail -2 /tmp/take-out.log
# 高解像度収録(〜5120² まで実測クリーン)
viz/venv/bin/python viz/alien_viz.py --look ink --offscreen --width 2560 --record /tmp/hi.mp4 --exit-after 30
```

Expected result: 凍結確認では status が STOPPED。蘇生後は viz が `world from server: 3000x3000` と `static structure: N points` を出力し、take が `take written: takes/check-*.mp4` で終了、スピーカーは無音。

Checks not run: `[NOT_RUN]` 本家テストスイート(前述)。`[NOT_RUN]` raytrek4090 での再稼働確認 — 移行後未検証。`[NOT_RUN]` LFS クローンの初回取得検証(別マシンでの `git lfs pull` 動作)。

## Safety Boundaries

- `[VERIFIED]` Preserve: ローカル `scenes/`(完成版正典・LFS)、`logs/*.csv`(実験一次データ)、`takes/`(完成テイク、git 管理外)、リモート mmmmm の `scenes/` と `archive/`(Meridian 以外の庭はリモートが唯一の実体)。
- `[VERIFIED]` Preserve: 他プロジェクトのプロセス — raytrek の `hf-gpu-queue` / `hf-model-tester-comfyui`。mmmmm に現在 tmux セッションはないが、プロジェクト外のセッションが現れたら触らない。
- `[VERIFIED]` `external/vcpkg` はピン済みサブモジュール — 変更・コミット禁止(CLAUDE.md ハード規則)。動画・AIFF の大物は git に入れない(.sim の LFS 管理は例外として確立済み)。
- `[BLOCKED]` Approval required before: スピーカーから音を出す収録・上演(QUIET=0 はユーザー明示時のみ)、GitHub リポジトリの公開設定変更、upstream への push/PR。
- `[UNKNOWN]` Do not assume: リモートホストの sudo 権限(ユーザー実行が必要)、Tailscale 共有ノードの逆方向フロー(ホスト発は Mac に届かない — サブスクライバ方式を崩さない)。

## Next Agent

Use this sequence:

1. Read the instruction files and the source-of-truth paths above.
2. Recheck the current checkout and dirty state.
3. Run the frozen-state verification block above.
4. Nothing is running and nothing is owed: continue only from **Pending Work**(全て「ユーザーの反応待ち」か「依頼待ち」)or a new user request.
5. Update this handoff with new evidence before ending the task.

Do not treat the conversation summary as a substitute for this document. Do not claim the work is complete until the stated acceptance evidence exists.

## Change Log

| Timestamp | Agent / session label | Change | Evidence |
| --- | --- | --- | --- |
| `2026-08-31` | `Claude Code` | initial capture(移行・5庭・組曲完成時点) | git ab7b98cf7 |
| `2026-08-31` | `Claude Code` | wild 長期観察を稼働、組曲 v3、aizuri ルック、rain/islands シーン取り違え修正、ctl の env 化 | git afd774b69 |
| `2026-09-01` | `Claude Code` | 6時間セッション: Meridian v1→v2(動くゾーン初使用)、太陽追従の実証、wild 11時間回収、--offscreen 録画 | git a3c40aa40; `sound/meridian2-centroid.png` |
| `2026-09-01 夜` | `Claude Code` | 組曲 v4(晩鐘コーダ)、aizuri 素材、server `--params`、日長実験開始、centroid ツール恒久化、keeper 複数庭対応、録画 pipe 詰まり根治 | git cb321fcc6; `takes/suite-20260901-201738.mp4` |
| `2026-09-01 深夜` | `Claude Code` | 日長実験収穫: 4 発見を確定(強化 0.41 / 老化 / 位相 1/4 日)、比較図・遅い太陽タイムラプス送付 | git cf62262e7; `sound/meridian-age-compare.png` |
| `2026-09-02` | `Claude Code` | **完成版凍結**: 正典 5 ファイルを LFS コミット、全機材停止、タグ `gardens-v1.0` | git 9bc9646df; tag gardens-v1.0 |
| `2026-09-02` | `Claude Code` | 解像度非依存化と上限実測(5120² 収録クリーン、表示は vsync 律速)。クランプ・筆致・take.sh 基準を修正 | git a072c60ed |
| `2026-09-04` | `Claude Code` | handoff 全面更新(凍結後の実地確認: git クリーン、リモート無プロセス、archive 31 件、/tmp マスター消滅を記録) | 本書 |
