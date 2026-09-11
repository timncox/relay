/*
 * Relay on the Hermetic Modular Alchemy Lab V2 -- MIDI to CV, up to four
 * voices, over the front USB-C.
 *
 * The engine is core/relay.c (shared with the Daisy Patch build); this file
 * is the panel: six pots on two pages, three buttons, eight CV-out jacks and
 * 102 LEDs, plus the USB MIDI device on the front port.
 *
 *   PLAY   CHANNEL · VOICES · PRIORITY · GLIDE · BEND · CLOCK
 *   SETUP  SRC A · CC A · SRC B · CC B · BASE · J10           (hold B3)
 *   B1     tap = all notes off
 *   B2     tap = test note (base note, then +1, +2, +3 octaves, then off)
 *   Settings (B2+B3 2 s): retrig, USB mode, glide legato; page 1 = SD picker
 *
 * Jacks by VOICES (J3..J6 are the MCP4728, J7/J8 the STM DAC, J9/J10 the
 * DC-coupled codec outs; all +-5 V, 0..5 V for unipolar things):
 *
 *   Mono  J3 pitch  J4 vel     J5 src A   J6 src B   J7 gate   J8 trig   J9 clock  J10 run/reset
 *   Duo   J3 pitch1 J4 pitch2  J5 vel1    J6 vel2    J7 gate1  J8 gate2  J9 clock  J10 run/reset
 *   Tri   J3 pitch1 J4 pitch2  J5 pitch3  J6 src A   J7 gate1  J8 gate2  J9 gate3  J10 clock
 *   Quad  J3 pitch1 J4 pitch2  J5 pitch3  J6 pitch4  J7 gate1  J8 gate2  J9 gate3  J10 gate4
 *
 * Pitch goes on the MCP jacks because those have a per-jack DAC calibration
 * fit in the board's cal record; the codec outs are 24-bit but nominal.
 *
 * USB: libDaisy's USB device stack is one class at a time (a global picks
 * CDC or MIDI descriptors), so USB MIDI and HostLink cannot share a boot.
 * The Settings screen's USB selector chooses; MIDI is the default and the
 * point of the firmware. Pick HostLink to reach presets and the card from
 * the web programmer, then switch back. Takes effect on the next boot.
 *
 * Shared by copy with smack/mark/belt-alchemy: the shape of this file, the
 * picker, the .lds, the autosave.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daisy_seed.h"
#include "hid/midi.h"
#include "hid/usb.h"
#include "hid/usb_midi.h"
#include "usbd_def.h"
#include "usbd_desc.h"
#include "usbd_ctlreq.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "extras.h"
#include "picker.h"
#include "relay.h"

#ifndef RELAY_VERSION
#define RELAY_VERSION "0.0.0"
#endif
#ifndef RELAY_GIT_HASH
#define RELAY_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

static constexpr uint32_t kBlockSize = 24u;

enum : uint8_t { kPagePlay = 0, kPageSetup = 1, kNumAppPages = 2 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::MidiUsbHandler midi;
static relay_t             R;
static bool                g_hostlink_mode = false;

/* ---- colours ------------------------------------------------------------- */

