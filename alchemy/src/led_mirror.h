/**
 * @file led_mirror.h
 * @brief Keeps a copy of the last LED frame shown, for the iPhone's screen.
 *
 * The Alchemy Lab's "screen" is its 102 LEDs: six pot rings plus the
 * buttons.  LedPanel writes them through an ILedStrip; this sits between
 * the two, passes every call through, and copies the frame on Show().  The
 * phone block (phone_ext.h) sends that copy, so the iPhone draws exactly
 * what the module's rings show: pages, catch pips, the picker's progress.
 *
 *   static LedMirror<kLedTotal> mirror;
 *   mirror.Wrap(hw.strip);
 *   hw.leds.Init(mirror, hw.Layout());   // after hw.Init()
 *
 * Header-only, no hardware.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "alchemy/led/led_strip.h"

namespace phone {

/** What the phone block reads: a frame and how many frames there have been. */
class ILedSource
{
  public:
    virtual ~ILedSource() = default;
    virtual uint16_t       Count() const   = 0;
    virtual const uint8_t* Frame() const   = 0;   /* Count() × RGB */
    virtual uint32_t       Shown() const   = 0;   /* frames shown so far */
};

template <uint16_t N>
class LedMirror : public alchemy::ILedStrip, public ILedSource
{
  public:
    void Wrap(alchemy::ILedStrip& inner) { inner_ = &inner; }

    void SetPixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) override
    {
        if (idx < N)
        {
            work_[3u * idx]      = r;
            work_[3u * idx + 1u] = g;
            work_[3u * idx + 2u] = b;
        }
        if (inner_) inner_->SetPixel(idx, r, g, b);
    }
    void Clear() override
    {
        std::memset(work_, 0, sizeof work_);
        if (inner_) inner_->Clear();
    }
    void Show() override
    {
        std::memcpy(shown_, work_, sizeof shown_);
        frames_++;
        if (inner_) inner_->Show();
    }
    bool     Busy() const override { return inner_ ? inner_->Busy() : false; }
    uint16_t NumLeds() const override { return inner_ ? inner_->NumLeds() : N; }

    uint16_t       Count() const override { return N; }
    const uint8_t* Frame() const override { return shown_; }
    uint32_t       Shown() const override { return frames_; }

  private:
    alchemy::ILedStrip* inner_ = nullptr;
    uint8_t             work_[3u * N]  = {};
    uint8_t             shown_[3u * N] = {};
    uint32_t            frames_        = 0u;
};

} // namespace phone
