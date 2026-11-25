/**
 * @file phone_ext.h
 * @brief HostLink "phone block" (commands 0x70–0x77): what the iPhone app
 *        needs beyond knobs, presets and files.
 *
 * Knobs, settings and presets already travel as HostLink live state and
 * slots (§3), and samples as the filesystem block (§8, 0x50–0x5F).  This
 * block adds the rest, each piece optional:
 *
 *   0x70 PHONE_INFO   → u8 status, u8 version (2), u8 flags, u16 led_count,
 *                       str layout ("alchemy-lab-v2"), u32 unit
 *                       flags: bit0 SCREEN, bit1 firmware switching, bit2 PRESS,
 *                       bit3 STATE, bit4 CTRL.  unit tells two modules apart (a hash of
 *                       the MCU's unique ID; 0 = unknown); version 1 ends
 *                       after the layout
 *   0x71 SCREEN       → u8 status, u32 frame, u16 n, u8 rgb[3n]
 *                       the LED frame last shown on the panel (led_mirror.h)
 *   0x72 FW_LIST      → u8 status, u8 count, { str name, u32 size } × count
 *                       the .bin files in /alchemy on the card (FS_NO_CARD
 *                       without one); upload new ones with FS_* first
 *   0x73 FW_FLASH     ← str name   → u8 status
 *                       starts the picker's compare / erase / write / verify
 *                       on that file; the module reboots into it when done
 *   0x74 FW_STATUS    → u8 status, u8 state, u8 progress (0–255), str error
 *                       state: 0 idle, 1 comparing, 2 erasing, 3 writing,
 *                       4 verifying, 5 done (rebooting), 6 same image, 7 error
 *   0x75 PRESS        ← u8 button, u8 gesture (0 tap, 1 down, 2 up) → u8 status
 *                       a panel button, for firmwares that wire it up
 *   0x76 STATE        ← [u16 since] → u8 status, u16 frame, u8 page, u8 flags,
 *                       u8 touch_page, u8 touch_pot, u16 touch_age_ms,
 *                       u8 cpu, u8 in_level, u8 out_level, then the
 *                       firmware's own tail (u8 tag, u8 version, …)
 *                       what the phone's own screen shows beyond the knob
 *                       values: which page is up, which pot a hand last
 *                       moved and how long ago, load and levels.  flags:
 *                       bit0 Settings open, bit1 firmware switch running.
 *                       Levels are peaks since the last read, 0 = −60 dBFS
 *                       or below, 255 = 0 dBFS.  touch_* are 0xFF for none.
 *                       since is the last trace sample the phone already
 *                       has, for a tail that streams one (Belt's pitch).
 *   0x77 CTRL         ← u8 present, u8 n, { u8 type, u8 a, u8 b, u8 c } × n
 *                     → u8 status, u8 version (1), u8 present_seen,
 *                       u8 lp_leds[81], u8 xl_leds[40]
 *                       controllers on the phone's hub (Launchpad Mini MK3,
 *                       Launch Control XL, an XInput gamepad) driving the
 *                       module as in Launchpad USB mode; the reply is the
 *                       full LED frame for the phone to show on them.
 *                       present: bit0 Launchpad, bit1 XL, bit2 gamepad,
 *                       bit3 keyboard (notes, sustain); n = 0 polls.
 *                       See CONTROLLERS.md.
 *
 * Unknown or unwired commands answer UNSUPPORTED, so the app shows only
 * what this firmware can do.  Hardware-free: the firmware passes in its LED
 * mirror and a firmware switcher (the picker: picker::Remote() in Belt).
 */

#pragma once

#include <cstdint>

#include "alchemy/host_link/extension.h"
#include "alchemy/host_link/wire.h"
#include "led_mirror.h"

