/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "apu_xaudio2.h"

/* Hearing safety, for every output path: master volume, then a soft limiter
 * whose output never exceeds -6 dBFS (16384). c * tanh(x / c) is linear for
 * quiet signals and bends smoothly towards the ceiling for loud ones, so a
 * spike (a mixdown summing many bins, a decoder glitch) is capped without the
 * harsh edge of hard clipping. */
static float g_xa2_volume = 0.5f;

void apu_output_safety(const int16_t *in, int16_t *out, int n)
{
    const float ceiling = 16384.0f;
    int i;
    for (i = 0; i < n; i++)
        out[i] = (int16_t)(ceiling * tanhf((float)in[i] * g_xa2_volume / ceiling));
}

void xa2_set_master_volume(float v)
{
    g_xa2_volume = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* The XAudio2 backend is Windows-only; on other hosts the same xa2_* calls
 * are served by SDL2 audio (below). */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      12     /* 12 x 256-sample blocks = 64 ms of headroom */

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static Xa2Stats                g_xa2_stats;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) {
        g_xa2_stats.dropped++;
        return 0;
    }
    {
        static LARGE_INTEGER f, prev;
        LARGE_INTEGER now;
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        if (state.BuffersQueued == 0 && g_xa2_stats.submitted) {
            g_xa2_stats.underruns++;
            if (getenv("RECOMP_APU_TRACE") && g_xa2_stats.underruns < 20)
                fprintf(stderr, "[XA2] underrun: %.1f ms since the previous block\n",
                        (now.QuadPart - prev.QuadPart) * 1000.0 / f.QuadPart);
        }
        prev = now;
    }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    {
        int i, n = copy_samples * XA2_CHANNELS;
        for (i = 0; i < n; i++) {
            int a = samples[i] < 0 ? -samples[i] : samples[i];
            if (a >= 32767) g_xa2_stats.clipped++;
            if (a > g_xa2_stats.peak) g_xa2_stats.peak = a;
        }
        g_xa2_stats.samples += (uint64_t)copy_samples;
    }
    apu_output_safety(samples, &g_xa2_bufs[idx][0][0], copy_samples * XA2_CHANNELS);

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    g_xa2_stats.submitted++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

int xa2_queued(void)
{
    XAUDIO2_VOICE_STATE state;
    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)state.BuffersQueued;
}

int xa2_queue_target(void)
{
    return 8;
}

void xa2_get_stats(Xa2Stats *out)
{
    *out = g_xa2_stats;          /* ponytail: unlocked; counters only grow */
    g_xa2_stats.peak = 0;
}

#else /* !_WIN32 -- SDL2 audio (Linux, Nintendo Switch) */

/* The same contract as the XAudio2 path: the APU frame thread submits one
 * 256-sample block per 8 EP frames and paces itself by xa2_queued(), so the
 * output device's clock drains the queue and drives the APU clock (and with
 * it DirectSound's play cursor, which paces the title's movies).
 *
 * SDL_QueueAudio, not a callback: the frame thread already produces blocks
 * at the device's rate, and a callback would need a second ring and lock.
 * SDL copies each block into its own queue, so the samples never go to the
 * sound service from guest memory (on Horizon the audren driver copies from
 * SDL's mix buffer into its own pool).
 *
 * RECOMP_AUDIO=0 keeps the host device closed; the APU then paces by wall
 * clock as before. RECOMP_AUDIO_BLOCKS sets the queue depth the frame thread
 * keeps (256-sample blocks, default 8 = 43 ms; Switch 12 = 64 ms, since its
 * threads time-slice in 10 ms quanta). RECOMP_AUDIO_VOLUME = 0..100. */

#include <stdlib.h>
#include <SDL.h>

#define SDLA_SAMPLE_RATE   48000
#define SDLA_CHANNELS      2
#define SDLA_BLOCK_SAMPLES 256
#define SDLA_BLOCK_BYTES   (SDLA_BLOCK_SAMPLES * SDLA_CHANNELS * (int)sizeof(int16_t))
#define SDLA_MAX_SAMPLES   1024
#ifdef __SWITCH__
#define SDLA_DEFAULT_BLOCKS 12
#else
#define SDLA_DEFAULT_BLOCKS 8
#endif

static SDL_AudioDeviceID g_sdla_dev = 0;
static int               g_sdla_target = SDLA_DEFAULT_BLOCKS;
static int               g_sdla_frames_written = 0;
static Xa2Stats          g_sdla_stats;

static void dump_init(void);

int xa2_init(void)
{
    SDL_AudioSpec want, have;
    const char *e;

    if (g_sdla_dev) return 1;

    e = getenv("RECOMP_AUDIO");
    if (e && strcmp(e, "0") == 0) {
        fprintf(stderr, "[AUDIO] host output off (RECOMP_AUDIO=0)\n");
        return 0;
    }
    e = getenv("RECOMP_AUDIO_BLOCKS");
    if (e && atoi(e) >= 2 && atoi(e) <= 64) g_sdla_target = atoi(e);
    e = getenv("RECOMP_AUDIO_VOLUME");
    if (e) xa2_set_master_volume((float)atoi(e) / 100.0f);

    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[AUDIO] SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_zero(want);
    want.freq     = SDLA_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = SDLA_CHANNELS;
    want.samples  = 1024;
    want.callback = NULL;       /* SDL_QueueAudio */

    /* No allowed changes: SDL converts if the device wants another format. */
    g_sdla_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_sdla_dev) {
        fprintf(stderr, "[AUDIO] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    SDL_PauseAudioDevice(g_sdla_dev, 0);
    dump_init();

    g_sdla_frames_written = 0;
    fprintf(stderr, "[AUDIO] SDL %s output: %d Hz, %d ch, %d-sample device buffer,"
                    " queue %d x %d samples\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
            have.freq, have.channels, have.samples,
            g_sdla_target, SDLA_BLOCK_SAMPLES);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_sdla_dev) return;
    SDL_CloseAudioDevice(g_sdla_dev);
    g_sdla_dev = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fprintf(stderr, "[AUDIO] shut down (%d blocks written, %llu underruns,"
                    " %llu dropped)\n", g_sdla_frames_written,
            (unsigned long long)g_sdla_stats.underruns,
            (unsigned long long)g_sdla_stats.dropped);
}