static constexpr LedPanel::Rgb kColChan   = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColChanHi = {0xFF, 0x90, 0x20};
static constexpr LedPanel::Rgb kColOmni   = {0x30, 0x30, 0x40};
static constexpr LedPanel::Rgb kColVoice  = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColPrio   = {0xC0, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColGlide  = {0x40, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColBend   = {0xFF, 0x60, 0x60};
static constexpr LedPanel::Rgb kColClock  = {0x40, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColSrc    = {0xFF, 0xC0, 0x40};
static constexpr LedPanel::Rgb kColCc     = {0xFF, 0xE0, 0x80};
static constexpr LedPanel::Rgb kColBase   = {0x80, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColJ10    = {0xFF, 0x80, 0xC0};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColSetup  = {0x30, 0x10, 0x40};
static constexpr LedPanel::Rgb kGreen     = {0x00, 0xFF, 0x00};
static constexpr LedPanel::Rgb kRed       = {0xFF, 0x00, 0x00};
static constexpr LedPanel::Rgb kAmber     = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kBlue      = {0x00, 0x50, 0xFF};
static constexpr LedPanel::Rgb kMagenta   = {0xFF, 0x00, 0xA0};
static constexpr LedPanel::Rgb kIdle      = {0x00, 0x00, 0x30};

/* ---- labels (descriptor metadata; static storage, borrowed by pointer) -- */

static const char* const kChanLabels[17] = {"Omni", "1", "2", "3", "4", "5", "6", "7", "8",
                                             "9", "10", "11", "12", "13", "14", "15", "16"};
static const char* const kVoiceLabels[4] = {"Mono", "Duo", "Tri", "Quad"};
static const char* const kPrioLabels[3]  = {"Last", "Low", "High"};
static const char* const kClockLabels[6] = {"1/32", "1/16", "1/8", "1/4", "1/2", "Bar"};
static const uint8_t     kClockTicks[6]  = {3, 6, 12, 24, 48, 96};
static const char* const kSrcLabels[5]   = {"Vel", "Mod", "CC", "AfterT", "Bend"};
static const char* const kBaseLabels[5]  = {"C-1", "C0", "C1", "C2", "C3"};
static const uint8_t     kBaseNotes[5]   = {12, 24, 36, 48, 60};
static const char* const kJ10Labels[2]   = {"Run", "Reset"};
static const char* const kOnOff[2]       = {"Off", "On"};
static const char* const kUsbLabels[2]   = {"MIDI", "HostLink"};
static const char* const kLegatoLabels[2] = {"Always", "Legato"};

/* ---- rings ---------------------------------------------------------------- */

static uint32_t g_note_flash_until = 0;

/* CHANNEL: omni = the whole arc dim; a channel = one LED at position
 * (channel - 1) mod 13, white for 1..13 and amber for 14..16 so the wrap is
 * visible. The ring brightens for a moment on every note. */
static void ChannelRing(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                        float norm, uint32_t t_ms, void* /*ctx*/)
{
    int zone = (int)(norm * 17.0f);
    if(zone > 16) zone = 16;
    const bool flash = t_ms < g_note_flash_until;
    if(zone == 0)
    {
        LedPanel::Rgb c = flash ? kColChan : kColOmni;
        for(int i = 0; i < geo.arc_leds; i++)
            panel.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)i, 12.0f),
                                panel.ScaleGlobal(c));
        return;
    }
    const int      idx = (zone - 1) % geo.arc_leds;
    LedPanel::Rgb  c   = zone > geo.arc_leds ? kColChanHi : kColChan;
    for(int i = 0; i < geo.arc_leds; i++)
    {
        LedPanel::Rgb col = i == idx ? c : (flash ? kColOmni : kColDim);
        panel.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)i, 12.0f),
                            panel.ScaleGlobal(col));
    }
}

/* ---- pages --------------------------------------------------------------- */

/* PLAY. Panel positions, front view:
 *      [P1] B1 [P2]        CHANNEL, VOICES
 *      [P3] B2 [P4]        PRIORITY, GLIDE
 *      [P5] B3 [P6]        BEND, CLOCK                                     */

static VirtualKnob k_chan = VirtualKnob(kPotTopLeft, "Channel")
    .Selector(17).Labels(kChanLabels).Ident("channel")
    .Ring(Custom(ChannelRing));

static VirtualKnob k_voices = VirtualKnob(kPotTopRight, "Voices")
    .Selector(4).Labels(kVoiceLabels).Ident("voices")
    .Ring(SelectorRing(kColVoice, kColDim, 4));

static VirtualKnob k_prio = VirtualKnob(kPotMiddleLeft, "Priority")
    .Selector(3).Labels(kPrioLabels).Ident("priority")
    .Ring(SelectorRing(kColPrio, kColDim, 3));