namespace phone {

enum Cmd : uint8_t
{
    kInfo     = 0x70,
    kScreen   = 0x71,
    kFwList   = 0x72,
    kFwFlash  = 0x73,
    kFwStatus = 0x74,
    kPress    = 0x75,
    kState    = 0x76,
    kCtrl     = 0x77,
    kLast     = 0x7F,   /* block reserved through here */
};

enum FwState : uint8_t
{
    kFwIdle = 0, kFwComparing, kFwErasing, kFwWriting, kFwVerifying, kFwDone, kFwSame, kFwError,
};

/** Firmware switching from the card, implemented by the picker. */
class IFirmwareSwitcher
{
  public:
    virtual ~IFirmwareSwitcher() = default;
    /** Re-list /alchemy.  Returns the file count, or a negative
     *  hostlink::Status (FS_NO_CARD) when there is no card. */
    virtual int         List(uint32_t now_ms)      = 0;
    virtual const char* Name(int i) const          = 0;
    virtual uint32_t    Size(int i) const          = 0;
    /** Start flashing @p name (as listed).  Returns a hostlink::Status. */
    virtual uint8_t     Start(const char* name, uint32_t now_ms) = 0;
    virtual FwState     State() const              = 0;
    virtual float       Progress() const           = 0;   /* 0..1 */
    virtual const char* Error() const              = 0;
};

using PressFn = bool (*)(uint8_t button, uint8_t gesture, void* ctx);

/** Controllers forwarded by the phone (CTRL), implemented by launchpad.h's
 *  remote backend (ctrl_link.h).  Called from the HostLink service. */
class IControllers
{
  public:
    static constexpr uint8_t kVersion = 1u;
    static constexpr int     kLpLeds  = 81;
    static constexpr int     kXlLeds  = 40;

    virtual ~IControllers() = default;
    /** One request, already length-checked: @p events holds 4 × @p n bytes. */
    virtual void           Apply(uint8_t present, const uint8_t* events, uint8_t n,
                                 uint32_t now_ms)  = 0;
    virtual uint8_t        Present() const = 0;   /* present_seen */
    virtual const uint8_t* LpLeds() const  = 0;   /* kLpLeds */
    virtual const uint8_t* XlLeds() const  = 0;   /* kXlLeds */
};

/** The generic part of a STATE reply; see the table above. */
struct StateHeader
{
    uint16_t frame        = 0;
    uint8_t  page         = 0;
    uint8_t  flags        = 0;
    uint8_t  touch_page   = 0xFFu;
    uint8_t  touch_pot    = 0xFFu;
    uint16_t touch_age_ms = 0xFFFFu;
    uint8_t  cpu          = 0;
    uint8_t  in_level     = 0;
    uint8_t  out_level    = 0;
};

/** What a firmware tells the phone's screen, called from the HostLink
 *  service (the main loop, never the audio callback). */
class IStateSource
{
  public:
    virtual ~IStateSource() = default;
    virtual void Header(StateHeader& h, uint32_t now_ms) = 0;
    /** Its own fields after the header: u8 tag, u8 version, then anything
     *  that fits the reply.  @p since is the phone's last trace sample. */
    virtual void Tail(alchemy::hostlink::FrameWriter& w, uint16_t since, uint32_t now_ms) = 0;
};

/** A level byte for STATE from a linear peak (1.0 = full scale). */
uint8_t LevelByte(float peak);

class PhoneExtension : public alchemy::hostlink::IHostlinkExtension
{
  public:
    static constexpr uint8_t kVersion = 2u;

    PhoneExtension& Screen(const ILedSource& leds) { leds_ = &leds; return *this; }
    PhoneExtension& Firmware(IFirmwareSwitcher& fw) { fw_ = &fw; return *this; }
    PhoneExtension& Press(PressFn fn, void* ctx) { press_ = fn; press_ctx_ = ctx; return *this; }
    PhoneExtension& Layout(const char* name) { layout_ = name; return *this; }
    PhoneExtension& State(IStateSource& s) { state_ = &s; return *this; }
    PhoneExtension& Unit(uint32_t id) { unit_ = id; return *this; }
    PhoneExtension& Controllers(IControllers& c) { ctrl_ = &c; return *this; }

    uint8_t FirstCmd() const override { return kInfo; }
    uint8_t LastCmd() const override { return kLast; }
    void    Handle(const alchemy::hostlink::ParsedFrame& f,
                   alchemy::hostlink::FrameWriter& w, uint32_t now_ms) override;
    const char* DescriptorRootJson() const override { return "\"phone\":{\"v\":2}"; }

  private:
    const ILedSource*  leds_      = nullptr;
    IFirmwareSwitcher* fw_        = nullptr;
    PressFn            press_     = nullptr;
    void*              press_ctx_ = nullptr;
    const char*        layout_    = "alchemy-lab-v2";
    IStateSource*      state_     = nullptr;
    uint32_t           unit_      = 0;
    IControllers*      ctrl_      = nullptr;
};

} // namespace phone
