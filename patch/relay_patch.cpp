/*
 * Relay on the Daisy Patch -- MIDI to CV.
 *
 *   MIDI in     TRS jack (the Patch's UART MIDI), and the Seed's micro-USB
 *               as a class-compliant USB MIDI device; both feed one engine
 *   CV OUT 1    pitch, 1 V/oct, 0 V at the base note, 0..5 V (calibrated)
 *   CV OUT 2    assignable: velocity, mod wheel, any CC, aftertouch, bend,
 *               trigger, clock, run, reset
 *   GATE OUT    gate (with an optional retrigger dip)
 *   knob 1      octave transpose, -2..+2
 *   knob 2      glide time
 *   encoder     turn = menu, push+turn = edit; push+turn on Mods = module picker
 *
 * One voice: the Patch has one pitch output and one gate. The engine
 * (core/relay.c, shared with the Alchemy Lab build) does priority, legato,
 * retrigger, glide, bend and the clock; this file is the panel and the DAC.
 *
 * DAC scaling is a calibration, not a constant. From the Rev8 schematic
 * (sheet 2, "DAC Outputs") each CV out is a 33k/49k9 inverting stage into a
 * 100k/100k inverting stage: non-inverting overall, gain 1.512, so the DAC's
 * 0..3.3 V becomes 0..4.99 V ("OUT: 0 - 5V"). VDDA and resistor tolerance
 * move that per board, so "Cal" in the menu trims the full-scale volts and a
 * zero offset; both persist in QSPI. Unheard as of this writing.
 */
#include "daisy_patch.h"
#include "hid/midi.h"
#include "hid/usb_midi.h"
#include "util/PersistentStorage.h"
#include "module_picker.h"
#include <cstdio>
#include <cstring>
#include <cmath>

#include "relay.h"

#ifndef RELAY_VERSION
#define RELAY_VERSION "0.0.0"
#endif
#ifndef RELAY_GIT_HASH
#define RELAY_GIT_HASH "dev"
#endif

using namespace daisy;

DaisyPatch     hw;
MidiUsbHandler usb_midi;
static relay_t R;

/* ------------------------------------------------------------------------ */
/* Persistent settings (QSPI, high in the 8 MB chip, well above any app)     */

enum Cv2Src
{
    CV2_VEL,
    CV2_MOD,
    CV2_CC,
    CV2_AT,
    CV2_BEND,
    CV2_TRIG,
    CV2_CLOCK,
    CV2_RUN,
    CV2_RESET,
    CV2_COUNT
};
static const char *const CV2_NAME[CV2_COUNT]
    = {"Vel", "Mod", "CC", "AfterT", "Bend", "Trig", "Clock", "Run", "Reset"};
static const char *const PRIO_NAME[3]  = {"Last", "Low", "High"};
static const char *const ONOFF[2]      = {"Off", "On"};
static const char *const LEGATO_NAME[2] = {"Always", "Legato"};
/* Clock divisions in 24-ppqn ticks. */
static const uint8_t     CLOCK_TICKS[6] = {3, 6, 12, 24, 48, 96};
static const char *const CLOCK_NAME[6]  = {"1/32", "1/16", "1/8", "1/4", "1/2", "Bar"};
/* MIDI note at 0 V: C-1..C3 in Ableton's naming (C3 = 60). */
static const uint8_t     BASE_NOTE[5] = {12, 24, 36, 48, 60};
static const char *const BASE_NAME[5] = {"C-1", "C0", "C1", "C2", "C3"};

struct Settings
{
    uint32_t magic;
    uint8_t  channel;   /* 0 omni, 1..16 */
    uint8_t  priority;  /* 0 last, 1 low, 2 high */
    uint8_t  retrig;
    uint8_t  legato;    /* glide only on overlapping notes */
    uint8_t  base;      /* index into BASE_NOTE */
    uint8_t  bend;      /* semitones 0..24 */
    uint8_t  clock;     /* index into CLOCK_TICKS */
    uint8_t  cv2;       /* Cv2Src */
    uint8_t  cv2_cc;    /* CC number for CV2_CC */
    int16_t  cal_scale_mv;  /* volts at DAC full scale, in mV */
    int16_t  cal_offset_mv; /* volts at DAC code 0, in mV */

