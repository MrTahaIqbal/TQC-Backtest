#pragma once
/*
 * ring_buffer.hpp  -  BigBoyAgent TQC Backtest | Taha Iqbal
 *
 * Lock-free MPSC (Multiple-Producer Single-Consumer) ring buffer.
 * Requires C++20.  Portable to x86-64, ARM64, and any TSO or weak-ordering arch.
 *
 * ── Architecture ─────────────────────────────────────────────────────────────
 *
 *  head_   Unbounded monotonic write cursor.  Producers CAS this atomically to
 *          claim a slot.  Slot index = head & (N-1).
 *
 *  tail_   Unbounded monotonic read cursor.  The single consumer advances this
 *          after reading.  Slot index = tail & (N-1).
 *
 *  slots_  Array of cache-line-padded commit flags.  A producer sets
 *          slots_[slot].ready = true (release) after writing buf_[slot].
 *          The consumer spins on slots_[slot].ready (acquire) before reading,
 *          then clears it (relaxed) before advancing tail_ (release).
 *
 *  Both cursors are UNBOUNDED (never wrapped) — slot indices are derived via
 *  bitmasking only at the point of array access, which eliminates the ABA
 *  hazard present when the cursor itself is stored wrapped.
 *
 * ── Fixes applied ────────────────────────────────────────────────────────────
 *
 *  FIX-ABA  Replaced wrapped head_/tail_ counters with unbounded monotonic
 *           counters.  Old design: next = (head+1)&(N-1) stored directly into
 *           head_.  With small N a preempted producer could have its stale CAS
 *           succeed after the counter wrapped through a full cycle, silently
 *           claiming an in-use slot and corrupting the payload.
 *
 *  FIX-FS   ready_ is now an array of cache-line-padded Slot structs.
 *           Old design: std::array<std::atomic<bool>, N> packed 64 flags per
 *           cache line; every push/pop from adjacent slots invalidated the same
 *           line across all producer cores — O(N) false-sharing invalidations.
 *
 *  FIX-NE   push() noexcept is now sound.  Old design: buf_[head]=std::move(val)
 *           after CAS — if T's move-assign threw, the slot was permanently
 *           claimed but never committed, hanging the consumer forever.  Fixed by
 *           enforcing NothrowMovable<T> via C++20 concept.
 *
 *  FIX-BUF  Added alignas(CACHE_LINE_SIZE) to buf_[] so the array never shares
 *           a cache line with tail_ or any other field.
 *
 *  FIX-CAP  Old full-check: (head+1)&(N-1)==tail_ — used N-1 slots, wasted one.
 *           New check: head - tail >= N — full N-slot capacity is now available.
 *
 *  FIX-ND   push(), pop(), size(), empty(), capacity() are [[nodiscard]].
 *           A silently discarded push() return value means dropped items with
 *           no diagnostic; a discarded pop() optional loses work silently.
 *
 *  FIX-SPIN CPU pause hint emitted on every tight-spin iteration (before the
 *           yield threshold).  Reduces memory-bus pressure and pipeline stalls
 *           during the common sub-microsecond producer-commit window.
 *
 *  FIX-OBS  Added size(), empty(), capacity() — necessary for monitoring,
 *           backpressure signalling, and diagnostic logging.
 *
 *  FIX-CON  C++20 concept NothrowMovable<T> replaces unconstrained typename T.
 *           Produces an actionable compile error instead of a deep template
 *           instantiation failure or a silent runtime livelock.
 *
 *  FIX-CL   Replaced hardcoded 64 with std::hardware_destructive_interference_size
 *           (C++20) plus a portable fallback.  Correct on x86-64 (64 B),
 *           ARM64 (64 B), and Apple M-series (128 B in some configs).
 *
 *  FIX-CTR  Removed the redundant constructor loop.  C++20 guarantees that
 *           value-initialisation of std::atomic<bool> yields false; the Slot
 *           default member initialiser {false} is sufficient.
 *
 *  FIX-SA   Added static_assert(sizeof(Slot)==CACHE_LINE_SIZE) so a platform
 *           where sizeof(std::atomic<bool>) != 1 is caught at compile time
 *           rather than silently producing under-padded slots.
 */

#include <atomic>
#include <array>
#include <concepts>
#include <cstddef>
#include <new>          // std::hardware_destructive_interference_size
#include <optional>
#include <thread>
#include <type_traits>

// ── Cache-line size ───────────────────────────────────────────────────────────
// FIX-CL: Use the C++20 standard constant where available; fall back to 64.
// The fallback is correct for x86-64 and standard ARM64 (64 B lines).
// Note: GCC warns if <new> is included but the constant is unused under some
// -Wpedantic flags; the #ifdef suppresses that warning path cleanly.
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t CACHE_LINE_SIZE =
        std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t CACHE_LINE_SIZE = 64u;
#endif

