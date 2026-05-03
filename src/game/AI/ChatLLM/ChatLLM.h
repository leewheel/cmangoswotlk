#ifndef _CHAT_LLM_H
#define _CHAT_LLM_H

#include "Common.h"
#include <string>

class Player;
class Creature;
class WorldSession;

namespace ChatLLM
{
    void LoadConfig();
    void Initialize();
    void Shutdown();

    // Player whispers an NPC → LLM response
    void HandleNPCWhisper(Player* player, Creature* creature, const std::string& message);

    // Player whispers a PlayerBot → LLM response
    void HandleBotWhisper(Player* player, Player* targetBot, const std::string& message);

    // Build ChatML formatted prompt
    std::string BuildPrompt(const std::string& systemPrompt, const std::string& userMessage,
                            const std::string& charName, const std::string& playerName);
}

#endif
