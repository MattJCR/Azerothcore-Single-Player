// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * SP03 — cofres itinerantes. Los cofres fijos no cambian de punto al agotarse
 * y pueden quedar invisibles o inaccesibles. Este módulo selecciona puntos de
 * un catálogo, valida suelo/ruta/interacción, conserva plazos en la BD y sólo
 * materializa objetos cerca de jugadores humanos. No toca pools ajenos.
 */
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SlowTick.h"
#include "Timer.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr uint32 ENTRY_BASE = 700000;
constexpr float INTERACTION_RADIUS = 2.4f;
constexpr float MIN_CHEST_DISTANCE = 100.0f;

struct Config
{
    bool enabled = true;
    uint32 locationSeconds = 3600;
    std::array<uint32, 3> respawnSeconds = {1800, 5400, 14400};
    float candidateRadius = 250.0f;
    uint32 spawnSpell = 30262;
    uint32 scanMs = 5000;
    uint32 testSeconds = 90;
};
Config cfg;

struct Point
{
    uint32 id = 0;
    uint32 mapId = 0;
    uint32 zoneId = 0;
    float x = 0, y = 0, z = 0, o = 0;
    bool validated = false;
    uint64 lastAutoAttempt = 0;
};

struct Zone
{
    uint32 mapId = 0;
    std::array<uint8, 3> quotas = {};
    bool enabled = false;
    uint64 nextAutoValidation = 0;
    std::vector<Point> points;
};

struct SlotKey
{
    uint32 zoneId = 0;
    uint8 quality = 0;
    uint8 number = 0;
    bool operator<(SlotKey const& other) const
    {
        if (zoneId != other.zoneId) return zoneId < other.zoneId;
        if (quality != other.quality) return quality < other.quality;
        return number < other.number;
    }
};

struct Slot
{
    uint32 pointId = 0;
    uint32 previousPointId = 0;
    uint64 locationUntil = 0;
    uint64 respawnAt = 0;
    bool opened = false;
    bool looted = false;
    ObjectGuid liveGuid;
    uint64 nextAttempt = 0; // límite de coste; no afecta al estado persistente
};

std::map<uint32, Zone> zones;
std::map<SlotKey, Slot> slots;
std::unordered_map<uint32, Point const*> pointsById;
std::atomic<uint32> lastBotInteractionDenied{0};
std::atomic<uint32> lastBotLootDenied{0};
struct TestChest
{
    ObjectGuid guid;
    uint32 mapId;
    uint32 ownerGuid;
    uint64 expiresAt;
    float x, y;
};
std::vector<TestChest> testChests;
uint32 elapsedMs = 0;

uint64 Now()
{
    return static_cast<uint64>(GameTime::GetGameTime().count());
}

void LoadConfig()
{
    cfg.enabled = sConfigMgr->GetOption<bool>("SPTreasure.Enable", true);
    cfg.locationSeconds = std::max(60u, sConfigMgr->GetOption<uint32>("SPTreasure.LocationSeconds", 3600));
    cfg.respawnSeconds[0] = std::max(1u, sConfigMgr->GetOption<uint32>("SPTreasure.BasicRespawnSeconds", 1800));
    cfg.respawnSeconds[1] = std::max(1u, sConfigMgr->GetOption<uint32>("SPTreasure.RareRespawnSeconds", 5400));
    cfg.respawnSeconds[2] = std::max(1u, sConfigMgr->GetOption<uint32>("SPTreasure.EpicRespawnSeconds", 14400));
    cfg.candidateRadius = std::max(20.0f, sConfigMgr->GetOption<float>("SPTreasure.CandidateRadius", 250.0f));
    cfg.spawnSpell = sConfigMgr->GetOption<uint32>("SPTreasure.SpawnSpell", 30262);
    cfg.scanMs = std::max(1000u, sConfigMgr->GetOption<uint32>("SPTreasure.ScanMs", 5000));
    cfg.testSeconds = std::max(10u, sConfigMgr->GetOption<uint32>("SPTreasure.TestSeconds", 90));
}

GameObject* LiveTestChest(TestChest const& chest)
{
    Map* map = sMapMgr->FindBaseNonInstanceMap(chest.mapId);
    return map ? map->GetGameObject(chest.guid) : nullptr;
}

