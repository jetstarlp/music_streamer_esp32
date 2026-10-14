/*
 * Host unit tests for the hardware-independent parts of the bench firmware:
 * main/bench_util.c and components/audio_out/tone_gen.c.
 * Run: firmware/tools/host_test_bench.sh
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_util.h"
#include "fade.h"
#include "pcm_engine.h"
#include "pcm_ring.h"
#include "tone_gen.h"

static int s_fail;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            s_fail++;                                                            \
        }                                                                        \
    } while (0)

static void test_parse_tone(void)
{
    bench_tone_args_t a;
    CHECK(bench_parse_tone(0, NULL, &a) == NULL);
    CHECK(a.freq_hz == 1000.0f && a.dbfs == -20.0f && a.seconds == 5.0f);

    char *ok[] = {"440", "-6", "2.5"};
    CHECK(bench_parse_tone(3, ok, &a) == NULL);
    CHECK(a.freq_hz == 440.0f && a.dbfs == -6.0f && a.seconds == 2.5f);

    char *positive[] = {"60", "0"};
    CHECK(bench_parse_tone(2, positive, &a) == NULL && a.dbfs == 0.0f);

    char *unsigned_level[] = {"1000", "20"};
    CHECK(bench_parse_tone(2, unsigned_level, &a) == NULL && a.dbfs == -20.0f);

    char *plus[] = {"1000", "+3"};
    CHECK(bench_parse_tone(2, plus, &a) != NULL);
    char *low_freq[] = {"5"};
    CHECK(bench_parse_tone(1, low_freq, &a) != NULL);
    char *junk[] = {"1k"};
    CHECK(bench_parse_tone(1, junk, &a) != NULL);
    char *nan_arg[] = {"nan"};
    CHECK(bench_parse_tone(1, nan_arg, &a) != NULL);
    char *too_quiet[] = {"1000", "-120"};
    CHECK(bench_parse_tone(2, too_quiet, &a) != NULL);
    char *too_long[] = {"1000", "-20", "4000"};
    CHECK(bench_parse_tone(3, too_long, &a) != NULL);
    char *too_many[] = {"1", "2", "3", "4"};
    CHECK(bench_parse_tone(4, too_many, &a) != NULL);
}

static void test_checks(void)
{
    CHECK(bench_check_ssid("Home") == NULL);
    CHECK(bench_check_ssid("") != NULL);
    CHECK(bench_check_ssid("123456789012345678901234567890123") != NULL);   /* 33 bytes */
    CHECK(bench_check_ssid("bad\tssid") != NULL);

    CHECK(bench_check_wifi_password("") == NULL);            /* open network */
    CHECK(bench_check_wifi_password("short") != NULL);
    CHECK(bench_check_wifi_password("longenough") == NULL);
    CHECK(bench_check_wifi_password(
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == NULL);
    CHECK(bench_check_wifi_password(
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg") != NULL);

    CHECK(bench_check_device_name("Music Streamer") == NULL);
    CHECK(bench_check_device_name(" leading") != NULL);
    CHECK(bench_check_device_name("") != NULL);

    CHECK(bench_check_credential("0123456789abcdef0123456789abcdef") == NULL);
    CHECK(bench_check_credential("with space") != NULL);
    CHECK(bench_is_hex32("0123456789abcdef0123456789ABCDEF"));
    CHECK(!bench_is_hex32("0123456789abcdef"));
}

static void test_mask_and_join(void)
{
    char out[32];
    bench_mask_secret("0123456789abcdef0123456789abcdef", out, sizeof(out));
    CHECK(strcmp(out, "****cdef") == 0);
    bench_mask_secret("", out, sizeof(out));
    CHECK(strcmp(out, "(not set)") == 0);
    bench_mask_secret("abcdefgh", out, sizeof(out));    /* short: only a quarter shown */
    CHECK(strcmp(out, "****gh") == 0);

    char *words[] = {"Wohnzimmer", "Streamer"};
    CHECK(bench_join_args(2, words, out, sizeof(out)) && strcmp(out, "Wohnzimmer Streamer") == 0);
    char tiny[8];
    CHECK(!bench_join_args(2, words, tiny, sizeof(tiny)) && tiny[0] == '\0');
}

static void test_led(void)
{
    CHECK(bench_led_is_on(BENCH_LED_CONNECTED, 123));
    CHECK(bench_led_is_on(BENCH_LED_NO_WIFI, 100) && !bench_led_is_on(BENCH_LED_NO_WIFI, 700));
    CHECK(bench_led_is_on(BENCH_LED_PLAYING, 50) && !bench_led_is_on(BENCH_LED_PLAYING, 150));
}

static void test_merge_wifi(void)
{
    settings_wifi_t stored[2] = {{"A", "passwordA"}, {"B", "passwordB"}};
    settings_wifi_t out[5];
    CHECK(bench_merge_wifi(stored, 2, "", "", out, 5) == 2);
    CHECK(bench_merge_wifi(stored, 2, "C", "passwordC", out, 5) == 3 && strcmp(out[2].ssid, "C") == 0);
    CHECK(bench_merge_wifi(stored, 2, "A", "other", out, 5) == 2);         /* no duplicate */
    CHECK(bench_merge_wifi(stored, 2, "C", "passwordC", out, 2) == 2);     /* respects max */
}

static void test_tone(void)
{
    tone_gen_t t;
    CHECK(!tone_gen_init(&t, 44100, 30000.0f, -20.0f, 1000));      /* above Nyquist */
    CHECK(!tone_gen_init(&t, 44100, 1000.0f, 1.0f, 1000));         /* above 0 dBFS */
    CHECK(!tone_gen_init(&t, 44100, 1000.0f, -20.0f, 0));
    CHECK(fabsf(tone_gen_dbfs_to_peak(0.0f) - 32767.0f) < 0.5f);
    CHECK(fabsf(tone_gen_dbfs_to_peak(-20.0f) - 3276.7f) < 0.5f);

    /* 100 ms, 1 kHz, 0 dBFS: exact length, starts and ends near zero, full peak in the middle */
    CHECK(tone_gen_init(&t, 44100, 1000.0f, 0.0f, 100));
    int16_t buf[2 * 8000];
    size_t n = tone_gen_fill(&t, buf, 8000);
    CHECK(n == 4410);
    CHECK(tone_gen_done(&t));
    CHECK(tone_gen_fill(&t, buf, 10) == 0);
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        CHECK(buf[2 * i] == buf[2 * i + 1]);                        /* both channels equal */
        int v = abs(buf[2 * i]);
        if (v > peak) {
            peak = v;
        }
    }
    CHECK(peak >= 32700);
    CHECK(abs(buf[0]) < 50 && abs(buf[2 * (n - 1)]) < 200);         /* faded edges */

    /* No click on an early stop: the step between neighbours stays small. */
    CHECK(tone_gen_init(&t, 44100, 1000.0f, -6.0f, 5000));
    n = tone_gen_fill(&t, buf, 2000);
    int16_t last = buf[2 * (n - 1)];
    tone_gen_stop(&t);
    size_t m = tone_gen_fill(&t, buf, 8000);
    CHECK(m == 441);                                                 /* exactly one 10 ms fade */
    CHECK(abs(buf[0] - last) < 3000);                                /* one sine step at 1 kHz */
    CHECK(abs(buf[2 * (m - 1)]) < 200);
}

static void test_fade(void)
{
    int16_t buf[2 * 300];
    for (int i = 0; i < 300; i++) {
        buf[2 * i] = 20000;
        buf[2 * i + 1] = -20000;
    }
    fade_in_s16(buf, 300, FADE_FRAMES);
    CHECK(abs(buf[0]) < 100 && abs(buf[1]) < 100);           /* starts near zero */
    CHECK(buf[2 * 255] > 19900 && buf[2 * 255 + 1] < -19900); /* ends at ~full level */
    CHECK(buf[2 * 299] == 20000);                              /* untouched after the fade */
    for (int i = 1; i < 256; i++) {
        CHECK(buf[2 * i] >= buf[2 * (i - 1)]);                 /* monotonic, no step */
    }
    fade_in_s16(buf, 10, FADE_FRAMES);                         /* short chunk: no overrun */

    int16_t ramp[2 * FADE_FRAMES];
    fade_out_ramp_s16(32767, -32768, ramp, FADE_FRAMES);
    CHECK(ramp[0] > 32500 && ramp[1] < -32500);                /* continues from the last sample */
    CHECK(ramp[2 * (FADE_FRAMES - 1)] == 0 && ramp[2 * (FADE_FRAMES - 1) + 1] == 0);
    int maxstep = 0;
    for (int i = 1; i < FADE_FRAMES; i++) {
        int d = abs(ramp[2 * i] - ramp[2 * (i - 1)]);
        if (d > maxstep) {
            maxstep = d;
        }
    }
    CHECK(maxstep <= 129);                                      /* 32767 / 256 per frame */
}


/* ---- pcm_ring / pcm_engine ---------------------------------------------- */

static void test_ring(void)
{
    static uint8_t store[1000];
    pcm_ring_t r;
    pcm_ring_init(&r, store, sizeof(store));
    CHECK(r.size == 512);                                   /* rounded down to a power of two */
    CHECK(pcm_ring_used(&r) == 0 && pcm_ring_free(&r) == 512);
    uint8_t in[700], out[700];
    for (int i = 0; i < 700; i++) {
        in[i] = (uint8_t)(i * 7 + 3);
    }
    CHECK(pcm_ring_write(&r, in, 700) == 512);              /* only what fits */
    CHECK(pcm_ring_write(&r, in, 1) == 0);
    CHECK(pcm_ring_read(&r, out, 300) == 300 && memcmp(out, in, 300) == 0);
    CHECK(pcm_ring_write(&r, in + 512, 188) == 188);        /* wraps around */
    CHECK(pcm_ring_read(&r, out, 700) == 400 && memcmp(out, in + 300, 400) == 0);
    CHECK(pcm_ring_used(&r) == 0);
    /* counters near 2^32 */
    r.wr = r.rd = 0xFFFFFF00u;
    CHECK(pcm_ring_write(&r, in, 512) == 512 && pcm_ring_used(&r) == 512);
    CHECK(pcm_ring_read(&r, out, 512) == 512 && memcmp(out, in, 512) == 0);
    CHECK(pcm_ring_write(&r, in, 100) == 100 && pcm_ring_drop(&r) == 100 && pcm_ring_used(&r) == 0);
}

#define ENG_RING (64 * 1024)
static uint8_t s_music_store[ENG_RING], s_tone_store[8192];

static void put_const(pcm_ring_t *r, size_t frames, int16_t l, int16_t rr)
{
    for (size_t i = 0; i < frames; i++) {
        int16_t f[2] = {l, rr};
        pcm_ring_write(r, f, sizeof(f));
    }
}

/* largest step between neighbouring left samples in buf (and from `prev`) */
static int max_step(const int16_t *buf, size_t frames, int16_t prev)
{
    int m = 0;
    for (size_t i = 0; i < frames; i++) {
        int d = abs(buf[2 * i] - prev);
        if (d > m) {
            m = d;
        }
        prev = buf[2 * i];
    }
    return m;
}

static void test_engine(void)
{
    pcm_ring_t music, tone;
    pcm_engine_t e;
    int16_t buf[2 * 512];
    pcm_ring_init(&music, s_music_store, sizeof(s_music_store));
    pcm_ring_init(&tone, s_tone_store, sizeof(s_tone_store));

    /* prebuffer: silence until 4096 bytes are there, then a fade-in */
    pcm_engine_init(&e, &music, &tone, 4096, 2000);
    put_const(&music, 512, 10000, -10000);                  /* 2048 bytes: not enough */
    pcm_engine_fill(&e, buf, 512);
    CHECK(buf[0] == 0 && buf[2 * 511] == 0 && e.frames_played == 0);
    put_const(&music, 2000, 10000, -10000);
    pcm_engine_fill(&e, buf, 512);
    CHECK(e.frames_played == 512);
    CHECK(abs(buf[0]) < 100 && buf[2 * 300] == 10000 && buf[2 * 300 + 1] == -10000);
    CHECK(max_step(buf, 512, 0) <= 10000 / 256 + 1);

    /* runs dry in the middle of a buffer: ramp from the last sample, then zeros */
    pcm_engine_fill(&e, buf, 512);
    pcm_engine_fill(&e, buf, 512);
    pcm_engine_fill(&e, buf, 512);                          /* 2512 frames in total */
    CHECK(e.frames_played == 2048);
    pcm_engine_fill(&e, buf, 512);
    CHECK(e.frames_played == 2512 && e.underruns == 1);
    CHECK(buf[2 * 463] == 10000);                           /* last real frame (index 463) */
    CHECK(max_step(buf, 512, 10000) <= 10000 / 256 + 1);    /* no click */
    pcm_engine_fill(&e, buf, 512);                          /* rest of the ramp, then silence */
    CHECK(buf[2 * 511] == 0 && max_step(buf, 512, buf[0]) <= 40);

    /* prebuffer timeout: some audio but less than the prebuffer -> plays after 2000 frames */
    put_const(&music, 100, 5000, 5000);
    for (int i = 0; i < 4; i++) {
        pcm_engine_fill(&e, buf, 512);                      /* waited 0, 512, 1024, 1536 */
    }
    CHECK(e.frames_played == 2512);
    pcm_engine_fill(&e, buf, 512);
    CHECK(e.frames_played == 2612);

    /* pause: ramp down, keep the ring; resume: fade in */
    pcm_engine_init(&e, &music, &tone, 0, 0);
    pcm_ring_drop(&music);
    put_const(&music, 4000, 20000, 20000);
    pcm_engine_fill(&e, buf, 512);
    e.paused = true;
    pcm_engine_fill(&e, buf, 512);
    CHECK(max_step(buf, 512, 20000) <= 20000 / 256 + 1 && buf[2 * 511] == 0);
    CHECK(pcm_ring_used(&music) == (4000 - 512) * 4);
    e.paused = false;
    pcm_engine_fill(&e, buf, 512);
    CHECK(abs(buf[0]) < 200 && max_step(buf, 512, 0) <= 20000 / 256 + 1);

    /* flush while sounding: everything dropped, ramp down, new audio fades in */
    pcm_engine_flush(&e);
    CHECK(pcm_ring_used(&music) == 0 && e.frames_dropped == 4000 - 1024);
    put_const(&music, 1000, -15000, -15000);                /* new position, arrives at once */
    pcm_engine_fill(&e, buf, 512);
    CHECK(max_step(buf, 512, 20000) <= 20000 / 256 + 1);
    CHECK(buf[2 * 255] == 0 || abs(buf[2 * 255]) < 100);   /* ramp ends at 0 ... */
    CHECK(buf[2 * 511] < -14000);                           /* ... then the new audio */

    /* ramp_from (DMA rewrite after flush) */
    pcm_engine_ramp_from(&e, 30000, 30000);
    pcm_ring_drop(&music);
    pcm_engine_fill(&e, buf, 512);
    CHECK(max_step(buf, 512, 30000) <= 30000 / 256 + 1 && buf[2 * 511] == 0);

    /* tone: music ramps down first, tone plays, music fades back in */
    pcm_engine_init(&e, &music, &tone, 0, 0);
    pcm_ring_drop(&music);
    put_const(&music, 5000, 8000, 8000);
    pcm_engine_fill(&e, buf, 512);
    put_const(&tone, 600, 1234, 1234);
    e.tone_mode = true;
    pcm_engine_fill(&e, buf, 512);
    CHECK(max_step(buf, 256, 8000) <= 8000 / 256 + 1);     /* ramp */
    CHECK(buf[2 * 256] == 1234);                            /* tone right after the ramp */
    size_t music_left = pcm_ring_used(&music);
    e.tone_eof = true;
    pcm_engine_fill(&e, buf, 512);                          /* 344 tone frames, then music */
    CHECK(buf[2 * 343] == 1234 && !e.tone_mode && e.tone_finished);
    CHECK(abs(buf[2 * 344]) < 100 && buf[2 * 511] > 0);    /* music fades in */
    CHECK(pcm_ring_used(&music) == music_left - 168 * 4);

    /* volume: unity is bit-exact, a change ramps, half gain halves */
    pcm_engine_init(&e, &music, &tone, 0, 0);
    pcm_ring_drop(&music);
    e.fade_in_pos = FADE_FRAMES;                            /* no fade-in for this test */
    put_const(&music, 3000, 20000, -20000);
    pcm_engine_fill(&e, buf, 512);
    CHECK(buf[0] == 20000 && buf[1] == -20000 && buf[2 * 511] == 20000);
    e.gain_target = PCM_ENGINE_GAIN_UNITY / 2;
    pcm_engine_fill(&e, buf, 512);
    CHECK(buf[2 * 511] == 10000 && buf[2 * 511 + 1] == -10000);
    CHECK(max_step(buf, 512, 20000) <= 20000 / 512 + 2);    /* ramped, no click */
    pcm_engine_fill(&e, buf, 512);
    CHECK(buf[0] == 10000 && buf[2 * 511] == 10000);
    e.gain_target = 0;
    pcm_engine_fill(&e, buf, 512);
    pcm_engine_fill(&e, buf, 512);
    CHECK(buf[0] == 0 && buf[2 * 511] == 0);
}

int main(void)
{
    test_ring();
    test_engine();
    test_fade();
    test_parse_tone();
    test_checks();
    test_mask_and_join();
    test_led();
    test_merge_wifi();
    test_tone();
    if (s_fail) {
        fprintf(stderr, "%d check(s) failed\n", s_fail);
        return 1;
    }
    printf("all bench host tests passed\n");
    return 0;
}