    bool operator==(const Settings &o) const
    {
        return magic == o.magic && channel == o.channel && priority == o.priority
               && retrig == o.retrig && legato == o.legato && base == o.base && bend == o.bend
               && clock == o.clock && cv2 == o.cv2 && cv2_cc == o.cv2_cc
               && cal_scale_mv == o.cal_scale_mv && cal_offset_mv == o.cal_offset_mv;
    }
    bool operator!=(const Settings &o) const { return !(*this == o); }
};
static constexpr uint32_t kMagic = 0x52594C31; /* 'RYL1' */
static constexpr uint32_t kSettingsOffset = 0x00700000; /* 7 MB into QSPI */

static Settings defaults()
{
    Settings s;
    memset(&s, 0, sizeof s);
    s.magic         = kMagic;
    s.channel       = 0;
    s.priority      = 0;
    s.retrig        = 0;
    s.legato        = 0;
    s.base          = 2; /* C1 = 36 */
    s.bend          = 2;
    s.clock         = 1; /* 1/16 */
    s.cv2           = CV2_VEL;
    s.cv2_cc        = 1;
    s.cal_scale_mv  = 4990;
    s.cal_offset_mv = 0;
    return s;
}

static PersistentStorage<Settings> storage(hw.seed.qspi);
static Settings                   *S;
static bool                        g_dirty       = false;
static uint32_t                    g_dirty_since = 0;

/* ------------------------------------------------------------------------ */
/* Knobs                                                                     */

static int   g_octave    = 0;    /* knob 1, -2..+2 */
static float g_knob_oct  = 0.5f;
static int   g_glide_ms  = 0;    /* knob 2 */
static int   g_test_step = 0;    /* 0 off, 1.. = (step-1) volts */
static int   g_test_note = -1;

static relay_cfg_t build_cfg(void)
{
    relay_cfg_t c;
    relay_default_cfg(&c);
    c.channel      = S->channel;
    c.voices       = 1;
    c.priority     = S->priority;
    c.retrig       = S->retrig;
    c.glide_legato = S->legato;
    int base       = (int)BASE_NOTE[S->base] - 12 * g_octave;
    if(base < 0) base = 0;
    if(base > 127) base = 127;
    c.base_note  = (uint8_t)base;
    c.bend_range = S->bend;
    c.clock_div  = CLOCK_TICKS[S->clock];
    c.glide_ms   = (uint16_t)g_glide_ms;
    c.trig_ms    = 5;
    c.reset_ms   = 10;
    return c;
}

static void test_set(int step);

static void apply_cfg(void)
{
    relay_cfg_t c = build_cfg();
    if(memcmp(&c, &R.cfg, sizeof c) == 0) return;
    /* A channel change silences every voice, the test note included;
     * forget it so the menu does not send a stray note-off later. */
    if(c.channel != R.cfg.channel)
    {
        g_test_note = -1;
        g_test_step = 0;
    }
    bool rebase = c.base_note != R.cfg.base_note;
    relay_set_cfg(&R, &c);
    /* The octave knob (or Base) moved under a test note: move the note so
     * the jack still shows the volts the menu names. */
    if(rebase && g_test_step) test_set(g_test_step);
}

static void read_knobs(void)
{
    /* Octave: five zones with hysteresis, so a knob parked on a boundary
     * cannot flicker the transpose. */
    float k = hw.GetKnobValue(DaisyPatch::CTRL_1);
    if(fabsf(k - g_knob_oct) > 0.02f)
    {
        g_knob_oct = k;
        int z      = (int)(k * 5.0f);
        if(z > 4) z = 4;
        g_octave = z - 2;
    }
    /* Glide: 0..2000 ms on a square law, off below 3 ms. */
    float g  = hw.GetKnobValue(DaisyPatch::CTRL_2);
    int   ms = (int)(g * g * 2000.0f);
    if(ms < 3) ms = 0;
    if(abs(ms - g_glide_ms) > 2 || (ms == 0) != (g_glide_ms == 0)) g_glide_ms = ms;
}

/* ------------------------------------------------------------------------ */
/* MIDI in: libDaisy's parsers feed the engine as complete messages          */

