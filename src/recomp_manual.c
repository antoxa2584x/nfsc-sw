/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* The generated register model (g_eax/g_esp are thread-local there) and the
 * XBOX_PTR/MEM32 accessors; the register names below are its macros. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;

/* ── CRT memmove/memcpy ───────────────────────────────────────
 *
 * Two copies of the MSVC CRT memmove are linked (0x0032C800 and 0x002A9450;
 * memcpy is the same body). Their tail and backward-copy paths dispatch
 * through UnwindDown-style vectors indexed with a *negative* register -- the
 * table address in the instruction is the end of the table -- which the
 * lifter's forward table reader cannot follow, so those jumps were lowered
 * to indirect tail calls into the middle of the function and failed to
 * resolve: every copy whose length was not a multiple of 4, and every
 * overlapping backward copy, silently lost its tail.
 *
 * Replaced by the host memmove. cdecl (dst, src, count), returns dst, and
 * esi/edi are preserved, as the original restores them. Generated bodies are
 * skipped by tools.recomp --exclude-manual reading this file. */
static void crt_memmove(void)
{
    uint32_t dst = MEM32(esp + 4);
    uint32_t src = MEM32(esp + 8);
    uint32_t n   = MEM32(esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    eax = dst;
    esp += 4;   /* return address; cdecl, the caller pops the arguments */
}

void sub_0032C800(void) { crt_memmove(); }
void sub_0032ED70(void) { crt_memmove(); }

/* ── D3D fence wait, D3D_BlockOnTime (0x0035A0C0) ────────────
 *
 * stdcall (fence, flags), ret 8. The stock XDK 5849 D3D keeps its device at
 * 0x002F77A0, reached through the global at 0x002F7798:
 *   +0x2C  write sequence, bumped as fences are inserted
 *   +0x30  pointer to the semaphore the GPU releases as it completes work
 * and this spins until *(+0x30) catches up with the fence it was asked for.
 *
 * Without the pushbuffer executor nothing ever releases the semaphore, so the
 * GPU is reported caught up. With it (RECOMP_PB_EXEC) that would let D3D
 * reuse ring space and vertex memory the executor has not read yet (see the
 * toolkit's d3d8ltcg-device-context.md), so instead the real semaphore is
 * handed to the executor once and the original wait runs against it. */
#define NFSU2_D3D_DEVICE_PTR 0x00368118u
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_0035A0C0_gen(void);

void sub_0035A0C0(void)
{
    static int exec = -1;
    uint32_t dev = MEM32(NFSU2_D3D_DEVICE_PTR);
    uint32_t sem = dev ? MEM32(dev + 0x30) : 0;

    if (exec < 0)
        exec = getenv("RECOMP_PB_EXEC") != NULL;
    if (exec) {
        static uint32_t registered;
        if (sem && sem != registered) {
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X\n", sem);
            registered = sem;
        }
        if (MEM32(esp) == 0x0035A6DCu && MEM32(esp + 12) == 0x0010B24Bu) {
            /* Carbon's renderer (sub_00112B60) starts each frame with
             * sub_0010B240: BlockOnFence on the fence the previous frame
             * ended with ([0x45C254]), then Swap. Only after that does it
             * build the next frame, so the game and the pushbuffer executor
             * take turns (NFSU2's main loop did the same; Eden race: game
             * waited ~50% of the time, executor ~20% idle).
             * RECOMP_FRAME_LAG=1 waits for the fence of the frame before
             * instead: two frames in flight. */
            static int lag = -1;
            static uint32_t prev;
            if (lag < 0) {
                const char *e = getenv("RECOMP_FRAME_LAG");
                lag = e && *e == '1';
                fprintf(stderr, "[D3D] frame fence: %s\n",
                        lag ? "previous frame (RECOMP_FRAME_LAG=1)" : "this frame");
            }
            if (lag) {
                uint32_t mine = MEM32(esp + 4);
                if (prev)
                    MEM32(esp + 4) = prev;
                prev = mine;
            }
        }
        sub_0035A0C0_gen();
        return;
    }
    if (sem)
        MEM32(sem) = MEM32(dev + 0x2C);     /* "GPU" is always caught up */
    eax = 0;
    esp += 4 + 8;                           /* return address, 2 stdcall args */
}

/* ── D3D interrupt handlers at DISPATCH_LEVEL ────────────────
 *
 * D3D services the GPU from its DPC (0x002F2530) and, with the interrupt
 * masked, from its own busy-waits (BlockUntilIdle and friends) on the game
 * thread: both call the vblank handler (0x00362050) and the PGRAPH handler
 * (0x003625C0), which queues flips. On the Xbox the two never interleave --
 * one is a DPC, the other runs with the interrupt off. Here the DPC runs on
 * the kernel timer thread, and lifted code hands over the guest lock at any
 * function entry, so the vblank count could move between D3D computing a
 * flip's target vblank and checking it. D3D retires a flip only when the
 * count *equals* its target, so that flip stayed pending forever and
 * PersistDisplay, which waits for pending flips, never returned: the hang
 * starting a Quick Race or Career. Run at DISPATCH_LEVEL (the dispatch lock,
 * kernel_hal.c), neither can be interrupted by the other.
 *
 * Raising blocks on the lock that the timer thread holds while it waits for
 * the guest lock, so the guest lock is let go for the raise, as a kernel
 * call would; already at DISPATCH (inside the DPC) there is nothing to take. */
extern uint8_t xbox_KfRaiseIrql(uint8_t irql);
extern void xbox_KfLowerIrql(uint8_t irql);
extern uint8_t xbox_CurrentIrql(void);
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth);
extern void xbox_Nv2aVblankTaken(void);

