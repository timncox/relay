/* relay core tests: the parser, mono priority and legato, retrigger, glide,
 * bend, poly allocation and stealing, controllers, clock and transport,
 * and the config-change silence rule. */
#include "relay.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        checks++;                                                            \
        if(!(cond))                                                          \
        {                                                                    \
            fails++;                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while(0)
#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

static void on(relay_t *r, int ch, int note, int vel) { relay_event(r, 0x90 | (ch - 1), note, vel); }
static void off(relay_t *r, int ch, int note) { relay_event(r, 0x80 | (ch - 1), note, 0); }
static void run_ms(relay_t *r, int ms)
{
    for(int i = 0; i < ms; i++) relay_tick(r, 1);
}

/* ---- parser ------------------------------------------------------------ */

static void test_parser(void)
{
    relay_t r;
    relay_init(&r, NULL);

    /* Plain note on / off. */
    relay_feed(&r, 0x90);
    relay_feed(&r, 60);
    relay_feed(&r, 100);
    CHECK(relay_gate(&r, 0));
    CHECK(relay_voice_note(&r, 0) == 60);

    /* Running status: the next note-on has no status byte. */
    relay_feed(&r, 62);
    relay_feed(&r, 90);
    CHECK(relay_voice_note(&r, 0) == 62);

    /* Realtime byte in the middle of a message must not break it. */
    relay_feed(&r, 0x80);
    relay_feed(&r, 62);
    relay_feed(&r, 0xF8); /* clock, interleaved */
    relay_feed(&r, 0);
    CHECK(relay_voice_note(&r, 0) == 60); /* fell back to the held 60 */
    CHECK(r.tick == 1);

    /* Note-on with velocity 0 is a note-off. */
    relay_feed(&r, 0x90);
    relay_feed(&r, 60);
    relay_feed(&r, 0);
    CHECK(!relay_gate(&r, 0));

    /* Sysex is skipped, including data inside it; the message after works. */
    relay_feed(&r, 0xF0);
    relay_feed(&r, 0x7E);
    relay_feed(&r, 0x60); /* would look like a note if it leaked */
    relay_feed(&r, 0x64);
    relay_feed(&r, 0xF7);
    CHECK(!relay_gate(&r, 0));
    relay_feed(&r, 0x90);
    relay_feed(&r, 64);
    relay_feed(&r, 1);
    CHECK(relay_voice_note(&r, 0) == 64);

    /* System common resets running status: a data byte after Song Select
     * (F3, one data byte) belongs to nothing. */
    relay_feed(&r, 0xF3);
    relay_feed(&r, 5);
    relay_feed(&r, 65);
    relay_feed(&r, 100); /* no status -> ignored */
    CHECK(relay_voice_note(&r, 0) == 64);

    /* Song position pointer (F2) sets the tick phase: 16 sixteenths = 96 ticks. */
    relay_feed(&r, 0xF2);
    relay_feed(&r, 16);
    relay_feed(&r, 0);
    CHECK(r.tick == 96);

    /* Two-byte messages: program change and channel pressure take one byte. */
    relay_feed(&r, 0xC0);
    relay_feed(&r, 7);
    relay_feed(&r, 0xD0);
    relay_feed(&r, 100);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_AT, 0), 100.0f / 127.0f, 1e-6f));
    /* Running status on channel pressure. */
    relay_feed(&r, 50);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_AT, 0), 50.0f / 127.0f, 1e-6f));

    /* Pitch bend: 14-bit, centre 8192. */
    relay_feed(&r, 0xE0);
    relay_feed(&r, 0x7F);
    relay_feed(&r, 0x7F);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_BEND, 0), 8191.0f / 8192.0f, 1e-6f));
    relay_feed(&r, 0x00);
    relay_feed(&r, 0x40);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_BEND, 0), 0.0f, 1e-6f));
}

/* ---- channel filter ---------------------------------------------------- */

