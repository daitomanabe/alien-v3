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

## 起動手順

1. リモートでサーバ起動:
   ```bash
   ssh raytrek4090
   cd ~/workspaces/alien-v3/build-ninja/Release
   ./alien_server -i ../../scenes/hanging-garden.sim --rate 20 --tps 200
   ```
   `--tps` はシミュレーション速度(=音と映像のテンポ)。無制限なら 0(4090 で約 1000 TPS)。

2. Mac で音:
   ```bash
   sclang sound/alien-sonification.scd
   ```

3. Mac で映像:
   ```bash
   viz/venv/bin/python viz/alien_viz.py            # world 5000x1500 がデフォルト
   ```

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
| 全体エネルギー | マスターフィルタ開閉 |

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

## シーン

alien-project.org の公式ブラウザ API から取得(現行 alpha.26 対応版):

```bash
curl -X POST https://api.alien-project.org/getversionedsimulationlist -d version=5.0.0
curl "https://api.alien-project.org/downloadcontent?id=47" -o scenes/hanging-garden.sim
```

- `scenes/hanging-garden.sim` — id 47, Evolution Presets/Hanging Garden (chrxh)
- `scenes/vents-initial.sim` — id 50, Hydrothermal Vents 初期状態(軽量)

旧 alpha.19 の .sim(過去プロジェクトの garden.sim 等)は現行コードでは読めない。