static uint8_t d3d_isr_enter(void)
{
    uint8_t old = xbox_CurrentIrql();

    if (old < 2) {
        int d = xbox_gil_suspend();
        old = xbox_KfRaiseIrql(2);
        xbox_gil_resume(d);
    }
    return old;
}

static void d3d_isr_leave(uint8_t old)
{
    if (old < 2)
        xbox_KfLowerIrql(old);
}

/* Flips retired by a handler call: D3D's retire index, at +0x1BC of the
 * block both handlers take in ecx (their own reads index the pending-flip
 * slots from it). The pushbuffer executor's FLIP_STALL waits on these, as
 * PGRAPH waits on the retire's PGRAPH_INCREMENT write. */
extern void nv2a_pb_flip_retired(unsigned n);

/* Vblank handler, thiscall. It ends by writing PCRTC_INTR and spinning until
 * PMC_INTR bit 24 drops. Its callers (the DPC, D3D's busy-waits) have read
 * the bits before calling it, so this is the vblank being taken: clear them
 * now, and the spin ends at once instead of holding the guest lock until the
 * runtime's ack thread comes round. */
extern void sub_00362050_gen(void);

void sub_00362050(void)
{
    uint32_t blk = ecx;
    /* Read only once at DISPATCH: read before, a retire by the other
     * handler (DPC thread vs a D3D busy-wait) in between was counted by
     * both, the executor's flip index ran one off for good, and every
     * FLIP_STALL then waited out its 250 ms (Carbon, console, 3.7 fps). */
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);

    xbox_Nv2aVblankTaken();
    sub_00362050_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);
    d3d_isr_leave(old);
}

/* PGRAPH handler, thiscall, ecx = the device's hardware block (+0 =
 * 0xFD000000). Reads a software-method trap from PGRAPH_INTR / NSOURCE /
 * TRAPPED_ADDR / TRAPPED_DATA, acknowledges it by writing PGRAPH_INTR back,
 * and acts on it: NOP(5) sets the event BlockOnTime sleeps on, a flip trap
 * queues the flip (sub_002F2080). The acknowledge is write-1-to-clear on
 * hardware and a no-op on RAM. Left pending, every turn of a busy-wait took
 * the same trap again (tens of thousands of flips a second, until the flip
 * targets were millions of vblanks ahead); guessed from the handler's
 * PGRAPH_FIFO writes, a trap posted during a call was sometimes dropped (the
 * fence event never set). So the executor posts traps under a lock held here
 * around the call, and a call that found one reports it taken, which clears
 * it (nv2a_pb_exec.c). */
extern void sub_003625C0_gen(void);
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);

void sub_003625C0(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t blk = ecx;
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);     /* at DISPATCH, as above */
    uint32_t nsource;

    nv2a_pb_trap_lock();
    nsource = nv[0x400108 / 4];
    sub_003625C0_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);   /* a flip queued on its vblank retires at once */
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    d3d_isr_leave(old);
}

/* ── Bounding box against the view frustum, sub_00103F50 ─────────────
 *
 * thiscall (this, const float min[3], const float max[3], matrix), ret 12.
 * The box's centre c = (max + min) * K and half-size e = max - c (both
 * stored as floats, as the original does), then for each of six planes
 * (n, d) at [this] + 0x140 + 16k: r = |n|.e, dist = n.c + d; below K2 on
 * dist + r means outside (return 0); below K2 on dist - r means the box
 * straddles that plane. Returns 2 when inside all six, 1 when straddling.
 * With a matrix the box is first transformed by it (sub_000FE4E0, below).
 *
 * NFSU2's copy (0x9A330, ported 2026-10-06) was the hottest function of its
 * main thread in a race (~5% on the console): every object, every view,
 * every frame. Carbon's sums its terms in another order (r: z, y, x; dist:
 * z, x, y), kept here. Written in C it keeps
 * everything in registers. The arithmetic is the lifted code's -- doubles,
 * in the same order -- so results match it exactly; RECOMP_NATIVE_CHECK=1
 * runs both and reports any difference. RECOMP_NATIVE=0 turns it off. */
extern void sub_00103F50_gen(void);
extern void sub_000FE4E0_gen(void);

