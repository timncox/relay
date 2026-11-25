/**
 * @file hlmidi.h
 * @brief HostLink over USB MIDI SysEx: the byte pipe, with no hardware.
 *
 * iPhones can't open a USB serial port, but they speak class-compliant USB
 * MIDI to anything plugged into their USB-C port.  This file carries the
 * HostLink byte stream (COBS frames, 0x00 delimiters; the Alchemy SDK's
 * docs/hostlink-protocol.md) inside SysEx messages, so the same HostLink
 * firmware code and the same host logic run over MIDI unchanged.
 *
 *   F0 7D 48 4C 01  <packed data ...>  F7
 *   |  |  'H''L' version
 *   |  non-commercial manufacturer id
 *
 * Packed data is the raw bytes 7 at a time: one byte holding the top bits
 * (bit i = top bit of data byte i), then up to 7 data bytes with their top
 * bit cleared.  A message holds at most kMaxRawPerMessage raw bytes; the
 * stream is just the concatenation of every message's raw bytes, in order.
 * HostLink's own COBS framing and CRC32 sit on top, so a lost or damaged
 * message costs one HostLink retry, never a wrong value.
 *
 * Header-only and hardware-free: the firmware transport
 * (midi_usb_transport.h) and the host tests both use it.  The iPhone app
 * has a byte-for-byte Swift port (ios/.../SysExPipe.swift) checked against
 * the same golden vectors (tests/golden/hlmidi_golden.json).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hlmidi {

constexpr uint8_t kSysexStart   = 0xF0;
constexpr uint8_t kSysexEnd     = 0xF7;
constexpr uint8_t kManufacturer = 0x7D;   /* non-commercial / educational */
constexpr uint8_t kTag0         = 0x48;   /* 'H' */
constexpr uint8_t kTag1         = 0x4C;   /* 'L' */
constexpr uint8_t kVersion      = 0x01;
constexpr size_t  kHeaderLen    = 5u;     /* F0 7D 48 4C 01 */

/** Raw bytes per SysEx message: 20 groups of 7, so 160 packed bytes and a
 *  166-byte message.  Small enough for CoreMIDI and libDaisy's buffers. */
constexpr size_t kMaxRawPerMessage = 140u;

constexpr size_t PackedLen(size_t raw) { return raw + (raw + 6u) / 7u; }
constexpr size_t MessageLen(size_t raw) { return kHeaderLen + PackedLen(raw) + 1u; }

/** USB-MIDI 1.0 event packets (4 bytes each) a message of @p raw bytes
 *  needs: SysEx goes 3 bytes per packet. */
constexpr size_t UsbLen(size_t raw) { return 4u * ((MessageLen(raw) + 2u) / 3u); }

/** 8-bit → 7-bit.  Returns bytes written to @p dst (PackedLen(n)). */
inline size_t Pack7(const uint8_t* src, size_t n, uint8_t* dst)
{
    size_t o = 0u;
    for (size_t i = 0u; i < n; i += 7u)
    {
        const size_t g   = (n - i < 7u) ? n - i : 7u;
        uint8_t      top = 0u;
        for (size_t k = 0u; k < g; k++)
            top |= static_cast<uint8_t>(((src[i + k] >> 7) & 1u) << k);
        dst[o++] = top;
        for (size_t k = 0u; k < g; k++)
            dst[o++] = static_cast<uint8_t>(src[i + k] & 0x7Fu);
    }
    return o;
}

/** A whole SysEx message (F0 … F7) for up to kMaxRawPerMessage raw bytes.
 *  Returns its length, or 0 if @p n is too big or @p cap too small. */
inline size_t EncodeMessage(const uint8_t* raw, size_t n, uint8_t* dst, size_t cap)
{
    if (n > kMaxRawPerMessage || cap < MessageLen(n)) return 0u;
    dst[0] = kSysexStart;
    dst[1] = kManufacturer;
    dst[2] = kTag0;
    dst[3] = kTag1;
    dst[4] = kVersion;
    size_t o = kHeaderLen + Pack7(raw, n, dst + kHeaderLen);
    dst[o++] = kSysexEnd;
    return o;
}

