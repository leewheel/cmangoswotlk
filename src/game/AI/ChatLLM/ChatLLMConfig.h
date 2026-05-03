#ifndef _CHAT_LLM_CONFIG_H
#define _CHAT_LLM_CONFIG_H

#include "Common.h"
#include <string>

struct ChatLLMConfig
{
    static ChatLLMConfig& instance();

    void LoadFromConfig();

    bool    m_enabled;
    std::string m_modelPath;
    int32   m_numThreads;
    int32   m_gpuLayers;
    int32   m_maxContext;
    int32   m_maxResponse;
    float   m_temperature;
    float   m_topP;

    ChatLLMConfig() : m_enabled(false), m_numThreads(4), m_gpuLayers(0), m_maxContext(2048), m_maxResponse(200), m_temperature(0.8f), m_topP(0.9f) {}
};

#define sChatLLMConfig ChatLLMConfig::instance()

#endif