/* sub_000FE4E0: cdecl (matrix, float min[3], float max[3]) -- the box's
 * corners transformed by a 4x4 row-major matrix, as a box again (Arvo): both
 * start at the translation row, and for each row i and column j the larger
 * of min[i]*M[i][j] and max[i]*M[i][j] goes to max[j], the smaller to min[j].
 * As the original: the max product is rounded to float before the compare
 * (it goes through memory), the sums are kept in extended (here double)
 * precision and stored as floats at the end, and an unordered compare adds
 * the min product to max. */
static void box_transform(uint32_t m, float mn[3], float mx[3])
{
    double hi[3], lo[3];
    int i, j;
    for (j = 0; j < 3; j++)
        hi[j] = lo[j] = (double)MEMF(m + 0x30u + 4u * j);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            double mm = (double)MEMF(m + 16u * i + 4u * j);
            double e = (double)mn[i] * mm;
            double f = (double)(float)((double)mx[i] * mm);
            if (e < f) { hi[j] += f; lo[j] += e; }
            else       { hi[j] += e; lo[j] += f; }
        }
    for (j = 0; j < 3; j++) {
        mn[j] = (float)lo[j];
        mx[j] = (float)hi[j];
    }
}

static uint32_t frustum_box_native(uint32_t self, uint32_t pmin, uint32_t pmax,
                                   uint32_t matrix, uint32_t *out_ecx, uint32_t *out_edx)
{
    const double K = (double)MEMF(0x003A3C28u), K2 = (double)MEMF(0x003A3880u);
    float mn[3] = { MEMF(pmin), MEMF(pmin + 4), MEMF(pmin + 8) };
    float mx[3] = { MEMF(pmax), MEMF(pmax + 4), MEMF(pmax + 8) };
    float ax, ay, az, bx, by, bz;
    if (matrix)
        box_transform(matrix, mn, mx);
    ax = mn[0]; ay = mn[1]; az = mn[2];
    bx = mx[0]; by = mx[1]; bz = mx[2];
    float cx = (float)(((double)bx + (double)ax) * K);
    float cy = (float)(((double)by + (double)ay) * K);
    float cz = (float)(((double)bz + (double)az) * K);
    float ex = (float)((double)bx - (double)cx);
    float ey = (float)((double)by - (double)cy);
    float ez = (float)((double)bz - (double)cz);
    uint32_t planes = MEM32(self) + 0x144u, k;
    int straddle = 0;

    for (k = 0; k < 6; k++) {
        uint32_t p = planes + 16u * k;
        double nx = MEMF(p - 4), ny = MEMF(p), nz = MEMF(p + 4), d = MEMF(p + 8);
        double r = (fabs(nz) * ez + fabs(ny) * ey) + fabs(nx) * ex;
        double dist = ((cz * nz + cx * nx) + cy * ny) + d;
        if (dist + r < K2) {
            *out_ecx = p;
            *out_edx = k + 1;
            return 0;
        }
        if (dist - r < K2)
            straddle = 1;
    }
    *out_ecx = planes + 96u;
    *out_edx = 7;
    return straddle ? 1u : 2u;
}

void sub_00103F50(void)
{
    static int mode = -1;               /* 0 lifted, 1 native, 2 native + check */
    static unsigned long calls, with_matrix, mismatches;
    uint32_t self = ecx, pmin = MEM32(esp + 4), pmax = MEM32(esp + 8);
    uint32_t matrix = MEM32(esp + 12), r, rc, rd;

    if (mode < 0) {
        const char *e = getenv("RECOMP_NATIVE"), *c = getenv("RECOMP_NATIVE_CHECK");
        mode = (e && *e == '0') ? 0 : (c && *c == '1') ? 2 : 1;
    }
    calls++;
    with_matrix += matrix != 0;
    if (mode == 0) {
        sub_00103F50_gen();
        return;
    }
    r = frustum_box_native(self, pmin, pmax, matrix, &rc, &rd);
    if (mode == 2) {
        sub_00103F50_gen();             /* pops its own arguments */
        if (eax != r && mismatches++ < 20)
            fprintf(stderr, "[native] sub_00103F50 mismatch: lifted %u native %u "
                    "(box %08X-%08X this %08X)\n", eax, r, pmin, pmax, self);
        if ((calls & 0xFFFFF) == 0)
            fprintf(stderr, "[native] sub_00103F50: %lu calls, %lu with a matrix, "
                    "%lu mismatches\n", calls, with_matrix, mismatches);
        return;
    }
    eax = r;
    ecx = rc;
    edx = rd;
    esp += 16;                          /* return address + three arguments */
}

/* sub_000FE4E0 on its own (it has other callers): box_transform on guest
 * memory. Returns max (eax), as the original leaves it; cdecl. */