uint32 RemoveTestChests(uint32 ownerGuid)
{
    uint32 removed = 0;
    for (auto it = testChests.begin(); it != testChests.end();)
    {
        if (ownerGuid && it->ownerGuid != ownerGuid)
        {
            ++it;
            continue;
        }
        if (GameObject* go = LiveTestChest(*it))
            go->Delete();
        it = testChests.erase(it);
        ++removed;
    }
    return removed;
}

void Save(SlotKey const& key, Slot const& slot)
{
    WorldDatabase.DirectExecute(
        "INSERT INTO sp_treasure_slot (zone_id,quality,slot_no,point_id,previous_point_id,location_until,respawn_at,opened) "
        "VALUES ({},{},{},{},{},{},{},{}) ON DUPLICATE KEY UPDATE "
        "point_id=VALUES(point_id),previous_point_id=VALUES(previous_point_id),"
        "location_until=VALUES(location_until),respawn_at=VALUES(respawn_at),opened=VALUES(opened)",
        key.zoneId, uint32(key.quality), uint32(key.number), slot.pointId,
        slot.previousPointId, slot.locationUntil, slot.respawnAt, uint32(slot.opened));
}

Point const* FindPoint(Zone const& zone, uint32 id)
{
    for (Point const& point : zone.points)
        if (point.id == id)
            return &point;
    return nullptr;
}

void LoadData()
{
    pointsById.clear();
    zones.clear();
    slots.clear();
    if (QueryResult result = WorldDatabase.Query(
        "SELECT zone_id,map_id,basic_slots,rare_slots,epic_slots,enabled FROM sp_treasure_zone"))
    {
        do
        {
            Field* f = result->Fetch();
            Zone& zone = zones[f[0].Get<uint16>()];
            zone.mapId = f[1].Get<uint16>();
            zone.quotas = {f[2].Get<uint8>(), f[3].Get<uint8>(), f[4].Get<uint8>()};
            zone.enabled = f[5].Get<uint8>() != 0;
        } while (result->NextRow());
    }
    if (QueryResult result = WorldDatabase.Query(
        "SELECT id,map_id,zone_id,x,y,z,orientation,validated FROM sp_treasure_point"))
    {
        do
        {
            Field* f = result->Fetch();
            Point point;
            point.id = f[0].Get<uint32>();
            point.mapId = f[1].Get<uint16>();
            point.zoneId = f[2].Get<uint16>();
            point.x = f[3].Get<float>(); point.y = f[4].Get<float>();
            point.z = f[5].Get<float>(); point.o = f[6].Get<float>();
            point.validated = f[7].Get<uint8>() != 0;
            auto zone = zones.find(point.zoneId);
            if (zone != zones.end() && zone->second.mapId == point.mapId)
                zone->second.points.push_back(point);
        } while (result->NextRow());
    }
    for (auto const& [zoneId, zone] : zones)
        for (Point const& point : zone.points)
            pointsById.emplace(point.id, &point);
    if (QueryResult result = WorldDatabase.Query(
        "SELECT zone_id,quality,slot_no,point_id,previous_point_id,location_until,respawn_at,opened FROM sp_treasure_slot"))
    {
        do
        {
            Field* f = result->Fetch();
            SlotKey key{f[0].Get<uint16>(), f[1].Get<uint8>(), f[2].Get<uint8>()};
            Slot slot;
            slot.pointId = f[3].Get<uint32>();
            slot.previousPointId = f[4].Get<uint32>();
            slot.locationUntil = f[5].Get<uint64>();
            slot.respawnAt = f[6].Get<uint64>();
            slot.opened = f[7].Get<uint8>() != 0;
            slots.emplace(key, slot);
        } while (result->NextRow());
    }
    for (auto const& [zoneId, zone] : zones)
        for (uint8 quality = 1; quality <= 3; ++quality)
            for (uint8 number = 0; number < zone.quotas[quality - 1]; ++number)
            {
                SlotKey key{zoneId, quality, number};
                if (slots.find(key) == slots.end())
                {
                    Slot& slot = slots[key];
                    Save(key, slot);
                }
            }
    LOG_INFO("module", "SP03: cargadas {} zonas y {} plazas de tesoro", zones.size(), slots.size());
}