static VirtualKnob k_glide = VirtualKnob(kPotMiddleRight, "Glide")
    .Exp(1.f, 2000.f).Ident("glide").Unit("ms")
    .Ring(Level(kColGlide));

static VirtualKnob k_bend = VirtualKnob(kPotBottomLeft, "Bend")
    .Linear(0.f, 24.f).Ident("bend").Unit("st")
    .Ring(Level(kColBend));

static VirtualKnob k_clock = VirtualKnob(kPotBottomRight, "Clock")
    .Selector(6).Labels(kClockLabels).Ident("clock")
    .Ring(SelectorRing(kColClock, kColDim, 6));

/* SETUP: hold B3. */

static VirtualKnob k_srca = VirtualKnob(kPotTopLeft, "Src A")
    .Selector(5).Labels(kSrcLabels).Ident("src_a")
    .Ring(SelectorRing(kColSrc, kColDim, 5));

static VirtualKnob k_cca = VirtualKnob(kPotTopRight, "CC A")
    .Linear(0.f, 127.f).Ident("cc_a")
    .Ring(Level(kColCc));

static VirtualKnob k_srcb = VirtualKnob(kPotMiddleLeft, "Src B")
    .Selector(5).Labels(kSrcLabels).Ident("src_b")
    .Ring(SelectorRing(kColSrc, kColDim, 5));

static VirtualKnob k_ccb = VirtualKnob(kPotMiddleRight, "CC B")
    .Linear(0.f, 127.f).Ident("cc_b")
    .Ring(Level(kColCc));

static VirtualKnob k_base = VirtualKnob(kPotBottomLeft, "0 V note")
    .Selector(5).Labels(kBaseLabels).Ident("base")
    .Ring(SelectorRing(kColBase, kColDim, 5));

static VirtualKnob k_j10 = VirtualKnob(kPotBottomRight, "J10")
    .Selector(2).Labels(kJ10Labels).Ident("j10")
    .Ring(SelectorRing(kColJ10, kColDim, 2));

static Page play_page = Page(kPagePlay).Name("Play").Color("#ffffff")
    .Knobs(k_chan, k_voices, k_prio, k_glide, k_bend, k_clock);

static Page setup_page = Page(kPageSetup).Name("Setup").Color("#c060ff")
    .Knobs(k_srca, k_cca, k_srcb, k_ccb, k_base, k_j10);

/* ---- descriptor metadata (the Mono map; the others are in the manual) --- */

static Jack jk_j1  ("J1",  "unused",      JackSig::AudioIn);
static Jack jk_j2  ("J2",  "unused",      JackSig::AudioIn);
static Jack jk_j3  ("J3",  "Pitch",       JackSig::CvBi);
static Jack jk_j4  ("J4",  "Velocity",    JackSig::CvBi);
static Jack jk_j5  ("J5",  "Src A",       JackSig::CvBi);
static Jack jk_j6  ("J6",  "Src B",       JackSig::CvBi);
static Jack jk_j7  ("J7",  "Gate",        JackSig::Trig);
static Jack jk_j8  ("J8",  "Trigger",     JackSig::Trig);
static Jack jk_j9  ("J9",  "Clock",       JackSig::Trig);
static Jack jk_j10 ("J10", "Run / Reset", JackSig::Trig);

static VirtualButton bt_panic = VirtualButton("b1", "All notes off")
    .Action("Tap", "Every gate down, every voice released");

static VirtualButton bt_test = VirtualButton("b2", "Test note")
    .Action("Tap", "Hold the 0 V note; tap again for +1, +2, +3 octaves, then off");

