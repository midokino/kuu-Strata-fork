// strata_ds4_launch(kernel, grid, block, shared, stream, args...): the engine's one spelling of a kernel launch.  On CUDA it is
// kernel<<<grid, block, shared, stream>>>(args...); on Metal metal/include/metal_compat/cuda_launch.h maps the same call onto
// a dispatch.  (Every file that launches kernels includes this header.)
#pragma once
#if (defined(__CUDACC__) || defined(__HIPCC__) || defined(__HIP__)) && !defined(STRATA_METAL)
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <memory>
#include <tuple>
#include <type_traits>
#include <vector>

// (local) STRATA_DS4_GRAPH=3: a layer's launches and stream copies taped instead of run - each one's parameters copied by
// value into an arena and hashed - while the host's side of the layer (row lists, the mailbox's bookkeeping, the deferred
// parts) runs as always.  The model then launches the executable graph it keeps for (layer, window size, hash), or, the first
// time it sees that hash, records the tape into a new one (or plays it on the stream).  Only the calls on the tape's stream
// are taped (and only from the thread that set it); the others run.
struct Ds4Tape {
    cudaStream_t s = nullptr;
    uint64_t h = 1469598103934665603ull;
    struct Ent {
        void (*run)(const uint8_t*, cudaStream_t);
        const uint8_t* rec;
    };
    std::vector<Ent> ents;
    std::vector<std::unique_ptr<uint8_t[]>> chunks;   // (64 KB each: a record never moves)
    size_t used = 0, n_chunk = 0;
    static constexpr size_t kChunk = 64 << 10;
    void reset(cudaStream_t st) {
        s = st;
        h = 1469598103934665603ull;
        ents.clear();
        used = 0;
        n_chunk = 0;
    }
    void mix(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*) p;
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    template <class R> R* put(void (*run)(const uint8_t*, cudaStream_t)) {
        static_assert(sizeof(R) <= kChunk, "a tape record over 64 KB");
        used = (used + 15) & ~(size_t) 15;
        if (n_chunk == 0 || used + sizeof(R) > kChunk) {
            if (n_chunk == chunks.size()) chunks.emplace_back(new uint8_t[kChunk]);
            ++n_chunk;
            used = 0;
        }
        uint8_t* p = chunks[n_chunk - 1].get() + used;
        used += sizeof(R);
        ents.push_back({run, p});
        return (R*) p;
    }
    void play(cudaStream_t st) const {
        for (const Ent& e : ents) e.run(e.rec, st);
    }
};
extern thread_local Ds4Tape* strata_ds4_tape;

template <class... KA> struct Ds4KRec {
    void (*k)(KA...);
    dim3 g, b;
    size_t sh;
    std::tuple<std::remove_cv_t<KA>...> a;
};
template <class... KA> void strata_ds4_krun(const uint8_t* p, cudaStream_t s) {
    const Ds4KRec<KA...>& r = *(const Ds4KRec<KA...>*) p;
    std::apply([&](const auto&... x) { r.k<<<r.g, r.b, r.sh, s>>>(x...); }, r.a);
}

template <class... KA, class... CA>
inline void strata_ds4_launch_at(const char* file, int line, void (*kernel)(KA...), dim3 grid, dim3 block, size_t shared,
                                 cudaStream_t stream, CA&&... args) {
    if (Ds4Tape* tp = strata_ds4_tape; tp != nullptr && stream == tp->s) {
        static_assert((std::is_trivially_destructible_v<std::remove_cv_t<KA>> && ...), "a kernel parameter the tape cannot hold");
        using R = Ds4KRec<KA...>;
        R* r = new (tp->put<R>(&strata_ds4_krun<KA...>)) R{kernel, grid, block, shared, {static_cast<KA>(args)...}};
        tp->mix(&r->k, sizeof(r->k));
        tp->mix(&grid, sizeof(grid));
        tp->mix(&block, sizeof(block));
        tp->mix(&shared, sizeof(shared));
        std::apply([&](const auto&... x) { (tp->mix(&x, sizeof(x)), ...); }, r->a);
        return;
    }
    kernel<<<grid, block, shared, stream>>>(static_cast<KA>(args)...);
    static const bool check = std::getenv("DS4_LAUNCH_CHECK") != nullptr;   // report the launch that failed (not a later call)
    if (check) {
        const cudaError_t e = cudaGetLastError();
        if (e != cudaSuccess)
            std::fprintf(stderr, "ds4 launch failed at %s:%d: %s (grid %u,%u,%u block %u,%u,%u shared %zu)\n", file, line,
                         cudaGetErrorString(e), grid.x, grid.y, grid.z, block.x, block.y, block.z, shared);
    }
}
#define strata_ds4_launch(...) strata_ds4_launch_at(__FILE__, __LINE__, __VA_ARGS__)