GameObject* LiveObject(SlotKey const& key, Slot const& slot)
{
    if (!slot.liveGuid)
        return nullptr;
    auto zone = zones.find(key.zoneId);
    if (zone == zones.end())
        return nullptr;
    Map* map = sMapMgr->FindBaseNonInstanceMap(zone->second.mapId);
    return map ? map->GetGameObject(slot.liveGuid) : nullptr;
}

bool ReservedNearby(Point const& candidate)
{
    for (auto const& [key, slot] : slots)
        if (slot.pointId)
        {
            auto occupied = pointsById.find(slot.pointId);
            if (occupied != pointsById.end() && occupied->second->mapId == candidate.mapId &&
                std::hypot(candidate.x - occupied->second->x, candidate.y - occupied->second->y) < MIN_CHEST_DISTANCE)
                return true;
        }
    return false;
}

uint32 CountSeparatedPoints(Zone const& zone)
{
    std::vector<Point const*> selected;
    for (Point const& point : zone.points)
    {
        if (!point.validated)
            continue;
        bool clear = true;
        for (Point const* other : selected)
            if (std::hypot(point.x - other->x, point.y - other->y) < MIN_CHEST_DISTANCE)
            {
                clear = false;
                break;
            }
        if (clear)
            selected.push_back(&point);
    }
    return uint32(selected.size());
}

// El origen es un jugador humano que ha llegado normalmente a la zona.
// Sólo se valida al seleccionar destino; nunca por bot ni en cada tick.
bool ValidatePoint(Player* player, Point const& point, char const** reason = nullptr)
{
    auto reject = [reason](char const* why) {
        if (reason) *reason = why;
        return false;
    };
    Map* map = player->GetMap();
    if (!map || map->GetId() != point.mapId || player->GetZoneId() != point.zoneId)
        return reject("mapa o zona distinta");
    float ground = map->GetHeight(player->GetPhaseMask(), point.x, point.y,
                                  point.z + 5.0f, true, 12.0f);
    if (!std::isfinite(ground) || ground < -50000.0f || std::fabs(ground - point.z) > 1.5f)
        return reject("suelo no valido");
    if (map->GetZoneId(player->GetPhaseMask(), point.x, point.y, ground) != point.zoneId)
        return reject("borde de zona");
    if (map->IsInWater(player->GetPhaseMask(), point.x, point.y, ground, 1.8f))
        return reject("en agua");

    for (uint32 n = 0; n < 8; ++n)
    {
        float angle = float(n) * 0.78539816f;
        float x = point.x + std::cos(angle) * INTERACTION_RADIUS;
        float y = point.y + std::sin(angle) * INTERACTION_RADIUS;
        float z = map->GetHeight(player->GetPhaseMask(), x, y, ground + 5.0f, true, 12.0f);
        if (!std::isfinite(z) || z < -50000.0f || std::fabs(z - ground) > 1.2f)
            continue;
        if (map->IsInWater(player->GetPhaseMask(), x, y, z, 1.8f))
            continue;
        if (!map->isInLineOfSight(x, y, z + 1.5f, point.x, point.y, ground + 0.7f,
                                  player->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                  VMAP::ModelIgnoreFlags::Nothing))
            continue;
        PathGenerator path(player);
        if (!path.CalculatePath(x, y, z))
            continue;
        PathType type = path.GetPathType();
        if (!(type & PATHFIND_NORMAL) ||
            (type & (PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOPATH |
                     PATHFIND_NOT_USING_PATH | PATHFIND_SHORT | PATHFIND_FARFROMPOLY)))
            continue;
        G3D::Vector3 const& end = path.GetActualEndPosition();
        if (std::hypot(end.x - x, end.y - y) > 1.0f || std::fabs(end.z - z) > 1.5f)
            continue;
        if (reason) *reason = "ruta y espacio validos";
        return true;
    }
    return reject("sin acercamiento valido");
}

void RecordValidation(Player* player, Point& point, char const* reason, bool passed)
{
    if (passed)
    {
        point.validated = true;
        point.z = player->GetMap()->GetHeight(player->GetPhaseMask(), point.x,
                   point.y, point.z + 5.0f, true, 12.0f) + 0.2f;
    }
    WorldDatabase.DirectExecute(
        "UPDATE sp_treasure_point SET validated={},checked_at=NOW(),check_result='{}',z={} WHERE id={}",
        uint32(point.validated), reason ? reason : "sin resultado", point.z, point.id);
}

