# ALIEN CUDA → Metal 移植計画

対象: https://github.com/chrxh/alien (develop, v5.0.0-alpha 系)
目標: Apple Silicon (M1以降 / 開発機 M5) でエンジン・GUI をネイティブ動作させる
方針: upstream が HIP でやったのと同じ「第3バックエンド」として `USE_METAL` を追加し、
デバイスコードは可能な限り CUDA と共通ソース化する（フォークの乖離を最小化）

---

## 1. 調査結果サマリー（2026-07-05 時点の develop HEAD = 3eba952）

### 移植に有利な事実
| 項目 | 状況 | Metal 対応 |
|---|---|---|
| Dynamic parallelism | **不使用**（全カーネルはホスト起動、KERNEL_CALL マクロ集約） | 問題なし |
| Warp intrinsics (__shfl等) | 0件 | 問題なし |
| Texture / surface | 0件 | 問題なし |
| cuRAND / Thrust / cuBLAS | 不使用（乱数は事前生成テーブル + atomicInc） | 問題なし |
| Unified/pinned memory, multi-GPU, events | 不使用 | 問題なし |
| ホスト側 CUDA API | 約30関数のみ（malloc/memcpy/stream/graph/interop/symbol） | 全て対応先あり |
| float atomicAdd | 26箇所（エネルギー・速度・力の累積） | **M1+ は atomic_float fetch_add ネイティブ対応（MSL 2.4+）** |
| __shared__ / __syncthreads | 74 / 14ファイル | threadgroup / threadgroup_barrier に1:1対応 |
| __constant__ | SimulationParameters 等 2シンボルのみ | constant バッファ + setBytes |
| CUDA Graphs | タイムステップを設定キー毎にキャプチャ・キャッシュ | Metal はエンコード自体が軽い。毎フレーム再エンコードで開始、必要なら ICB |
| テスト | EngineTests 約50スイートが approxCompare（許容誤差）方式 | **そのまま Metal 版の検証装置になる** |
| GUI | GLFW + ImGui。macOS 用 GL3.2 分岐が既に存在 | GLFW は Metal と共存可（GLFW_NO_API） |
| レンダリング | CUDA-GL interop だが **no-interop フォールバック実装済み**（ホスト経由コピー） | 段階移行に利用可能 |
| HIP 前例 | external/hip/cuda_to_hip.h（force-include 変換ヘッダー） + USE_HIP CMake 分岐 | 設計パターンとして踏襲 |

### 難所（設計が必要な箇所）
1. **MSL のアドレス空間修飾**: デバイスコード内の全ポインタ/参照に `device` 等の修飾が必要。
   約19k行 × 全ポインタ宣言に及ぶ機械的変換 → マクロ（例 `GPTR(T)`）で共通ソース化する。
   CUDA ビルドでは空定義、Metal ビルドでは `device` に展開。
2. **64bit atomics 不在**: `Array::_numEntries`(uint64_t) の atomicAdd64。
   → オブジェクト系カウンタは 32bit 化で十分（実用上 ~10^7 個）。
   Heap（バイト単位, 最大数GB）のみ「16Bアライン単位カウント」にして uint32 × 16B = 64GB 上限を確保。
3. **double 2箇所**: `SimulationData::externalEnergy`（double の atomicAdd！）と MaxAgeBalancer。
   Metal は FP64 非対応 → タイムステップ内デルタを atomic_float に集約し、
   ステップ末尾の 1-thread カーネルで hi/lo split-float（Kahan）に合算する2段方式に再設計。
4. **HashMap のスピンロック** (`while(atomicExch_block(...))`): Apple GPU は SIMD グループ内の
   独立前進保証がなく、同一 simdgroup 内で待ち合うとデッドロックの可能性。
   Entities.cuh の tryLock（失敗時スキップ）は安全、HashMap::getLock（待機ループ）は要再設計。
5. **Geometry Shader 8本**: Metal に GS はない。全て「点→クアッド」「線→クアッド」の定型展開
   → インスタンシング + 頂点プル（vertex_id/instance_id から device バッファを読む）で置換。
6. **C++17/20 機能**: if constexpr, std::decay_t 等（主に MutationProcessor.cuh）、constinit。
   Metal コンパイラ(AppleClang系)は実際には多くの C++17 を受けるが、std:: → metal:: の
   type_traits シムを用意。ホスト混在ヘッダー（std::map 等を含む .cuh）はホスト/デバイス分離が必要。
