// Spike: runtime verification kernels replicating ALIEN's CUDA device-code patterns.
// Each kernel mirrors a specific pattern found in source/EngineKernels/.

#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------
// K1: Array bump allocator (Array.cuh getNewElement/getSubArray, 32-bit port)
// ABORT() is replaced by an error-flag buffer (DEVICE_CHECK replacement).
// ---------------------------------------------------------------------------

struct ArrayHeader
{
    atomic_uint numEntries;
    uint capacity;
};

kernel void spikeArrayAlloc(
    device ArrayHeader& hdr [[buffer(0)]],
    device float* data [[buffer(1)]],
    device atomic_uint& errorFlag [[buffer(2)]],
    uint tid [[thread_position_in_grid]])
{
    uint oldIndex = atomic_fetch_add_explicit(&hdr.numEntries, 1u, memory_order_relaxed);
    if (oldIndex >= hdr.capacity) {
        atomic_fetch_sub_explicit(&hdr.numEntries, 1u, memory_order_relaxed);
        atomic_store_explicit(&errorFlag, 1u, memory_order_relaxed);
        return;
    }
    data[oldIndex] = 1.0f;
}

// ---------------------------------------------------------------------------
// K2: Object-style struct with device-pointer members + GPU-side linking and
// pointer chasing (Entities.cuh Object/ObjectConnection pattern).
// ---------------------------------------------------------------------------

struct SpikeObject;

struct SpikeConnection
{
    device SpikeObject* object;
    float distance;
    float angleFromPrevious;
};

struct SpikeObject
{
    float2 pos;
    float2 vel;
    float energy;
    uint numConnections;
    SpikeConnection connections[6];

    device SpikeObject* getConnectedObject(uint index) device
    {
        return connections[index % numConnections].object;
    }
};

