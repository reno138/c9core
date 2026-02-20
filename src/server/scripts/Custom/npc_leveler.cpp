/*
 * This file is part of the C9Core Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file npc_leveler.cpp
 *
 * NPCs that can level a player to 60, 70, or 80.
 * Registered for:
 *   601004 — Georgebe (Horde, Orgrimmar)
 *   601008 — Jasonmet  (Alliance, Stormwind)
 *
 * Interaction flow:
 *   1. Player right-clicks the NPC.
 *   2. Gossip menu shows level options (only levels ABOVE current level shown).
 *   3. Player selects target level; NPC calls GiveLevel(targetLevel).
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "GossipDef.h"
#include "ScriptedGossip.h"
#include "Log.h"

// ── NPC entry IDs ─────────────────────────────────────────────────────────────
enum LevelerNpcEntry : uint32
{
    NPC_GEORGEBE = 601004,   ///< Horde leveler — Orgrimmar
    NPC_JASONMET = 601008,   ///< Alliance leveler — Stormwind
};

// ── Gossip action IDs (= target level) ───────────────────────────────────────
enum LevelerAction : uint32
{
    ACTION_LEVEL_60 = 60,
    ACTION_LEVEL_70 = 70,
    ACTION_LEVEL_80 = 80,
};

// ── NPC script ────────────────────────────────────────────────────────────────
class npc_leveler : public CreatureScript
{
public:
    explicit npc_leveler(char const* name) : CreatureScript(name) { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        uint8 currentLevel = player->GetLevel();

        ClearGossipMenuFor(player);

        if (currentLevel >= 80)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "You are already level 80. Nothing to do here.", GOSSIP_SENDER_MAIN, 0);
        }
        else
        {
            if (currentLevel < ACTION_LEVEL_60)
                AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                    "|TInterface\\icons\\achievement_level_60:24|t Level me to 60",
                    GOSSIP_SENDER_MAIN, ACTION_LEVEL_60, "Bring me to level 60?", 0, false);

            if (currentLevel < ACTION_LEVEL_70)
                AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                    "|TInterface\\icons\\achievement_level_70:24|t Level me to 70",
                    GOSSIP_SENDER_MAIN, ACTION_LEVEL_70, "Bring me to level 70?", 0, false);

            if (currentLevel < ACTION_LEVEL_80)
                AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                    "|TInterface\\icons\\achievement_level_80:24|t Level me to 80",
                    GOSSIP_SENDER_MAIN, ACTION_LEVEL_80, "Bring me to level 80?", 0, false);
        }

        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);

        if (action == 0)
            return true; // "already max level" dummy option

        uint8 targetLevel = static_cast<uint8>(action);
        uint8 currentLevel = player->GetLevel();

        // Sanity: only level up, never down, and only valid targets
        if (targetLevel != 60 && targetLevel != 70 && targetLevel != 80)
            return true;

        if (currentLevel >= targetLevel)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "You are already level {} or higher.", currentLevel);
            return true;
        }

        LOG_INFO("server.worldserver", "npc_leveler: Leveling player '{}' ({}) from {} to {}",
                 player->GetName(), player->GetGUID().ToString(), currentLevel, targetLevel);

        player->GiveLevel(targetLevel);
        player->InitTalentForLevel();
        player->SetUInt32Value(PLAYER_XP, 0);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "You have been leveled to {}! Enjoy your journey.", targetLevel);

        return true;
    }
};

// ── Script registration ───────────────────────────────────────────────────────
void AddSC_npc_leveler()
{
    new npc_leveler("npc_georgebe");
    new npc_leveler("npc_jasonmet");
}
