/* relay -- MIDI to CV core. See relay.h. */
#include "relay.h"
#include <string.h>

/* ------------------------------------------------------------------------ */
/* configuration                                                             */

void relay_default_cfg(relay_cfg_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->channel      = 0;
    cfg->voices       = 1;
    cfg->priority     = RELAY_PRIO_LAST;
    cfg->retrig       = 0;
    cfg->glide_legato = 0;
    cfg->base_note    = 36;
    cfg->bend_range   = 2;
    cfg->clock_div    = 6;
    cfg->glide_ms     = 0;
    cfg->trig_ms      = 5;
    cfg->reset_ms     = 10;
}

static void sanitize(relay_cfg_t *c)
{
    if(c->channel > 16) c->channel = 0;
    if(c->voices < 1) c->voices = 1;
    if(c->voices > RELAY_MAX_VOICES) c->voices = RELAY_MAX_VOICES;
    if(c->priority > RELAY_PRIO_HIGH) c->priority = RELAY_PRIO_LAST;
    if(c->base_note > 127) c->base_note = 127;
    if(c->bend_range > 24) c->bend_range = 24;
    if(c->clock_div < 1) c->clock_div = 1;
    if(c->trig_ms < 1) c->trig_ms = 1;
    if(c->reset_ms < 1) c->reset_ms = 1;
}

void relay_init(relay_t *r, const relay_cfg_t *cfg)
{
    memset(r, 0, sizeof *r);
    if(cfg) r->cfg = *cfg;
    else relay_default_cfg(&r->cfg);
    sanitize(&r->cfg);
    r->last_voice = -1;
    r->clock_stale = RELAY_CLOCK_STALE_MS;
    for(int i = 0; i < RELAY_MAX_VOICES; i++)
    {
        r->v[i].pitch  = r->cfg.base_note;
        r->v[i].target = r->cfg.base_note;
    }
}

void relay_set_cfg(relay_t *r, const relay_cfg_t *cfg)
{
    relay_cfg_t c = *cfg;
    sanitize(&c);
    bool silence = c.channel != r->cfg.channel || c.voices != r->cfg.voices;
    r->cfg = c;
    if(silence) relay_all_notes_off(r);
}

/* ------------------------------------------------------------------------ */
/* voices                                                                    */

static void voice_retarget(relay_t *r, relay_voice_t *v, uint8_t note, bool was_held)
{
    v->note   = note;
    v->target = (float)note;
    bool glide = r->cfg.glide_ms > 0 && (!r->cfg.glide_legato || was_held);
    if(glide && v->pitch != v->target)
    {
        v->rate = (v->target - v->pitch) / (float)r->cfg.glide_ms;
    }
    else
    {
        v->pitch = v->target;
        v->rate  = 0.0f;
    }
}

static void voice_fire(relay_t *r, relay_voice_t *v, bool retrig_dip)
{
    v->trig_left = r->cfg.trig_ms;
    if(retrig_dip) v->dip_left = RELAY_RETRIG_MS;
}

/* Mono: which held note sounds, by priority. stack_n > 0 assumed. */
static int mono_pick(const relay_t *r)
{
    int best = r->stack_n - 1; /* last */
    if(r->cfg.priority == RELAY_PRIO_LOW)
    {
        for(int i = 0; i < r->stack_n; i++)
            if(r->stack_note[i] < r->stack_note[best]) best = i;
    }
    else if(r->cfg.priority == RELAY_PRIO_HIGH)
    {
        for(int i = 0; i < r->stack_n; i++)
            if(r->stack_note[i] > r->stack_note[best]) best = i;
    }
    return best;
}

static void stack_remove(relay_t *r, uint8_t note)
{
    for(int i = 0; i < r->stack_n; i++)
    {
        if(r->stack_note[i] != note) continue;
        for(int j = i; j < r->stack_n - 1; j++)
        {
            r->stack_note[j] = r->stack_note[j + 1];
            r->stack_vel[j]  = r->stack_vel[j + 1];
        }
        r->stack_n--;
        return;
    }
}

static void stack_push(relay_t *r, uint8_t note, uint8_t vel)
{
    stack_remove(r, note);
    if(r->stack_n == RELAY_STACK)
    {
        /* Drop the oldest; nobody holds 32 keys on purpose. */
        memmove(r->stack_note, r->stack_note + 1, RELAY_STACK - 1);
        memmove(r->stack_vel, r->stack_vel + 1, RELAY_STACK - 1);
        r->stack_n--;
    }
    r->stack_note[r->stack_n] = note;
    r->stack_vel[r->stack_n]  = vel;
    r->stack_n++;
}

