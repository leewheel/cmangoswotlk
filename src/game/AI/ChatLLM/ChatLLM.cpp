#include "ChatLLM.h"
#include "ChatLLMConfig.h"
#include "LlamaMgr.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Player.h"
#include "Entities/Creature.h"
#include "Globals/ObjectMgr.h"
#include "Server/WorldSession.h"
#include "World/World.h"
#include "Chat/Chat.h"
#include "Log/Log.h"
#include <sstream>
#include <algorithm>

namespace ChatLLM
{

void LoadConfig()
{
    sChatLLMConfig.LoadFromConfig();
}

void Initialize()
{
    if (!sChatLLMConfig.m_enabled)
        return;

    if (!sLlamaMgr.Initialize())
    {
        sLog.outError("ChatLLM: Initialization failed, disabling");
        sChatLLMConfig.m_enabled = false;
        return;
    }
}

void Shutdown()
{
    if (sChatLLMConfig.m_enabled)
        sLlamaMgr.Shutdown();
}

static std::string GetPersonalityPrompt(uint32 personalityId)
{
    auto result = WorldDatabase.PQuery(
        "SELECT prompt FROM chat_llm_personality WHERE id = %u", personalityId);
    if (!result)
        return "";

        Field* fields = result->Fetch();
        return fields[0].GetCppString();
}

static std::string GetNPCPrompt(uint32 creatureEntry)
{
    auto result = WorldDatabase.PQuery(
        "SELECT p.prompt FROM chat_llm_npc n "
        "INNER JOIN chat_llm_personality p ON p.id = n.personality_id "
        "WHERE n.creature_entry = %u AND n.enabled = 1", creatureEntry);
    if (!result)
        return "";

        Field* fields = result->Fetch();
        return fields[0].GetCppString();
}

static std::string GetBotPrompt(uint32 guidLow)
{
    auto result = CharacterDatabase.PQuery(
        "SELECT p.prompt FROM chat_llm_bot b "
        "INNER JOIN world.chat_llm_personality p ON p.id = b.personality_id "
        "WHERE b.guid = %u AND b.enabled = 1", guidLow);
    if (!result)
        return "";

        Field* fields = result->Fetch();
        return fields[0].GetCppString();
}

std::string BuildPrompt(const std::string& systemPrompt, const std::string& userMessage,
                        const std::string& charName, const std::string& playerName)
{
    std::ostringstream ss;
    ss << "<bos>";
    if (!systemPrompt.empty())
        ss << "<start_of_turn>system\n" << systemPrompt << "<end_of_turn>\n";
    ss << "<start_of_turn>user\n"
       << playerName << " says: " << userMessage << "<end_of_turn>\n"
       << "<start_of_turn>model\n";
    return ss.str();
}

// Simple string replace helper
static void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
}

// Trim whitespace
static std::string Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

void HandleNPCWhisper(Player* player, Creature* creature, const std::string& message)
{
    if (!sChatLLMConfig.m_enabled || !player || !creature)
        return;

    uint32 creatureEntry = creature->GetEntry();
    std::string systemPrompt = GetNPCPrompt(creatureEntry);
    if (systemPrompt.empty())
    {
        ChatHandler(player).PSendSysMessage("This creature has no AI personality set.");
        return;
    }

    std::string prompt = BuildPrompt(systemPrompt, message,
        creature->GetName(), player->GetName());
    uint32 guidLow = player->GetGUIDLow();
    std::string creatureName = creature->GetName();

    sLlamaMgr.InferAsync(prompt, [guidLow, creatureName](std::string response)
    {
        if (response.empty()) return;

        Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guidLow));
        if (!player || !player->GetSession()) return;

        ReplaceAll(response, "<end_of_turn>", "");
        ReplaceAll(response, "<eos>", "");
        response = Trim(response);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, response.c_str(), LANG_UNIVERSAL,
            CHAT_TAG_NONE, ObjectGuid(), creatureName.c_str());
        player->GetSession()->SendPacket(data);
    });
}

void HandleBotWhisper(Player* player, Player* targetBot, const std::string& message)
{
    if (!sChatLLMConfig.m_enabled || !player || !targetBot)
        return;

    uint32 botGuidLow = targetBot->GetGUIDLow();
    std::string systemPrompt = GetBotPrompt(botGuidLow);
    if (systemPrompt.empty())
    {
        ChatHandler(player).PSendSysMessage("This bot has no AI personality set.");
        return;
    }

    std::string prompt = BuildPrompt(systemPrompt, message,
        targetBot->GetName(), player->GetName());
    uint32 playerGuidLow = player->GetGUIDLow();
    std::string botName = targetBot->GetName();

    sLlamaMgr.InferAsync(prompt, [playerGuidLow, botName](std::string response)
    {
        if (response.empty()) return;

        Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, playerGuidLow));
        if (!player || !player->GetSession()) return;

        ReplaceAll(response, "<end_of_turn>", "");
        ReplaceAll(response, "<eos>", "");
        response = Trim(response);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, response.c_str(), LANG_UNIVERSAL,
            CHAT_TAG_NONE, ObjectGuid(), botName.c_str());
        player->GetSession()->SendPacket(data);
    });
}

}  // namespace ChatLLM
