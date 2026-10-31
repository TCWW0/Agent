#pragma once
#include "my_agent/domain/profile.hpp"
#include "my_agent/tool/effects.hpp"

#include <cstdint>

namespace my_agent::tool::policy{
    enum class Decision : std::uint8_t {
        Allow,
        Prompt,
    };

    [[nodiscard]]
    constexpr Decision permission(EffectSet effects,Profile profile) noexcept
    {
        if (profile == Profile::Write){
            return Decision::Allow;
        }
        if(effects.has(Effect::WriteFs)||effects.has(Effect::Net)){
            return Decision::Prompt;
        }
        if (effects.has(Effect::Exec)){
            return Decision::Prompt;
        }
        if (profile == Profile::Minimal && effects.has(Effect::ReadFs)){
            return Decision::Prompt;
        }
        return Decision::Allow;
    }

namespace proofs{

inline constexpr std::uint16_t kAllowedMaskBits[3] = {
    0xFFFFu,
    0x0003u,
    0x0001u,
};

static_assert(static_cast<std::uint8_t>(Profile::Write) == 0);
static_assert(static_cast<std::uint8_t>(Profile::Ask) == 1);
static_assert(static_cast<std::uint8_t>(Profile::Minimal) == 2);

inline constexpr std::uint8_t kAllEffectBits =
    static_cast<std::uint8_t>(Effect::ReadFs)
    | static_cast<std::uint8_t>(Effect::WriteFs)
    | static_cast<std::uint8_t>(Effect::Net)
    | static_cast<std::uint8_t>(Effect::Exec);

constexpr bool exhaustive_matches_spec() noexcept
{
    for (std::uint8_t mask = 0; mask <= kAllEffectBits; ++mask) {
        for (std::uint8_t index = 0; index <= 2u; ++index) {
            const Profile profile = static_cast<Profile>(index);
            const bool spec_allows = ((kAllowedMaskBits[index] >> mask) & 1u) != 0;
            const bool impl_allows = permission(EffectSet{mask}, profile) == Decision::Allow;
            if (spec_allows != impl_allows) return false;
        }
    }
    return true;
}

static_assert(exhaustive_matches_spec(), "permission() 与独立规格在至少一个上不一致 !!!");

static_assert(static_cast<std::uint8_t>(Effect::ReadFs) == 0b0001);
static_assert(static_cast<std::uint8_t>(Effect::WriteFs) == 0b0010);
static_assert(static_cast<std::uint8_t>(Effect::Net) == 0b0100);
static_assert(static_cast<std::uint8_t>(Effect::Exec) == 0b1000);

static_assert(kAllEffectBits == 0x0Fu, "effect 位宽改变 -- 修改编译器约束逻辑 !!!");

} // namespace proffs

} // namespace my_agent::tool::policy