void sub_000FE4E0(void)
{
    static int mode = -1;
    uint32_t m = MEM32(esp + 4), pmin = MEM32(esp + 8), pmax = MEM32(esp + 12);
    float mn[3], mx[3];
    int j;

    if (mode < 0) {
        const char *e = getenv("RECOMP_NATIVE"), *c = getenv("RECOMP_NATIVE_CHECK");
        mode = (e && *e == '0') ? 0 : (c && *c == '1') ? 2 : 1;
    }
    if (!mode) {
        sub_000FE4E0_gen();
        return;
    }
    for (j = 0; j < 3; j++) {
        mn[j] = MEMF(pmin + 4u * j);
        mx[j] = MEMF(pmax + 4u * j);
    }
    box_transform(m, mn, mx);
    if (mode == 2) {
        static unsigned long calls, bad;
        sub_000FE4E0_gen();             /* writes the guest's boxes itself */
        calls++;
        for (j = 0; j < 3; j++)
            if (memcmp(&mn[j], (const void *)XBOX_PTR(pmin + 4u * j), 4)
                || memcmp(&mx[j], (const void *)XBOX_PTR(pmax + 4u * j), 4)) {
                if (bad++ < 20)
                    fprintf(stderr, "[native] sub_000FE4E0 mismatch at %u: native %g %g,"
                            " lifted %g %g\n", j, mn[j], mx[j],
                            MEMF(pmin + 4u * j), MEMF(pmax + 4u * j));
                break;
            }
        if ((calls & 0xFFFFF) == 0)
            fprintf(stderr, "[native] sub_000FE4E0: %lu calls, %lu mismatches\n", calls, bad);
        return;
    }
    for (j = 0; j < 3; j++) {
        MEMF(pmin + 4u * j) = mn[j];
        MEMF(pmax + 4u * j) = mx[j];
    }
    eax = pmax;
    ecx = pmin + 12u;
    edx = m + 8u + 48u;
    esp += 4;                           /* cdecl: the caller pops the arguments */
}

/* ── 4x4 matrix multiply, sub_002EFCB4 ─────────────────────────
 *
 * stdcall (out, a, b), ret 12, eax = out: out = a * b, row-major floats, in
 * SSE (shufps a[i][j] across the row, mulps by row j of b, addps). Every
 * xmm register of the lifted body is thread-local, so each call was ~100
 * TLS accesses (calls on Horizon). NFSU2's sub_000A2EA0 called it twice per object
 * drawn; at a drag start line (Coastal Express, ~2300 draws a frame) it was
 * 15-18% of the main thread on x86, native -22% main-thread time per frame.
 * Same sums in the same order, ((p0 + p1) + p2) + p3, no FMA contraction;
 * all rows are computed before the store, as out may alias a or b.
 * RECOMP_NATIVE=0 lifted, RECOMP_NATIVE_CHECK=1 both and compare (Linux race:
 * 0 mismatches in 4.4M calls). */
extern void sub_002EFCB4_gen(void);

__attribute__((optimize("fp-contract=off")))
static void mat4_mul(float *o, const float *a, const float *b)
{
    float r[16];
    int i, k;
    for (i = 0; i < 4; i++)
        for (k = 0; k < 4; k++) {
            float s = a[4 * i] * b[k];
            s = s + a[4 * i + 1] * b[4 + k];
            s = s + a[4 * i + 2] * b[8 + k];
            s = s + a[4 * i + 3] * b[12 + k];
            r[4 * i + k] = s;
        }
    memcpy(o, r, sizeof r);
}

void sub_002EFCB4(void)
{
    static int mode = -1;               /* 0 lifted, 1 native, 2 native + check */
    uint32_t out = MEM32(esp + 4), a = MEM32(esp + 8), b = MEM32(esp + 12);

    if (mode < 0) {
        const char *e = getenv("RECOMP_NATIVE"), *c = getenv("RECOMP_NATIVE_CHECK");
        mode = (e && *e == '0') ? 0 : (c && *c == '1') ? 2 : 1;
    }
    if (mode == 0) {
        sub_002EFCB4_gen();
        return;
    }
    if (mode == 2) {
        static unsigned long calls, bad;
        float o[16], ma[16], mb[16];
        memcpy(ma, (const void *)XBOX_PTR(a), 64);
        memcpy(mb, (const void *)XBOX_PTR(b), 64);
        mat4_mul(o, ma, mb);
        sub_002EFCB4_gen();             /* pops its own arguments */
        calls++;
        if (memcmp(o, (const void *)XBOX_PTR(out), 64) && bad++ < 20)
            fprintf(stderr, "[native] sub_002EFCB4 mismatch: out %08X a %08X b %08X\n",
                    out, a, b);
        if ((calls & 0xFFFFF) == 0)
            fprintf(stderr, "[native] sub_002EFCB4: %lu calls, %lu mismatches\n", calls, bad);
        return;
    }
    mat4_mul((float *)XBOX_PTR(out), (const float *)XBOX_PTR(a), (const float *)XBOX_PTR(b));
    eax = out;
    esp += 16;                          /* return address + three arguments */
}