static void test_channel(void)
{
    relay_t     r;
    relay_cfg_t c;
    relay_default_cfg(&c);
    c.channel = 3;
    relay_init(&r, &c);

    on(&r, 1, 60, 100);
    CHECK(!relay_gate(&r, 0));
    CHECK(relay_take_activity(&r) & RELAY_ACT_OTHER);
    CHECK(r.last_rx_channel == 1);
    on(&r, 3, 60, 100);
    CHECK(relay_gate(&r, 0));
    CHECK(r.last_rx_channel == 3);

    /* Omni hears everything. */
    c.channel = 0;
    relay_set_cfg(&r, &c);
    CHECK(!relay_gate(&r, 0)); /* the channel change silenced the held note */
    on(&r, 9, 61, 100);
    CHECK(relay_voice_note(&r, 0) == 61);

    /* A voice-count change silences too; other changes do not. */
    c.voices = 2;
    relay_set_cfg(&r, &c);
    CHECK(!relay_any_gate(&r));
    on(&r, 9, 61, 100);
    c.bend_range = 12;
    relay_set_cfg(&r, &c);
    CHECK(relay_any_gate(&r));
}

/* ---- mono -------------------------------------------------------------- */

static void test_mono_priority(void)
{
    relay_t     r;
    relay_cfg_t c;
    relay_default_cfg(&c);
    c.base_note = 36;
    relay_init(&r, &c);

    /* Last-note: legato fallback on release. */
    on(&r, 1, 48, 100);
    CHECK(NEAR(relay_pitch_volts(&r, 0), 1.0f, 1e-6f));
    CHECK(relay_trig(&r, 0));
    run_ms(&r, 10);
    CHECK(!relay_trig(&r, 0));
    on(&r, 1, 55, 80);
    CHECK(relay_voice_note(&r, 0) == 55);
    CHECK(NEAR(relay_vel(&r, 0), 80.0f / 127.0f, 1e-6f));
    CHECK(relay_gate(&r, 0));
    CHECK(!relay_trig(&r, 0)); /* retrig off: no trigger, no dip */
    off(&r, 1, 55);
    CHECK(relay_gate(&r, 0));
    CHECK(relay_voice_note(&r, 0) == 48);
    CHECK(NEAR(relay_vel(&r, 0), 100.0f / 127.0f, 1e-6f));
    off(&r, 1, 48);
    CHECK(!relay_gate(&r, 0));

    /* Low priority. */
    c.priority = RELAY_PRIO_LOW;
    relay_set_cfg(&r, &c);
    on(&r, 1, 60, 100);
    on(&r, 1, 52, 100);
    CHECK(relay_voice_note(&r, 0) == 52);
    on(&r, 1, 70, 100);
    CHECK(relay_voice_note(&r, 0) == 52);
    off(&r, 1, 52);
    CHECK(relay_voice_note(&r, 0) == 60);
    off(&r, 1, 60);
    off(&r, 1, 70);
    CHECK(!relay_gate(&r, 0));

    /* High priority. */
    c.priority = RELAY_PRIO_HIGH;
    relay_set_cfg(&r, &c);
    on(&r, 1, 60, 100);
    on(&r, 1, 52, 100);
    CHECK(relay_voice_note(&r, 0) == 60);
    on(&r, 1, 70, 100);
    CHECK(relay_voice_note(&r, 0) == 70);
    off(&r, 1, 70);
    CHECK(relay_voice_note(&r, 0) == 60);
    relay_all_notes_off(&r);
    CHECK(!relay_gate(&r, 0));

    /* Releasing a note that never sounded changes nothing. */
    c.priority = RELAY_PRIO_LAST;
    relay_set_cfg(&r, &c);
    on(&r, 1, 40, 100);
    on(&r, 1, 41, 100);
    off(&r, 1, 40);
    CHECK(relay_voice_note(&r, 0) == 41);
    off(&r, 1, 41);
    CHECK(!relay_gate(&r, 0));

    /* Stack overflow keeps the newest. */
    for(int i = 0; i < RELAY_STACK + 4; i++) on(&r, 1, 20 + i, 100);
    CHECK(relay_voice_note(&r, 0) == 20 + RELAY_STACK + 3);
    relay_all_notes_off(&r);
}