static void feed(MidiEvent &e)
{
    switch(e.type)
    {
        case NoteOff: relay_event(&R, 0x80 | e.channel, e.data[0], e.data[1]); break;
        case NoteOn: relay_event(&R, 0x90 | e.channel, e.data[0], e.data[1]); break;
        case PolyphonicKeyPressure: relay_event(&R, 0xA0 | e.channel, e.data[0], e.data[1]); break;
        case ControlChange:
        case ChannelMode: relay_event(&R, 0xB0 | e.channel, e.data[0], e.data[1]); break;
        case ProgramChange: relay_event(&R, 0xC0 | e.channel, e.data[0], 0); break;
        case ChannelPressure: relay_event(&R, 0xD0 | e.channel, e.data[0], 0); break;
        case PitchBend: relay_event(&R, 0xE0 | e.channel, e.data[0], e.data[1]); break;
        case SystemRealTime: relay_event(&R, 0xF8 + (uint8_t)e.srt_type, 0, 0); break;
        case SystemCommon:
            if(e.sc_type == SongPositionPointer) relay_event(&R, 0xF2, e.data[0], e.data[1]);
            break;
        default: break;
    }
}

static bool g_trs_seen, g_usb_seen; /* for the display */

static void pump_midi(void)
{
    hw.midi.Listen(); /* restarts the UART after an overrun */
    while(hw.midi.HasEvents())
    {
        MidiEvent e = hw.midi.PopEvent();
        feed(e);
        g_trs_seen = true;
    }
    while(usb_midi.HasEvents())
    {
        MidiEvent e = usb_midi.PopEvent();
        feed(e);
        g_usb_seen = true;
    }
}

/* ------------------------------------------------------------------------ */
/* Outputs                                                                   */

static bool g_pitch_clipped;

static uint16_t volts_to_code(float v, bool *clipped)
{
    float scale  = (float)S->cal_scale_mv / 1000.0f;
    float offset = (float)S->cal_offset_mv / 1000.0f;
    if(scale < 1.0f) scale = 1.0f;
    float n = (v - offset) / scale;
    if(clipped) *clipped = n < 0.0f || n > 1.0f;
    if(n < 0.0f) n = 0.0f;
    if(n > 1.0f) n = 1.0f;
    return (uint16_t)(n * 4095.0f + 0.5f);
}

static float cv2_value(void)
{
    switch(S->cv2)
    {
        case CV2_VEL: return relay_source(&R, RELAY_SRC_VEL, 0);
        case CV2_MOD: return relay_source(&R, RELAY_SRC_MOD, 0);
        case CV2_CC: return relay_source(&R, RELAY_SRC_CC, S->cv2_cc);
        case CV2_AT: return relay_source(&R, RELAY_SRC_AT, 0);
        case CV2_BEND: return 0.5f + 0.5f * relay_source(&R, RELAY_SRC_BEND, 0);
        case CV2_TRIG: return relay_trig(&R, 0) ? 1.0f : 0.0f;
        case CV2_CLOCK: return relay_clock(&R) ? 1.0f : 0.0f;
        case CV2_RUN: return relay_run(&R) ? 1.0f : 0.0f;
        case CV2_RESET: return relay_reset(&R) ? 1.0f : 0.0f;
        default: return 0.0f;
    }
}

static void write_outputs(void)
{
    uint16_t c1 = volts_to_code(relay_pitch_volts(&R, 0), &g_pitch_clipped);
    float    v2 = cv2_value();
    if(v2 < 0.0f) v2 = 0.0f;
    if(v2 > 1.0f) v2 = 1.0f;
    uint16_t c2 = (uint16_t)(v2 * 4095.0f + 0.5f);
    hw.seed.dac.WriteValue(DacHandle::Channel::ONE, c1);
    hw.seed.dac.WriteValue(DacHandle::Channel::TWO, c2);
    hw.gate_output.Write(relay_gate(&R, 0));
}

/* ------------------------------------------------------------------------ */
/* Test note: a held note at 0, 1, 2, 3 or 4 V, for tuning an oscillator   */
/* with a meter or by ear. Injected as MIDI so it obeys the same rules as a */
/* played note. Absolute volts: the note is the engine's live 0 V note      */
/* (Base with the octave knob applied) plus whole octaves, so calibration   */
/* holds wherever knob 1 sits.                                               */

static void test_set(int step)
{
    int ch = S->channel ? S->channel - 1 : 0;
    if(g_test_note >= 0)
    {
        relay_event(&R, 0x80 | ch, (uint8_t)g_test_note, 0);
        g_test_note = -1;
    }
    g_test_step = step;
    if(step > 0)
    {
        int note = (int)R.cfg.base_note + 12 * (step - 1);
        if(note > 127) note = 127;
        g_test_note = note;
        relay_event(&R, 0x90 | ch, (uint8_t)note, 100);
    }
}