/* ── Float to integer, sub_0032C47C (_ftol2) ──────────────────
 *
 * MSVC's CRT _ftol2: st(0) truncated to a 64-bit integer in edx:eax, popped.
 * Every (int)float cast in the title calls it (589 sites); the hottest
 * function of the main thread in a Linux race (4.4%). It rounds with fistp
 * (the control word's mode), then corrects towards zero by the sign of the
 * float (x - r): x >= 0 and x - r < 0 -> r - 1; x < 0 (sign of (float)x)
 * and x - r > 0 -> r + 1. r == 0 or the integer indefinite is returned as
 * is. Exact, including ecx, the frame it publishes and the x87 top.
 * RECOMP_NATIVE=0 lifted, RECOMP_NATIVE_CHECK=1 both and compare. */
extern void sub_0032C47C_gen(void);

__attribute__((noinline, cold)) static int64_t ftol_fist_slow(double x, uint16_t cw)
{
    return recomp_fist(x, cw, 64);
}

static inline int64_t ftol_fist(double x, uint16_t cw)
{
    if (__builtin_expect(((cw >> 10) & 3u) == 0 && fabs(x) < 0x1p63, 1))
        return (int64_t)rint(x);        /* host rounding is never changed: nearest */
    return ftol_fist_slow(x, cw);       /* other modes, NaN, out of range */
}

static inline void ftol2_native(double x, uint32_t *peax, uint32_t *pedx, uint32_t *pecx)
{
    int64_t r = ftol_fist(x, g_fp_control_word);
    uint32_t lo = (uint32_t)r, hi = (uint32_t)((uint64_t)r >> 32), d;
    float fx = (float)x, fd;
    uint32_t sx;

    if (lo == 0 && (hi & 0x7FFFFFFFu) == 0) {   /* 0 or the indefinite */
        *peax = lo;
        *pedx = hi;
        return;
    }
    fd = (float)(x - (double)r);
    memcpy(&d, &fd, 4);
    memcpy(&sx, &fx, 4);
    if (sx & 0x80000000u) {
        d ^= 0x80000000u;
        r += d >= 0x80000001u;          /* carry of d + 0x7FFFFFFF */
    } else {
        r -= d >= 0x80000001u;          /* borrow */
    }
    *peax = (uint32_t)r;
    *pedx = (uint32_t)((uint64_t)r >> 32);
    *pecx = d + 0x7FFFFFFFu;
}

/* Modes other than plain native, out of line so that the hot path below is
 * small enough for LTO to inline into its 581 lifted callers. */
static int s_ftol_mode = -1;            /* 0 lifted, 1 native, 2 native + check */

__attribute__((noinline, cold)) static void ftol2_slow(void)
{
    double x = g_fp_stack[g_fp_top & 7];
    uint32_t a, d, c = ecx, frame = esp - 4u;

    if (s_ftol_mode < 0) {
        const char *e = getenv("RECOMP_NATIVE"), *k = getenv("RECOMP_NATIVE_CHECK");
        s_ftol_mode = (e && *e == '0') ? 0 : (k && *k == '1') ? 2 : 1;
    }
    if (s_ftol_mode == 0) {
        sub_0032C47C_gen();
        return;
    }
    ftol2_native(x, &a, &d, &c);
    if (s_ftol_mode == 2) {
        static unsigned long calls, bad;
        int top = (g_fp_top + 1) & 7;
        uint32_t sp = esp + 4u;
        sub_0032C47C_gen();
        calls++;
        if ((eax != a || edx != d || ecx != c || g_fp_top != top || esp != sp
             || g_ebp != frame) && bad++ < 20)
            fprintf(stderr, "[native] sub_0032C47C mismatch for %.17g: lifted %08X:%08X "
                    "ecx %08X, native %08X:%08X ecx %08X\n", x, edx, eax, ecx, d, a, c);
        if ((calls & 0xFFFFFF) == 0)
            fprintf(stderr, "[native] sub_0032C47C: %lu calls, %lu mismatches\n", calls, bad);
        return;
    }
    eax = a;
    edx = d;
    ecx = c;
    g_fp_top = (g_fp_top + 1) & 7;
    g_ebp = g_seh_ebp = frame;
    esp += 4;
}

inline void sub_0032C47C(void)
{
    uint32_t a, d, c, frame;
    int top;

    if (__builtin_expect(s_ftol_mode != 1, 0)) {
        ftol2_slow();                   /* first call, lifted or check mode */
        return;
    }
    top = g_fp_top;
    c = ecx;
    frame = esp - 4u;
    ftol2_native(g_fp_stack[top & 7], &a, &d, &c);
    eax = a;
    edx = d;
    ecx = c;
    g_fp_top = (top + 1) & 7;
    g_ebp = g_seh_ebp = frame;          /* as its push ebp / mov ebp, esp left them */
    esp += 4;                           /* return address */
}