static void test_retrig(void)
{
    relay_t     r;
    relay_cfg_t c;
    relay_default_cfg(&c);
    c.retrig  = 1;
    c.trig_ms = 5;
    relay_init(&r, &c);

    on(&r, 1, 60, 100);
    run_ms(&r, 20);
    CHECK(relay_gate(&r, 0));
    on(&r, 1, 62, 100);
    /* Gate dips for RELAY_RETRIG_MS, then the trigger shows. */
    CHECK(!relay_gate(&r, 0));
    CHECK(!relay_trig(&r, 0));
    run_ms(&r, RELAY_RETRIG_MS);
    CHECK(relay_gate(&r, 0));
    CHECK(relay_trig(&r, 0));
    run_ms(&r, 5);
    CHECK(!relay_trig(&r, 0));
    /* Falling back on release also retriggers. */
    off(&r, 1, 62);
    CHECK(!relay_gate(&r, 0));
    run_ms(&r, RELAY_RETRIG_MS);
    CHECK(relay_gate(&r, 0));
    CHECK(relay_voice_note(&r, 0) == 60);
    off(&r, 1, 60);
    CHECK(!relay_gate(&r, 0));
}

static void test_glide_and_bend(void)
{
    relay_t     r;
    relay_cfg_t c;
    relay_default_cfg(&c);
    c.glide_ms   = 100;
    c.base_note  = 36;
    c.bend_range = 2;
    relay_init(&r, &c);

    on(&r, 1, 36, 100); /* first note: from the idle pitch (36) -> no movement */
    CHECK(NEAR(relay_pitch_volts(&r, 0), 0.0f, 1e-6f));
    on(&r, 1, 48, 100); /* one octave up over 100 ms */
    run_ms(&r, 50);
    CHECK(NEAR(relay_pitch_volts(&r, 0), 0.5f, 0.02f));
    run_ms(&r, 60);
    CHECK(NEAR(relay_pitch_volts(&r, 0), 1.0f, 1e-6f));

    /* Bend is immediate even mid-glide, and scaled by the range. */
    off(&r, 1, 48);
    run_ms(&r, 200);
    relay_event(&r, 0xE0, 0x7F, 0x7F); /* full up = +2 semitones */
    CHECK(NEAR(relay_pitch_volts(&r, 0), 2.0f / 12.0f, 1e-3f));
    relay_event(&r, 0xE0, 0x00, 0x00); /* full down = -2 semitones */
    CHECK(NEAR(relay_pitch_volts(&r, 0), -2.0f / 12.0f, 1e-3f));
    relay_event(&r, 0xE0, 0x00, 0x40);

    /* Legato-only glide: a fresh note jumps, an overlapping note glides. */
    c.glide_legato = 1;
    relay_set_cfg(&r, &c);
    off(&r, 1, 36);
    on(&r, 1, 60, 100);
    CHECK(NEAR(relay_pitch_volts(&r, 0), 2.0f, 1e-6f));
    on(&r, 1, 72, 100);
    run_ms(&r, 10);
    CHECK(relay_pitch_volts(&r, 0) < 2.5f);
    run_ms(&r, 200);
    CHECK(NEAR(relay_pitch_volts(&r, 0), 3.0f, 1e-6f));
    relay_all_notes_off(&r);

    /* Downward glide lands exactly too. */
    c.glide_legato = 0;
    relay_set_cfg(&r, &c);
    on(&r, 1, 84, 100);
    run_ms(&r, 200);
    on(&r, 1, 60, 100);
    run_ms(&r, 200);
    CHECK(NEAR(relay_pitch_volts(&r, 0), 2.0f, 1e-6f));
}

/* ---- poly -------------------------------------------------------------- */

static int voice_of(const relay_t *r, int note)
{
    for(int i = 0; i < r->cfg.voices; i++)
        if(relay_voice_note(r, i) == note) return i;
    return -1;
}

