#include "ChatLLMConfig.h"
#include "Config/Config.h"

ChatLLMConfig& ChatLLMConfig::instance()
{
    static ChatLLMConfig instance;
    return instance;
}

void ChatLLMConfig::LoadFromConfig()
{
    m_enabled       = sConfig.GetBoolDefault("ChatLLM.Enabled", false);
    m_modelPath     = sConfig.GetStringDefault("ChatLLM.ModelPath", "ChatMod/Gemma-4-E4B-Claude-Abliterated.Q4_K_M.gguf");
    m_numThreads    = sConfig.GetIntDefault("ChatLLM.NumThreads", 4);
    m_gpuLayers     = sConfig.GetIntDefault("ChatLLM.GPULayers", 0);
    m_maxContext    = sConfig.GetIntDefault("ChatLLM.MaxContext", 2048);
    m_maxResponse   = sConfig.GetIntDefault("ChatLLM.MaxResponse", 200);
    m_temperature   = sConfig.GetFloatDefault("ChatLLM.Temperature", 0.8f);
    m_topP          = sConfig.GetFloatDefault("ChatLLM.TopP", 0.9f);

    if (m_enabled)
        sLog.outString("ChatLLM: enabled, model=%s threads=%d gpu_layers=%d", m_modelPath.c_str(), m_numThreads, m_gpuLayers);
}
