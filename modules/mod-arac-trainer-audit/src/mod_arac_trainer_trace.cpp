// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Targeted trainer protocol diagnostics. No packet mutation or player access
 * from the send hook (it can run outside the player's map thread).
 * Config is atomic because config reload and packet hooks can overlap.
 */
#include "Config.h"
#include "Log.h"
#include "Opcodes.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <atomic>

namespace
{
    std::atomic<uint32> traceAccount{0};

    void Trace(WorldSession* session, WorldPacket const& packet, bool received)
    {
        uint32 const account = traceAccount.load(std::memory_order_relaxed);
        if (!account || !session || session->GetAccountId() != account)
            return;

        uint16 const opcode = packet.GetOpcode();
        bool const buy = received && opcode == CMSG_TRAINER_BUY_SPELL;
        bool const success = !received && opcode == SMSG_TRAINER_BUY_SUCCEEDED;
        bool const failure = !received && opcode == SMSG_TRAINER_BUY_FAILED;
        bool const list = !received && opcode == SMSG_TRAINER_LIST;
        bool const open = received && (opcode == CMSG_TRAINER_LIST || opcode == CMSG_GOSSIP_HELLO);
        if (!buy && !success && !failure && !list && !open)
            return;

        // ByteBuffer::read(offset) is const: do not consume the original packet.
        char const* kind = buy ? "BUY" : success ? "SUCCEEDED" : failure ? "FAILED" : list ? "LIST" : "OPEN";
        if (packet.size() < (failure ? 16u : (buy || success || list) ? 12u : 8u))
        {
            LOG_WARN("module", "[trainer-trace] account={} {} {} malformed size={}",
                     account, received ? "RX" : "TX", kind, packet.size());
            return;
        }
        uint64 const guid = packet.read<uint64>(0);
        uint32 const spell = (buy || success || failure) ? packet.read<uint32>(8) : 0;
        uint32 const reason = failure ? packet.read<uint32>(12) : 0;
        LOG_INFO("module", "[trainer-trace] account={} {} {} guid={} spell={} reason={} bytes={}",
                 account, received ? "RX" : "TX", kind, guid, spell, reason, packet.size());
    }
}

class arac_trainer_trace_config : public WorldScript
{
public:
    arac_trainer_trace_config() : WorldScript("arac_trainer_trace_config", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }
    void OnAfterConfigLoad(bool /*reload*/) override
    {
        uint32 const account = sConfigMgr->GetOption<bool>("AracTrainerAudit.Enable", true)
            ? sConfigMgr->GetOption<uint32>("AracTrainerAudit.TraceAccountId", 0) : 0;
        traceAccount.store(account, std::memory_order_relaxed);
        LOG_INFO("module", "[trainer-trace] account={} (0=disabled)", account);
    }
};

class arac_trainer_trace_packets : public ServerScript
{
public:
    arac_trainer_trace_packets() : ServerScript("arac_trainer_trace_packets",
        { SERVERHOOK_CAN_PACKET_RECEIVE, SERVERHOOK_CAN_PACKET_SEND }) { }
    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        Trace(session, packet, true);
        return true;
    }
    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        Trace(session, packet, false);
        return true;
    }
};

void AddSC_mod_arac_trainer_trace()
{
    new arac_trainer_trace_config();
    new arac_trainer_trace_packets();
}