static VirtualButton bt_setup = VirtualButton("b3", "Setup")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("MIDI to CV, one to four voices, over the front USB-C")
    .Preamble(
        "**Relay** turns MIDI from the front USB-C into pitch, gate, "
        "velocity and modulation on the jacks. CHANNEL and VOICES set what "
        "it listens to and how many voices it lays out; the jack map "
        "follows VOICES (Mono: J3 pitch, J4 velocity, J5/J6 the two "
        "assignable sources, J7 gate, J8 trigger, J9 clock, J10 run or "
        "reset. Duo: pitches on J3/J4, velocities J5/J6, gates J7/J8. Tri: "
        "pitches J3-J5, src A on J6, gates J7-J9, clock J10. Quad: pitches "
        "J3-J6, gates J7-J10). Pitch is 1 V/oct from the 0 V note on the "
        "Setup page; PRIORITY, GLIDE and BEND do what they say. B1 is "
        "panic; B2 holds a test note for tuning. The Settings screen's USB "
        "selector swaps the port between MIDI and HostLink on the next "
        "boot; this page is only reachable in HostLink mode.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageSetup);
static Presets     presets(hw.seed.qspi);
static Settings    settings(hw, &pager);
static SdCard      sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "relay_alchemy", "Relay",
                           RELAY_VERSION, RELAY_GIT_HASH);
static RelayExtras    extras;
static SelectorHandle s_retrig, s_usb, s_legato;

/* ---- USB MIDI product string ---------------------------------------------- */

/* The ST USB core fetches the product string through a function pointer in
 * the external port's descriptor table; repointing it before Init renames
 * the device (the SDK's CDC transport does the same). */
static const char* s_product_name = "Relay";
static uint8_t     s_product_desc[2u + 2u * 48u];

static uint8_t* ProductStrOverride(USBD_SpeedTypeDef /*speed*/, uint16_t* length)
{
    USBD_GetString(reinterpret_cast<uint8_t*>(const_cast<char*>(s_product_name)),
                   s_product_desc, length);
    return s_product_desc;
}

/* ---- MIDI in ---------------------------------------------------------------- */

