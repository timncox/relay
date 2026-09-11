/*
 * Preset extras: nothing Relay needs to remember lives outside the pager
 * and the settings screen, so this carries only a schema tag. Every
 * Alchemy firmware in the family (Smack, Mark, Belt, Relay) has its own tag
 * here, so a slot 0 written by one of them fails the schema gate in the
 * next and lands in that firmware's first-boot defaults rather than being
 * read as its own settings.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include "alchemy/surface/serializable.h"

struct RelayExtras : public alchemy::Serializable
{
    uint32_t reserved = 0;

    size_t SerializedSize() const override { return 4u; }

    void Serialize(uint8_t* out) const override { std::memcpy(out, &reserved, 4u); }

    bool Deserialize(const uint8_t* in) override
    {
        std::memcpy(&reserved, in, 4u);
        return true;
    }

    /* 'RLY' + layout version. Bump the low byte when the layout changes. */
    uint32_t SchemaHash() const override { return 0x524C5900u | 0x01u; }
};