/* ------------------------------------------------------------------------ */
/* Menu                                                                      */

enum MenuItem
{
    MI_CHAN,
    MI_PRIO,
    MI_RETRIG,
    MI_BEND,
    MI_BASE,
    MI_CV2,
    MI_CV2CC,
    MI_CLOCK,
    MI_LEGATO,
    MI_CALSCL,
    MI_CALOFS,
    MI_TEST,
    MI_MODS,
    MI_COUNT
};
static const char *const MENU_LABEL[MI_COUNT] = {"Chan",  "Prio",   "Retrig", "Bend", "Base",
                                                 "CV2",   "CV2 CC", "Clock",  "Glide", "CalScl",
                                                 "CalOfs", "Test",  "Mods"};
static constexpr int      MENU_ROWS = 4;
static int                g_sel = MI_CHAN, g_top = 0;
static bool               enc_down, enc_turned;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void mark_dirty(void)
{
    g_dirty       = true;
    g_dirty_since = System::GetNow();
}

static void menu_edit(int inc)
{
    switch(g_sel)
    {
        case MI_CHAN: S->channel = (uint8_t)clampi(S->channel + inc, 0, 16); break;
        case MI_PRIO: S->priority = (uint8_t)clampi(S->priority + inc, 0, 2); break;
        case MI_RETRIG: S->retrig = (uint8_t)clampi(S->retrig + inc, 0, 1); break;
        case MI_BEND: S->bend = (uint8_t)clampi(S->bend + inc, 0, 24); break;
        case MI_BASE: S->base = (uint8_t)clampi(S->base + inc, 0, 4); break;
        case MI_CV2: S->cv2 = (uint8_t)clampi(S->cv2 + inc, 0, CV2_COUNT - 1); break;
        case MI_CV2CC: S->cv2_cc = (uint8_t)clampi(S->cv2_cc + inc, 0, 127); break;
        case MI_CLOCK: S->clock = (uint8_t)clampi(S->clock + inc, 0, 5); break;
        case MI_LEGATO: S->legato = (uint8_t)clampi(S->legato + inc, 0, 1); break;
        case MI_CALSCL: S->cal_scale_mv = (int16_t)clampi(S->cal_scale_mv + 5 * inc, 4000, 6000); break;
        case MI_CALOFS: S->cal_offset_mv = (int16_t)clampi(S->cal_offset_mv + inc, -300, 300); break;
        case MI_TEST: test_set(clampi(g_test_step + inc, 0, 5)); return; /* not saved */
        case MI_MODS:
            picker::run(hw); /* modal; returns only if the user backs out */
            enc_down = false; /* its release edge happened inside the picker */
            return;
    }
    mark_dirty();
}

/* A press is tracked by libDaisy's own edges, never by Pressed(): RisingEdge
 * (switch state 0x7f) comes one 1 ms update BEFORE Pressed() (0xff), and a
 * bounce while turning a held knob drops Pressed() for up to 8 ms. Clearing
 * enc_down on !Pressed() threw every press away in the pass that set it, so
 * push-and-turn only moved the cursor on hardware (found 2026-10-03).
 * FallingEdge (0x80) needs 7 clean low samples: it fires once, on release. */
static void encoder(void)
{
    if(hw.encoder.RisingEdge())
    {
        enc_down   = true;
        enc_turned = false;
    }
    int inc = hw.encoder.Increment();
    if(inc)
    {
        if(enc_down)
        {
            enc_turned = true;
            menu_edit(inc);
        }
        else
        {
            g_sel = clampi(g_sel + inc, 0, MI_COUNT - 1);
            if(g_sel < g_top) g_top = g_sel;
            if(g_sel >= g_top + MENU_ROWS) g_top = g_sel - MENU_ROWS + 1;
        }
    }
    if(hw.encoder.FallingEdge()) enc_down = false;
}

/* ------------------------------------------------------------------------ */
/* Display                                                                   */

static const char *const NOTE_NAME[12]
    = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static void note_name(int note, char *out, size_t n)
{
    if(note < 0)
    {
        snprintf(out, n, "---");
        return;
    }
    /* Ableton's naming: MIDI 60 = C3. */
    snprintf(out, n, "%s%d", NOTE_NAME[note % 12], note / 12 - 2);
}

