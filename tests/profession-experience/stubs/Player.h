// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <cmath>
using uint8 = uint8_t;
using uint32 = uint32_t;
using int32 = int32_t;
enum { SKILL_ALCHEMY=171, SKILL_BLACKSMITHING=164, SKILL_COOKING=185,
    SKILL_ENCHANTING=333, SKILL_ENGINEERING=202, SKILL_FIRST_AID=129,
    SKILL_HERBALISM=182, SKILL_INSCRIPTION=773, SKILL_JEWELCRAFTING=755,
    SKILL_LEATHERWORKING=165, SKILL_LOCKPICKING=633, SKILL_MINING=186,
    SKILL_SKINNING=393, SKILL_TAILORING=197, SKILL_FISHING=356,
    PLAYER_NEXT_LEVEL_XP=1 };
struct SkillLineAbilityEntry
{
    uint32 SkillLine, Spell, TrivialSkillLineRankHigh, TrivialSkillLineRankLow, MinSkillLineRank;
};
struct Player
{
    std::map<uint32,uint32> current, maximum;
    uint32 nextXP=100000, awarded=0, calls=0;
    uint32 GetPureSkillValue(uint32 id) { return current[id]; }
    uint32 GetPureMaxSkillValue(uint32 id) { return maximum[id]; }
    uint32 GetUInt32Value(uint32) { return nextXP; }
    void GiveXP(uint32 xp, void*) { awarded += xp; ++calls; }
};