/* ── Small math leaves of the main thread ─────────────────────
 *
 * Each about 0.3-0.8% of the main thread in a Linux race, more on the
 * console, where every x87 slot and register the lifted bodies touch is
 * thread-local. Exact as the lifted code computes them (x87 values as
 * doubles, rounded to float where the original stores), including the
 * registers, flags and frame they leave behind. RECOMP_NATIVE=0 lifted,
 * RECOMP_NATIVE_CHECK=1 both and compare. */
static int native_mode(void)
{
    const char *e = getenv("RECOMP_NATIVE"), *c = getenv("RECOMP_NATIVE_CHECK");
    return (e && *e == '0') ? 0 : (c && *c == '1') ? 2 : 1;
}

static void native_report(const char *name, unsigned long *calls, unsigned long *bad, int ok,
                          uint32_t a0, uint32_t a1, uint32_t a2)
{
    ++*calls;
    if (!ok && (*bad)++ < 20)
        fprintf(stderr, "[native] %s mismatch: args %08X %08X %08X\n", name, a0, a1, a2);
    if ((*calls & 0xFFFFF) == 0 || *calls == 4096)
        fprintf(stderr, "[native] %s: %lu calls, %lu mismatches\n", name, *calls, *bad);
}

/* sub_0005D1F0: cdecl (out, m, v) -- out = v * m, m a row-major 4x4 with
 * the translation in row 3; all three sums before the stores. */
extern void sub_0005D1F0_gen(void);

static void xform_point(uint32_t m, uint32_t v, float o[3])
{
    double v0 = MEMF(v), v1 = MEMF(v + 4), v2 = MEMF(v + 8);
    o[0] = (float)(((MEMF(m + 0x20) * v2 + MEMF(m + 0x10) * v1) + MEMF(m + 0x00) * v0)
                   + MEMF(m + 0x30));
    o[1] = (float)(((MEMF(m + 0x24) * v2 + MEMF(m + 0x04) * v0) + MEMF(m + 0x14) * v1)
                   + MEMF(m + 0x34));
    o[2] = (float)(((MEMF(m + 0x28) * v2 + MEMF(m + 0x08) * v0) + MEMF(m + 0x18) * v1)
                   + MEMF(m + 0x38));
}

void sub_0005D1F0(void)
{
    static int mode = -1;
    uint32_t out = MEM32(esp + 4), m = MEM32(esp + 8), v = MEM32(esp + 12), e0 = esp;
    float o[3];

    if (mode < 0)
        mode = native_mode();
    if (mode == 0) {
        sub_0005D1F0_gen();
        return;
    }
    xform_point(m, v, o);
    if (mode == 2) {
        static unsigned long calls, bad;
        uint32_t sv_edx = edx;
        int top = g_fp_top;
        sub_0005D1F0_gen();
        native_report("sub_0005D1F0", &calls, &bad,
                      !memcmp(o, (const void *)XBOX_PTR(out), 12) && eax == out && ecx == v
                      && edx == sv_edx && g_fp_top == top && esp == e0 + 4u
                      && g_ebp == e0 - 4u && g_seh_ebp == e0 - 4u, out, m, v);
        return;
    }
    memcpy((void *)XBOX_PTR(out), o, 12);
    eax = out;
    ecx = v;
    g_ebp = g_seh_ebp = e0 - 4u;
    esp += 4;
}

/* sub_00048B60: cdecl (dst, src) -- a 4x4 float matrix copied row by row
 * (each row read before it is written; the middle two of each row through
 * the x87, as doubles). Its argument slot src ends as src[15]'s bits. */
extern void sub_00048B60_gen(void);

void sub_00048B60(void)
{
    static int mode = -1;
    uint32_t e0 = esp, dst = MEM32(esp + 4), src = MEM32(esp + 8), r;

    if (mode < 0)
        mode = native_mode();
    if (mode == 0) {
        sub_00048B60_gen();
        return;
    }
    if (mode == 2) {
        static unsigned long calls, bad;
        uint32_t want[16];
        for (r = 0; r < 16; r++) {
            volatile double f = MEMF(src + 4u * r);
            float g = (float)f;
            if ((r & 3) == 1 || (r & 3) == 2)
                memcpy(&want[r], &g, 4);
            else
                want[r] = MEM32(src + 4u * r);
        }
        sub_00048B60_gen();
        native_report("sub_00048B60", &calls, &bad,
                      !memcmp(want, (const void *)XBOX_PTR(dst), 64) && eax == dst
                      && ecx == want[12] && edx == want[15] && MEM32(e0 + 8u) == want[15]
                      && esp == e0 + 4u && g_ebp == e0 - 4u && g_seh_ebp == e0 - 4u,
                      dst, src, 0);
        return;
    }
    for (r = 0; r < 64; r += 16) {
        uint32_t w0 = MEM32(src + r), w3 = MEM32(src + r + 12);
        volatile double f1 = MEMF(src + r + 4), f2 = MEMF(src + r + 8);
        MEMF(dst + r + 4) = (float)f1;
        MEM32(dst + r) = w0;
        MEMF(dst + r + 8) = (float)f2;
        MEM32(dst + r + 12) = w3;
    }
    MEM32(e0 + 8u) = MEM32(src + 0x3C);
    eax = dst;
    ecx = MEM32(src + 0x30);
    edx = MEM32(src + 0x3C);
    g_ebp = g_seh_ebp = e0 - 4u;
    esp += 4;
}