static void test_poly(void)
{
    relay_t     r;
    relay_cfg_t c;
    relay_default_cfg(&c);
    c.voices  = 4;
    c.trig_ms = 5;
    relay_init(&r, &c);

    on(&r, 1, 60, 100);
    on(&r, 1, 64, 90);
    on(&r, 1, 67, 80);
    CHECK(voice_of(&r, 60) == 0);
    CHECK(voice_of(&r, 64) == 1);
    CHECK(voice_of(&r, 67) == 2);
    CHECK(relay_gate(&r, 0) && relay_gate(&r, 1) && relay_gate(&r, 2) && !relay_gate(&r, 3));
    CHECK(NEAR(relay_vel(&r, 1), 90.0f / 127.0f, 1e-6f));
    CHECK(relay_trig(&r, 2));

    /* Release 64, play 71: the free voice is reused (the released one). */
    off(&r, 1, 64);
    CHECK(!relay_gate(&r, 1));
    on(&r, 1, 71, 100);
    CHECK(voice_of(&r, 71) == 3 || voice_of(&r, 71) == 1);
    /* Voice 3 was never used (order 0) so it is the one released longest ago. */
    CHECK(voice_of(&r, 71) == 3);
    on(&r, 1, 72, 100);
    CHECK(voice_of(&r, 72) == 1);

    /* All four held: the next note steals the oldest (60 in voice 0) and
     * retriggers that voice. */
    run_ms(&r, 20);
    on(&r, 1, 74, 100);
    CHECK(voice_of(&r, 74) == 0);
    CHECK(voice_of(&r, 60) == -1);
    CHECK(!relay_gate(&r, 0)); /* dip */
    run_ms(&r, RELAY_RETRIG_MS);
    CHECK(relay_gate(&r, 0));
    CHECK(relay_trig(&r, 0));

    /* Same note again retriggers in place, no second voice. */
    run_ms(&r, 20);
    on(&r, 1, 74, 50);
    CHECK(voice_of(&r, 74) == 0);
    CHECK(NEAR(relay_vel(&r, 0), 50.0f / 127.0f, 1e-6f));
    int count = 0;
    for(int i = 0; i < 4; i++) count += relay_voice_note(&r, i) == 74;
    CHECK(count == 1);

    /* Note-off of a stolen note must not touch the voice that took it. */
    off(&r, 1, 60);
    CHECK(voice_of(&r, 74) == 0);
    run_ms(&r, 5);
    CHECK(relay_gate(&r, 0));

    /* Poly aftertouch reaches the last voice; channel pressure competes. */
    relay_event(&r, 0xA0, 74, 90);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_AT, 0), 90.0f / 127.0f, 1e-6f));
    relay_event(&r, 0xD0, 120, 0);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_AT, 0), 120.0f / 127.0f, 1e-6f));

    /* Two-voice mode: pitches are per voice and independent. */
    c.voices = 2;
    c.base_note = 36;
    relay_set_cfg(&r, &c);
    on(&r, 1, 48, 100);
    on(&r, 1, 60, 100);
    CHECK(NEAR(relay_pitch_volts(&r, 0), 1.0f, 1e-6f));
    CHECK(NEAR(relay_pitch_volts(&r, 1), 2.0f, 1e-6f));
    CHECK(!relay_gate(&r, 2)); /* out of range reads silent */
    relay_all_notes_off(&r);
    CHECK(!relay_any_gate(&r));
}

/* ---- controllers ------------------------------------------------------- */

static void test_controllers(void)
{
    relay_t r;
    relay_init(&r, NULL);

    relay_event(&r, 0xB0, 1, 64);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_MOD, 0), 64.0f / 127.0f, 1e-6f));
    relay_event(&r, 0xB0, 74, 127);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_CC, 74), 1.0f, 1e-6f));
    on(&r, 1, 60, 33);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_VEL, 0), 33.0f / 127.0f, 1e-6f));

    /* All notes off (123) and all sound off (120). */
    CHECK(relay_gate(&r, 0));
    relay_event(&r, 0xB0, 123, 0);
    CHECK(!relay_gate(&r, 0));
    on(&r, 1, 60, 33);
    relay_event(&r, 0xB0, 120, 0);
    CHECK(!relay_gate(&r, 0));

    /* Reset all controllers zeroes bend, mod, pressure. */
    relay_event(&r, 0xE0, 0x7F, 0x7F);
    relay_event(&r, 0xD0, 99, 0);
    relay_event(&r, 0xB0, 121, 0);
    CHECK(NEAR(relay_source(&r, RELAY_SRC_BEND, 0), 0.0f, 1e-6f));
    CHECK(NEAR(relay_source(&r, RELAY_SRC_MOD, 0), 0.0f, 1e-6f));
    CHECK(NEAR(relay_source(&r, RELAY_SRC_AT, 0), 0.0f, 1e-6f));
    /* ...but not an ordinary CC. */
    CHECK(NEAR(relay_source(&r, RELAY_SRC_CC, 74), 1.0f, 1e-6f));
}

