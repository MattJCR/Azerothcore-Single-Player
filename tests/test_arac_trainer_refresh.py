# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Compile the real module with small core doubles; no WoW protocol emulation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MOCKS = r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
using uint32 = uint32_t;
struct ConfigMgr {
    std::map<std::string, bool> values;
    template<class T> T GetOption(char const* key, T fallback) {
        auto it = values.find(key); return it == values.end() ? fallback : it->second;
    }
} configMgr;
auto* sConfigMgr = &configMgr;
struct Creature { uint32 entry; uint32 GetEntry() { return entry; } };
struct WorldSession {
    int lists = 0;
    Creature* last = nullptr;
    void SendTrainerList(Creature* npc) { ++lists; last = npc; }
};
struct Player {
    WorldSession* session;
    bool fit = true;
    WorldSession* GetSession() { return session; }
    bool IsSpellFitByClassAndRace(uint32) { return fit; }
    char const* GetName() { return "Druida"; }
    int getRace() { return 7; }
    int getClass() { return 11; }
};
enum { WORLDHOOK_ON_AFTER_CONFIG_LOAD, PLAYERHOOK_CAN_LEARN_SPELL,
       PLAYERHOOK_ON_AFTER_TRAIN_SPELL };
struct WorldScript {
    WorldScript(char const*, std::initializer_list<int>) {}
    virtual void OnAfterConfigLoad(bool) {}
};
struct PlayerScript {
    PlayerScript(char const*, std::initializer_list<int>) {}
    virtual bool OnPlayerCanLearnSpell(Player*, uint32) { return true; }
    virtual void OnPlayerAfterTrainSpell(Player*, Creature*, uint32) {}
};
int purchases = 0;
#define LOG_INFO(...) (++purchases)
#define LOG_WARN(...) ((void)0)
'''
CASES = r'''
int main() {
    mod_arac_trainer_audit_world world;
    mod_arac_trainer_audit_player hook;
    WorldSession session;
    Player player{&session};
    Creature druid{26324}, priest{26327}, native{4217};
    configMgr.values["AracTrainerAudit.RefreshDruidTrainerList"] = true;
    world.OnAfterConfigLoad(false);
    hook.OnPlayerAfterTrainSpell(&player, &druid, 1082);
    assert(session.lists == 1 && session.last == &druid && purchases == 1);
    // Logging is optional; turning it off must not turn off the mitigation.
    configMgr.values["AracTrainerAudit.LogTrainerPurchases"] = false;
    world.OnAfterConfigLoad(true);
    hook.OnPlayerAfterTrainSpell(&player, &druid, 1822);
    assert(session.lists == 2 && purchases == 1);
    hook.OnPlayerAfterTrainSpell(&player, &priest, 1);
    hook.OnPlayerAfterTrainSpell(&player, &native, 1);
    hook.OnPlayerAfterTrainSpell(&player, nullptr, 1);
    hook.OnPlayerAfterTrainSpell(nullptr, &druid, 1);
    player.session = nullptr;
    hook.OnPlayerAfterTrainSpell(&player, &druid, 1);
    player.session = &session;
    assert(session.lists == 2);
    configMgr.values["AracTrainerAudit.RefreshDruidTrainerList"] = false;
    world.OnAfterConfigLoad(true);
    hook.OnPlayerAfterTrainSpell(&player, &druid, 1);
    assert(session.lists == 2);
    configMgr.values["AracTrainerAudit.RefreshDruidTrainerList"] = true;
    configMgr.values["AracTrainerAudit.Enable"] = false;
    world.OnAfterConfigLoad(true);
    hook.OnPlayerAfterTrainSpell(&player, &druid, 1);
    assert(session.lists == 2);
    configMgr.values["AracTrainerAudit.Enable"] = true;
    world.OnAfterConfigLoad(true);
    player.fit = false;
    assert(!hook.OnPlayerCanLearnSpell(&player, 1082));
    player.fit = true;
    assert(hook.OnPlayerCanLearnSpell(&player, 1082));
    hook.OnPlayerAfterTrainSpell(&player, &druid, 5221);
    assert(session.lists == 3);
}
'''


class TrainerRefreshTest(unittest.TestCase):
    def test_packet_trace_is_scoped_and_does_not_consume_packets(self):
        compiler = shutil.which(os.environ.get("CXX", "g++"))
        if not compiler:
            self.skipTest("g++/CXX required")
        mocks = MOCKS.replace("struct WorldSession {", "struct WorldSession {\n"
                              "uint32 account = 1; uint32 GetAccountId() { return account; }")
        mocks += r'''
#include <cstring>
#include <vector>
#include <stdexcept>
using uint16 = uint16_t;
using uint64 = uint64_t;
enum { CMSG_TRAINER_BUY_SPELL=1, SMSG_TRAINER_BUY_SUCCEEDED,
       SMSG_TRAINER_BUY_FAILED, SMSG_TRAINER_LIST, CMSG_TRAINER_LIST,
       CMSG_GOSSIP_HELLO, SERVERHOOK_CAN_PACKET_RECEIVE, SERVERHOOK_CAN_PACKET_SEND };
