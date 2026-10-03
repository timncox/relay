/*
 * relay -- MIDI to CV, the part that has no hardware in it.
 *
 * Raw MIDI bytes go in (relay_feed) or already-parsed messages (relay_event);
 * the board asks each millisecond what the jacks should show: a pitch in
 * volts per voice, a gate, a trigger pulse, a velocity, the assignable
 * sources (mod wheel, any CC, aftertouch, bend), a clock pulse, run and
 * reset. Nothing here knows a DAC's range: pitch comes out as 1 V/oct volts
 * relative to the 0 V note and the board clamps and calibrates.
 *
 * Voices 1..4. With one voice the held notes form a stack and the sounding
 * note is chosen by priority (last / low / high); a note-off falls back to
 * the next note legato, gate held. With more voices each note takes a free
 * voice (the one released longest ago), and when none is free the voice
 * holding the oldest note is stolen.
 *
 * Time-based glide (constant time, not constant rate); pitch bend is added
 * after the glide, never glided. Retrigger, when on, drops the gate for
 * RELAY_RETRIG_MS whenever a voice takes a new note while already sounding.
 *
 * Clock: every 24-ppqn tick is counted whether or not the transport runs
 * (hardware masters send clock all the time); the clock output goes high
 * on every clock_div-th tick and low halfway through, or after a short
 * fixed pulse for the finest division. Start resets the divider and fires
 * the reset pulse; Start/Continue raise run, Stop drops it.
 *
 * Everything runs in one thread: feed/event and tick are meant to be called
 * from the same control loop. No allocation, no floating-point in the
 * parser.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_MAX_VOICES 4
#define RELAY_STACK 32            /* held notes remembered for mono priority */
#define RELAY_RETRIG_MS 2         /* gate dip on retrigger */
#define RELAY_CLOCK_PULSE_MS 5    /* clock pulse when the division is one tick */
#define RELAY_CLOCK_STALE_MS 2000 /* no tick this long -> clock output low */

enum { RELAY_PRIO_LAST = 0, RELAY_PRIO_LOW = 1, RELAY_PRIO_HIGH = 2 };

/* Assignable sources for the "whatever you want" outputs. */
typedef enum {
    RELAY_SRC_VEL = 0,  /* velocity of the sounding note (of the voice the
                           last note-on landed in), 0..1; held after release */
    RELAY_SRC_MOD,      /* CC 1, 0..1 */
    RELAY_SRC_CC,       /* CC number given to relay_source(), 0..1 */
    RELAY_SRC_AT,       /* aftertouch: channel pressure, or poly pressure of
                           the last note, whichever is higher, 0..1 */
    RELAY_SRC_BEND,     /* pitch bend, -1..1 */
    RELAY_SRC_COUNT
} relay_src_t;

typedef struct {
    uint8_t  channel;      /* 0 = omni, 1..16 */
    uint8_t  voices;       /* 1..RELAY_MAX_VOICES */
    uint8_t  priority;     /* RELAY_PRIO_*, one voice only */
    uint8_t  retrig;       /* 1 = gate dip + trigger on every new note */
    uint8_t  glide_legato; /* 1 = glide only when a note was already held */
    uint8_t  base_note;    /* MIDI note that sits at 0 V */
    uint8_t  bend_range;   /* semitones at full bend, 0..24 */
    uint8_t  clock_div;    /* 24-ppqn ticks per clock pulse, >= 1 */
    uint16_t glide_ms;     /* 0 = off */
    uint8_t  trig_ms;      /* trigger pulse length */
    uint8_t  reset_ms;     /* reset pulse length */
} relay_cfg_t;

typedef struct {
    uint8_t  note;
    uint8_t  vel;          /* 0..127 as received */
    uint8_t  poly_at;      /* poly pressure for this note */
    bool     held;         /* gate wants to be high */
    uint32_t order;        /* stamp of the last note-on (held) or note-off */
    float    pitch;        /* current pitch, MIDI note units, after glide */
    float    target;       /* where the glide is heading */
    float    rate;         /* semitones per ms while gliding, 0 = arrived */
    uint16_t trig_left;    /* ms of trigger pulse remaining */
    uint16_t dip_left;     /* ms of retrigger gate dip remaining */
} relay_voice_t;