static uint32_t g_act_trs_until, g_act_usb_until, g_act_other_until;
static int      g_other_ch;

static void draw(void)
{
    char     line[32], nm[16];
    uint32_t now = System::GetNow();
    hw.display.Fill(false);

    /* Row 0: name, channel, which input is alive. */
    if(S->channel) snprintf(line, sizeof line, "RELAY ch%02d", S->channel);
    else snprintf(line, sizeof line, "RELAY omni");
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);
    snprintf(line, sizeof line, "%s %s", now < g_act_trs_until ? "TRS" : "   ",
             now < g_act_usb_until ? "USB" : "   ");
    hw.display.SetCursor(128 - 7 * 6, 0);
    hw.display.WriteString(line, Font_6x8, true);

    /* Row 1: the sounding note, its volts, the gate. */
    int   note = relay_voice_note(&R, 0);
    float v    = relay_pitch_volts(&R, 0);
    note_name(note, nm, sizeof nm);
    int mv = (int)lroundf(v * 1000.0f);
    snprintf(line, sizeof line, "%-4s %s%d.%03dV", nm, mv < 0 ? "-" : "+", abs(mv) / 1000,
             abs(mv) % 1000);
    hw.display.SetCursor(0, 10);
    hw.display.WriteString(line, Font_7x10, true);
    if(relay_gate(&R, 0)) hw.display.DrawRect(112, 10, 121, 19, true, true);
    else hw.display.DrawRect(112, 10, 121, 19, true, false);
    if(g_pitch_clipped)
    {
        /* Right of the gate box (112-121): x 122 + 6 px is the last column
         * a 6-px glyph fits in; row 1's text ends by x 91. */
        hw.display.SetCursor(122, 12);
        hw.display.WriteString("!", Font_6x8, true);
    }

    /* Row 2: velocity, mod, aftertouch, bend; or who is talking on the
     * wrong channel. */
    if(now < g_act_other_until && S->channel)
    {
        snprintf(line, sizeof line, "MIDI on ch%02d, not %02d", g_other_ch, S->channel);
    }
    else
    {
        int   b   = (int)lroundf(relay_source(&R, RELAY_SRC_BEND, 0) * 100.0f);
        float oct = (float)g_octave;
        snprintf(line, sizeof line, "V%03d M%03d B%+04d O%+d%s", (int)(relay_vel(&R, 0) * 127.0f + 0.5f),
                 R.cc[1], b, (int)oct, g_glide_ms ? "g" : " ");
    }
    hw.display.SetCursor(0, 22);
    hw.display.WriteString(line, Font_6x8, true);

    /* Rows 4-7: the menu, four rows of it, scrolling. */
    for(int i = 0; i < MENU_ROWS; i++)
    {
        int row = g_top + i;
        if(row >= MI_COUNT) break;
        char num[16];
        const char *value = "";
        switch(row)
        {
            case MI_CHAN:
                if(S->channel) snprintf(num, sizeof num, "%d", S->channel);
                else snprintf(num, sizeof num, "Omni");
                value = num;
                break;
            case MI_PRIO: value = PRIO_NAME[S->priority]; break;
            case MI_RETRIG: value = ONOFF[S->retrig]; break;
            case MI_BEND:
                snprintf(num, sizeof num, "%d st", S->bend);
                value = num;
                break;
            case MI_BASE:
                snprintf(num, sizeof num, "%s (%d)", BASE_NAME[S->base], BASE_NOTE[S->base]);
                value = num;
                break;
            case MI_CV2: value = CV2_NAME[S->cv2]; break;
            case MI_CV2CC:
                snprintf(num, sizeof num, "%d", S->cv2_cc);
                value = num;
                break;
            case MI_CLOCK: value = CLOCK_NAME[S->clock]; break;
            case MI_LEGATO: value = LEGATO_NAME[S->legato]; break;
            case MI_CALSCL:
                snprintf(num, sizeof num, "%d.%03dV", S->cal_scale_mv / 1000, S->cal_scale_mv % 1000);
                value = num;
                break;
            case MI_CALOFS:
                snprintf(num, sizeof num, "%+dmV", S->cal_offset_mv);
                value = num;
                break;
            case MI_TEST:
                if(g_test_step) snprintf(num, sizeof num, "%dV", g_test_step - 1);
                else snprintf(num, sizeof num, "Off");
                value = num;
                break;
            case MI_MODS:
                /* "0.1.0 abc1234" fills the row to 126 px; git may hand
                 * back a longer hash, so keep seven digits. */
                snprintf(num, sizeof num, "%s %.7s", RELAY_VERSION, RELAY_GIT_HASH);
                value = num;
                break;
        }
        snprintf(line, sizeof line, "%c%-6s %s", row == g_sel ? '>' : ' ', MENU_LABEL[row], value);
        hw.display.SetCursor(0, 32 + i * 8);
        hw.display.WriteString(line, Font_6x8, true);
    }

    /* Clock and run, bottom right, only when there is a clock, and not
     * while the Mods row (version + hash, the full width) is down there. */
    bool mods_on_bottom = g_top + MENU_ROWS - 1 == MI_MODS;
    if(R.clock_stale < RELAY_CLOCK_STALE_MS && !mods_on_bottom)
    {
        hw.display.SetCursor(128 - 4 * 6, 56);
        hw.display.WriteString(relay_run(&R) ? "RUN" : "clk", Font_6x8, true);
        if(relay_clock(&R)) hw.display.DrawRect(123, 57, 127, 61, true, true);
    }

    hw.display.Update();
}