kernel void spikeLinkObjects(
    device SpikeObject* objects [[buffer(0)]],
    constant uint& n [[buffer(1)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid >= n) {
        return;
    }
    device SpikeObject& obj = objects[tid];
    obj.pos = float2(float(tid), float(tid) * 2.0f);
    obj.vel = float2(0.0f, 0.0f);
    obj.energy = float(tid);
    obj.numConnections = 2;
    obj.connections[0].object = &objects[(tid + 1) % n];
    obj.connections[0].distance = 1.0f;
    obj.connections[1].object = &objects[(tid + n - 1) % n];
    obj.connections[1].distance = 1.0f;
}

kernel void spikeChasePointers(
    device SpikeObject* objects [[buffer(0)]],
    constant uint& n [[buffer(1)]],
    device float* result [[buffer(2)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid != 0) {
        return;
    }
    // Walk the ring via stored device pointers, summing energy.
    float sum = 0.0f;
    device SpikeObject* p = &objects[0];
    for (uint i = 0; i < n; ++i) {
        sum += p->energy;
        p = p->getConnectedObject(0);
    }
    result[0] = sum;
    // float2 native operators (Math.cuh operators must be #ifdef'd out on Metal)
    float2 a = objects[1].pos + objects[2].pos - objects[0].pos;
    result[1] = a.x + a.y;
}

// ---------------------------------------------------------------------------
// K3: atomic_float fetch_add including the add-then-revert pattern
// (EnergyProcessor.cuh:348, AttackerProcessor.cuh:161).
// ---------------------------------------------------------------------------

kernel void spikeAtomicFloat(
    device atomic_float& pool [[buffer(0)]],
    device atomic_float& accum [[buffer(1)]],
    device atomic_uint& reverts [[buffer(2)]],
    uint tid [[thread_position_in_grid]])
{
    float delta = 0.5f;
    float orig = atomic_fetch_add_explicit(&pool, -delta, memory_order_relaxed);
    if (orig < delta) {
        atomic_fetch_add_explicit(&pool, delta, memory_order_relaxed);
        atomic_fetch_add_explicit(&reverts, 1u, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&accum, delta, memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// K4: Faithful spin lock as in HashMap.cuh Entry::getLock / Object::getLock:
// plain int member accessed through reinterpret_cast to atomic_int, spinning
// inside a single simdgroup (threadgroup of 8 = ALIEN's block size).
// This is the forward-progress / livelock probe.
// ---------------------------------------------------------------------------

struct LockedCounter
{
    int locked;   // 0 = unlocked, 1 = locked (same layout as ALIEN's structs)
    int counter;  // protected by the lock, non-atomic access
};

kernel void spikeSpinLock(
    device LockedCounter& lc [[buffer(0)]],
    constant uint& iterations [[buffer(1)]],
    uint tid [[thread_position_in_grid]])
{
    device atomic_int* lock = reinterpret_cast<device atomic_int*>(&lc.locked);
    for (uint i = 0; i < iterations; ++i) {
        while (1 == atomic_exchange_explicit(lock, 1, memory_order_relaxed)) {
        }
        lc.counter += 1;
        atomic_exchange_explicit(lock, 0, memory_order_relaxed);
    }
}

// Variant with tryLock + skip (Entities.cuh tryLock pattern — should always be safe).
kernel void spikeTryLock(
    device LockedCounter& lc [[buffer(0)]],
    constant uint& iterations [[buffer(1)]],
    device atomic_uint& skips [[buffer(2)]],
    uint tid [[thread_position_in_grid]])
{
    device atomic_int* lock = reinterpret_cast<device atomic_int*>(&lc.locked);
    for (uint i = 0; i < iterations; ++i) {
        if (0 == atomic_exchange_explicit(lock, 1, memory_order_relaxed)) {
            lc.counter += 1;
            atomic_exchange_explicit(lock, 0, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&skips, 1u, memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// K5: Lock-free open-addressing insert (candidate HashMap redesign):
// CAS the key slot from EMPTY(0) to key — no spin lock at all.
// ---------------------------------------------------------------------------

kernel void spikeCASInsert(
    device atomic_uint* keys [[buffer(0)]],
    device uint* values [[buffer(1)]],
    constant uint& tableSize [[buffer(2)]],
    device atomic_uint& inserted [[buffer(3)]],
    uint tid [[thread_position_in_grid]])
{
    uint key = (tid % 50u) + 1u;  // duplicate keys on purpose
    uint slot = key % tableSize;
    for (uint probe = 0; probe < tableSize; ++probe) {
        uint expected = 0u;
        bool won = atomic_compare_exchange_weak_explicit(
            &keys[slot], &expected, key, memory_order_relaxed, memory_order_relaxed);
        if (won) {
            values[slot] = key * 10u;
            atomic_fetch_add_explicit(&inserted, 1u, memory_order_relaxed);
            return;
        }
        if (expected == key) {
            values[slot] = key * 10u;  // insertOrAssign semantics
            return;
        }
        slot = (slot + 1u) % tableSize;
    }
}

// ---------------------------------------------------------------------------
// K6: Generic [&]-capturing lambdas passed to template functions
// (objectMap.executeForEach pattern) plus if-constexpr type dispatch
// (MutationProcessor.cuh mutateNumber pattern) with a hand-rolled std shim.
// ---------------------------------------------------------------------------

namespace alien_std
{
    // MSL: reference specializations must be written per address space
    template <typename T> struct remove_ref { using type = T; };
    template <typename T> struct remove_ref<thread T&> { using type = T; };
    template <typename T> struct remove_ref<device T&> { using type = T; };
    template <typename T> struct remove_ref<threadgroup T&> { using type = T; };
    template <typename T> struct remove_ref<constant T&> { using type = T; };
    template <typename T> struct remove_cv_s { using type = T; };
    template <typename T> struct remove_cv_s<const T> { using type = T; };
    template <typename T> using decay_t = typename remove_cv_s<typename remove_ref<T>::type>::type;

    template <typename T> struct is_integral { static constant constexpr bool value = false; };
    template <> struct is_integral<int> { static constant constexpr bool value = true; };
    template <> struct is_integral<uint> { static constant constexpr bool value = true; };
    template <> struct is_integral<short> { static constant constexpr bool value = true; };
    template <> struct is_integral<uchar> { static constant constexpr bool value = true; };
}

template <typename F>
inline void forEachInRange(uint start, uint end, F func)
{
    for (uint i = start; i < end; ++i) {
        func(i);
    }
}

template <typename T>
inline void mutateNumberLike(thread T& value, float delta, thread float& accum)
{
    using ValueType = alien_std::decay_t<T>;
    if constexpr (alien_std::is_integral<ValueType>::value) {
        value = static_cast<ValueType>(static_cast<int>(value) + static_cast<int>(metal::round(delta)));
    } else {
        value = value + static_cast<ValueType>(delta);
    }
    accum += metal::abs(delta);
}

kernel void spikeLambdaTraits(
    device float* result [[buffer(0)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid != 0) {
        return;
    }
    float sum = 0.0f;
    uint count = 0;
    int intVal = 10;
    float floatVal = 1.0f;
    float accum = 0.0f;
    forEachInRange(0u, 5u, [&](uint i) {
        sum += float(i);
        count += 1;
        mutateNumberLike(intVal, 1.4f, accum);    // integral branch: +1 per call (round(1.4)=1)
        mutateNumberLike(floatVal, 0.25f, accum); // float branch: +0.25 per call
    });
    result[0] = sum;             // expected 0+1+2+3+4 = 10
    result[1] = float(count);    // expected 5
    result[2] = float(intVal);   // expected 10 + 5*1 = 15
    result[3] = floatVal;        // expected 1.0 + 5*0.25 = 2.25
    result[4] = accum;           // expected 5*(1.4+0.25) = 8.25

    // half support (Genome weights use __half on CUDA side)
    half h = half(1.5f);
    h = h + half(0.25f);
    result[5] = float(h);        // expected 1.75
}