void ValidateNearby(Player* player, Zone& zone)
{
    if (zone.nextAutoValidation > Now() || zone.points.empty())
        return;

    uint32 quota = zone.quotas[0] + zone.quotas[1] + zone.quotas[2];
    std::vector<Point const*> separated;
    for (Point const& point : zone.points)
        if (point.validated && player->GetDistance2d(point.x, point.y) <= cfg.candidateRadius)
        {
            bool clear = true;
            for (Point const* other : separated)
                if (std::hypot(point.x - other->x, point.y - other->y) < MIN_CHEST_DISTANCE)
                {
                    clear = false;
                    break;
                }
            if (clear)
                separated.push_back(&point);
        }
    if (separated.size() >= std::max(2u, quota * 2))
        return;
    zone.nextAutoValidation = Now() + 10;

    uint32 start = urand(0, uint32(zone.points.size() - 1));
    uint32 checked = 0;
    for (uint32 n = 0; n < zone.points.size() && checked < 8; ++n)
    {
        Point& point = zone.points[(start + n) % zone.points.size()];
        if (point.validated || point.lastAutoAttempt + 300 > Now() ||
            player->GetDistance2d(point.x, point.y) > cfg.candidateRadius)
            continue;
        point.lastAutoAttempt = Now();
        ++checked;
        char const* reason = nullptr;
        bool passed = ValidatePoint(player, point, &reason);
        RecordValidation(player, point, reason, passed);
    }
}

void Retire(SlotKey const& key, Slot& slot, bool collected)
{
    if (GameObject* go = LiveObject(key, slot))
        go->Delete();
    slot.liveGuid.Clear();
    slot.previousPointId = slot.pointId;
    slot.pointId = 0;
    slot.locationUntil = 0;
    slot.opened = false;
    slot.looted = false;
    slot.respawnAt = collected ? Now() + cfg.respawnSeconds[key.quality - 1] : 0;
    Save(key, slot);
}

void ResolveCrowdedSlots()
{
    for (auto it = slots.begin(); it != slots.end(); ++it)
    {
        Slot& slot = it->second;
        if (!slot.pointId)
            continue;
        auto pointIt = pointsById.find(slot.pointId);
        if (pointIt == pointsById.end())
            continue;
        Point const* point = pointIt->second;
        for (auto earlier = slots.begin(); earlier != it; ++earlier)
        {
            if (!earlier->second.pointId)
                continue;
            auto other = pointsById.find(earlier->second.pointId);
            if (other != pointsById.end() && other->second->mapId == point->mapId &&
                std::hypot(point->x - other->second->x, point->y - other->second->y) < MIN_CHEST_DISTANCE)
            {
                Retire(it->first, slot, false);
                break;
            }
        }
    }
}

GameObject* CreateChest(uint32 entry, Point const& point, Player* player)
{
    Map* map = player->GetMap();
    float half = point.o * 0.5f;
    G3D::Quat rotation;
    rotation.x = 0; rotation.y = 0;
    rotation.z = std::sin(half); rotation.w = std::cos(half);
    GameObject* go = new GameObject();
    if (!go->Create(map->GenerateLowGuid<HighGuid::GameObject>(), entry, map,
                    player->GetPhaseMask(), point.x, point.y, point.z, point.o,
                    rotation, 0, GO_STATE_READY) || !map->AddToMap(go))
    {
        delete go;
        LOG_ERROR("module", "SP03: no se pudo crear cofre {} en punto {}", entry, point.id);
        return nullptr;
    }
    if (cfg.spawnSpell)
        go->CastSpell(nullptr, cfg.spawnSpell);
    return go;
}

bool Spawn(SlotKey const& key, Slot& slot, Point const& point, Player* player)
{
    uint32 entry = ENTRY_BASE + key.zoneId * 3 + key.quality;
    GameObject* go = CreateChest(entry, point, player);
    if (!go)
        return false;
    slot.liveGuid = go->GetGUID();
    return true;
}

