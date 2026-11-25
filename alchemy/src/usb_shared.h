/*
 * usb_shared -- the front USB-C's role (Mac / Launchpad / Audio / Phone /
 * Phone+Audio) as ONE setting for every firmware on the card, kept in
 * /alchemy/usb.cfg ("mac", "launchpad", "audio", "phone" or "phone+audio").
 * Each firmware still has its own USB port
 * selector; at boot it adopts the card's value, and a change in any
 * firmware's Settings is written back here, so switching firmware in the
 * picker keeps the mode.
 *
 * Shared by copy with smack-alchemy, belt-alchemy and seq-alchemy.
 */
#pragma once
#include <stdint.h>

#include "alchemy/storage/sd_card.h"

namespace usbshared
{

/* -1 = no file (or unreadable), 0 = Mac, 1 = Launchpad, 2 = Audio (the
 * Lab as a USB audio interface, usb_audio.h), 3 = Phone (USB MIDI carrying
 * HostLink, for the iPhone app), 4 = Phone+Audio (one composite device:
 * the Audio interface plus Phone's MIDI, usb_phone_audio_desc.h). A firmware
 * without an Audio choice treats 2 as Mac; an older copy of this file reads
 * "phone" as -1 and keeps its own setting. "phone+audio" starts with 'p' on
 * purpose: a copy that knows Phone but not Phone+Audio reads it as 3, Phone
 * (the app still reaches that firmware, without the audio), and one older
 * still reads -1. Neither writes the file back unless its own setting is
 * changed, so the card keeps "phone+audio". A firmware with a 4-zone
 * selector must not adopt 4 (Belt's has 5). Waits up to
 * timeout_ms for the card to mount. Call from main(), before the loop. */
int Load(alchemy::SdCard& sd, uint32_t timeout_ms);

/* Write the mode. Returns false if the card is not there. */
bool Save(alchemy::SdCard& sd, int mode, uint32_t now_ms);

} // namespace usbshared
