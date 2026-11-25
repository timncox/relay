/**
 * @file switch_transport.h
 * @brief An IHostTransport that forwards to whichever transport the
 *        firmware picks at boot.
 *
 * Host::Transport() must be set before presets.BootLoad(), but Tim's
 * firmwares only know the USB port mode (Mac / Launchpad / Audio / Phone)
 * after BootLoad() restored the settings and the card's /alchemy/usb.cfg
 * was read.  Give the Host this, then Use() the CDC or the MIDI transport
 * once the mode is known.  Until then, and in modes with no HostLink, it
 * reads nothing and swallows writes.
 */

#pragma once

#include "alchemy/host_link/transport.h"

namespace phone {

class SwitchTransport : public alchemy::hostlink::IHostTransport
{
  public:
    void Use(alchemy::hostlink::IHostTransport& t) { t_ = &t; }

    size_t Read(uint8_t* dst, size_t max) override { return t_ ? t_->Read(dst, max) : 0u; }
    size_t Write(const uint8_t* src, size_t n) override { return t_ ? t_->Write(src, n) : n; }
    size_t WriteSpace() const override { return t_ ? t_->WriteSpace() : 4096u; }
    void   Pump() override { if (t_) t_->Pump(); }
    bool   Connected() const override { return t_ ? t_->Connected() : false; }

  private:
    alchemy::hostlink::IHostTransport* t_ = nullptr;
};

} // namespace phone
