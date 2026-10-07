// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Compila el fuente real parcheado. Los dobles sólo sustituyen la API del core;
// las pruebas en vivo verifican el recorrido de hechizos y la integración.
#include <cassert>
#include <iostream>
#include "ProfessionExp.cpp"

int main()
{
    LoadPEConfigScript config;
    config.OnBeforeConfigLoad(false);
    auto set=[](PEConfig e,float v) { peConfigData.OverwriteConfigValue<float>(e,v); };
    for (uint32 i=0; i<17; ++i) set(PEConfig(i),0.01f);
    set(PEConfig::MULT_ORANGE,1.0f); set(PEConfig::MULT_YELLOW,0.5f);
    set(PEConfig::MULT_GREEN,0.25f); set(PEConfig::MULT_GRAY,0.0f);
    set(PEConfig::MULT_CURVE,0.0f);
    RewardExperienceScript script;
    unsigned checks=0;
    auto check=[&](bool value) { assert(value); ++checks; };
    uint32 gain=0; // La ausencia de subida de habilidad no impide la recompensa.
    // Las 12 rutas de fabricación incluyen fundición y desencantamiento.
    for (auto pair : {std::pair{171u,2330u}, {164u,2660u}, {185u,2538u},
        {333u,13262u}, {333u,7418u}, {202u,3918u}, {129u,3275u},
        {773u,52739u}, {755u,25255u}, {165u,2149u}, {186u,2657u}, {197u,2963u}})
    {
        SkillLineAbilityEntry recipe{pair.first,pair.second,100,50,1};
        for (auto test : {std::pair{1u,1000u}, {50u,500u}, {75u,250u}, {100u,0u}})
        {
            Player p; p.current[pair.first]=test.first; p.maximum[pair.first]=150;
            mgr.calls=0;
            script.OnPlayerUpdateCraftingSkill(&p,&recipe,test.first,gain);
            check(p.awarded==test.second && p.calls==1 && mgr.calls==1 && mgr.source==42 && gain==0);
        }
        Player p; p.current[pair.first]=75; p.maximum[pair.first]=75;
        script.OnPlayerUpdateCraftingSkill(&p,&recipe,75,gain); check(p.calls==0);
        p.maximum[pair.first]=150;
        script.OnPlayerUpdateCraftingSkill(&p,&recipe,75,gain); check(p.awarded==250);
    }
    // Recolección: herboristería, minería, desuello y ganzúa. Repetir el hook
    // se recompensa: el módulo conserva el control de aperturas del core.
    for (uint32 id : {182,186,393,633})
    {
        for (auto test : {std::pair{1u,1000u}, {26u,500u}, {51u,250u}, {101u,0u}})
        {
            Player p; p.current[id]=test.first; p.maximum[id]=150;
            mgr.calls=0; script.OnPlayerUpdateGatheringSkill(&p,id,test.first,101,51,26,gain);
            check(p.awarded==test.second && p.calls==1 && mgr.calls==1);
        }
        Player p; p.current[id]=75; p.maximum[id]=75;
        script.OnPlayerUpdateGatheringSkill(&p,id,75,101,51,26,gain); check(p.calls==0);
    }
    for (int id : {755,773})
    {
        Player p; p.current[id]=1; p.maximum[id]=75;
        script.OnPlayerUpdateGatheringSkill(&p,id,1,101,51,26,gain); check(p.calls==0);
    }
    // Pesca normal y basura tienen exactamente el mismo tratamiento.
    for (auto test : {std::pair{1u,1000u}, {26u,500u}, {51u,250u}, {101u,0u}})
        for (int roll : {1,100})
        {
            Player p; p.current[356]=test.first; p.maximum[356]=150;
            check(script.OnPlayerUpdateFishingSkill(&p,test.first,1,1,roll));
            check(p.awarded==test.second && p.calls==1);
        }
    Player capped; capped.current[356]=75; capped.maximum[356]=75;
    script.OnPlayerUpdateFishingSkill(&capped,75,100,100,1); check(capped.calls==0);
    peConfigData.OverwriteConfigValue<bool>(PEConfig::BLOCK_AT_SKILL_CAP,false);
    script.OnPlayerUpdateFishingSkill(&capped,75,100,100,1); check(capped.awarded==1000);
    peConfigData.OverwriteConfigValue<bool>(PEConfig::BLOCK_AT_SKILL_CAP,true);
    Player max; max.current[356]=450; max.maximum[356]=450;
    script.OnPlayerUpdateFishingSkill(&max,450,500,100,1); check(max.calls==0);
    // Apagado por parámetros: ninguna de las familias entrega XP.
    for (uint32 i=0; i<17; ++i) set(PEConfig(i),0.0f);
    Player off; off.current[356]=off.current[186]=off.current[171]=1;
    off.maximum[356]=off.maximum[186]=off.maximum[171]=75;
    SkillLineAbilityEntry r{171,2330,100,50,1};
    script.OnPlayerUpdateFishingSkill(&off,1,1,100,1);
    script.OnPlayerUpdateGatheringSkill(&off,186,1,101,51,26,gain);
    script.OnPlayerUpdateCraftingSkill(&off,&r,1,gain); check(off.calls==0);
    // Curva fraccionaria corregida: 4^(225/450)=2, no 1.
    set(PEConfig::MULT_CURVE,4.0f);
    Player curve; script.RewardXP(&curve,1,300,275,250,225,0.01f);
    check(curve.awarded==2000);
    std::cout << checks << " comprobaciones OK (fuente real del módulo)\n";
}