/* x87 compare as the lifted code records it; with sw, also fnstsw ax. */
static inline uint16_t fcmp_rec(double a, double b, int top, int sw)
{
    g_fp_cmp = RECOMP_FCMP(a, b);
    g_fp_cc = RECOMP_FCMP_CC(g_fp_cmp);
    if (sw)
        eax = (eax & 0xFFFF0000u) | (uint16_t)(((top & 7u) << 11) | g_fp_cc);
    return g_fp_cc;
}

/* sub_0005DC70: cdecl (const float p[2], const float s[2], const float q[2],
 * float m) -> 1 when q lies within [p - m, p + s + m] on both axes. */
extern void sub_0005DC70_gen(void);

void sub_0005DC70(void)
{
    static int mode = -1;
    uint32_t e0 = esp, p = MEM32(esp + 4), s = MEM32(esp + 8), q = MEM32(esp + 12);
    uint32_t sv_eax = eax, sv_edx = edx, res = 0, a, d = edx;
    double m = MEMF(esp + 16);
    int top = g_fp_top, k, cmp;
    uint16_t cc;

    if (mode < 0)
        mode = native_mode();
    if (mode == 0) {
        sub_0005DC70_gen();
        return;
    }
    for (k = 0; k < 2; k++) {
        if (!(fcmp_rec((double)MEMF(p + 4u * k) - m, MEMF(q + 4u * k), top, 1) & 0x4100u))
            break;
        d = s;
        if ((fcmp_rec(m + MEMF(s + 4u * k), MEMF(q + 4u * k), top, 1) & 0x4500u) == 0x0100u)
            break;
    }
    res = k == 2;
    a = res; cmp = g_fp_cmp; cc = g_fp_cc;
    if (mode == 2) {
        static unsigned long calls, bad;
        eax = sv_eax;
        edx = sv_edx;
        sub_0005DC70_gen();
        native_report("sub_0005DC70", &calls, &bad,
                      eax == a && ecx == q && edx == d && g_fp_cmp == cmp && g_fp_cc == cc
                      && g_fp_top == top && esp == e0 + 4u
                      && g_ebp == e0 - 4u && g_seh_ebp == e0 - 4u, p, s, q);
        return;
    }
    eax = res;
    ecx = q;
    edx = d;
    g_ebp = g_seh_ebp = e0 - 4u;
    esp += 4;
}

/* sub_0035EE20: D3D's SSE 4x4 multiply, stdcall (out, a, b), ret 12 --
 * the same sums as sub_002EFCB4 (mat4_mul). Leaves the result rows in
 * xmm2..xmm5 and a[3][2] * b row 2 / a[3][3] * b row 3 in xmm0 / xmm1;
 * eax = a, ecx = out. */
extern void sub_0035EE20_gen(void);

__attribute__((optimize("fp-contract=off")))
static void d3d_mat_mul(uint32_t out, uint32_t a, uint32_t b, RecompXmm x[6])
{
    float ma[16], mb[16], o[16];
    int k;
    memcpy(ma, (const void *)XBOX_PTR(a), 64);
    memcpy(mb, (const void *)XBOX_PTR(b), 64);
    mat4_mul(o, ma, mb);
    for (k = 0; k < 4; k++) {
        x[0].f[k] = ma[14] * mb[8 + k];
        x[1].f[k] = ma[15] * mb[12 + k];
    }
    memcpy(&x[2], o, 64);
}

void sub_0035EE20(void)
{
    static int mode = -1;
    uint32_t e0 = esp, out = MEM32(esp + 4), a = MEM32(esp + 8), b = MEM32(esp + 12);
    RecompXmm x[6];

    if (mode < 0)
        mode = native_mode();
    if (mode == 0) {
        sub_0035EE20_gen();
        return;
    }
    d3d_mat_mul(out, a, b, x);
    if (mode == 2) {
        static unsigned long calls, bad;
        sub_0035EE20_gen();
        native_report("sub_0035EE20", &calls, &bad,
                      !memcmp(&x[2], (const void *)XBOX_PTR(out), 64) && eax == a && ecx == out
                      && !memcmp(&x[0], &g_xmm0, 16) && !memcmp(&x[1], &g_xmm1, 16)
                      && !memcmp(&x[2], &g_xmm2, 16) && !memcmp(&x[3], &g_xmm3, 16)
                      && !memcmp(&x[4], &g_xmm4, 16) && !memcmp(&x[5], &g_xmm5, 16)
                      && esp == e0 + 16u, out, a, b);
        return;
    }
    memcpy((void *)XBOX_PTR(out), &x[2], 64);
    g_xmm0 = x[0]; g_xmm1 = x[1]; g_xmm2 = x[2];
    g_xmm3 = x[3]; g_xmm4 = x[4]; g_xmm5 = x[5];
    eax = a;
    ecx = out;
    esp += 16;                          /* return address + three arguments */
}