7. **DEVICE_CHECK / printf デバッグ**: ABORT()（nullポインタ書込）は不可
   → エラーフラグバッファ + コマンドバッファ完了時のホスト検査に置換。os_log も利用可。

### コード規模
- デバイスコード: source/EngineKernels/ ~19,150行、__global__ カーネル **約149本**
  （Simulation 46 / Edit 30 / DataAccess 28 / GC 15 / Geometry 10 / Selection 6 / Statistics 6 / その他）
- ホスト側 GPU 制御: source/EngineImpl/ ~6,000行（*KernelsService 群 + SimulationCudaFacade）
- GLSL シェーダー: source/Shaders/ ~2,750行（VS/FS/GS 文字列ヘッダー）

---

## 2. アーキテクチャ戦略

### 2.1 バックエンド構成
```
                     EngineInterface (変更なし)
                            │
                   SimulationCudaFacade 相当
                            │
        ┌──────────────┬────┴─────────┐
      CUDA           HIP (既存)      Metal (新規)
   nvcc 単一ソース   cuda_to_hip.h   ┌ ホスト: metal-cpp (純C++, ObjC最小)
                                     └ デバイス: 共通 .cuh を MSL としてコンパイル
                                        + external/metal/cuda_to_metal.h シム
```

- **共通ソース戦略**: EngineKernels の .cuh 群（Processor 群・Math・データ構造）を
  CUDA / MSL 両方でコンパイルできるようにする。CUDA 固有部（atomicAdd, __syncthreads,
  printf, __device__ 修飾, アドレス空間マクロ）はシムヘッダーで吸収。
  → upstream の活発な開発（ほぼ毎日コミット）に追従しやすい。
- ホスト側は `Metal*KernelsService` 群を新規実装（既存 *KernelsService と同一インターフェース）。
  metal-cpp を external/ に追加し、Objective-C++ は ImGui バックエンド等最小限に留める。
- MSL コンパイル: ビルド時に xcrun metal で .metallib 化（CMake カスタムコマンド）。
  149 カーネルは系統別に複数 .metallib へ分割しビルド時間を管理。

### 2.2 メモリ・ポインタグラフ
- Object→Object / Cell→Creature→Genome の生ポインタ網は、**単一 MTLHeap** から全確保し
  ヒープ全体を useHeap / residency set で常駐化すれば GPU アドレスは安定・有効。
- CudaMemoryManager と同位置に MetalMemoryManager（MTLHeap + サブアロケータ）を実装。
- CPU↔GPU 転送（TO 群）: Apple Silicon は UMA なので storageModeShared で memcpy に単純化。

### 2.3 レンダリングの2段階移行（リスク分割の要）
- **段階A（暫定）**: エンジンだけ Metal 化し、描画は既存 OpenGL 3.2 パスを継続。
  interop の代わりに既存 no-interop フォールバック（GPU→ホスト→glBufferData）を使う。
  macOS の GL は非推奨だが 4.1 まで動作する。→ エンジン移植と描画移植を独立に検証できる。
- **段階B（本命）**: Metal ネイティブレンダラー。
  - ImGui: imgui_impl_metal + imgui_impl_glfw（vcpkg imgui[metal-binding]）
  - GS 8本 → インスタンス展開 VS（1プリミティブ=1インスタンス、4〜20頂点）
  - ポストプロセス群（Blur/DeNoise/ToneMapping/Metaballs/...）→ MSL フルスクリーンパス（機械変換）
  - FBO RGBA16F → MTLPixelFormatRGBA16Float
  - **ゼロコピー**: エンジンの MTLBuffer を頂点プルで直接描画（interop 概念自体が消滅）

---

## 3. フェーズ計画

### Phase 0: macOS ビルド基盤（推奨: Opus 4.8）
- vcpkg + CMake を macOS (arm64) で通す。GPU 非依存ターゲット（Base, Network, PersisterImpl,
  EngineInterface, Gui, Cli）を先にビルド。engine はスタブ実装で GUI 起動まで。
- WinReg / _WIN32 分岐の macOS 対応（既に Linux 分岐あり、差分は小さい）
- **ゲート**: alien 起動、ImGui UI 表示（シミュレーションなし）。
  EngineInterfaceTests / NetworkTests / PersisterTests がパス。