/** A SysEx message as USB-MIDI 1.0 event packets on cable 0: CIN 0x4 for
 *  three bytes that continue it, then CIN 0x5/0x6/0x7 for the packet that
 *  ends it with one, two or three bytes.  Returns bytes written (a
 *  multiple of 4), or 0 if @p cap is too small. */
inline size_t SysexToUsb(const uint8_t* msg, size_t len, uint8_t* dst, size_t cap)
{
    const size_t need = 4u * ((len + 2u) / 3u);
    if (len < 2u || cap < need) return 0u;
    size_t i = 0u, o = 0u;
    while (len - i > 3u)
    {
        dst[o++] = 0x04u;
        dst[o++] = msg[i++];
        dst[o++] = msg[i++];
        dst[o++] = msg[i++];
    }
    const size_t last = len - i;   /* 1..3, the last holds F7 */
    dst[o++] = static_cast<uint8_t>(0x04u + last);
    for (size_t k = 0u; k < 3u; k++)
        dst[o++] = (k < last) ? msg[i + k] : 0u;
    return o;
}

/** EncodeMessage + SysexToUsb in one step. */
inline size_t EncodeUsb(const uint8_t* raw, size_t n, uint8_t* dst, size_t cap)
{
    uint8_t msg[MessageLen(kMaxRawPerMessage)];
    const size_t len = EncodeMessage(raw, n, msg, sizeof msg);
    return len ? SysexToUsb(msg, len, dst, cap) : 0u;
}

/**
 * Streaming decoder: MIDI bytes (or USB-MIDI packets) in, HostLink bytes
 * out through @p Sink (any callable taking uint8_t).  Ignores everything
 * that isn't one of our messages: notes, clock, other SysEx.  Real-time
 * bytes (F8–FF) inside a message are skipped, as MIDI allows; any other
 * status byte abandons the message.  No buffering: each raw byte comes
 * out as soon as its packed byte arrives, so it is safe to run in the USB
 * receive interrupt.
 */
class Decoder
{
  public:
    template <typename Sink>
    void FeedByte(uint8_t b, Sink&& sink)
    {
        if (b >= 0xF8u) return;                 /* real-time: transparent */
        if (b == kSysexStart)
        {
            state_ = kMfrWait;
            pos_   = 0u;
            return;
        }
        if (b & 0x80u)                          /* F7 or any other status */
        {
            if (state_ == kData && b == kSysexEnd) messages_++;
            state_ = kIdle;
            return;
        }
        switch (state_)
        {
            case kIdle: return;
            case kMfrWait: state_ = (b == kManufacturer) ? kTagHWait : kIdle; return;
            case kTagHWait: state_ = (b == kTag0) ? kTagLWait : kIdle; return;
            case kTagLWait: state_ = (b == kTag1) ? kVersionWait : kIdle; return;
            case kVersionWait: state_ = (b == kVersion) ? kData : kIdle; pos_ = 0u; return;
            case kData:
                if (pos_ == 0u) top_ = b;
                else sink(static_cast<uint8_t>(b | (((top_ >> (pos_ - 1u)) & 1u) << 7)));
                pos_ = static_cast<uint8_t>((pos_ + 1u) & 7u);
                return;
        }
    }

    /** USB-MIDI 1.0 event packets (any multiple of 4 bytes; a short tail is
     *  dropped).  Uses the code index number for how many bytes count. */
    template <typename Sink>
    void FeedUsb(const uint8_t* buf, size_t len, Sink&& sink)
    {
        static constexpr uint8_t kCinSize[16] = {0, 0, 2, 3, 3, 1, 2, 3,
                                                 3, 3, 3, 3, 2, 2, 3, 1};
        for (size_t i = 0u; i + 4u <= len; i += 4u)
        {
            const uint8_t n = kCinSize[buf[i] & 0x0Fu];
            for (uint8_t k = 0u; k < n; k++) FeedByte(buf[i + 1u + k], sink);
        }
    }

    /** Complete messages of ours seen so far (diagnostic). */
    uint32_t Messages() const { return messages_; }

  private:
    enum State : uint8_t { kIdle, kMfrWait, kTagHWait, kTagLWait, kVersionWait, kData };
    State    state_    = kIdle;
    uint8_t  pos_      = 0u;    /* 0 = next byte is a top-bits byte */
    uint8_t  top_      = 0u;
    uint32_t messages_ = 0u;
};

} // namespace hlmidi
