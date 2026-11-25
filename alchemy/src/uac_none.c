/*
 * uac_none.c -- the two usb_audio.c entry points midi_usb_transport.cpp
 * links against, for a firmware with no Phone+Audio mode (Kerf, Pulse).
 * MidiUsbHostTransport only calls them after InitOnAudio(), which these
 * firmwares never do; the plain Phone mode runs its own USB MIDI device.
 *
 * Shared by copy between Kerf and Pulse.
 */
#include <stdint.h>

#include "usb_audio.h"

void UAC_MidiSetRx(UAC_MidiRxFn fn) { (void)fn; }

uint8_t UAC_MidiTransmit(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
    return 0u;
}
