/*
 * phone_front.h -- the front USB-C for an Alchemy Lab firmware that isn't
 * Belt: Mac (HostLink over USB serial, the web programmer) or Phone
 * (HostLink over class-compliant USB MIDI, the iPhone app).
 *
 * The transports, the phone block (phone_ext.h) and the card's shared
 * /alchemy/usb.cfg come from a belt-alchemy checkout (src/). This file adds
 * the small glue each firmware would otherwise repeat:
 *
 *   - the USB port selector's two choices, and the mapping to and from the
 *     card's value (Belt's five: Mac 0, Launchpad 1, Audio 2, Phone 3,
 *     Phone+Audio 4). Phone and Phone+Audio both mean Phone here; anything
 *     else means Mac. The card is only written when this firmware's own
 *     selector is changed, so a Launchpad or Phone+Audio card survives a
 *     visit to this firmware.
 *   - the panel buttons pressed from the phone (PRESS, 0x75): a tap is the
 *     button down for 80 ms, so each firmware's own tap logic sees it.
 *   - the unit id that tells two modules apart on the phone.
 *
 * Shared by copy between Kerf (comb-filter/alchemy/src), Pulse
 * (belt-alchemy pulse/src) and every firmware that uses phone_kit.h.
 */
#pragma once

#include <stdint.h>

namespace phonefront
{

/* The selector's labels: index 0 Mac, 1 Phone. */
inline const char* const kUsbLabels[2] = {"Mac", "Phone"};

inline constexpr const char* kUsbHelp =
    "**Mac** (default): the front USB-C is HostLink over USB serial, for the "
    "web programmer, presets and the card. **Phone**: the front USB-C is USB "
    "MIDI, for the iPhone app (the panel's lights, the knobs, the buttons, "
    "the card and firmware switching). One setting for every firmware on "
    "the card. From the next power-up; hold B1 while powering up for Mac "
    "mode once.";

/* usb.cfg -> selector index, or -1 for no file (keep this firmware's own). */
inline int FromShared(int shared)
{
    if (shared < 0) return -1;
    return (shared == 3 || shared == 4) ? 1 : 0;
}

/* selector index -> usb.cfg value. */
inline int ToShared(int sel) { return sel == 1 ? 3 : 0; }

/* Panel buttons B1-B3 as the phone presses them. */
struct Buttons
{
    static constexpr uint32_t kTapMs = 80u;

    bool     held[3]      = {false, false, false};
    uint32_t tap_until[3] = {0u, 0u, 0u};
    uint32_t down_at[3]   = {0u, 0u, 0u};   /* when the press began */

    bool Down(int b, uint32_t now) const
    {
        return held[b] || (int32_t)(tap_until[b] - now) > 0;
    }

    /* gesture 0 tap, 1 down, 2 up (phone_ext.h PRESS). */
    bool Press(uint8_t button, uint8_t gesture, uint32_t now)
    {
        if (button > 2u || gesture > 2u) return false;
        if (gesture != 2u && !Down(button, now)) down_at[button] = now;
        if (gesture == 0u) tap_until[button] = now + kTapMs;
        else               held[button] = (gesture == 1u);
        return true;
    }

    /* Drop everything (Settings opened, say). */
    void Release()
    {
        for (int b = 0; b < 3; b++)
        {
            held[b]      = false;
            tap_until[b] = 0u;
        }
    }
};

/* The STM32H7's 96-bit unique ID. */
#ifdef UID_BASE
inline const volatile uint32_t* const kUid = (const volatile uint32_t*)UID_BASE;
#else
inline const volatile uint32_t* const kUid = (const volatile uint32_t*)0x1FF1E800UL;
#endif

/* FNV-1a of the MCU's 96-bit unique ID (as Belt): two modules on one phone
 * get different names. Never 0 (0 = unknown). */
inline uint32_t UnitId(const volatile uint32_t* uid = kUid)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 3; i++)
    {
        const uint32_t w = uid[i];
        for (int b = 0; b < 4; b++)
        {
            h ^= (w >> (8 * b)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h ? h : 1u;
}

} // namespace phonefront