static void feed(daisy::MidiEvent& e)
{
    using namespace daisy;
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

/* ---- knobs -> engine config -------------------------------------------------- */

/*
 * Selector knobs need hysteresis: a pot parked on a zone boundary would
 * otherwise chatter across it on ADC noise, and CHANNEL or VOICES chattering
 * silences every voice each time (relay_set_cfg does that on purpose). The
 * SDK leaves zone-commit debouncing to the app (docs/pages-and-layers.md).
 */
#define HYST 0.02f

struct Zoned
{
    int   zones;
    int   last      = -1;
    float last_norm = 0.0f;
};

static int zoned(Zoned& z, float norm)
{
    if(norm < 0.0f) norm = 0.0f;
    if(norm > 1.0f) norm = 1.0f;
    int v = (int)(norm * (float)z.zones);
    if(v >= z.zones) v = z.zones - 1;
    if(z.last >= 0 && v != z.last && fabsf(norm - z.last_norm) < HYST) return z.last;
    if(v != z.last)
    {
        z.last      = v;
        z.last_norm = norm;
    }
    return v;
}

static Zoned z_chan{17}, z_voices{4}, z_prio{3}, z_clock{6}, z_srca{5}, z_srcb{5}, z_base{5}, z_j10{2};

static int g_src_a = 1, g_src_b = 3, g_cc_a = 1, g_cc_b = 74, g_j10_reset = 0;

/* Test note state (B2), see test_set() below. */
static int g_test_step = 0;
static int g_test_note = -1;

static void apply_knobs(void)
{
    relay_cfg_t c = R.cfg;
    c.channel     = (uint8_t)zoned(z_chan, k_chan.Norm());
    c.voices      = (uint8_t)(zoned(z_voices, k_voices.Norm()) + 1);
    c.priority    = (uint8_t)zoned(z_prio, k_prio.Norm());
    c.clock_div   = kClockTicks[zoned(z_clock, k_clock.Norm())];
    c.base_note   = kBaseNotes[zoned(z_base, k_base.Norm())];
    float g       = k_glide.Value();
    c.glide_ms    = g <= 2.0f ? 0 : (uint16_t)g;
    c.bend_range  = (uint8_t)lroundf(k_bend.Value());
    c.retrig      = (uint8_t)((int)s_retrig.Value() == 1);
    c.glide_legato = (uint8_t)((int)s_legato.Value() == 1);
    c.trig_ms     = 5;
    c.reset_ms    = 10;
    if(memcmp(&c, &R.cfg, sizeof c) != 0)
    {
        /* A channel or voice-count change silences every voice, the test
         * note included; forget it so B2 does not send a stray note-off
         * later and the amber light matches what is sounding. */
        if(c.channel != R.cfg.channel || c.voices != R.cfg.voices)
        {
            g_test_note = -1;
            g_test_step = 0;
        }
        relay_set_cfg(&R, &c);
    }

    g_src_a     = zoned(z_srca, k_srca.Norm());
    g_src_b     = zoned(z_srcb, k_srcb.Norm());
    g_cc_a      = (int)lroundf(k_cca.Value());
    g_cc_b      = (int)lroundf(k_ccb.Value());
    g_j10_reset = zoned(z_j10, k_j10.Norm());
}

/* ---- jacks --------------------------------------------------------------------- */

enum OutKind : uint8_t { O_PITCH, O_VEL, O_SRC_A, O_SRC_B, O_GATE, O_TRIG, O_CLOCK, O_RUNRST };
struct OutSpec
{
    uint8_t kind;
    uint8_t voice;
};
static const OutSpec kJackMap[4][kNumCvJacks] = {
    /* Mono */ {{O_PITCH, 0}, {O_VEL, 0}, {O_SRC_A, 0}, {O_SRC_B, 0}, {O_GATE, 0}, {O_TRIG, 0}, {O_CLOCK, 0}, {O_RUNRST, 0}},
    /* Duo  */ {{O_PITCH, 0}, {O_PITCH, 1}, {O_VEL, 0}, {O_VEL, 1}, {O_GATE, 0}, {O_GATE, 1}, {O_CLOCK, 0}, {O_RUNRST, 0}},
    /* Tri  */ {{O_PITCH, 0}, {O_PITCH, 1}, {O_PITCH, 2}, {O_SRC_A, 0}, {O_GATE, 0}, {O_GATE, 1}, {O_GATE, 2}, {O_CLOCK, 0}},
    /* Quad */ {{O_PITCH, 0}, {O_PITCH, 1}, {O_PITCH, 2}, {O_PITCH, 3}, {O_GATE, 0}, {O_GATE, 1}, {O_GATE, 2}, {O_GATE, 3}},
};

static float source_volts(int src, int cc)
{
    switch(src)
    {
        case 0: return 5.0f * relay_source(&R, RELAY_SRC_VEL, 0);
        case 1: return 5.0f * relay_source(&R, RELAY_SRC_MOD, 0);
        case 2: return 5.0f * relay_source(&R, RELAY_SRC_CC, (uint8_t)cc);
        case 3: return 5.0f * relay_source(&R, RELAY_SRC_AT, 0);
        case 4: return 5.0f * relay_source(&R, RELAY_SRC_BEND, 0);
        default: return 0.0f;
    }
}

static float jack_volts(const OutSpec& o)
{
    switch(o.kind)
    {
        case O_PITCH:
        {
            float v = relay_pitch_volts(&R, o.voice);
            if(v > 5.0f) v = 5.0f;
            if(v < -5.0f) v = -5.0f;
            return v;
        }
        case O_VEL: return 5.0f * relay_vel(&R, o.voice);
        case O_SRC_A: return source_volts(g_src_a, g_cc_a);
        case O_SRC_B: return source_volts(g_src_b, g_cc_b);
        case O_GATE: return relay_gate(&R, o.voice) ? 5.0f : 0.0f;
        case O_TRIG: return relay_trig(&R, o.voice) ? 5.0f : 0.0f;
        case O_CLOCK: return relay_clock(&R) ? 5.0f : 0.0f;
        case O_RUNRST: return (g_j10_reset ? relay_reset(&R) : relay_run(&R)) ? 5.0f : 0.0f;
        default: return 0.0f;
    }
}

static float g_last_volts[kNumCvJacks];
static bool  g_first_write = true;

/* Stage what changed on the MCP jacks and latch once; the STM DAC and codec
 * jacks are direct. A Flush with nothing staged returns at once. */
static void write_jacks(void)
{
    const OutSpec* map = kJackMap[R.cfg.voices - 1];
    for(uint8_t j = 0; j < kNumCvJacks; j++)
    {
        float v = jack_volts(map[j]);
        if(!g_first_write && v == g_last_volts[j]) continue;
        g_last_volts[j] = v;
        hw.cv_jacks[j].StageVolts(v);
    }
    g_first_write = false;
    hw.FlushCvOutputs();
}

/* ---- test note ------------------------------------------------------------------ */

static void test_set(int step)
{
    int ch = R.cfg.channel ? R.cfg.channel - 1 : 0;
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

/* ---- buttons (1 ms poll) ---------------------------------------------------------- */

struct Press
{
    bool down   = false;
    bool veto   = false;   /* B3 was held during the press: release is inert */
};
static Press b1, b2;
static uint32_t g_panic_flash_until = 0;
static uint32_t g_last_poll = 0;

static void OnPoll(uint32_t now)
{
    /* MIDI first, then time, then the jacks: the shortest path from a byte
     * on the wire to a volt on the panel is one poll. */
    if(!g_hostlink_mode)
    {
        midi.Listen();
        while(midi.HasEvents())
        {
            daisy::MidiEvent e = midi.PopEvent();
            feed(e);
        }
    }
    uint32_t dt = now - g_last_poll;
    g_last_poll = now;
    if(dt > 50) dt = 50;
    relay_tick(&R, dt);
    write_jacks();

    if(settings.IsActive())
    {
        b1.down = b2.down = false;
        return;
    }

    /* B1: all notes off. On release, so it cannot double with a chord. */
    {
        const bool pressed = hw.buttons[kButtonB1].Pressed();
        if(pressed && !b1.down) b1.down = true;
        if(b1.down && !pressed)
        {
            b1.down = false;
            test_set(0);
            relay_all_notes_off(&R);
            g_panic_flash_until = now + 200;
        }
    }

    /* B2: test note. Stands down while B3 is held: B2+B3 held two seconds
     * is the Settings chord, and every trip into Settings would otherwise
     * leave a test note sounding. */
    {
        const bool pressed = hw.buttons[kButtonB2].Pressed();
        const bool shift   = hw.buttons[kButtonB3].Pressed();
        if(pressed && !b2.down)
        {
            b2.down = true;
            b2.veto = shift;
        }
        if(b2.down && shift) b2.veto = true;
        if(b2.down && !pressed)
        {
            b2.down = false;
            if(!b2.veto) test_set(g_test_step >= 4 ? 0 : g_test_step + 1);
        }
    }
}

/* ---- control frame (~60 Hz) --------------------------------------------------------- */

static bool     g_dirty       = false;
static uint32_t g_dirty_since = 0;
static bool     prev_settings = false;
static uint8_t  g_saved_cfg_hash[sizeof(relay_cfg_t)];

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();
    apply_knobs();

    /* Anything on the Setup page or in Settings is worth a flash write when
     * it moves; the Play page too (channel, voices...). Compare the resolved
     * config rather than raw pots so ADC noise never triggers a save. */
    struct
    {
        relay_cfg_t c;
        int         a, b, ca, cb, j;
    } snap = {R.cfg, g_src_a, g_src_b, g_cc_a, g_cc_b, g_j10_reset};
    static decltype(snap) last;
    static bool           primed = false;
    if(!primed || memcmp(&snap, &last, sizeof snap) != 0)
    {
        if(primed)
        {
            g_dirty       = true;
            g_dirty_since = now;
        }
        last   = snap;
        primed = true;
    }

    const bool sact = settings.IsActive();
    if(prev_settings && !sact)
    {
        g_dirty       = true;
        g_dirty_since = now;
    }
    prev_settings = sact;

    uint32_t act = relay_take_activity(&R);
    if(act & RELAY_ACT_NOTE) g_note_flash_until = now + 60;

    /* Autosave slot 0, the working state, a few seconds after the last
     * change and only with hands off: Save() erases the sectors the record
     * covers from this thread, and the 1 ms poll is paused for the duration
     * (a gate edge during the write is late by that much). */
    if(g_dirty && now - g_dirty_since >= 5000u && !sact && !picker::Busy()
       && !hw.buttons[kButtonB1].Pressed() && !hw.buttons[kButtonB2].Pressed()
       && !hw.buttons[kButtonB3].Pressed())
    {
        presets.Save(0);
        g_dirty = false;
    }
    (void)g_saved_cfg_hash;
}

/* ---- LEDs ---------------------------------------------------------------------------- */

/*
 * The SDK draws the rings from the knobs. Added on top:
 *   VOICES ring  one green pip per gated voice, spread across the arc
 *   B1 pair      green while any gate is high; red flash on panic
 *   B2 pair      blue on each clock pulse; amber while a test note sounds;
 *                steady magenta when the USB port is in HostLink mode
 *   B3 pair      dim Setup tint (the SDK paints it while the page is held)
 */
static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if(settings.IsActive()) return;   /* Settings owns the buttons */

    const ArcGeometry& geo = hw.Arc();
    const int          n   = R.cfg.voices;
    for(int v = 0; v < n; v++)
    {
        if(!relay_gate(&R, v)) continue;
        const float pos  = n == 1 ? 0.5f : (float)v / (float)(n - 1);
        const float hour = geo.start_hour + geo.step_hours * pos * (float)(geo.arc_leds - 1);
        L.SetRingByHour(kPotTopRight, fmodf(hour, 12.0f), L.ScaleGlobal(kGreen));
    }

    LedPanel::Rgb b1c = relay_any_gate(&R) ? kGreen : kIdle;
    if(t_ms < g_panic_flash_until) b1c = kRed;
    L.SetButtonPair(kButtonB1, L.ScaleGlobal(b1c));

    LedPanel::Rgb b2c;
    if(g_hostlink_mode) b2c = kMagenta;
    else if(g_test_step) b2c = kAmber;
    else if(relay_clock(&R)) b2c = kBlue;
    else b2c = kIdle;
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(b2c));

    if(pager.Page() != kPageSetup)
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(kColSetup));
}