GameObject* SpawnTestChest(Player* player, uint32 entry, uint32 seconds = 0)
{
    if (!player || !player->IsInWorld() || player->GetMap()->Instanceable())
        return nullptr;
    Point point;
    point.mapId = player->GetMapId();
    point.zoneId = player->GetZoneId();
    point.o = player->GetOrientation();
    float const facing = player->GetOrientation();
    for (float distance : {3.5f, 5.5f, 7.5f, 9.5f})
        for (float offset : {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f,
                             2.0f, -2.0f, 2.5f, -2.5f, 3.14f})
        {
            float const angle = facing + offset;
            point.x = player->GetPositionX() + std::cos(angle) * distance;
            point.y = player->GetPositionY() + std::sin(angle) * distance;
            float const ground = player->GetMap()->GetHeight(player->GetPhaseMask(), point.x,
                point.y, player->GetPositionZ() + 5.0f, true, 12.0f);
            if (!std::isfinite(ground) || ground < -50000.0f ||
                std::fabs(ground - player->GetPositionZ()) > 1.5f)
                continue;
            point.z = ground + 0.2f;
            bool occupied = false;
            for (TestChest const& chest : testChests)
                if (chest.mapId == point.mapId && std::hypot(point.x - chest.x, point.y - chest.y) < 2.5f)
                {
                    occupied = true;
                    break;
                }
            if (occupied || !ValidatePoint(player, point))
                continue;
            if (GameObject* go = CreateChest(entry, point, player))
            {
                testChests.push_back({go->GetGUID(), point.mapId,
                    player->GetGUID().GetCounter(), Now() + (seconds ? seconds : cfg.testSeconds), point.x, point.y});
                return go;
            }
        }
    return nullptr;
}

void FillSlot(SlotKey const& key, Slot& slot, Zone& zone, Player* player)
{
    if (slot.respawnAt > Now() || zone.points.empty())
        return;
    uint32 start = urand(0, uint32(zone.points.size() - 1));
    uint32 checked = 0;
    for (uint32 n = 0; n < zone.points.size() && checked < 40; ++n)
    {
        Point& point = zone.points[(start + n) % zone.points.size()];
        Point const* previous = FindPoint(zone, slot.previousPointId);
        if (!point.validated || point.id == slot.previousPointId ||
            (previous && std::hypot(point.x - previous->x, point.y - previous->y) < MIN_CHEST_DISTANCE) ||
            ReservedNearby(point) ||
            player->GetDistance2d(point.x, point.y) > cfg.candidateRadius)
            continue;
        ++checked;
        if (!ValidatePoint(player, point))
            continue;
        slot.pointId = point.id;
        slot.locationUntil = Now() + cfg.locationSeconds;
        slot.respawnAt = 0;
        slot.opened = false;
        Save(key, slot);
        if (!Spawn(key, slot, point, player))
        {
            slot.pointId = 0;
            slot.locationUntil = 0;
            slot.nextAttempt = Now() + 30;
            Save(key, slot);
        }
        return;
    }
    slot.nextAttempt = Now() + 30;
}

void Process(Player* player)
{
    auto zoneIt = zones.find(player->GetZoneId());
    if (zoneIt == zones.end() || !zoneIt->second.enabled ||
        zoneIt->second.mapId != player->GetMapId())
        return;
    Zone& zone = zoneIt->second;
    ValidateNearby(player, zone);
    for (uint8 quality = 1; quality <= 3; ++quality)
        for (uint8 number = 0; number < zone.quotas[quality - 1]; ++number)
        {
            SlotKey key{player->GetZoneId(), quality, number};
            Slot& slot = slots[key];
            if (slot.looted || (slot.pointId && slot.locationUntil <= Now()))
                Retire(key, slot, slot.looted || slot.opened);
            if (!slot.pointId)
            {
                if (slot.nextAttempt <= Now())
                    FillSlot(key, slot, zone, player);
                continue;
            }
            Point const* point = FindPoint(zone, slot.pointId);
            if (!point)
            {
                Retire(key, slot, false);
                continue;
            }
            if (player->GetDistance2d(point->x, point->y) > cfg.candidateRadius)
                continue;
            if (!LiveObject(key, slot))
            {
                if (slot.opened)
                    Retire(key, slot, true); // descarga de grid: nunca volver a sortear botín abierto
                else if (slot.nextAttempt <= Now())
                {
                    if (ValidatePoint(player, *point))
                        Spawn(key, slot, *point, player);
                    else
                        slot.nextAttempt = Now() + 30;
                }
            }
        }
}