int xa2_is_active(void)
{
    return g_sdla_dev != 0;
}

int xa2_queued(void)
{
    if (!g_sdla_dev) return 0;
    return (int)(SDL_GetQueuedAudioSize(g_sdla_dev) / SDLA_BLOCK_BYTES);
}

int xa2_queue_target(void)
{
    return g_sdla_target;
}

/* RECOMP_AUDIO_DUMP=<path>,<start s>,<seconds>: the exact PCM handed to the
 * device (48 kHz s16 stereo, after volume and limiter), from <start> seconds
 * after the device opened, kept in RAM and written in one go when full -- a
 * console capture to compare with a Linux one (SD writes while playing would
 * disturb what is being measured). */
static struct {
    int      on;
    FILE    *f;
    char     path[256];
    uint64_t skip, left;                /* samples */
    int16_t *buf;
    size_t   n, cap;                    /* int16 values */
} s_dump;

static void dump_init(void)
{
    const char *e = getenv("RECOMP_AUDIO_DUMP");
    char *c1, *c2;
    if (!e || !*e) return;
    snprintf(s_dump.path, sizeof s_dump.path, "%s", e);
    c1 = strchr(s_dump.path, ',');
    c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    if (!c1 || !c2) return;
    *c1 = 0;
    s_dump.skip = (uint64_t)atoi(c1 + 1) * SDLA_SAMPLE_RATE;
    s_dump.left = (uint64_t)atoi(c2 + 1) * SDLA_SAMPLE_RATE;
    s_dump.cap = (size_t)s_dump.left * SDLA_CHANNELS;
    s_dump.buf = s_dump.cap ? (int16_t *)malloc(s_dump.cap * sizeof(int16_t)) : NULL;
    if (!s_dump.buf) return;
    s_dump.on = 1;
    fprintf(stderr, "[AUDIO] dump: %llu s from %llu s to %s\n",
            (unsigned long long)(s_dump.left / SDLA_SAMPLE_RATE),
            (unsigned long long)(s_dump.skip / SDLA_SAMPLE_RATE), s_dump.path);
}

static void dump_block(const int16_t *pcm, int num_samples)
{
    size_t take;
    if (!s_dump.on) return;
    if (s_dump.skip >= (uint64_t)num_samples) { s_dump.skip -= num_samples; return; }
    pcm += s_dump.skip * SDLA_CHANNELS;
    num_samples -= (int)s_dump.skip;
    s_dump.skip = 0;
    take = (size_t)num_samples * SDLA_CHANNELS;
    if (take > s_dump.cap - s_dump.n) take = s_dump.cap - s_dump.n;
    memcpy(s_dump.buf + s_dump.n, pcm, take * sizeof(int16_t));
    s_dump.n += take;
    if (s_dump.n < s_dump.cap) return;
    s_dump.on = 0;
    s_dump.f = fopen(s_dump.path, "wb");
    if (s_dump.f) {
        fwrite(s_dump.buf, sizeof(int16_t), s_dump.n, s_dump.f);
        fclose(s_dump.f);
    }
    fprintf(stderr, "[AUDIO] dump written: %s (%zu bytes)%s\n", s_dump.path,
            s_dump.n * sizeof(int16_t), s_dump.f ? "" : " -- open failed");
    free(s_dump.buf);
    s_dump.buf = NULL;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int16_t buf[SDLA_MAX_SAMPLES * SDLA_CHANNELS];
    int i, n, queued;

    if (!g_sdla_dev) return 0;
    if (num_samples > SDLA_MAX_SAMPLES) num_samples = SDLA_MAX_SAMPLES;

    /* A device that stopped draining (a suspended applet) must not grow the
     * queue without bound; the frame thread stops waiting on it too. */
    queued = xa2_queued();
    if (queued >= g_sdla_target * 4) {
        g_sdla_stats.dropped++;
        return 0;
    }
    if (queued == 0 && g_sdla_stats.submitted) {
        g_sdla_stats.underruns++;
        if (getenv("RECOMP_APU_TRACE") && g_sdla_stats.underruns < 20)
            fprintf(stderr, "[AUDIO] underrun\n");
    }

    n = num_samples * SDLA_CHANNELS;
    for (i = 0; i < n; i++) {
        int a = samples[i] < 0 ? -samples[i] : samples[i];
        if (a >= 32767) g_sdla_stats.clipped++;
        if (a > g_sdla_stats.peak) g_sdla_stats.peak = a;
    }
    g_sdla_stats.samples += (uint64_t)num_samples;
    apu_output_safety(samples, buf, n);
    dump_block(buf, num_samples);

    if (SDL_QueueAudio(g_sdla_dev, buf, (Uint32)(n * sizeof(int16_t))) != 0)
        return 0;
    g_sdla_frames_written++;
    g_sdla_stats.submitted++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return SDLA_BLOCK_SAMPLES;
}

void xa2_get_stats(Xa2Stats *out)
{
    *out = g_sdla_stats;         /* unlocked; counters only grow */
    g_sdla_stats.peak = 0;
}

#endif /* _WIN32 */
