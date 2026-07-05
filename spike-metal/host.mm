// Spike host driver: compiles MSL probes + runtime kernels and verifies
// ALIEN's device-code patterns on the actual GPU.
// Build: clang++ -fobjc-arc -std=c++17 host.mm -framework Metal -framework Foundation -o spike_host

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

static id<MTLDevice> gDevice;
static id<MTLCommandQueue> gQueue;
static int gPass = 0, gFail = 0;

static void report(const char* name, bool ok, const char* detail = "")
{
    printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    ok ? ++gPass : ++gFail;
}

static id<MTLLibrary> compileSource(NSString* name, NSString* src, bool verbose)
{
    NSError* err = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new];
    id<MTLLibrary> lib = [gDevice newLibraryWithSource:src options:opts error:&err];
    if (!lib && verbose && err) {
        NSString* msg = [err.localizedDescription substringToIndex:MIN((NSUInteger)500, err.localizedDescription.length)];
        printf("    compile error (%s): %s\n", name.UTF8String, msg.UTF8String);
    }
    return lib;
}

static id<MTLComputePipelineState> pso(id<MTLLibrary> lib, NSString* fn)
{
    NSError* err = nil;
    id<MTLFunction> f = [lib newFunctionWithName:fn];
    if (!f) { printf("    function not found: %s\n", fn.UTF8String); return nil; }
    id<MTLComputePipelineState> p = [gDevice newComputePipelineStateWithFunction:f error:&err];
    if (!p) { printf("    pso error: %s\n", err.localizedDescription.UTF8String); }
    return p;
}

static id<MTLBuffer> makeBuf(size_t len) { return [gDevice newBufferWithLength:len options:MTLResourceStorageModeShared]; }