/* ---- audio ----------------------------------------------------------------------------- */

static void AudioCallback(daisy::AudioHandle::InputBuffer  /*in*/,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    /* J9/J10 are claimed as CV outs; the SDK's shim overwrites them with
     * the SetVolts targets after this returns. Silence otherwise. */
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = 0.0f;
        out[1][i] = 0.0f;
    }
}

/* ---- boot -------------------------------------------------------------------------------- */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);

    relay_cfg_t cfg;
    relay_default_cfg(&cfg);
    relay_init(&R, &cfg);

    /* Every jack is an output here. J3..J8 close their DG411; J9/J10 are
     * claimed from the audio callback. */
    for(uint8_t j = 0; j < kNumCvJacks; j++)
    {
        hw.cv_jacks[j].EnableCvOutput();
        hw.cv_jacks[j].SetVolts(0.0f);
    }

    /* Storage + settings. sd.Init() registers the SDRAM volume; no card I/O
     * until something opens it. */
    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);
    s_retrig = settings.Page(kSettingsMain).Pot(1)
        .Selector(kOnOff).Default(0)
        .Ident("retrig").Name("Retrigger")
        .Help("**On** drops the gate for a moment and fires the trigger "
              "whenever a voice takes a new note while already sounding, "
              "so a legato phrase re-attacks. **Off** is legato: the pitch "
              "moves and the gate stays up.");
    /* Pots 2 and 3 are the SDK's preset slot / action pair; 0 is brightness. */
    s_usb = settings.Page(kSettingsMain).Pot(4)
        .Selector(kUsbLabels).Default(0)
        .Ident("usb").Name("USB port")
        .Help("**MIDI**: the front USB-C is a USB MIDI device (the point of "
              "this firmware). **HostLink**: it is the web programmer's port "
              "instead, for presets and the card. Takes effect on the next "
              "boot. The B2 light is magenta in HostLink mode.");
    s_legato = settings.Page(kSettingsMain).Pot(5)
        .Selector(kLegatoLabels).Default(0)
        .Ident("glide.legato").Name("Glide")
        .Help("**Always** glides into every note. **Legato** glides only "
              "when the new note overlaps one still held.");

    /* HostLink: identity, descriptor, SD file access. Only attached to the
     * loop in HostLink mode; declaring it costs nothing. */
    host.Product("Relay")
        .Pages(play_page, setup_page)
        .Jacks(jk_j1, jk_j2, jk_j3, jk_j4, jk_j5, jk_j6, jk_j7, jk_j8, jk_j9, jk_j10)
        .Buttons(bt_panic, bt_test, bt_setup)
        .Attach(manual)
        .Extend(fs_ext);

    /* Preset payload: both pages, the settings screen, and the extras. */
    presets.Manage(pager);
    presets.Manage(settings);
    presets.Manage(extras);
    presets.Init();
    const bool had_boot = presets.HasValid(0);
    presets.BootLoad();

    /* Physical pot positions, primed. */
    float phys[kNumPots];
    for(int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    for(uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();

    if(!had_boot)
    {
        /* First boot after a flash. Zone i of N sits at (i + 0.5) / N. */
        pager.SetStored(kPageSetup, kPotTopLeft,     1.5f / 5.0f,   phys); /* src A = Mod   */
        pager.SetStored(kPageSetup, kPotTopRight,    1.0f / 127.0f, phys); /* CC A = 1      */
        pager.SetStored(kPageSetup, kPotMiddleLeft,  3.5f / 5.0f,   phys); /* src B = AfterT*/
        pager.SetStored(kPageSetup, kPotMiddleRight, 74.0f / 127.0f, phys); /* CC B = 74    */
        pager.SetStored(kPageSetup, kPotBottomLeft,  2.5f / 5.0f,   phys); /* base = C1 (36)*/
        pager.SetStored(kPageSetup, kPotBottomRight, 0.5f / 2.0f,   phys); /* J10 = Run     */
    }

    /* The PLAY page comes up honest: whatever a pot points at is the
     * channel, the voice count, the priority. Setup keeps its stored values
     * behind the catch. */
    for(uint8_t p = 0; p < kNumPots; p++)
        pager.SetStored(kPagePlay, p, phys[p], phys);

    /* USB: one class per boot. Decided by the stored setting. */
    g_hostlink_mode = (int)s_usb.Value() == 1;
    if(!g_hostlink_mode)
    {
        HS_Desc.GetProductStrDescriptor = &ProductStrOverride;
        daisy::MidiUsbHandler::Config mcfg;
        mcfg.transport_config.periph = daisy::MidiUsbTransport::Config::EXTERNAL;
        midi.Init(mcfg);
        midi.StartReceive();
    }

    hw.StartAudio(AudioCallback);
    g_last_poll = System::GetNow();

    loop.Use(pager)
        .Use(settings)
        .Use(play_page)
        .Use(setup_page)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);
    if(g_hostlink_mode) loop.Use(host);

    for(;;) loop.Tick();
}
