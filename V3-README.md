# ALIEN v3 — 独自ビジュアル + SuperCollider ソニフィケーション

[chrxh/alien](https://github.com/chrxh/alien)(CUDA 人工生命シミュレータ)のフォーク。
シミュレーションのアルゴリズムは本家のまま、**見た目と音を外部で再解釈する**構成。

```
raytrek4090 (RTX 4090)                          Mac
┌──────────────────────────┐          ┌──────────────────────────────┐
│ alien_server (headless)  │          │                              │
│  CUDA simulation ~1000TPS│  OSC/UDP │ SuperCollider                │
│  ├ 統計・系統・捕食イベント ──────────▶│  sound/alien-sonification.scd│
│  └ ジオメトリ 4000点/frame ──────────▶│ viz/alien_viz.py (moderngl)  │
│    (12B/点, ~35chunk)    │ 947KB/s  │  加算グロー + 残像トレイル      │
└──────────────────────────┘          └──────────────────────────────┘
```

## ネットワーク(重要)

raytrek4090 は共有 tailnet ノードのため **GPU 側から Mac への新規フローは落ちる**。
そこで受信側が UDP で任意のデータグラムを送って購読する方式(サーバは最後に
hello をくれた送信元へ返信ストリームを流す。keepalive 10 秒)。

- OSC 購読: udp/12000(sclang は langPort 57120 のソケットから送る → 返信も 57120 へ)
- ジオメトリ購読: udp/12001

## 起動手順(すべて Mac の CLI から)

```bash
scripts/alien-ctl.sh start [scene] [tps]   # リモートでサーバ起動(tmux)
scripts/alien-ctl.sh status                # 状態確認
scripts/alien-ctl.sh log 30                # サーバログ
scripts/alien-ctl.sh cataclysm 3           # カタクリズム発動(捕食・爆発を誘発 → 音と朱ブロット)
scripts/alien-ctl.sh stop                  # 停止
```

受け手(それぞれ独立に購読・同時可):

```bash
sclang sound/alien-sonification.scd                  # 音
viz/venv/bin/python viz/alien_viz.py                 # 映像(インク・ルック。--look glow で発光系)
viz/venv/bin/python sound/param_monitor.py           # 音響パラメータのリアルタイムグラフ
```

`param_monitor.py --headless --export out.png --exit-after 60` で GUI なしの記録も可能。
`--tps` はシミュレーション速度=作品のテンポ(0 で無制限、4090 で約 1000 TPS)。

## 音のマッピング(sound/alien-sonification.scd)

| シミュレーション | 音 |
|---|---|
| 系統(上位8) | ドローン声部(スロット制) |
| 個体数 | 音量 |
| 平均世代(進化の深さ) | ピッチ上昇 |
| 筋活動 delta | トレモロ/シマー |
| 捕食エネルギー delta | ノイズ成分(grit) |
| 変異 delta | デチューン |
| 系統の色 | ピッチクラス |
| 攻撃イベント(位置つき) | クリック(x→パン、y→音域) |
| 爆発イベント | 低域インパクト |
| 全体エネルギー | マスターフィルタ開閉(ドローンのみ。イベント音はトランジェント保持) |

同じ OSC ストリームを `sound/param_monitor.py` が購読し、上記の全パラメータを
マッピング名つきでリアルタイムプロットする(sound/param-graph-sample.png 参照)。
シグナルフロー: drones → LPF ─┬→ FreeVerb2 → Limiter → out
                events ────────┘  (ReplaceOut のバスを分離。イベントを直接 out 0 に
                                   出すと master の ReplaceOut に消されるので注意)

## 見た目(viz/alien_viz.py)

- `--look ink`(デフォルト): 生成り紙に Beer-Lambert 減法混色の墨。セル=墨点(系統色を
  微かに含む)、接続線=骨格ストローク、流体=薄い霞、**捕食=朱のブロット**、
  にじみブラー付き残像。オリジナルのネオン調と正反対の版画的ルック。
- `--look glow`: 黒地に加算グロー(比較用)。
- 幾何ストリームは点(12B)+ 線分(20B)の2タイプ、チャンク UDP(欠損許容)。

## ビルド(リモート)

```bash
cmake --preset ninja -DCMAKE_CUDA_ARCHITECTURES=89   # 4090 = sm_89(デフォルトの 75;120 は CUDA 12.1 で不可)
cmake --build --preset ninja-release --target alien_server
```

GCC 11 ホスト対応のため `std::views::zip` は `aliencompat::zip`(source/Base/ZipCompat.h)、
`std::format` は ostringstream に置換済み。

## エンジンへの追加(upstream との差分)

- `EngineInterface/HostRenderData.h` — ホスト側ジオメトリコンテナ
- `CudaGeometryBuffers::copyToHost` — no-interop 経路の D2H コピー(OpenGL 不要)
- `SimulationCudaFacade::extractRenderDataToHost` / `SimulationFacade::tryExtractRenderDataToHost`
- `source/Server/` — alien_server 本体(OSC エンコーダ・ジオメトリストリーマ込み、依存追加なし)

## 庭を自作する(alien_genesis + garden_env)

```bash
# 1. 既存シーンを解剖 → parameters.json + 種ライブラリ(creature-*.content / genome-*.genome)
./alien_genesis dump -i scenes/hanging-garden.sim -o garden-dump --top 4

# 2. 環境を自分の空間設計に書き換え(生態系バランスは継承、11ゾーンの配置・力場を再設計)
python3 tools/garden_env.py --base garden-dump/parameters.json \
  --out scenes/vortex-params.json --design vortex --world 3000x3000

# 3. プロシージャル生成(spiral: アルキメデス螺旋 + 内向きの蔓 + 中心寄りの霧)
./alien_genesis new -o scenes/my-garden.sim \
  --params scenes/vortex-params.json --layout spiral --world 3000x3000 --turns 2.6 \
  --seed garden-dump/creature-0.content --seed garden-dump/creature-1.content \
  --seeds 12 --tendril-length 80 --fluid 70000 --energy 2500 --rng 7
```

- レイアウト: `--layout shelves`(波打つ棚、Hanging Garden 型)/ `--layout spiral`(渦)
- **形態の自作**: `--body-shape keep,hexagon,zigzag,tube --body-nodes 0,5,4,6` — 種ゲノムの
  遺伝子形状(体制)とノード数を株ごとに巡回適用。v5 の種は「Base ノード + セル付帯の
  constructor + 色別外部エネルギー流入」で育つため、形の差し替えだけで別形態の種族になる
  (segment=茎 / hexagon=団子コロニー を実験で確認済み)
- 環境デザイン vortex: 全域 Perlin の呼吸 + 中心の Radial 渦 + コア乱流 + 螺旋腕の風3つ。
  food chain・エネルギー経済・変異率は dump 元から継承(生態系が成立しやすい)
- 種は元シーンの単細胞生物(2遺伝子)。播種時に株ごとに lineageId(=音の声部)と色
  (=ピッチクラス/差し色)を振り分ける
- 世界サイズはサーバが毎 tick `/alien/world`(OSC)+ ジオメトリ info パケットで通知し、
  SC / viz は自動追従する(viz はウィンドウ生成前にサーバの world を待つ)
- 壁など静的構造は type=3 チャンネルで**全点を1回だけ**配信(エポック方式、30秒ごと再送)。
  受信側がキャッシュするので毎フレームのサンプリングは動く物に集中する
- 育った庭の保存: `scripts/alien-ctl.sh save` → サーバ側 `saved-<timestep>.sim`
- 抽出時に生物→棚のアンカー接続は切断される(参照先が無くなり
  `DescConverterService::setConnections` が落ちるため)

## シーン

alien-project.org の公式ブラウザ API から取得(現行 alpha.26 対応版):

```bash
curl -X POST https://api.alien-project.org/getversionedsimulationlist -d version=5.0.0
curl "https://api.alien-project.org/downloadcontent?id=47" -o scenes/hanging-garden.sim
```

- `scenes/hanging-garden.sim` — id 47, Evolution Presets/Hanging Garden (chrxh)
- `scenes/vents-initial.sim` — id 50, Hydrothermal Vents 初期状態(軽量)

旧 alpha.19 の .sim(過去プロジェクトの garden.sim 等)は現行コードでは読めない。