/* Activity bits, set by events, cleared by relay_take_activity(). */
enum {
    RELAY_ACT_NOTE   = 1u << 0,
    RELAY_ACT_CC     = 1u << 1,
    RELAY_ACT_BEND   = 1u << 2,
    RELAY_ACT_AT     = 1u << 3,
    RELAY_ACT_CLOCK  = 1u << 4,
    RELAY_ACT_START  = 1u << 5,
    RELAY_ACT_STOP   = 1u << 6,
    RELAY_ACT_OTHER  = 1u << 7, /* a channel message on a channel we ignore */
    RELAY_ACT_ANY    = 1u << 8, /* any byte at all */
};

typedef struct {
    relay_cfg_t cfg;

    /* parser */
    uint8_t status;        /* running status, 0 = none */
    uint8_t data[2];
    uint8_t need, have;
    bool    in_sysex;

    /* mono note stack, arrival order, index 0 oldest */
    uint8_t stack_note[RELAY_STACK];
    uint8_t stack_vel[RELAY_STACK];
    int     stack_n;

    relay_voice_t v[RELAY_MAX_VOICES];
    uint32_t      order;   /* monotonic stamp */

    int16_t bend;          /* -8192..8191 */
    uint8_t cc[128];
    uint8_t at;            /* channel pressure */
    uint8_t last_vel;      /* of the last note-on on our channel */
    int     last_voice;    /* voice the last note-on landed in */

    /* clock / transport */
    bool     running;
    uint32_t tick;         /* ticks since Start (or since boot) */
    bool     clock_hi;
    uint16_t clock_left;   /* ms until the fixed pulse ends (div 1 only) */
    uint32_t clock_stale;  /* ms since the last tick */
    uint16_t reset_left;

    uint32_t activity;
    uint8_t  last_rx_channel; /* 1..16, of the last channel message seen, 0 = none */
} relay_t;

/* Sane defaults: omni, one voice, last-note, no retrig, base 36, bend 2,
 * clock 1/16 (6 ticks), no glide, 5 ms trigger, 10 ms reset. */
void relay_default_cfg(relay_cfg_t *cfg);

void relay_init(relay_t *r, const relay_cfg_t *cfg);

/* Replace the configuration. A change of channel or voice count silences
 * everything first (a pot swept across zones must never leave a gate up);
 * any other change is applied live. */
void relay_set_cfg(relay_t *r, const relay_cfg_t *cfg);

/* One raw MIDI byte. Running status, realtime interleaving and sysex are
 * handled here. */
void relay_feed(relay_t *r, uint8_t byte);

/* One complete message: status with channel (0x80..0xEF) plus data, or a
 * system byte (0xF0..0xFF) with data as applicable. */
void relay_event(relay_t *r, uint8_t status, uint8_t d1, uint8_t d2);

/* Advance timers and glides by dt_ms. Call at about 1 kHz, BEFORE the
 * pass's events: dt is time that already passed, and charging it to a
 * pulse an event just started shortens that pulse (a 2 ms retrigger dip
 * can vanish). Whole ms of a finer clock, remainder carried. */
void relay_tick(relay_t *r, uint32_t dt_ms);

/* All voices off, gates down, controllers left alone. */
void relay_all_notes_off(relay_t *r);

/* Outputs. voice is 0..cfg.voices-1; anything else reads as silent. */
float relay_pitch_volts(const relay_t *r, int voice); /* 1 V/oct from base_note, bend included, unclamped */
bool  relay_gate(const relay_t *r, int voice);
bool  relay_trig(const relay_t *r, int voice);
float relay_vel(const relay_t *r, int voice);         /* 0..1 */
float relay_source(const relay_t *r, relay_src_t src, uint8_t cc); /* see relay_src_t */
bool  relay_clock(const relay_t *r);
bool  relay_run(const relay_t *r);
bool  relay_reset(const relay_t *r);
bool  relay_any_gate(const relay_t *r);

/* Sounding note of a voice (for a display), or -1 when silent. */
int relay_voice_note(const relay_t *r, int voice);

/* Read and clear the activity bits. */
uint32_t relay_take_activity(relay_t *r);

#ifdef __cplusplus
}
#endif