struct WorldPacket {
    uint16 opcode;
    std::vector<unsigned char> bytes;
    uint16 GetOpcode() const { return opcode; }
    size_t size() const { return bytes.size(); }
    template<class T> T read(size_t offset) const {
        if (offset + sizeof(T) > bytes.size()) throw std::out_of_range("packet");
        T value; std::memcpy(&value, bytes.data()+offset, sizeof(T)); return value;
    }
};
struct ServerScript {
    ServerScript(char const*, std::initializer_list<int>) {}
    virtual bool CanPacketReceive(WorldSession*, WorldPacket const&) { return true; }
    virtual bool CanPacketSend(WorldSession*, WorldPacket const&) { return true; }
};
int traces=0, malformed=0;
#undef LOG_INFO
#undef LOG_WARN
#define LOG_INFO(...) (++traces)
#define LOG_WARN(...) (++malformed)
'''
        cases = r'''
int main() {
    arac_trainer_trace_packets hook;
    arac_trainer_trace_config settings;
    WorldSession session;
    WorldPacket packet{CMSG_TRAINER_BUY_SPELL, std::vector<unsigned char>(12, 1)};
    auto original = packet.bytes;
    assert(hook.CanPacketReceive(&session, packet)); assert(traces == 0);
    traceAccount.store(1);
    assert(hook.CanPacketReceive(nullptr, packet)); assert(traces == 0);
    session.account=2;
    assert(hook.CanPacketReceive(&session, packet)); assert(traces == 0);
    session.account=1;
    assert(hook.CanPacketReceive(&session, packet)); assert(traces == 1);
    assert(packet.bytes == original);
    assert(hook.CanPacketSend(&session, packet)); assert(traces == 1);
    for (auto op : {SMSG_TRAINER_BUY_SUCCEEDED, SMSG_TRAINER_BUY_FAILED, SMSG_TRAINER_LIST}) {
        packet.opcode=op; packet.bytes.resize(16);
        assert(hook.CanPacketSend(&session, packet));
    }
    assert(traces == 4);
    packet.opcode=999;
    assert(hook.CanPacketReceive(&session, packet)); assert(traces == 4);
    for (auto op : {CMSG_TRAINER_BUY_SPELL, CMSG_TRAINER_LIST, CMSG_GOSSIP_HELLO}) {
        packet.opcode=op;
        for (size_t n=0; n<8; ++n) {
            packet.bytes.resize(n);
            assert(hook.CanPacketReceive(&session, packet));
        }
    }
    assert(malformed == 24);
    packet.opcode=SMSG_TRAINER_BUY_FAILED; packet.bytes.resize(15);
    assert(hook.CanPacketSend(&session, packet)); assert(malformed == 25);
    configMgr.values["AracTrainerAudit.TraceAccountId"]=true;
    settings.OnAfterConfigLoad(true); assert(traceAccount.load()==1);
    configMgr.values["AracTrainerAudit.Enable"]=false;
    settings.OnAfterConfigLoad(true); assert(traceAccount.load()==0);
}
'''
        source = ROOT / "modules/mod-arac-trainer-audit/src/mod_arac_trainer_trace.cpp"
        with tempfile.TemporaryDirectory(prefix="trainer-packets-") as directory:
            tmp = Path(directory)
            (tmp / "Mocks.h").write_text(mocks, encoding="utf8")
            for name in ("Config", "Log", "Opcodes", "ScriptMgr", "WorldPacket", "WorldSession"):
                (tmp / (name + ".h")).write_text('#include "Mocks.h"\n', encoding="utf8")
            unit = tmp / "test.cpp"
            unit.write_text(source.read_text(encoding="utf8") + cases, encoding="utf8")
            binary = tmp / "test"
            subprocess.run([compiler, "-std=c++17", "-I", str(tmp), str(unit), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_real_module(self):
        compiler = shutil.which(os.environ.get("CXX", "g++"))
        if not compiler:
            self.skipTest("g++/CXX required")
        source = ROOT / "modules/mod-arac-trainer-audit/src/mod_arac_trainer_audit.cpp"
        with tempfile.TemporaryDirectory(prefix="arac-trainer-test-") as directory:
            tmp = Path(directory)
            (tmp / "Mocks.h").write_text(MOCKS, encoding="utf-8")
            for header in ("Config", "Creature", "Log", "Player", "ScriptMgr", "WorldSession"):
                (tmp / (header + ".h")).write_text('#include "Mocks.h"\n', encoding="utf-8")
            unit = tmp / "test.cpp"
            unit.write_text(source.read_text(encoding="utf-8") + CASES, encoding="utf-8")
            binary = tmp / "test"
            subprocess.run([compiler, "-std=c++17", "-I", str(tmp), str(unit),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
