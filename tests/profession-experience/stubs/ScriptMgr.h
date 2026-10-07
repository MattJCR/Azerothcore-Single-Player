// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Player.h"
#include <initializer_list>
enum { PLAYERHOOK_ON_UPDATE_CRAFTING_SKILL, PLAYERHOOK_ON_UPDATE_GATHERING_SKILL,
    PLAYERHOOK_ON_UPDATE_FISHING_SKILL, WORLDHOOK_ON_BEFORE_CONFIG_LOAD };
enum PlayerXPSource { XPSOURCE_KILL, XPSOURCE_QUEST };
struct PlayerScript
{
    PlayerScript(char const*, std::initializer_list<int>) {}
    virtual ~PlayerScript() = default;
    virtual bool OnPlayerUpdateFishingSkill(Player*,int32,int32,int32,int32) { return true; }
    virtual void OnPlayerUpdateGatheringSkill(Player*,uint32,uint32,uint32,uint32,uint32,uint32&) {}
    virtual void OnPlayerUpdateCraftingSkill(Player*,SkillLineAbilityEntry const*,uint32,uint32&) {}
};
struct WorldScript
{
    WorldScript(char const*, std::initializer_list<int>) {}
    virtual ~WorldScript() = default;
    virtual void OnBeforeConfigLoad(bool) {}
};
struct ScriptMgr
{
    int calls=0, source=-1;
    void OnPlayerGiveXP(Player*, uint32&, void*, uint8 s) { ++calls; source=int(s); }
};
inline ScriptMgr mgr;
inline ScriptMgr* sScriptMgr=&mgr;
