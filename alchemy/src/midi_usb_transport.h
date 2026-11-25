/**
 * @file midi_usb_transport.h
 * @brief HostLink over class-compliant USB MIDI (target-only).
 *
 * A drop-in IHostTransport next to the SDK's CdcUsbTransport: hand it to
 * Host::Transport() and HostLink runs unchanged, but the front USB-C
 * enumerates as a USB MIDI device ("Alchemy Lab") instead of a serial
 * port.  iPhones and iPads (USB-C, or Lightning with the camera adapter),
 * Macs and Chrome's WebMIDI all open that without a driver.  The bytes
 * travel inside SysEx, see hlmidi.h.
 *
 * Uses libDaisy's own MIDI device mode (usbd_mode = USBD_MODE_MIDI, the
 * same switch daisy::MidiUsbTransport flips), but drives the endpoint
 * itself, with the same ping-pong transmit as CdcUsbTransport: libDaisy's
 * MidiUsbTransport reuses its transmit buffer while the previous transfer
 * may still be in flight, which is fine for notes and not for a stream.
 *
 * Receive: the USB interrupt hands us USB-MIDI packets; hlmidi::Decoder
 * turns ours into HostLink bytes in a lock-free ring (IRQ writes, Poll
 * reads).  Overflow drops bytes; COBS + CRC make that a host retry.
 *
 * Phone+Audio mode: InitOnAudio() instead of Init() runs the same rings,
 * decoder and ping-pong transmit over the MIDI interface of usb_audio.c's
 * composite device (UAC_StartWithMidi, usb_phone_audio_desc.h) rather than
 * libDaisy's MIDI device, so HostLink reaches the same handlers.
 *
 * One instance per system (the receive callback has no context pointer),
 * and not at the same time as a CdcUsbTransport on the same port: the
 * port is either serial or MIDI from power-up.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "alchemy/host_link/transport.h"
#include "alchemy/host_link/wire.h"
#include "hid/usb.h"
#include "hlmidi.h"

namespace alchemy {
namespace hostlink {

namespace detail {
constexpr size_t Pow2(size_t v)
{
    size_t p = 1u;
    while (p < v) p <<= 1u;
    return p;
}
} // namespace detail

class MidiUsbHostTransport : public IHostTransport
{
  public:
    MidiUsbHostTransport() = default;

    /**
     * Switches libDaisy to MIDI descriptors and starts the device on
     * @p periph: FS_EXTERNAL for the Alchemy Lab V2's front USB-C,
     * FS_INTERNAL on V1 (see cdc_transport.h).  @p product_name is the
     * USB product string, which iOS and macOS show as the MIDI device's
     * name; pass a string literal.
     */
    void Init(daisy::UsbHandle& usb,
              daisy::UsbHandle::UsbPeriph periph,
              const char* product_name = nullptr);

    /**
     * Phone+Audio: HostLink over the USB MIDI interface of the composite
     * audio device (usb_audio.h UAC_StartWithMidi). Call before
     * UAC_StartWithMidi, so no packet arrives without a receiver.
     */
    void InitOnAudio();

    size_t Read(uint8_t* dst, size_t max) override;
    size_t Write(const uint8_t* src, size_t n) override;
    size_t WriteSpace() const override;
    void   Pump() override;

    uint32_t RxDropped() const { return rx_dropped_; }
    uint32_t RxMessages() const { return decoder_.Messages(); }

  private:
    static void RxTrampoline(uint8_t* buf, uint32_t* len);
    static void RxTrampolineAudio(const uint8_t* buf, uint32_t len);
    bool        Transmit(uint8_t* buf, size_t n);
    void        OnRx(const uint8_t* buf, uint32_t len);

    static constexpr size_t kRxRing = detail::Pow2(kMaxWire * 3u);
    static constexpr size_t kTxRing = detail::Pow2(kMaxWire * 3u);
    /* One SysEx message per USB transfer: 140 raw bytes → 224 USB bytes. */
    static constexpr size_t kChunk = hlmidi::UsbLen(hlmidi::kMaxRawPerMessage);

    daisy::UsbHandle*           usb_    = nullptr;
    daisy::UsbHandle::UsbPeriph periph_ = daisy::UsbHandle::FS_INTERNAL;
    bool                        on_audio_ = false;   /* Phone+Audio */
    hlmidi::Decoder             decoder_;

    uint8_t           rx_[kRxRing];
    volatile uint16_t rx_w_       = 0u;
    volatile uint16_t rx_r_       = 0u;
    uint32_t          rx_dropped_ = 0u;

    uint8_t  tx_[kTxRing];
    uint16_t tx_w_ = 0u;
    uint16_t tx_r_ = 0u;

    alignas(4) uint8_t chunk_[2][kChunk];
    uint8_t  chunk_sel_   = 0u;
    uint16_t pending_len_ = 0u;

    static MidiUsbHostTransport* s_instance;
};

} // namespace hostlink
} // namespace alchemy