static void mono_note_on(relay_t *r, uint8_t note, uint8_t vel)
{
    relay_voice_t *v        = &r->v[0];
    bool           was_held = v->held;
    stack_push(r, note, vel);
    int k = mono_pick(r);
    if(!was_held)
    {
        v->held  = true;
        v->vel   = r->stack_vel[k];
        v->order = ++r->order;
        voice_retarget(r, v, r->stack_note[k], false);
        voice_fire(r, v, false);
        r->last_voice = 0;
        return;
    }
    if(r->stack_note[k] != v->note)
    {
        v->vel   = r->stack_vel[k];
        v->order = ++r->order;
        voice_retarget(r, v, r->stack_note[k], true);
        if(r->cfg.retrig) voice_fire(r, v, true);
    }
    /* A new note that does not win priority changes nothing audible. */
    r->last_voice = 0;
}

static void mono_note_off(relay_t *r, uint8_t note)
{
    relay_voice_t *v = &r->v[0];
    stack_remove(r, note);
    if(!v->held) return;
    if(r->stack_n == 0)
    {
        v->held  = false;
        v->order = ++r->order;
        return;
    }
    int k = mono_pick(r);
    if(r->stack_note[k] != v->note)
    {
        v->vel = r->stack_vel[k];
        voice_retarget(r, v, r->stack_note[k], true);
        if(r->cfg.retrig) voice_fire(r, v, true);
    }
}

static void poly_note_on(relay_t *r, uint8_t note, uint8_t vel)
{
    int n = r->cfg.voices;
    /* Same note already sounding: retrigger it in place. */
    for(int i = 0; i < n; i++)
    {
        relay_voice_t *v = &r->v[i];
        if(v->held && v->note == note)
        {
            v->vel   = vel;
            v->order = ++r->order;
            voice_fire(r, v, true);
            r->last_voice = i;
            return;
        }
    }
    /* Free voice released longest ago; else steal the oldest held. */
    int best = -1;
    for(int i = 0; i < n; i++)
    {
        relay_voice_t *v = &r->v[i];
        if(v->held) continue;
        if(best < 0 || v->order < r->v[best].order) best = i;
    }
    bool steal = best < 0;
    if(steal)
    {
        for(int i = 0; i < n; i++)
            if(best < 0 || r->v[i].order < r->v[best].order) best = i;
    }
    relay_voice_t *v = &r->v[best];
    bool any_held = false;
    for(int i = 0; i < n; i++) any_held |= r->v[i].held;
    v->held    = true;
    v->vel     = vel;
    v->poly_at = 0;
    v->order   = ++r->order;
    voice_retarget(r, v, note, any_held);
    voice_fire(r, v, steal);
    r->last_voice = best;
}

static void poly_note_off(relay_t *r, uint8_t note)
{
    int n = r->cfg.voices;
    for(int i = 0; i < n; i++)
    {
        relay_voice_t *v = &r->v[i];
        if(v->held && v->note == note)
        {
            v->held  = false;
            v->order = ++r->order;
        }
    }
}

void relay_all_notes_off(relay_t *r)
{
    r->stack_n = 0;
    for(int i = 0; i < RELAY_MAX_VOICES; i++)
    {
        relay_voice_t *v = &r->v[i];
        if(v->held)
        {
            v->held  = false;
            v->order = ++r->order;
        }
        v->dip_left  = 0;
        v->trig_left = 0;
        v->rate      = 0.0f;
        v->pitch     = v->target;
    }
}

/* ------------------------------------------------------------------------ */
/* events                                                                    */

static void note_on(relay_t *r, uint8_t note, uint8_t vel)
{
    r->activity |= RELAY_ACT_NOTE;
    r->last_vel = vel;
    if(r->cfg.voices == 1) mono_note_on(r, note, vel);
    else poly_note_on(r, note, vel);
}

static void note_off(relay_t *r, uint8_t note)
{
    r->activity |= RELAY_ACT_NOTE;
    if(r->cfg.voices == 1) mono_note_off(r, note);
    else poly_note_off(r, note);
}

static void control_change(relay_t *r, uint8_t cc, uint8_t val)
{
    r->activity |= RELAY_ACT_CC;
    r->cc[cc & 0x7F] = val & 0x7F;
    switch(cc)
    {
        case 120: /* all sound off */
        case 123: /* all notes off */
            relay_all_notes_off(r);
            break;
        case 121: /* reset all controllers */
            r->bend  = 0;
            r->at    = 0;
            r->cc[1] = 0;
            for(int i = 0; i < RELAY_MAX_VOICES; i++) r->v[i].poly_at = 0;
            break;
        default: break;
    }
}

