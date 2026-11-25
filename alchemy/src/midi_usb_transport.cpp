/**
 * @file midi_usb_transport.cpp
 * @brief HostLink over USB MIDI (target-only).  See midi_usb_transport.h.
 */

#include "midi_usb_transport.h"

#include "usbd_def.h"     /* USBD_DescriptorsTypeDef               */
#include "usbd_desc.h"    /* FS_Desc / HS_Desc descriptor tables   */
#include "usbd_ctlreq.h"  /* USBD_GetString                        */
#include "usb_audio.h"    /* Phone+Audio: UAC_MidiSetRx / Transmit */

/* libDaisy's descriptor switch (src/hid/usb.cpp); daisy::MidiUsbTransport
 * sets it the same way before UsbHandle::Init. */
extern "C" uint8_t usbd_mode;
/* USBD_MODE_MIDI in libDaisy's patched usbd_cdc.h, which is not on an app's
 * include path. */
constexpr uint8_t kUsbdModeMidi = 1u;

namespace alchemy {
namespace hostlink {

namespace {

/* Same product-string override as cdc_transport.cpp: the ST core reads the
 * product string through a function pointer, so repointing it before
 * USBD_Init renames the device without touching libDaisy. */
const char* s_product_name = nullptr;
uint8_t     s_product_desc[2u + 2u * 48u];

uint8_t* ProductStrOverride(USBD_SpeedTypeDef /*speed*/, uint16_t* length)
{
    USBD_GetString(reinterpret_cast<uint8_t*>(const_cast<char*>(s_product_name)),
                   s_product_desc, length);
    return s_product_desc;
}

} // namespace

MidiUsbHostTransport* MidiUsbHostTransport::s_instance = nullptr;

void MidiUsbHostTransport::Init(daisy::UsbHandle& usb,
                                daisy::UsbHandle::UsbPeriph periph,
                                const char* product_name)
{
    usb_       = &usb;
    periph_    = periph;
    s_instance = this;

    if (product_name != nullptr)
    {
        s_product_name = product_name;
        USBD_DescriptorsTypeDef& desc =
            (periph == daisy::UsbHandle::FS_INTERNAL) ? FS_Desc : HS_Desc;
        desc.GetProductStrDescriptor = &ProductStrOverride;
    }

    usbd_mode = kUsbdModeMidi;
    usb.Init(periph);
    usb.SetReceiveCallback(&MidiUsbHostTransport::RxTrampoline, periph);
}

void MidiUsbHostTransport::InitOnAudio()
{
    usb_       = nullptr;
    on_audio_  = true;
    s_instance = this;
    UAC_MidiSetRx(&MidiUsbHostTransport::RxTrampolineAudio);
}

void MidiUsbHostTransport::RxTrampoline(uint8_t* buf, uint32_t* len)
{
    if (s_instance && buf && len) s_instance->OnRx(buf, *len);
}

void MidiUsbHostTransport::RxTrampolineAudio(const uint8_t* buf, uint32_t len)
{
    if (s_instance && buf) s_instance->OnRx(buf, len);
}

bool MidiUsbHostTransport::Transmit(uint8_t* buf, size_t n)
{
    if (on_audio_) return UAC_MidiTransmit(buf, static_cast<uint16_t>(n)) != 0u;
    return ((periph_ == daisy::UsbHandle::FS_INTERNAL) ? usb_->TransmitInternal(buf, n)
                                                       : usb_->TransmitExternal(buf, n))
           == daisy::UsbHandle::Result::OK;
}

void MidiUsbHostTransport::OnRx(const uint8_t* buf, uint32_t len)
{
    /* IRQ context, single producer.  On overflow, count and drop. */
    decoder_.FeedUsb(buf, len, [this](uint8_t b) {
        const uint16_t next = static_cast<uint16_t>((rx_w_ + 1u) & (kRxRing - 1u));
        if (next == rx_r_)
        {
            rx_dropped_++;
            return;
        }
        rx_[rx_w_] = b;
        rx_w_      = next;
    });
}

size_t MidiUsbHostTransport::Read(uint8_t* dst, size_t max)
{
    size_t         n = 0u;
    const uint16_t w = rx_w_;
    while (rx_r_ != w && n < max)
    {
        dst[n++] = rx_[rx_r_];
        rx_r_    = static_cast<uint16_t>((rx_r_ + 1u) & (kRxRing - 1u));
    }
    return n;
}

size_t MidiUsbHostTransport::WriteSpace() const
{
    const uint16_t used = static_cast<uint16_t>((tx_w_ - tx_r_) & (kTxRing - 1u));
    return kTxRing - 1u - used;
}

size_t MidiUsbHostTransport::Write(const uint8_t* src, size_t n)
{
    size_t accepted = 0u;
    while (accepted < n)
    {
        const uint16_t next = static_cast<uint16_t>((tx_w_ + 1u) & (kTxRing - 1u));
        if (next == tx_r_) break;
        tx_[tx_w_] = src[accepted++];
        tx_w_      = next;
    }
    return accepted;
}

void MidiUsbHostTransport::Pump()
{
    if (!usb_ && !on_audio_) return;

    /* A prepared chunk the endpoint refused last time goes first: the
     * other buffer may still be in flight until this one is accepted. */
    if (pending_len_ > 0u)
    {
        if (!Transmit(chunk_[chunk_sel_], pending_len_)) return;
        chunk_sel_ ^= 1u;
        pending_len_ = 0u;
    }

    if (tx_w_ == tx_r_) return;

    /* Up to one SysEx message's worth of the stream, wrapped. */
    uint8_t raw[hlmidi::kMaxRawPerMessage];
    size_t  n = 0u;
    while (tx_r_ != tx_w_ && n < sizeof raw)
    {
        raw[n++] = tx_[tx_r_];
        tx_r_    = static_cast<uint16_t>((tx_r_ + 1u) & (kTxRing - 1u));
    }
    pending_len_ = static_cast<uint16_t>(hlmidi::EncodeUsb(raw, n, chunk_[chunk_sel_], kChunk));

    if (pending_len_ > 0u && Transmit(chunk_[chunk_sel_], pending_len_))
    {
        chunk_sel_ ^= 1u;
        pending_len_ = 0u;
    }
}

} // namespace hostlink
} // namespace alchemy
