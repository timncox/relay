/*
 * phone_kit.h -- Phone mode and the shared USB port setting for an Alchemy
 * Lab firmware, in one object. Builds on phone_front.h (the labels, the
 * usb.cfg mapping, the phone's button presses, the unit id) and the
 * belt-alchemy transports.
 *
 * Wiring, in main():
 *
 *   hw.Init(...);
 *   led_mirror.Wrap(hw.strip);              // the phone sees the panel
 *   hw.leds.Init(led_mirror, hw.Layout());
 *   ...
 *   sd.Init();
 *   picker::Install(settings, kSettingsFirmware, sd, hw);
 *   front.Install(settings, kSettingsFirmware, 5);   // a free pot
 *   ...
 *   host.Extend(phone_ext).Transport(front.Link());
 *   front.Wire(phone_ext, led_mirror);
 *   ...
 *   presets.Init(); ...                       // settings loaded
 *   front.Start(sd, hw.buttons[kButtonB1], "Tally");   // before audio
 *
 * and once per control frame (OnFrame):
 *
 *   front.Service(sd, settings.IsActive(), now);
 *
 * Panel buttons: wherever the firmware hands hw.buttons[b] to the SDK (a
 * Pager's Cycle / Shift / Latch) or reads it itself, use front.Button(b)
 * instead. It is the panel button with the phone's presses added: a tap is
 * the button down for 80 ms, a hold is down until the phone lets go.
 *
 * Shared by copy between every Alchemy Lab firmware with Phone mode.
 */
#pragma once

#include <stdint.h>

#include "daisy_seed.h"

#include "alchemy/hw/i_button.h"
#include "alchemy/host_link/cdc_transport.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/settings.h"

#include "midi_usb_transport.h"
#include "phone_ext.h"
#include "phone_front.h"
#include "picker.h"
#include "switch_transport.h"
#include "usb_shared.h"

namespace phonefront
{

/* A panel button with the phone's presses ORed in. */
class PanelButton : public alchemy::IButton
{
  public:
    void Bind(alchemy::IButton& hw, Buttons& ph, uint8_t idx)
    {
        hw_  = &hw;
        ph_  = &ph;
        idx_ = idx;
    }

    bool Pressed() const override { return hw_->Pressed() || Virtual(); }

    bool RisingEdge() override
    {
        const bool hw_edge = hw_->RisingEdge();
        const bool v       = Virtual();
        const bool edge    = v && !rise_seen_;
        rise_seen_         = v;
        return hw_edge || edge;
    }

    bool FallingEdge() override
    {
        const bool hw_edge = hw_->FallingEdge();
        const bool v       = Virtual();
        const bool edge    = !v && fall_seen_;
        fall_seen_         = v;
        return hw_edge || edge;
    }

    float TimeHeldMs() const override
    {
        if (hw_->Pressed()) return hw_->TimeHeldMs();
        if (!Virtual()) return 0.0f;
        return (float)(daisy::System::GetNow() - ph_->down_at[idx_]);
    }

  private:
    bool Virtual() const { return ph_->Down(idx_, daisy::System::GetNow()); }

    alchemy::IButton* hw_        = nullptr;
    Buttons*          ph_        = nullptr;
    uint8_t           idx_       = 0;
    bool              rise_seen_ = false;
    bool              fall_seen_ = false;
};

class Front
{
  public:
    /* The USB port selector on a Settings page (Mac / Phone). */
    void Install(alchemy::Settings& settings, uint8_t page, uint8_t pot)
    {
        usb_port_ = settings.Page(page).Pot(pot)
                        .Selector(kUsbLabels).Default(0)
                        .Ident("usb").Name("USB port")
                        .Help(kUsbHelp);
    }

    /* Bind the three panel buttons (before anything takes Button(b)). */
    template <typename Hw>
    void BindButtons(Hw& hw)
    {
        for (uint8_t b = 0; b < 3; b++) buttons_[b].Bind(hw.buttons[b], ph_, b);
    }

    alchemy::IButton&         Button(uint8_t b) { return buttons_[b]; }
    alchemy::hostlink::IHostTransport& Link() { return link_; }
    bool                      Phone() const { return phone_; }

    /* The phone block: the panel's lights, firmware switching, the buttons
     * and the unit id. */
    template <typename Mirror>
    void Wire(phone::PhoneExtension& ext, Mirror& mirror)
    {
        ext.Screen(mirror)
            .Firmware(picker::Remote())
            .Press(&Front::OnPress, this)
            .Unit(UnitId());
    }

    /* Adopt the card's setting (one for every firmware), then open the
     * front port as Mac or Phone. B1 held through power-up is Mac mode for
     * this boot. Call after the presets have loaded, before audio. */
    void Start(alchemy::SdCard& sd, alchemy::IButton& b1, const char* name)
    {
        const int shared = FromShared(usbshared::Load(sd, 500));
        if (shared >= 0 && shared != (int)usb_port_.Value())
            usb_port_.Default((uint8_t)shared);
        const bool force_mac = b1.Pressed();
        phone_               = (int)usb_port_.Value() == 1 && !force_mac;
        if (phone_)
        {
            midi_.Init(usb_, daisy::UsbHandle::FS_EXTERNAL, "Alchemy Lab");
            link_.Use(midi_);
        }
        else
        {
            cdc_.Init(usb_, daisy::UsbHandle::FS_EXTERNAL, name);
            link_.Use(cdc_);
        }
        written_ = (int)usb_port_.Value();
    }

    /* Every control frame: step a firmware switch the phone started, and
     * write a changed USB port back to the card (only a change, so a
     * Launchpad or Phone+Audio card keeps its value through a visit here).
     * Returns true when it wrote the card. */
    bool Service(alchemy::SdCard& sd, bool settings_open, uint32_t now)
    {
        picker::Service(now);
        if (settings_open) ph_.Release();
        const int sel = (int)usb_port_.Value();
        if (!settings_open && sel != written_ && !picker::Busy()
            && usbshared::Save(sd, ToShared(sel), now))
        {
            written_ = sel;
            return true;
        }
        return false;
    }

  private:
    static bool OnPress(uint8_t button, uint8_t gesture, void* ctx)
    {
        return static_cast<Front*>(ctx)->ph_.Press(button, gesture,
                                                    daisy::System::GetNow());
    }

    alchemy::SelectorHandle          usb_port_;
    Buttons                          ph_;
    PanelButton                      buttons_[3];
    phone::SwitchTransport           link_;
    daisy::UsbHandle                 usb_;
    alchemy::hostlink::CdcUsbTransport        cdc_;
    alchemy::hostlink::MidiUsbHostTransport   midi_;
    bool                             phone_   = false;
    int                              written_ = -2;
};

} // namespace phonefront