static void clock_tick(relay_t *r)
{
    r->activity |= RELAY_ACT_CLOCK;
    r->clock_stale = 0;
    uint32_t div   = r->cfg.clock_div;
    uint32_t phase = r->tick % div;
    if(phase == 0)
    {
        r->clock_hi   = true;
        r->clock_left = div == 1 ? RELAY_CLOCK_PULSE_MS : 0;
    }
    else if(div >= 2 && phase == div / 2)
    {
        r->clock_hi = false;
    }
    r->tick++;
}

void relay_event(relay_t *r, uint8_t status, uint8_t d1, uint8_t d2)
{
    r->activity |= RELAY_ACT_ANY;
    if(status < 0x80) return;
    if(status >= 0xF0)
    {
        switch(status)
        {
            case 0xF8: clock_tick(r); break;
            case 0xFA: /* start */
                r->activity |= RELAY_ACT_START;
                r->running    = true;
                r->tick       = 0;
                r->reset_left = r->cfg.reset_ms;
                break;
            case 0xFB: /* continue */
                r->activity |= RELAY_ACT_START;
                r->running = true;
                break;
            case 0xFC: /* stop */
                r->activity |= RELAY_ACT_STOP;
                r->running = false;
                break;
            case 0xF2: /* song position: 14-bit sixteenths, 6 ticks each */
                r->tick = ((uint32_t)(d2 & 0x7F) << 7 | (d1 & 0x7F)) * 6u;
                break;
            case 0xFF: /* reset */
                relay_all_notes_off(r);
                r->running = false;
                break;
            default: break;
        }
        return;
    }

    uint8_t ch = (status & 0x0F) + 1;
    r->last_rx_channel = ch;
    if(r->cfg.channel != 0 && ch != r->cfg.channel)
    {
        r->activity |= RELAY_ACT_OTHER;
        return;
    }
    d1 &= 0x7F;
    d2 &= 0x7F;
    switch(status & 0xF0)
    {
        case 0x80: note_off(r, d1); break;
        case 0x90:
            if(d2 == 0) note_off(r, d1);
            else note_on(r, d1, d2);
            break;
        case 0xA0: /* poly pressure */
            r->activity |= RELAY_ACT_AT;
            for(int i = 0; i < r->cfg.voices; i++)
                if(r->v[i].held && r->v[i].note == d1) r->v[i].poly_at = d2;
            break;
        case 0xB0: control_change(r, d1, d2); break;
        case 0xC0: break; /* program change */
        case 0xD0:
            r->activity |= RELAY_ACT_AT;
            r->at = d1;
            break;
        case 0xE0:
            r->activity |= RELAY_ACT_BEND;
            r->bend = (int16_t)(((int)d2 << 7 | d1) - 8192);
            break;
        default: break;
    }
}

/* ------------------------------------------------------------------------ */
/* parser                                                                    */

static uint8_t data_bytes(uint8_t status)
{
    switch(status & 0xF0)
    {
        case 0xC0:
        case 0xD0: return 1;
        case 0xF0:
            switch(status)
            {
                case 0xF1: return 1; /* MTC quarter frame */
                case 0xF2: return 2; /* song position */
                case 0xF3: return 1; /* song select */
                default: return 0;   /* F6 tune request, F4/F5 undefined */
            }
        default: return 2;
    }
}

void relay_feed(relay_t *r, uint8_t byte)
{
    r->activity |= RELAY_ACT_ANY;
    if(byte >= 0xF8)
    {
        /* Realtime: handled at once, never disturbs anything in flight. */
        relay_event(r, byte, 0, 0);
        return;
    }
    if(byte & 0x80)
    {
        if(byte == 0xF0)
        {
            r->in_sysex = true;
            r->status   = 0;
            return;
        }
        if(byte == 0xF7)
        {
            r->in_sysex = false;
            return;
        }
        r->in_sysex = false;
        if(byte >= 0xF0)
        {
            /* System common clears running status. */
            r->status = 0;
            uint8_t n = data_bytes(byte);
            if(n == 0)
            {
                relay_event(r, byte, 0, 0);
                return;
            }
            r->status = byte;
            r->need   = n;
            r->have   = 0;
            return;
        }
        r->status = byte;
        r->need   = data_bytes(byte);
        r->have   = 0;
        return;
    }
    /* data byte */
    if(r->in_sysex || r->status == 0) return;
    r->data[r->have++] = byte;
    if(r->have < r->need) return;
    uint8_t s = r->status;
    if(s >= 0xF0) r->status = 0; /* system common: one shot */
    r->have = 0;
    relay_event(r, s, r->data[0], r->need > 1 ? r->data[1] : 0);
}