// ── Architecture-specific spin-wait hint ─────────────────────────────────────
// FIX-SPIN: On x86-64, _mm_pause signals the CPU that this is a spin-wait
// loop, reducing pipeline speculation pressure and memory-bus traffic.
// On ARM64, the equivalent is the `yield` hint instruction.
// On unknown architectures we emit a compiler fence, which at minimum prevents
// the compiler from hoisting the load out of the loop.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#  include <immintrin.h>
#  define TQC_SPIN_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#  define TQC_SPIN_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#  define TQC_SPIN_PAUSE() std::atomic_signal_fence(std::memory_order_seq_cst)
#endif

namespace tqc {

// ── C++20 concept: T must be nothrow-moveable ─────────────────────────────────
// FIX-NE / FIX-CON: Required for push() noexcept soundness.
// If T's move-assign could throw after head_ is already advanced, the slot
// would be permanently claimed but never committed (ready_ stays false) and
// the consumer would spin forever.  This concept makes the contract explicit
// and the compiler error actionable.
template<typename T>
concept NothrowMovable =
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_move_assignable_v<T>;

// ── MPSCRingBuffer ────────────────────────────────────────────────────────────
//
//  T — element type.  Must satisfy NothrowMovable.
//  N — capacity.  Must be a power of two, [2, 2^32].
//
template<NothrowMovable T, std::size_t N>
class MPSCRingBuffer {
    // ── Compile-time guards ───────────────────────────────────────────────────
    static_assert((N & (N - 1u)) == 0u,
        "MPSCRingBuffer: N must be a power of two");
    static_assert(N >= 2u,
        "MPSCRingBuffer: N must be at least 2");
    static_assert(N <= (std::size_t{1} << 32u),
        "MPSCRingBuffer: N exceeds practical limit (2^32)");

    // ── Per-slot commit flag — one full cache line each ───────────────────────
    // FIX-FS: Each flag occupies its own cache line so push/pop on adjacent
    // slots do not share a cache line and cause destructive false sharing.
    // sizeof(std::atomic<bool>) is platform-dependent (typically 1 byte);
    // explicit padding fills the remainder up to CACHE_LINE_SIZE.
    struct alignas(CACHE_LINE_SIZE) Slot {
        static_assert(sizeof(std::atomic<bool>) < CACHE_LINE_SIZE,
            "sizeof(atomic<bool>) >= CACHE_LINE_SIZE — padding would underflow");

        std::atomic<bool> ready{false};           // FIX-CTR: default-init to false
        char _pad[CACHE_LINE_SIZE - sizeof(std::atomic<bool>)]{};
    };

    // FIX-SA: Verify padding arithmetic is correct for this platform.
    static_assert(sizeof(Slot) == CACHE_LINE_SIZE,
        "Slot size != CACHE_LINE_SIZE — cache-line padding is incorrect on this platform");

public:
    // ── Constructor ───────────────────────────────────────────────────────────
    // FIX-CTR: Slot::ready default member initialiser ({false}) ensures all
    // flags are zero-initialised at construction.  Explicit loop removed.
    MPSCRingBuffer() noexcept = default;

    // Prevent accidental copy/move of a live lock-free structure.
    MPSCRingBuffer(const MPSCRingBuffer&)            = delete;
    MPSCRingBuffer& operator=(const MPSCRingBuffer&) = delete;
    MPSCRingBuffer(MPSCRingBuffer&&)                 = delete;
    MPSCRingBuffer& operator=(MPSCRingBuffer&&)      = delete;

    // ── push — producer side, safe for concurrent callers ────────────────────
    //
    // Returns true  — item enqueued successfully.
    // Returns false — buffer full; caller must retry or apply backpressure.
    //
    // Noexcept is sound: NothrowMovable<T> guarantees buf_[slot]=std::move(val)
    // cannot throw, so ready_ is always set after a successful CAS, and the
    // consumer never spins on an uncommitted slot.
    //
    // Memory ordering:
    //   CAS (acq_rel / relaxed):
    //     Acquire prevents the compiler/CPU reordering the tail_ load above the
    //     CAS read.  Release publishes the new head_ value so other producers
    //     observe the slot as claimed.
    //   slots_[slot].ready.store (release):
    //     Pairs with the consumer's acquire load in pop().  Guarantees the
    //     consumer observes buf_[slot] fully written before reading it.
    //
    [[nodiscard]] bool push(T&& val) noexcept {
        std::size_t head = head_.load(std::memory_order_relaxed);
        std::size_t next;
        do {
            // FIX-CAP + FIX-ABA: Full check with unbounded counters.
            // head - tail >= N means all N slots are occupied.
            // Unsigned subtraction is well-defined and wraps correctly even
            // when head_ has overflowed size_t (theoretical; ~10^19 ops needed).
            // tail_ loaded acquire: synchronises with consumer's release store
            // of tail_ in pop(), making slot reclamation visible.
            if (head - tail_.load(std::memory_order_acquire) >= N)
                return false;

            next = head + 1u;

            // FIX-ABA: CAS on unbounded head_.  On failure, head is updated
            // to the current value of head_, so the full-check and slot
            // re-derivation happen automatically on the next iteration.
        } while (!head_.compare_exchange_weak(
                     head, next,
                     std::memory_order_acq_rel,   // success
                     std::memory_order_relaxed));  // failure

        // We exclusively own slot `head & (N-1)`.
        const std::size_t slot = head & (N - 1u);
        buf_[slot] = std::move(val);                                // nothrow per concept
        slots_[slot].ready.store(true, std::memory_order_release);  // commit to consumer
        return true;
    }