class TreasureWorld : public WorldScript
{
public:
    TreasureWorld() : WorldScript("SPTreasureWorld") {}
    void OnAfterConfigLoad(bool reload) override
    {
        bool wasEnabled = cfg.enabled;
        LoadConfig();
        if (reload && wasEnabled && !cfg.enabled)
        {
            RemoveTestChests(0);
            for (auto& [key, slot] : slots)
                if (slot.pointId)
                    Retire(key, slot, false);
        }
        else if (reload && !wasEnabled && cfg.enabled)
            LoadData();
    }
    void OnStartup() override
    {
        if (cfg.enabled)
        {
            LoadData();
            ResolveCrowdedSlots();
        }
    }
    void OnUpdate(uint32 diff) override
    {
        if (!cfg.enabled || (elapsedMs += diff) < cfg.scanMs)
            return;
        elapsedMs = 0;
        uint32 const t0 = getMSTime();
        for (auto it = testChests.begin(); it != testChests.end();)
        {
            if (it->expiresAt > Now() && LiveTestChest(*it))
            {
                ++it;
                continue;
            }
            if (GameObject* go = LiveTestChest(*it))
                go->Delete();
            it = testChests.erase(it);
        }
        // También vencen las plazas de zonas sin jugadores o con grids descargadas.
        for (auto& [key, slot] : slots)
            if (slot.looted || (slot.pointId && slot.locationUntil <= Now()))
                Retire(key, slot, slot.looted || slot.opened);
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (player && player->IsInWorld() && player->GetSession() &&
                !player->GetSession()->IsHeadless())
                Process(player);
        SlowTick::WarnIfSlow("treasure", "OnUpdate", t0);
    }
};

class TreasureChest : public GameObjectScript
{
public:
    TreasureChest() : GameObjectScript("SPTreasureChest") {}
    bool OnGossipHello(Player* player, GameObject* go) override
    {
        if (!player || !go || !player->GetSession())
            return true;
        if (player->GetSession()->IsHeadless())
        {
            lastBotInteractionDenied.store(go->GetGUID().GetCounter(), std::memory_order_relaxed);
            return true;
        }
        for (auto& [key, slot] : slots)
            if (slot.liveGuid == go->GetGUID())
                return false;
        for (TestChest const& chest : testChests)
            if (chest.guid == go->GetGUID())
                return false;
        return true; // objeto ajeno o estado ya retirado
    }
    void OnLootStateChanged(GameObject* go, uint32 state, Unit* /*unit*/) override
    {
        if (state != GO_JUST_DEACTIVATED || !go)
            return;
        for (auto& [key, slot] : slots)
            if (slot.liveGuid == go->GetGUID())
            {
                slot.looted = true;
                return;
            }
    }
};

// La apertura normal usa SPELL_EFFECT_OPEN_LOCK y no pasa por OnGossipHello.
class TreasureLootGuard : public GlobalScript
{
public:
    TreasureLootGuard() : GlobalScript("SPTreasureLootGuard", {
        GLOBALHOOK_ON_ALLOWED_TO_LOOT_CONTAINER_CHECK
    }) { }
    bool OnAllowedToLootContainerCheck(Player const* player, ObjectGuid source) override
    {
        if (!source.IsGameObject() || !player || !player->GetSession() ||
            !player->GetSession()->IsHeadless())
            return false;
        for (auto const& [key, slot] : slots)
            if (slot.liveGuid == source)
            {
                lastBotLootDenied.store(source.GetCounter(), std::memory_order_relaxed);
                return true; // el hook devuelve true para vetar el saqueo
            }
        for (TestChest const& chest : testChests)
            if (chest.guid == source)
            {
                lastBotLootDenied.store(source.GetCounter(), std::memory_order_relaxed);
                return true;
            }
        return false;
    }
};

class TreasureLootState : public PlayerScript
{
public:
    TreasureLootState() : PlayerScript("SPTreasureLootState", {
        PLAYERHOOK_ON_BEFORE_SEND_LOOT
    }) { }
    void OnPlayerBeforeSendLoot(Player* /*player*/, ObjectGuid guid, Loot* /*loot*/) override
    {
        if (!guid.IsGameObject())
            return;
        for (auto& [key, slot] : slots)
            if (slot.liveGuid == guid && !slot.opened)
            {
                slot.opened = true;
                Save(key, slot);
                return;
            }
    }
};