/* ------------------------------------------------------------------------ */
/* time                                                                      */

static inline uint16_t dec16(uint16_t x, uint32_t dt)
{
    return dt >= x ? 0 : (uint16_t)(x - dt);
}

void relay_tick(relay_t *r, uint32_t dt_ms)
{
    if(dt_ms == 0) return;
    for(int i = 0; i < RELAY_MAX_VOICES; i++)
    {
        relay_voice_t *v = &r->v[i];
        v->trig_left     = dec16(v->trig_left, dt_ms);
        v->dip_left      = dec16(v->dip_left, dt_ms);
        if(v->rate != 0.0f)
        {
            float next = v->pitch + v->rate * (float)dt_ms;
            bool  done = v->rate > 0.0f ? next >= v->target : next <= v->target;
            if(done)
            {
                v->pitch = v->target;
                v->rate  = 0.0f;
            }
            else
            {
                v->pitch = next;
            }
        }
    }
    r->reset_left = dec16(r->reset_left, dt_ms);
    if(r->clock_left)
    {
        r->clock_left = dec16(r->clock_left, dt_ms);
        if(r->clock_left == 0) r->clock_hi = false;
    }
    if(r->clock_stale < RELAY_CLOCK_STALE_MS)
    {
        r->clock_stale += dt_ms;
        if(r->clock_stale >= RELAY_CLOCK_STALE_MS) r->clock_hi = false;
    }
}

/* ------------------------------------------------------------------------ */
/* outputs                                                                   */

static inline bool voice_ok(const relay_t *r, int voice)
{
    return voice >= 0 && voice < r->cfg.voices;
}

float relay_pitch_volts(const relay_t *r, int voice)
{
    if(!voice_ok(r, voice)) return 0.0f;
    const relay_voice_t *v = &r->v[voice];
    float semis = v->pitch - (float)r->cfg.base_note;
    semis += (float)r->bend / 8192.0f * (float)r->cfg.bend_range;
    return semis / 12.0f;
}

bool relay_gate(const relay_t *r, int voice)
{
    if(!voice_ok(r, voice)) return false;
    const relay_voice_t *v = &r->v[voice];
    return v->held && v->dip_left == 0;
}

bool relay_trig(const relay_t *r, int voice)
{
    if(!voice_ok(r, voice)) return false;
    const relay_voice_t *v = &r->v[voice];
    return v->trig_left > 0 && v->dip_left == 0;
}

float relay_vel(const relay_t *r, int voice)
{
    if(!voice_ok(r, voice)) return 0.0f;
    return (float)r->v[voice].vel / 127.0f;
}

bool relay_any_gate(const relay_t *r)
{
    for(int i = 0; i < r->cfg.voices; i++)
        if(relay_gate(r, i)) return true;
    return false;
}

int relay_voice_note(const relay_t *r, int voice)
{
    if(!voice_ok(r, voice) || !r->v[voice].held) return -1;
    return r->v[voice].note;
}

float relay_source(const relay_t *r, relay_src_t src, uint8_t cc)
{
    switch(src)
    {
        case RELAY_SRC_VEL: return (float)r->last_vel / 127.0f;
        case RELAY_SRC_MOD: return (float)r->cc[1] / 127.0f;
        case RELAY_SRC_CC: return (float)r->cc[cc & 0x7F] / 127.0f;
        case RELAY_SRC_AT:
        {
            uint8_t a = r->at;
            if(r->last_voice >= 0 && r->last_voice < r->cfg.voices)
            {
                const relay_voice_t *v = &r->v[r->last_voice];
                if(v->held && v->poly_at > a) a = v->poly_at;
            }
            return (float)a / 127.0f;
        }
        case RELAY_SRC_BEND: return (float)r->bend / 8192.0f;
        default: return 0.0f;
    }
}

bool relay_clock(const relay_t *r) { return r->clock_hi; }
bool relay_run(const relay_t *r) { return r->running; }
bool relay_reset(const relay_t *r) { return r->reset_left > 0; }

uint32_t relay_take_activity(relay_t *r)
{
    uint32_t a  = r->activity;
    r->activity = 0;
    return a;
}