/* ---- clock ------------------------------------------------------------- */

static void test_clock(void)
{
    relay_t     r;
    relay_cfg_t c;
    relay_default_cfg(&c);
    c.clock_div = 6; /* sixteenths */
    c.reset_ms  = 10;
    relay_init(&r, &c);

    CHECK(!relay_run(&r));
    relay_event(&r, 0xFA, 0, 0); /* start */
    CHECK(relay_run(&r));
    CHECK(relay_reset(&r));
    run_ms(&r, 10);
    CHECK(!relay_reset(&r));

    /* Tick 0 raises the clock, tick 3 (half of 6) lowers it. */
    relay_event(&r, 0xF8, 0, 0);
    CHECK(relay_clock(&r));
    relay_event(&r, 0xF8, 0, 0);
    relay_event(&r, 0xF8, 0, 0);
    CHECK(relay_clock(&r));
    relay_event(&r, 0xF8, 0, 0);
    CHECK(!relay_clock(&r));
    relay_event(&r, 0xF8, 0, 0);
    relay_event(&r, 0xF8, 0, 0);
    CHECK(!relay_clock(&r));
    relay_event(&r, 0xF8, 0, 0); /* tick 6: next pulse */
    CHECK(relay_clock(&r));

    /* Stop drops run but the clock keeps following ticks. */
    relay_event(&r, 0xFC, 0, 0);
    CHECK(!relay_run(&r));
    for(int i = 0; i < 3; i++) relay_event(&r, 0xF8, 0, 0);
    CHECK(!relay_clock(&r));
    relay_event(&r, 0xFB, 0, 0); /* continue: no reset */
    CHECK(relay_run(&r));
    CHECK(!relay_reset(&r));

    /* Start again resets the phase: the next tick is a boundary. */
    for(int i = 0; i < 2; i++) relay_event(&r, 0xF8, 0, 0); /* phase 4, 5 */
    relay_event(&r, 0xFA, 0, 0);
    relay_event(&r, 0xF8, 0, 0);
    CHECK(relay_clock(&r));

    /* Stale: no ticks for two seconds drops the output. */
    run_ms(&r, RELAY_CLOCK_STALE_MS + 1);
    CHECK(!relay_clock(&r));

    /* Division 1: a fixed pulse. */
    c.clock_div = 1;
    relay_set_cfg(&r, &c);
    relay_event(&r, 0xF8, 0, 0);
    CHECK(relay_clock(&r));
    run_ms(&r, RELAY_CLOCK_PULSE_MS - 1);
    CHECK(relay_clock(&r));
    run_ms(&r, 1);
    CHECK(!relay_clock(&r));

    /* Division 24 (quarter notes): high for 12 ticks. */
    c.clock_div = 24;
    relay_set_cfg(&r, &c);
    relay_event(&r, 0xFA, 0, 0);
    int highs = 0;
    for(int i = 0; i < 48; i++)
    {
        relay_event(&r, 0xF8, 0, 0);
        highs += relay_clock(&r);
    }
    CHECK(highs == 24);

    /* System reset silences and stops. */
    on(&r, 1, 60, 100);
    relay_event(&r, 0xFF, 0, 0);
    CHECK(!relay_gate(&r, 0));
    CHECK(!relay_run(&r));
}

/* ---- tick robustness --------------------------------------------------- */

static void test_ticks(void)
{
    relay_t     r;
    relay_cfg_t c;
    relay_default_cfg(&c);
    c.trig_ms = 5;
    relay_init(&r, &c);
    on(&r, 1, 60, 100);
    relay_tick(&r, 0); /* no-op */
    CHECK(relay_trig(&r, 0));
    relay_tick(&r, 1000); /* a big jump ends everything cleanly */
    CHECK(!relay_trig(&r, 0));
    CHECK(relay_gate(&r, 0));
    CHECK(relay_take_activity(&r) & RELAY_ACT_NOTE);
    CHECK(relay_take_activity(&r) == 0);
}

int main(void)
{
    test_parser();
    test_channel();
    test_mono_priority();
    test_retrig();
    test_glide_and_bend();
    test_poly();
    test_controllers();
    test_clock();
    test_ticks();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