class TreasureCommands : public CommandScript
{
public:
    TreasureCommands() : CommandScript("SPTreasureCommands") {}
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable test =
        {
            {"crear", HandleTestCreate, SEC_ADMINISTRATOR, Console::Yes},
            {"equipo", HandleTestEquipment, SEC_ADMINISTRATOR, Console::Yes},
            {"retirar", HandleTestRemove, SEC_ADMINISTRATOR, Console::Yes},
        };
        static ChatCommandTable sub =
        {
            {"estado", HandleStatus, SEC_GAMEMASTER, Console::No},
            {"validar", HandleValidate, SEC_GAMEMASTER, Console::No},
            {"activar", HandleActivate, SEC_ADMINISTRATOR, Console::No},
            {"desactivar", HandleDeactivate, SEC_ADMINISTRATOR, Console::No},
            {"prueba", test},
            {"", HandleStatus, SEC_GAMEMASTER, Console::No},
        };
        static ChatCommandTable root = {{"tesoro", sub}};
        return root;
    }
    static bool HandleTestCreate(ChatHandler* handler, PlayerIdentifier target,
                                 uint32 quality, Optional<uint32> lootZone)
    {
        Player* player = target.GetConnectedPlayer();
        if (!cfg.enabled || !player || !player->GetSession() || player->GetSession()->IsHeadless())
        {
            handler->SendSysMessage("El modulo debe estar activo y el personaje humano conectado.");
            return true;
        }
        uint32 zoneId = lootZone.value_or(player->GetZoneId());
        auto zone = zones.find(zoneId);
        if (quality < 1 || quality > 3 || zone == zones.end() ||
            !zone->second.quotas[quality - 1])
        {
            handler->SendSysMessage("Calidad o zona sin cofre configurado (calidades: 1 basico, 2 raro, 3 epico).");
            return true;
        }
        uint32 ownerGuid = player->GetGUID().GetCounter();
        if (std::count_if(testChests.begin(), testChests.end(),
            [ownerGuid](TestChest const& chest) { return chest.ownerGuid == ownerGuid; }) >= 12)
        {
            handler->SendSysMessage("Ya hay doce cofres de prueba para este personaje; retiralos primero.");
            return true;
        }
        uint32 entry = ENTRY_BASE + zoneId * 3 + quality;
        if (GameObject* go = SpawnTestChest(player, entry))
            handler->PSendSysMessage("Cofre de prueba {} (calidad {}, botin zona {}) frente a {}. Se retira en {} s o con .tesoro prueba retirar {}.",
                go->GetGUID().GetCounter(), quality, zoneId, player->GetName(), cfg.testSeconds, player->GetName());
        else
            handler->SendSysMessage("No hay suelo accesible y libre frente al personaje; muévelo a un espacio abierto.");
        return true;
    }
    static bool HandleTestRemove(ChatHandler* handler, PlayerIdentifier target)
    {
        uint32 count = RemoveTestChests(target.GetGUID().GetCounter());
        handler->PSendSysMessage("Retirados {} cofres de prueba de {}.", count, target.GetName());
        return true;
    }
    static bool HandleTestEquipment(ChatHandler* handler, PlayerIdentifier target,
                                    uint32 tier, uint32 variant)
    {
        Player* player = target.GetConnectedPlayer();
        if (!cfg.enabled || !player || !player->GetSession() || player->GetSession()->IsHeadless())
        {
            handler->SendSysMessage("El modulo debe estar activo y el personaje humano conectado.");
            return true;
        }
        if ((tier != 1 && tier != 2) || (variant != 1 && variant != 2))
        {
            handler->SendSysMessage("Uso: .tesoro prueba equipo <personaje> <tramo 1 intermedio / 2 final> <variante 1 / 2>.");
            return true;
        }
        uint32 ownerGuid = player->GetGUID().GetCounter();
        if (std::count_if(testChests.begin(), testChests.end(),
            [ownerGuid](TestChest const& chest) { return chest.ownerGuid == ownerGuid; }) >= 12)
        {
            handler->SendSysMessage("Ya hay doce cofres de prueba para este personaje; retiralos primero.");
            return true;
        }
        constexpr uint32 equipmentSeconds = 600;
        uint32 entry = 799000 + (tier - 1) * 2 + variant;
        if (GameObject* go = SpawnTestChest(player, entry, equipmentSeconds))
            handler->PSendSysMessage("Cofre de equipo {} (tramo {}, variante {}) frente a {}. Se retira en {} s o con .tesoro prueba retirar {}.",
                go->GetGUID().GetCounter(), tier, variant, player->GetName(), equipmentSeconds, player->GetName());
        else
            handler->SendSysMessage("No hay suelo accesible y libre frente al personaje; muevelo a un espacio abierto.");
        return true;
    }
    static bool HandleStatus(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return true;
        auto it = zones.find(player->GetZoneId());
        if (it == zones.end())
        {
            handler->SendSysMessage("Esta zona no tiene tesoros configurados.");
            return true;
        }
        Zone const& zone = it->second;
        uint32 approved = 0;
        for (Point const& point : zone.points)
            approved += point.validated ? 1 : 0;
        handler->PSendSysMessage(
            "Tesoros zona {}: {}. Puntos aprobados {}/{}. Cuotas basico/raros/epicos: {}/{}/{}.",
            player->GetZoneId(), zone.enabled ? "activos" : "inactivos", approved,
            zone.points.size(), uint32(zone.quotas[0]), uint32(zone.quotas[1]),
            uint32(zone.quotas[2]));
        handler->PSendSysMessage("Ultimo veto a bot: interaccion GUID {}, botin GUID {}.",
            lastBotInteractionDenied.load(std::memory_order_relaxed),
            lastBotLootDenied.load(std::memory_order_relaxed));
        for (auto const& [key, slot] : slots)
            if (key.zoneId == player->GetZoneId())
            {
                Point const* point = FindPoint(zone, slot.pointId);
                handler->PSendSysMessage("  calidad {} plaza {}: punto {} anterior {} vence {} repone {} abierto {} pos {} {} {}",
                    uint32(key.quality), uint32(key.number), slot.pointId,
                    slot.previousPointId, slot.locationUntil, slot.respawnAt, slot.opened,
                    point ? point->x : 0, point ? point->y : 0, point ? point->z : 0);
            }
        return true;
    }
    static bool HandleValidate(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return true;
        auto it = zones.find(player->GetZoneId());
        if (it == zones.end())
        {
            handler->SendSysMessage("Esta zona no tiene candidatos.");
            return true;
        }
        uint32 checked = 0, passed = 0;
        for (Point& point : it->second.points)
        {
            if (point.validated || player->GetDistance2d(point.x, point.y) > cfg.candidateRadius)
                continue;
            if (checked >= 60)
                break;
            ++checked;
            char const* reason = nullptr;
            bool valid = ValidatePoint(player, point, &reason);
            if (valid)
                ++passed;
            RecordValidation(player, point, reason, valid);
        }
        handler->PSendSysMessage("Tesoros: {} puntos examinados; {} aprobados desde su posicion.", checked, passed);
        return true;
    }
    static bool HandleActivate(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return true;
        auto it = zones.find(player->GetZoneId());
        if (it == zones.end()) return true;
        Zone& zone = it->second;
        uint32 approved = 0;
        for (Point const& point : zone.points)
            approved += point.validated ? 1 : 0;
        uint32 quota = zone.quotas[0] + zone.quotas[1] + zone.quotas[2];
        uint32 separated = CountSeparatedPoints(zone);
        if (separated < std::max(2u, quota * 2))
        {
            handler->PSendSysMessage("Faltan puntos accesibles: {} aprobados, {} separados, {} necesarios.",
                                      approved, separated, std::max(2u, quota * 2));
            return true;
        }
        zone.enabled = true;
        WorldDatabase.DirectExecute("UPDATE sp_treasure_zone SET enabled=1 WHERE zone_id={}", player->GetZoneId());
        handler->PSendSysMessage("Tesoros activados en la zona {}.", player->GetZoneId());
        return true;
    }
    static bool HandleDeactivate(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return true;
        auto it = zones.find(player->GetZoneId());
        if (it == zones.end()) return true;
        it->second.enabled = false;
        WorldDatabase.DirectExecute("UPDATE sp_treasure_zone SET enabled=0 WHERE zone_id={}", player->GetZoneId());
        for (auto& [key, slot] : slots)
            if (key.zoneId == player->GetZoneId() && slot.pointId)
                Retire(key, slot, false);
        handler->PSendSysMessage("Tesoros desactivados en la zona {}.", player->GetZoneId());
        return true;
    }
};

} // namespace

void AddSC_mod_treasure()
{
    new TreasureWorld();
    new TreasureChest();
    new TreasureLootGuard();
    new TreasureLootState();
    new TreasureCommands();
}