/* ── DirectSound DSP command post (0x0039406B) ───────────────
 *
 * thiscall, ecx = the DSP-side object. Copies a command block into the GP
 * DSP's scratch area, stores the command code at scratch + 0x810, and spins
 * until the DSP zeroes it (this loop does re-read memory). The scratch block
 * is ***(this + 8 + 0x10), the same chain the runtime's RECOMP_DSP_ACK notes
 * describe. There is no DSP, so register the word with the runtime's
 * completion list before running the original body. */
extern int xbox_ApuDspAckWord(uint32_t va);
extern void sub_0039406B_gen(void);

void sub_0039406B(void)
{
    uint32_t obj = MEM32(ecx + 8);
    uint32_t blk = obj ? MEM32(obj + 0x10) : 0;
    uint32_t scratch = blk ? MEM32(blk) : 0;

    if (scratch)
        xbox_ApuDspAckWord(scratch + 0x810);
    sub_0039406B_gen();
}

/* ── DirectSound EP watchdog reset (0x003947ED) ──────────────
 *
 * thiscall, no arguments. Carbon's DSOUND (a later 5849 build, flags 0x400B;
 * NFSU1/NFSU2 have no such check) ends its per-frame work (sub_00394A2D) by
 * reading a word in the EP DSP's memory through the pointer at 0x0039AD08
 * (0xFE85A018); the EP firmware keeps 0xCCCCCC there. Anything else means
 * the DSP died, and it calls this: reset the whole APU (both AC'97
 * channels, a 10 ms stall). The runtime's DSPs are stubbed and the APU model
 * reads their memory as 0, so the title reset the APU every frame (~60/s)
 * and the intro movie, paced by the audio clock, froze on one frame.
 *
 * The DSP memory cannot just start holding writes: DSOUND's mailboxes there
 * "complete" today because they read back 0. So only the watchdog's call
 * (return address 0x00394AD1) is skipped; DirectSound's own resets run. */
extern void sub_003947ED_gen(void);

void sub_003947ED(void)
{
    if (MEM32(esp) == 0x00394AD1u) {
        esp += 4;                       /* return address; no arguments */
        return;
    }
    sub_003947ED_gen();
}

/* ── DirectSound AC'97 channel reset (0x00399A64) ────────────
 *
 * thiscall, ecx = channel object. Sets RR (bit 1) in the channel's NABM
 * control byte, then waits for the controller to clear it -- except MSVC
 * hoisted the load out of the loop, so the original reads the byte once and
 * spins on the register copy forever unless the bit is already clear at that
 * one read:
 *
 *     mov  byte [eax+0xFEC0010B], 2
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * On hardware the reset has completed by then. The Windows runtime answers it
 * with a write trap on the NABM page; there is no fault handling on POSIX or
 * Switch, so this is the same function with the reset completing at once.
 * Everything else -- the lock taken through sub_00391BB0/sub_00391BD2, the
 * buffer-descriptor base store, the 0xFEC0017C write for channel 1 and the
 * sub_0039986E re-arm -- is as the original does it. */
void sub_00399A64(void)
{
    uint32_t ebp_saved = g_ebp, frame, chan, nabm;

    PUSH32(esp, ebp_saved);
    frame = esp;
    g_ebp = frame;
    g_seh_ebp = frame;
    esp -= 8;
    MEM32(frame - 4) = 0;
    PUSH32(esp, esi);
    esi = ecx;
    chan = esi;

    ecx = frame - 8;
    PUSH32(esp, 0x00399A78u); RECOMP_ABI_CALL(0x00391BB0u, sub_00391BB0);

    nabm = MEM32(MEM32(chan) * 4 + 0x0039A6B0u);
    MEM8(nabm + 0xFEC0010Bu) = 2;
    MEM8(nabm + 0xFEC0010Bu) &= (uint8_t)~2u;   /* reset complete */
    eax = nabm;
    SET_LO8(ecx, 0);

    MEM32(nabm + 0xFEC00100u) = MEM32(chan + 0x1C);
    if (MEM32(chan) == 1)
        MEM32(0xFEC0017Cu) = MEM32(chan + 0x28);

    PUSH32(esp, 1);
    PUSH32(esp, 1);
    ecx = chan;
    PUSH32(esp, MEM8(chan + 0x24));
    PUSH32(esp, MEM8(chan + 0x25));
    eax = MEM8(chan + 0x25);
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x00399ACBu); RECOMP_ABI_CALL(0x0039986Eu, sub_0039986E);

    ecx = frame - 8;
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x00399AD3u); RECOMP_ABI_CALL(0x00391BD2u, sub_00391BD2);

    POP32(esp, esi);
    esp = frame;
    POP32(esp, ebp_saved);
    g_ebp = ebp_saved;
    esp += 4;
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