### Phase 1: Metal ランタイム基盤 + 共通ソースシム（推奨: Fable 5 ★最重要設計）
- `external/metal/cuda_to_metal.h` 設計: アドレス空間マクロ体系（GPTR 等）、atomic シム、
  sync シム、half/float2 演算、type_traits マッピング、DEVICE_CHECK→エラーバッファ
- 難所 1〜4, 7 の設計判断（32bit カウンタ化、split-float 集約、HashMap ロック再設計）
- ホスト基盤: MetalContext / MetalMemoryManager / KernelDispatcher（PSO キャッシュ +
  STREAM_KERNEL_CALL 相当）/ ConstantMemory 相当
- アドレス空間マクロの全 .cuh への適用（機械変換は Sonnet 5 に委任、設計レビューは Fable 5）
- **ゲート**: Array / HashMap / NumberGenerator の単体 Metal カーネルテストがパス。
  CUDA ビルドが無変更で通ること（シム適用後も NVIDIA 側は byte-for-byte 同等）。

### Phase 2: 全カーネル移植（推奨: Opus 4.8 主体、難カーネルのみ Fable 5）
- 系統別に移植・検証: GC → DataAccess → Simulation 物理 → cellType 群（Constructor/
  Mutation が最難）→ Edit → Statistics → Selection → Geometry
- MetalSimulationFacade + Metal*KernelsService 完成、タイムステップループ稼働
- **ゲート**: EngineTests 全スイート（約50、approxCompare）を Metal バックエンドでパス
- 備考: ConstructorProcessor / MutationProcessor（block 協調 + 共有メモリ + genome 操作）は
  Fable 5 で。単純な物理カーネルは Opus 4.8 で十分。

### Phase 3: 描画統合・段階A（推奨: Sonnet 5〜Opus 4.8）
- no-interop パスを Metal バッファ→ホスト→GL VBO に接続
- **ゲート**: GUI 上でシミュレーションが動いて見える（機能一致、性能は問わない）

### Phase 4: Metal ネイティブレンダラー・段階B（推奨: Opus 4.8、GS再設計初回のみ Fable 5）
- CAMetalLayer + imgui_impl_metal 化、RenderPipeline の Metal 実装
- GS→インスタンス展開 VS 8本（1本目のパターン確立を Fable 5、残り7本の横展開は Sonnet 5）
- ポストプロセス GLSL→MSL 一括変換(Sonnet 5)、ゼロコピー化
- **ゲート**: GL 版とスクリーンショット比較で見た目一致、既定シーンで 60fps
- 完了後 OpenGL/glad/glew 依存を macOS ビルドから除去

### Phase 5: 最適化・仕上げ（推奨: Fable 5 で分析、Opus 4.8 で実装）
- Metal System Trace / GPU キャプチャでプロファイル
- threadgroup サイズ再考（現行 8 thread/block は Apple GPU の simdgroup=32 に対し過小の可能性）
- app bundle 化・署名・配布、README / CI（GitHub Actions macos ランナー）
- **ゲート**: M1 で実用フレームレート、代表シナリオ長時間安定

---

## 4. モデル使い分け指針

| モデル | 担当タスク |
|---|---|
| **Fable 5** | シム設計（Phase 1 全体）、メモリモデル/前進保証の検証（HashMap ロック、atomic 設計）、split-float 等の数値設計、Constructor/Mutation 系カーネル、EngineTests の難解な不一致デバッグ、性能分析と戦略、フェーズ間のアーキテクチャレビュー |
| **Opus 4.8** | Phase 0 ビルド配線、Phase 2 の大半のカーネル移植、Metal レンダラー実装、テスト失敗の一般的なデバッグ、CMake / metallib パイプライン |
| **Sonnet 5** | アドレス空間マクロの機械適用（数千行規模）、GLSL→MSL 定型変換（ポストプロセス・VS/FS）、GS 置換の横展開（2〜8本目）、ボイラープレート生成、ビルド/テスト実行とログ収集、clang-format |

運用: フェーズ単位でセッションを分け、本ファイルと各フェーズの完了メモ
（docs/metal-port/phase-N-notes.md を随時作成）を次セッションの冒頭で読ませる。

---

## 5. リスク台帳