// (local) the stream calls a layer makes, taped like the launches (STRATA_DS4_GRAPH=3).  The macros below send every later
// call in these files through them; "(cudaMemcpyAsync)(...)" is the runtime's own (a parenthesized name is not a macro call).
#if defined(__CUDACC__) && !defined(__HIPCC__) && !defined(__HIP__)
struct Ds4CpyRec { void* dst; const void* src; size_t n; cudaMemcpyKind k; };
struct Ds4SetRec { void* p; int v; size_t n; };
struct Ds4Cpy2Rec { void* dst; size_t dp; const void* src; size_t sp; size_t w, hh; cudaMemcpyKind k; };
struct Ds4EvRec { cudaEvent_t e; unsigned f; };
inline cudaError_t strata_ds4_memcpy_async(void* dst, const void* src, size_t n, cudaMemcpyKind k, cudaStream_t s = 0) {
    if (Ds4Tape* tp = strata_ds4_tape; tp != nullptr && s == tp->s) {
        Ds4CpyRec* r = new (tp->put<Ds4CpyRec>([](const uint8_t* p, cudaStream_t st) {
            const Ds4CpyRec& c = *(const Ds4CpyRec*) p;
            (cudaMemcpyAsync)(c.dst, c.src, c.n, c.k, st);
        })) Ds4CpyRec{dst, src, n, k};
        const int tag = 1;
        tp->mix(&tag, 4), tp->mix(&r->dst, 8), tp->mix(&r->src, 8), tp->mix(&n, 8), tp->mix(&k, sizeof(k));
        return cudaSuccess;
    }
    return (cudaMemcpyAsync)(dst, src, n, k, s);
}
inline cudaError_t strata_ds4_memset_async(void* p, int v, size_t n, cudaStream_t s = 0) {
    if (Ds4Tape* tp = strata_ds4_tape; tp != nullptr && s == tp->s) {
        new (tp->put<Ds4SetRec>([](const uint8_t* q, cudaStream_t st) {
            const Ds4SetRec& c = *(const Ds4SetRec*) q;
            (cudaMemsetAsync)(c.p, c.v, c.n, st);
        })) Ds4SetRec{p, v, n};
        const int tag = 2;
        tp->mix(&tag, 4), tp->mix(&p, 8), tp->mix(&v, 4), tp->mix(&n, 8);
        return cudaSuccess;
    }
    return (cudaMemsetAsync)(p, v, n, s);
}
inline cudaError_t strata_ds4_memcpy2d_async(void* dst, size_t dp, const void* src, size_t sp, size_t w, size_t hh,
                                             cudaMemcpyKind k, cudaStream_t s = 0) {
    if (Ds4Tape* tp = strata_ds4_tape; tp != nullptr && s == tp->s) {
        Ds4Cpy2Rec* r = new (tp->put<Ds4Cpy2Rec>([](const uint8_t* q, cudaStream_t st) {
            const Ds4Cpy2Rec& c = *(const Ds4Cpy2Rec*) q;
            (cudaMemcpy2DAsync)(c.dst, c.dp, c.src, c.sp, c.w, c.hh, c.k, st);
        })) Ds4Cpy2Rec{dst, dp, src, sp, w, hh, k};
        const int tag = 3;
        tp->mix(&tag, 4), tp->mix(&r->dst, 8), tp->mix(&dp, 8), tp->mix(&r->src, 8), tp->mix(&sp, 8), tp->mix(&w, 8),
            tp->mix(&hh, 8), tp->mix(&k, sizeof(k));
        return cudaSuccess;
    }
    return (cudaMemcpy2DAsync)(dst, dp, src, sp, w, hh, k, s);
}
inline cudaError_t strata_ds4_event_record(cudaEvent_t e, cudaStream_t s = 0) {
    if (Ds4Tape* tp = strata_ds4_tape; tp != nullptr && s == tp->s) {
        new (tp->put<Ds4EvRec>([](const uint8_t* q, cudaStream_t st) { (cudaEventRecord)(((const Ds4EvRec*) q)->e, st); }))
            Ds4EvRec{e, 0};
        const int tag = 4;
        tp->mix(&tag, 4), tp->mix(&e, sizeof(e));
        return cudaSuccess;
    }
    return (cudaEventRecord)(e, s);
}
inline cudaError_t strata_ds4_stream_wait_event(cudaStream_t s, cudaEvent_t e, unsigned f = 0) {
    if (Ds4Tape* tp = strata_ds4_tape; tp != nullptr && s == tp->s) {
        new (tp->put<Ds4EvRec>([](const uint8_t* q, cudaStream_t st) {
            const Ds4EvRec& c = *(const Ds4EvRec*) q;
            (cudaStreamWaitEvent)(st, c.e, c.f);
        })) Ds4EvRec{e, f};
        const int tag = 5;
        tp->mix(&tag, 4), tp->mix(&e, sizeof(e)), tp->mix(&f, 4);
        return cudaSuccess;
    }
    return (cudaStreamWaitEvent)(s, e, f);
}
#define cudaMemcpyAsync(...) strata_ds4_memcpy_async(__VA_ARGS__)
#define cudaMemsetAsync(...) strata_ds4_memset_async(__VA_ARGS__)
#define cudaMemcpy2DAsync(...) strata_ds4_memcpy2d_async(__VA_ARGS__)
#define cudaEventRecord(...) strata_ds4_event_record(__VA_ARGS__)
#define cudaStreamWaitEvent(...) strata_ds4_stream_wait_event(__VA_ARGS__)
#endif

// (the Metal debug dumps in ds4_model.cu name these; on CUDA there is no deferred queue and a device pointer is not a host view)
inline void ds4_metal_flush() {}
namespace strata::gpu {
inline void* host_view(const void* p) { return const_cast<void*>(p); }
}
#endif

// ds4_dbg("NAME"): the Mac engine's opt-in diagnostics (DS4_DBG_*, DS4_CPU_LOG, DS4_TLOG, ...).  On Metal it is an environment
// lookup; everywhere else it is the constant false, so the CUDA and HIP engines carry none of those checks on their hot paths.
#if defined(STRATA_METAL)
#include <cstdlib>
inline bool ds4_dbg(const char* name) { return std::getenv(name) != nullptr; }
#else
constexpr bool ds4_dbg(const char*) { return false; }
#endif