    // ── pop — consumer side — SINGLE consumer only ───────────────────────────
    //
    // Returns std::nullopt if the buffer is empty.
    // Otherwise returns the next item in FIFO order.
    //
    // Calling pop() from more than one thread concurrently is undefined
    // behaviour — this is an MPSC buffer, not MPMC.
    //
    // Memory ordering:
    //   head_ load (acquire):
    //     Ensures we observe all prior stores by producers before deciding
    //     the buffer is empty.
    //   slots_[slot].ready load (acquire):
    //     Pairs with producer's release store — ensures buf_[slot] is
    //     fully visible before we read it.
    //   slots_[slot].ready.store false (relaxed):
    //     The slot cannot be reclaimed by any producer until tail_ advances
    //     past it.  tail_'s release fence (below) provides the necessary
    //     happens-before for producers' subsequent acquire load of tail_.
    //   tail_.store (release):
    //     Pairs with producer's acquire load of tail_ in the full-check,
    //     making the reclaimed slot visible to producers.
    //
    [[nodiscard]] std::optional<T> pop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // Empty: consumer has caught up to the write cursor.
        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt;

        const std::size_t slot = tail & (N - 1u);

        // Spin until the producer commits this slot.
        // A producer may have advanced head_ (claiming the slot) but not yet
        // written the payload and set ready_.  This window is normally in the
        // nanosecond range.
        //
        // FIX-SPIN: TQC_SPIN_PAUSE() on every tight iteration reduces memory-
        // bus pressure and allows the hardware prefetcher to make progress.
        // After SPIN_YIELD_THRESHOLD iterations we yield to avoid starving
        // other threads on a loaded core.  We never return std::nullopt here —
        // once a slot is claimed (head_ advanced), we MUST deliver its item to
        // preserve exactly-once semantics.
        //
        // Liveness note: if a producer thread dies after claiming a slot but
        // before committing it, this spin becomes permanent.  This is acceptable
        // for this pipeline (producers are owned system threads, not untrusted
        // external code).  If producer liveness is not guaranteed, replace
        // ready_ with a sequence-number scheme and add a timeout.
        static constexpr int SPIN_YIELD_THRESHOLD = 512;

        for (int spin = 0;
             !slots_[slot].ready.load(std::memory_order_acquire);
             ++spin)
        {
            if (spin < SPIN_YIELD_THRESHOLD) {
                TQC_SPIN_PAUSE();
            } else {
                std::this_thread::yield();
            }
        }

        T val = std::move(buf_[slot]);
        slots_[slot].ready.store(false, std::memory_order_relaxed); // release slot
        tail_.store(tail + 1u, std::memory_order_release);           // advance cursor
        return val;
    }

    // ── Observers ─────────────────────────────────────────────────────────────
    // FIX-OBS: Required for monitoring, backpressure, and diagnostics.
    // These are approximate under concurrent push/pop — not linearisable —
    // but safe to call from any thread at any time.

    // Number of items currently enqueued (approximate).
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return h - t;  // unsigned: correct even across size_t overflow
    }

    // True if the buffer contains no items (approximate).
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    // Maximum number of items the buffer can hold.
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

private:
    // ── Data members ──────────────────────────────────────────────────────────
    //
    // Layout rationale:
    //   head_ and tail_ on separate cache lines to prevent producer/consumer
    //   false sharing on the cursors themselves.
    //   buf_ cache-line aligned so it does not share a partial line with tail_.
    //   slots_ needs no additional alignas because each Slot is already
    //   individually aligned to CACHE_LINE_SIZE via the struct attribute.

    // FIX-ABA: Unbounded monotonic write cursor.  Slot index = head_ & (N-1).
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> head_{0};

    // FIX-ABA: Unbounded monotonic read cursor.  Slot index = tail_ & (N-1).
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> tail_{0};

    // FIX-BUF: Cache-line aligned so no element shares a line with tail_.
    alignas(CACHE_LINE_SIZE) std::array<T, N> buf_{};

    // FIX-FS: Per-slot commit flags, each padded to a full cache line.
    std::array<Slot, N> slots_{};
};

} // namespace tqc

// Clean up internal macros — do not leak into including translation units.
#undef TQC_SPIN_PAUSE