| リスク | 影響 | 対策 |
|---|---|---|
| ~~HashMap スピンロックが simdgroup 内でデッドロック~~ **→ スパイクで LIVELOCK 実証済み（§6）** | 確定リスク | lock-free CAS 設計に置換（動作実証済み）。Object::getLock 待機型の使用箇所も要置換 |
| 8 thread/block 前提の暗黙依存（NEURONS_PER_CELL=16 等ブロックサイズ結合） | 正しさに影響 | 移植時はブロックサイズを変えない。最適化は Phase 5 まで凍結 |
| MSL コンパイラが特定 C++ 構文を拒否（40+ の [&] ラムダ等） | シム戦略の部分崩壊 | Phase 1 で「最難構文サンプル」を先行コンパイル（スパイク）。ダメな構文のみ書換 |
| upstream の活発な変更との衝突 | リベースコスト | 共通ソース戦略 + 変更は追加ファイル中心。CUDA ビルド無変更を CI で担保 |
| 決定論差（演算順序・fast math）で EngineTests が不安定 | 検証コスト増 | approxCompare 前提。許容誤差超過は個別分析。-ffast-math 相当の Metal オプションを CUDA と揃える |
| 19k 行 MSL のコンパイル時間 | 開発速度低下 | metallib 分割 + 差分ビルド。関数コンスタント活用は最小限に |

---

## 6. スパイク検証結果（2026-07-05 実施、M5 Max / macOS 26.5 実機）— **共通ソース戦略は成立**

検証コード: `spike-metal/`（runtime_kernels.metal + host.mm、ランタイムMSLコンパイル方式）
結果: ランタイムテスト 8/9 PASS。唯一の FAIL は「予想していたライブロックの実証」。

| テスト | 結果 | 意味 |
|---|---|---|
| Array bump alloc（32bit カウンタ + overflow→エラーフラグ） | PASS | ABORT()→エラーフラグ置換の実証込み |
| Object 風構造体（device ポインタメンバ + connections 配列）の GPU リンク & チェイス | PASS | ポインタグラフはそのまま移植可 |
| atomic_float fetch_add + revert パターン | PASS | エネルギー保存が完全一致（±0.0000）。26箇所の float atomics は無変換で移植可 |
| [&] ジェネリックラムダ + if constexpr + 自前 traits + half | PASS | MutationProcessor パターンは MSL で動作 |
| lock-free CAS insert（HashMap 再設計案） | PASS | 代替設計の動作実証済み |
| tryLock-skip（Entities.cuh Object::tryLock パターン） | PASS | counter+skips 完全整合。tryLock 系は安全 |
| **忠実スピンロック（HashMap::getLock / Object::getLock、TG=8 同一 simdgroup）** | **LIVELOCK** | **待機ループ型ロックは再設計必須（確定）** |

コンパイルプローブで確定した MSL 変換規則:
1. 参照/ポインタ型には全てアドレス空間修飾必須 → シムマクロ（`TREF/DREF/DPTR` 等）を全 .cuh に適用
2. プログラムスコープ / static メンバの constexpr 変数は `constant` 修飾必須（Array.cuh の `Const::` 等）
3. memory_order は relaxed のみ。acquire/release は**存在しない** →
   `__threadfence()` は `atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device)` に対応（M5 でコンパイル確認済み）
4. 64bit 整数 atomics 不可（M5 でも fetch_add 不能）→ カウンタ 32bit 化を確定
5. float2/float3 演算子は MSL ネイティブ → Math.cuh の自前 operator 群は `#ifndef ALIEN_METAL` ガード
6. variadic template + C++17 fold 式、device 参照返しテンプレート（`Array::at`）、
   `int` メンバへの `reinterpret_cast<device atomic_int*>` アトミックアクセスは全て可
7. **Xcode 不要**: オフライン metal コンパイラは CLT に無いが、`newLibraryWithSource:`（ランタイム
   コンパイル）は OS 標準で動く。alien は GLSL を文字列ヘッダーで持つ方式なので MSL も同方式とする

Phase 1 への設計指示（スパイクから確定）:
- HashMap（MapSectionCollector 用途）→ lock-free open addressing（CAS で EMPTY→key）に再設計
- Object::getLock（待機型）の使用箇所を洗い出し、tryLock+スキップ or 再試行キューに置換
  （CUDA 側も tryLock 主体なので影響範囲は限定的の見込み。要精査）
- Heap の 64bit バイトカウンタは「16B アライン単位の 32bit カウント」(上限 64GB) に変更

---

*本ファイルはローカル開発用（日本語）。upstream に PR する場合は英語化すること（CLAUDE.md 規約）。*