/* ------------------------------------------------------------------------ */

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    /* Audio passes through untouched; nothing here needs the codec, but a
     * running callback keeps the outputs quiet and defined. */
    for(size_t i = 0; i < size; i++)
        for(int c = 0; c < 4; c++) out[c][i] = in[c][i];
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48);

    storage.Init(defaults(), kSettingsOffset);
    S = &storage.GetSettings();
    if(S->magic != kMagic) *S = defaults();

    relay_cfg_t cfg = build_cfg();
    relay_init(&R, &cfg);

    hw.midi.StartReceive();
    MidiUsbHandler::Config ucfg;
    ucfg.transport_config.periph = MidiUsbTransport::Config::INTERNAL;
    usb_midi.Init(ucfg);
    usb_midi.StartReceive();

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    write_outputs();

    uint32_t last_draw = 0, last_tick_us = System::GetUs();
    for(;;)
    {
        uint32_t now = System::GetNow();

        /* Time first, then this pass's events (the encoder's Test note and
         * MIDI), then the jacks: a pulse started below is written at full
         * length and never charged for time that passed before it. Whole
         * ms from the microsecond clock with the remainder carried, so
         * rounding cannot shorten a pulse either (see the snap below). */
        uint32_t now_us = System::GetUs();
        /* GetUs() is TIM2's 32-bit count divided down to us, so it wraps
         * every ~21.5 s (200 MHz tick), not at 2^32: resync there (one pass
         * of time lost, pulses only lengthened) rather than read the jump
         * as a huge dt. */
        if(now_us < last_tick_us) last_tick_us = now_us;
        uint32_t dt     = (now_us - last_tick_us) / 1000u;
        last_tick_us += dt * 1000u;
        if(dt > 1000) dt = 1000; /* back from the picker or a QSPI save */
        relay_tick(&R, dt);

        hw.ProcessAllControls();
        encoder();
        read_knobs();
        apply_cfg();

        pump_midi();
        write_outputs();
        /* A note or Start may have begun a trigger, gate dip or reset
         * pulse: count it from the moment it reached the jacks, dropping
         * the sub-ms carry (under 1 ms of glide per note, inaudible). */
        if(R.activity & (RELAY_ACT_NOTE | RELAY_ACT_START)) last_tick_us = System::GetUs();

        uint32_t act = relay_take_activity(&R);
        if(act & RELAY_ACT_ANY)
        {
            if(g_trs_seen) g_act_trs_until = now + 150;
            if(g_usb_seen) g_act_usb_until = now + 150;
            g_trs_seen = g_usb_seen = false;
        }
        if(act & RELAY_ACT_OTHER)
        {
            g_other_ch        = R.last_rx_channel;
            g_act_other_until = now + 1500;
        }

        /* Save a few seconds after the last edit, hands off the encoder.
         * Save() blocks on the QSPI write; the audio callback keeps running. */
        if(g_dirty && now - g_dirty_since >= 2000 && !hw.encoder.Pressed())
        {
            storage.Save();
            g_dirty = false;
        }

        if(now - last_draw >= 33)
        {
            last_draw = now;
            draw();
        }
        System::Delay(1);
    }
}