// Returns: 0 = completed, 1 = timeout (likely livelock), 2 = error status
static int dispatch1D(id<MTLComputePipelineState> p, NSArray<id<MTLBuffer>>* bufs,
                      NSUInteger grid, NSUInteger tg, double timeoutSec)
{
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:p];
    for (NSUInteger i = 0; i < bufs.count; ++i) { [enc setBuffer:bufs[i] offset:0 atIndex:i]; }
    [enc dispatchThreads:MTLSizeMake(grid, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
    [enc endEncoding];
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { dispatch_semaphore_signal(sem); }];
    [cb commit];
    long r = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeoutSec * NSEC_PER_SEC)));
    if (r != 0) { return 1; }
    if (cb.status == MTLCommandBufferStatusError) {
        printf("    command buffer error: %s\n", cb.error.localizedDescription.UTF8String);
        return 2;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Compile-only probes for individually risky constructs
// ---------------------------------------------------------------------------

struct Probe { const char* name; const char* src; };

static const Probe kProbes[] = {
    {"P1 namespace-std-shim + inline constexpr var + if constexpr",
     R"(#include <metal_stdlib>
        namespace std {
            template <class T> struct is_integral { static constexpr bool value = false; };
            template <> struct is_integral<int> { static constexpr bool value = true; };
            template <class T> inline constexpr bool is_integral_v = is_integral<T>::value;
        }
        kernel void probe1(device int* out) {
            if constexpr (std::is_integral_v<int>) { out[0] = 1; } else { out[0] = 0; }
        })"},

    {"P2 program-scope constexpr in namespace (Array.cuh Const:: pattern)",
     R"(#include <metal_stdlib>
        namespace Const { constexpr float ArrayFillPercentage = 2.0f / 4.0f; }
        kernel void probe2(device float* out) { out[0] = Const::ArrayFillPercentage; })"},

    {"P3 acquire/release memory order on device atomics",
     R"(#include <metal_stdlib>
        using namespace metal;
        kernel void probe3(device atomic_int* a, device int* out) {
            int v = atomic_exchange_explicit(a, 1, memory_order_acquire);
            atomic_store_explicit(a, v, memory_order_release);
            out[0] = v;
        })"},

    {"P4a atomic_thread_fence(mem_device, seq_cst, device-scope)",
     R"(#include <metal_stdlib>
        using namespace metal;
        kernel void probe4a(device atomic_int* a) {
            atomic_store_explicit(a, 1, memory_order_relaxed);
            atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
        })"},

    {"P4b atomic_thread_fence(mem_device) single-arg",
     R"(#include <metal_stdlib>
        using namespace metal;
        kernel void probe4b(device atomic_int* a) {
            atomic_store_explicit(a, 1, memory_order_relaxed);
            atomic_thread_fence(mem_flags::mem_device);
        })"},

    {"P5 variadic template + C++17 fold expression",
     R"(#include <metal_stdlib>
        template <typename... Args> inline float sumAll(Args... args) { return (float(args) + ...); }
        kernel void probe5(device float* out) { out[0] = sumAll(1, 2.5f, 3u); })"},

    {"P6 user operator+ on float2 (expected FAIL: native operator exists)",
     R"(#include <metal_stdlib>
        using namespace metal;
        inline float2 operator+(float2 const& p, float2 const& q) { return float2(p.x + q.x, p.y + q.y); }
        kernel void probe6(device float* out) { float2 r = float2(1,2) + float2(3,4); out[0] = r.x; })"},

    {"P7 reference-typed alias to device memory in template (T& at(...) pattern)",
     R"(#include <metal_stdlib>
        template <typename T>
        struct Arr {
            device T* d;
            device T& at(uint i) device { return d[i]; }
        };
        kernel void probe7(device Arr<float>* a, device float* out) {
            a->at(0) = 42.0f;
            out[0] = a->at(0);
        })"},

    {"P8 64-bit ulong atomic fetch_add (expected FAIL on most GPUs)",
     R"(#include <metal_stdlib>
        using namespace metal;
        kernel void probe8(device atomic_ulong* a) {
            atomic_fetch_add_explicit(a, 1ul, memory_order_relaxed);
        })"},
};

int main(int argc, char** argv)
{
    @autoreleasepool {
        gDevice = MTLCreateSystemDefaultDevice();
        if (!gDevice) {
            printf("[FATAL] MTLCreateSystemDefaultDevice returned nil (sandbox or headless issue)\n");
            return 2;
        }
        gQueue = [gDevice newCommandQueue];
        printf("Device: %s\n", gDevice.name.UTF8String);
        printf("  Apple7 (M1): %d, Apple8 (M2): %d, Apple9 (M3+): %d, Metal3: %d\n",
               [gDevice supportsFamily:MTLGPUFamilyApple7],
               [gDevice supportsFamily:MTLGPUFamilyApple8],
               [gDevice supportsFamily:MTLGPUFamilyApple9],
               [gDevice supportsFamily:MTLGPUFamilyMetal3]);

        printf("\n=== Compile probes ===\n");
        for (auto const& p : kProbes) {
            id<MTLLibrary> lib = compileSource(@(p.name), @(p.src), true);
            printf("[%s] %s\n", lib ? "OK  " : "NO  ", p.name);
        }

        printf("\n=== Runtime kernels ===\n");
        NSString* path = argc > 1 ? @(argv[1]) : @"runtime_kernels.metal";
        NSError* rerr = nil;
        NSString* src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&rerr];
        if (!src) { printf("[FATAL] cannot read %s\n", path.UTF8String); return 2; }
        id<MTLLibrary> lib = compileSource(@"runtime_kernels", src, true);
        if (!lib) {
            printf("[FATAL] runtime kernel compilation failed — common-source strategy needs revision\n");
            return 1;
        }
        report("runtime_kernels.metal compiles", true);

        // --- K1: bump allocator ---
        {
            id<MTLComputePipelineState> p = pso(lib, @"spikeArrayAlloc");
            id<MTLBuffer> hdr = makeBuf(8), data = makeBuf(4 * 1000), flag = makeBuf(4);
            ((uint32_t*)hdr.contents)[0] = 0; ((uint32_t*)hdr.contents)[1] = 1000;
            int rc = dispatch1D(p, @[hdr, data, flag], 1000, 8, 10);
            uint32_t n = ((uint32_t*)hdr.contents)[0], ef = *(uint32_t*)flag.contents;
            bool dataOk = true;
            for (int i = 0; i < 1000; ++i) { if (((float*)data.contents)[i] != 1.0f) { dataOk = false; break; } }
            report("K1 Array bump alloc (exact fill)", rc == 0 && n == 1000 && ef == 0 && dataOk,
                   rc ? "dispatch failed" : "");

            // overflow → error flag (ABORT replacement)
            ((uint32_t*)hdr.contents)[0] = 0; ((uint32_t*)hdr.contents)[1] = 500;
            *(uint32_t*)flag.contents = 0;
            rc = dispatch1D(p, @[hdr, data, flag], 1000, 8, 10);
            n = ((uint32_t*)hdr.contents)[0]; ef = *(uint32_t*)flag.contents;
            report("K1 Array overflow -> error flag", rc == 0 && ef == 1 && n == 500);
        }

        // --- K2: pointer-linked objects ---
        {
            id<MTLComputePipelineState> pl = pso(lib, @"spikeLinkObjects");
            id<MTLComputePipelineState> pc = pso(lib, @"spikeChasePointers");
            const uint32_t N = 256;
            // sizeof(SpikeObject): float2*2 + float + uint + 6*(ptr+2 floats) -> use generous 256B/obj
            id<MTLBuffer> objs = makeBuf(256 * N), nbuf = makeBuf(4), res = makeBuf(8);
            *(uint32_t*)nbuf.contents = N;
            int rc1 = dispatch1D(pl, @[objs, nbuf], N, 8, 10);
            int rc2 = dispatch1D(pc, @[objs, nbuf, res], 1, 1, 10);
            float sum = ((float*)res.contents)[0], f2 = ((float*)res.contents)[1];
            float expect = (float)(N - 1) * N / 2.0f;
            char d[128]; snprintf(d, 128, "(sum=%.0f expect=%.0f, float2 op=%.0f expect=9)", sum, expect, f2);
            report("K2 device-pointer struct link+chase", rc1 == 0 && rc2 == 0 && sum == expect && f2 == 9.0f, d);
        }

        // --- K3: atomic_float with revert pattern (energy conservation) ---
        {
            id<MTLComputePipelineState> p = pso(lib, @"spikeAtomicFloat");
            if (!p) { report("K3 atomic_float fetch_add", false, "(pso failed — atomic_float unsupported?)"); }
            else {
                id<MTLBuffer> pool = makeBuf(4), accum = makeBuf(4), rev = makeBuf(4);
                *(float*)pool.contents = 100.0f; *(float*)accum.contents = 0; *(uint32_t*)rev.contents = 0;
                int rc = dispatch1D(p, @[pool, accum, rev], 1000, 8, 10);
                float pf = *(float*)pool.contents, af = *(float*)accum.contents;
                uint32_t rv = *(uint32_t*)rev.contents;
                char d[160]; snprintf(d, 160, "(pool=%.2f accum=%.2f reverts=%u conservation=%.4f)", pf, af, rv, af + pf);
                report("K3 atomic_float + revert conserves energy", rc == 0 && fabsf(af + pf - 100.0f) < 0.01f && rv > 0, d);
            }
        }

        // --- K5: lock-free CAS insert (HashMap redesign candidate) ---
        {
            id<MTLComputePipelineState> p = pso(lib, @"spikeCASInsert");
            const uint32_t TS = 128;
            id<MTLBuffer> keys = makeBuf(4 * TS), vals = makeBuf(4 * TS), ts = makeBuf(4), ins = makeBuf(4);
            memset(keys.contents, 0, 4 * TS); memset(vals.contents, 0, 4 * TS);
            *(uint32_t*)ts.contents = TS; *(uint32_t*)ins.contents = 0;
            int rc = dispatch1D(p, @[keys, vals, ts, ins], 1000, 8, 10);
            uint32_t inserted = *(uint32_t*)ins.contents;
            int nonZero = 0; bool valsOk = true;
            for (uint32_t i = 0; i < TS; ++i) {
                uint32_t k = ((uint32_t*)keys.contents)[i];
                if (k) { ++nonZero; if (((uint32_t*)vals.contents)[i] != k * 10) valsOk = false; }
            }
            char d[96]; snprintf(d, 96, "(inserted=%u slots=%d expect=50)", inserted, nonZero);
            report("K5 lock-free CAS insert", rc == 0 && inserted == 50 && nonZero == 50 && valsOk, d);
        }

        // --- K6: [&] generic lambda + if constexpr traits + half ---
        {
            id<MTLComputePipelineState> p = pso(lib, @"spikeLambdaTraits");
            id<MTLBuffer> res = makeBuf(4 * 6);
            int rc = dispatch1D(p, @[res], 1, 1, 10);
            float* r = (float*)res.contents;
            bool ok = rc == 0 && r[0] == 10 && r[1] == 5 && r[2] == 15 && fabsf(r[3] - 2.25f) < 1e-6 &&
                      fabsf(r[4] - 8.25f) < 1e-5 && fabsf(r[5] - 1.75f) < 1e-6;
            char d[160]; snprintf(d, 160, "(%.1f %.1f %.1f %.3f %.3f %.3f)", r[0], r[1], r[2], r[3], r[4], r[5]);
            report("K6 lambda + if-constexpr + half", ok, d);
        }

        // Spin-lock probes run LAST: a livelocked command buffer poisons the queue.
        gQueue = [gDevice newCommandQueue];
        // --- K4b: tryLock + skip (safe pattern) ---
        {
            id<MTLComputePipelineState> p = pso(lib, @"spikeTryLock");
            id<MTLBuffer> lc = makeBuf(8), it = makeBuf(4), sk = makeBuf(4);
            ((int32_t*)lc.contents)[0] = 0; ((int32_t*)lc.contents)[1] = 0; *(uint32_t*)sk.contents = 0;
            *(uint32_t*)it.contents = 10000;
            int rc = dispatch1D(p, @[lc, it, sk], 8, 8, 15.0);
            int32_t counter = ((int32_t*)lc.contents)[1];
            uint32_t skips = *(uint32_t*)sk.contents;
            char d[128]; snprintf(d, 128, "(counter=%d skips=%u sum=%u expect<=80000)", counter, skips, counter + skips);
            report("K4b tryLock-skip completes", rc == 0 && counter + skips == 80000 && counter > 0, d);
        }

        // --- K4: faithful spin lock inside one simdgroup (TG=8, ALIEN block size) ---
        {
            id<MTLComputePipelineState> p = pso(lib, @"spikeSpinLock");
            id<MTLBuffer> lc = makeBuf(8), it = makeBuf(4);
            ((int32_t*)lc.contents)[0] = 0; ((int32_t*)lc.contents)[1] = 0;
            *(uint32_t*)it.contents = 10000;
            int rc = dispatch1D(p, @[lc, it], 8, 8, 15.0);
            int32_t counter = ((int32_t*)lc.contents)[1];
            char d[128]; snprintf(d, 128, "(rc=%d counter=%d expect=80000)", rc, counter);
            if (rc == 1) {
                report("K4 spinlock TG=8 same-simdgroup", false, "(TIMEOUT -> LIVELOCK confirmed; HashMap redesign required)");
            } else {
                report("K4 spinlock TG=8 same-simdgroup", rc == 0 && counter == 80000, d);
            }
        }

        printf("\n=== Summary: %d passed, %d failed ===\n", gPass, gFail);
        return gFail == 0 ? 0 : 1;
    }
}
