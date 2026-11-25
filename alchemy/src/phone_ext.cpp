/**
 * @file phone_ext.cpp
 * @brief HostLink phone block.  See phone_ext.h.
 */

#include "phone_ext.h"

#include <cmath>
#include <cstring>

namespace phone {

using alchemy::hostlink::FrameWriter;
using alchemy::hostlink::ParsedFrame;
using alchemy::hostlink::Status;

namespace {
inline void Fail(FrameWriter& w, Status s) { w.U8(uint8_t(s)); }
}

uint8_t LevelByte(float peak)
{
    if (!(peak > 0.001f)) return 0u;          /* −60 dBFS and below, and NaN */
    if (peak >= 1.0f) return 255u;
    const float db = 20.0f * std::log10(peak);  /* −60 .. 0 */
    const int   v  = int((db + 60.0f) * (255.0f / 60.0f) + 0.5f);
    return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void PhoneExtension::Handle(const ParsedFrame& f, FrameWriter& w, uint32_t now_ms)
{
    switch (f.type)
    {
        case kInfo:
        {
            const uint8_t flags = uint8_t((leds_ ? 1u : 0u) | (fw_ ? 2u : 0u) | (press_ ? 4u : 0u)
                                          | (state_ ? 8u : 0u) | (ctrl_ ? 16u : 0u));
            w.U8(uint8_t(Status::Ok));
            w.U8(kVersion);
            w.U8(flags);
            w.U16(leds_ ? leds_->Count() : 0u);
            w.Str(layout_);
            w.U32(unit_);
            return;
        }
        case kScreen:
        {
            if (!leds_) return Fail(w, Status::Unsupported);
            w.U8(uint8_t(Status::Ok));
            w.U32(leds_->Shown());
            w.U16(leds_->Count());
            w.Bytes(leds_->Frame(), 3u * leds_->Count());
            return;
        }
        case kFwList:
        {
            if (!fw_) return Fail(w, Status::Unsupported);
            const int n = fw_->List(now_ms);
            if (n < 0) return Fail(w, Status(uint8_t(-n)));
            w.U8(uint8_t(Status::Ok));
            w.U8(uint8_t(n));
            for (int i = 0; i < n; i++)
            {
                w.Str(fw_->Name(i));
                w.U32(fw_->Size(i));
            }
            if (w.Overflowed())   /* too many names for one body */
            {
                w.Begin(uint8_t(f.type | 0x80u), f.seq);
                return Fail(w, Status::TooLarge);
            }
            return;
        }
        case kFwFlash:
        {
            if (!fw_) return Fail(w, Status::Unsupported);
            if (f.len < 1u || f.len < 1u + f.body[0] || f.body[0] == 0u || f.body[0] > 63u)
                return Fail(w, Status::BadArgs);
            char name[64];
            std::memcpy(name, f.body + 1, f.body[0]);
            name[f.body[0]] = '\0';
            if (std::strchr(name, '/') || std::strchr(name, '\\'))   /* a bare file name */
                return Fail(w, Status::BadArgs);
            w.U8(fw_->Start(name, now_ms));
            return;
        }
        case kFwStatus:
        {
            if (!fw_) return Fail(w, Status::Unsupported);
            float p = fw_->Progress();
            p       = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
            w.U8(uint8_t(Status::Ok));
            w.U8(uint8_t(fw_->State()));
            w.U8(uint8_t(p * 255.0f + 0.5f));
            w.Str(fw_->Error() ? fw_->Error() : "");
            return;
        }
        case kPress:
        {
            if (!press_) return Fail(w, Status::Unsupported);
            if (f.len != 2u || f.body[1] > 2u) return Fail(w, Status::BadArgs);
            w.U8(uint8_t(press_(f.body[0], f.body[1], press_ctx_) ? Status::Ok : Status::BadArgs));
            return;
        }
        case kState:
        {
            if (!state_) return Fail(w, Status::Unsupported);
            if (f.len != 0u && f.len != 2u) return Fail(w, Status::BadArgs);
            const uint16_t since = f.len == 2u ? uint16_t(f.body[0] | (f.body[1] << 8)) : 0u;
            StateHeader h;
            state_->Header(h, now_ms);
            w.U8(uint8_t(Status::Ok));
            w.U16(h.frame);
            w.U8(h.page);
            w.U8(h.flags);
            w.U8(h.touch_page);
            w.U8(h.touch_pot);
            w.U16(h.touch_age_ms);
            w.U8(h.cpu);
            w.U8(h.in_level);
            w.U8(h.out_level);
            state_->Tail(w, since, now_ms);
            if (w.Overflowed())
            {
                w.Begin(uint8_t(f.type | 0x80u), f.seq);
                return Fail(w, Status::TooLarge);
            }
            return;
        }
        case kCtrl:
        {
            if (!ctrl_) return Fail(w, Status::Unsupported);
            if (f.len < 2u || f.len != 2u + 4u * f.body[1]) return Fail(w, Status::BadArgs);
            ctrl_->Apply(f.body[0], f.body + 2, f.body[1], now_ms);
            w.U8(uint8_t(Status::Ok));
            w.U8(IControllers::kVersion);
            w.U8(ctrl_->Present());
            w.Bytes(ctrl_->LpLeds(), IControllers::kLpLeds);
            w.Bytes(ctrl_->XlLeds(), IControllers::kXlLeds);
            return;
        }
        default:
            return Fail(w, Status::Unsupported);
    }
}

} // namespace phone
