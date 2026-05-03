#include "LlamaMgr.h"
#include "ChatLLMConfig.h"
#include "Log/Log.h"
#include "Obfuscator.h"
#include "llama.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

LlamaMgr& LlamaMgr::instance()
{
    static LlamaMgr instance;
    return instance;
}

bool LlamaMgr::Initialize()
{
    if (!sChatLLMConfig.m_enabled)
        return false;

    llama_backend_init();

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = sChatLLMConfig.m_gpuLayers;

    m_model = llama_model_load_from_file(sChatLLMConfig.m_modelPath.c_str(), modelParams);
    if (!m_model)
    {
        sLog.outError("%s %s", OBFUSCATE("ChatLLM: Failed to load model:").c_str(), sChatLLMConfig.m_modelPath.c_str());
        return false;
    }

    m_vocab = llama_model_get_vocab(m_model);

    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx = sChatLLMConfig.m_maxContext;
    ctxParams.n_threads = sChatLLMConfig.m_numThreads;
    ctxParams.n_threads_batch = sChatLLMConfig.m_numThreads;

    m_ctx = llama_new_context_with_model(m_model, ctxParams);
    if (!m_ctx)
    {
        sLog.outError("%s", OBFUSCATE("ChatLLM: Failed to create context").c_str());
        llama_model_free(m_model);
        m_model = nullptr;
        m_vocab = nullptr;
        return false;
    }

    m_running = true;
    m_worker = std::thread(&LlamaMgr::WorkerLoop, this);

    int totalBytes = llama_model_size(m_model) / (1024 * 1024);
    sLog.outString("%s %d %s %d %s %d %s",
        OBFUSCATE("ChatLLM: Model loaded").c_str(), totalBytes,
        OBFUSCATE("MB, threads=").c_str(), sChatLLMConfig.m_numThreads,
        OBFUSCATE("ctx=").c_str(), sChatLLMConfig.m_maxContext,
        OBFUSCATE("tokens").c_str());
    return true;
}

void LlamaMgr::Shutdown()
{
    m_running = false;
    m_cv.notify_all();

    if (m_worker.joinable())
        m_worker.join();

    if (m_ctx)
    {
        llama_free(m_ctx);
        m_ctx = nullptr;
    }
    if (m_model)
    {
        llama_model_free(m_model);
        m_model = nullptr;
        m_vocab = nullptr;
    }

    llama_backend_free();
}

void LlamaMgr::InferAsync(const std::string& prompt, std::function<void(std::string)> callback)
{
    if (!m_running || !m_model || !m_ctx)
    {
        callback("");
        return;
    }

    ChatLLMRequest req;
    req.prompt = prompt;
    req.callback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(req));
    }
    m_cv.notify_one();
}

void LlamaMgr::WorkerLoop()
{
    while (m_running)
    {
        ChatLLMRequest req;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_queue.empty() || !m_running; });
            if (!m_running) break;
            if (m_queue.empty()) continue;

            req = std::move(m_queue.front());
            m_queue.pop();
        }

        std::string response = DoInference(req.prompt);
        if (req.callback)
            req.callback(response);
    }
}

static int32_t SampleToken(const float* logits, int32_t nVocab, float temp, float topP)
{
    std::vector<std::pair<int32_t, float>> sorted;
    sorted.reserve(nVocab);

    float maxLogit = logits[0];
    for (int32_t i = 0; i < nVocab; ++i)
        if (logits[i] > maxLogit) maxLogit = logits[i];

    float sum = 0.0f;
    for (int32_t i = 0; i < nVocab; ++i)
    {
        float val = std::exp((logits[i] - maxLogit) / temp);
        sorted.push_back({i, val});
        sum += val;
    }

    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    float cumProb = 0.0f;
    size_t cutoff = 0;
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        sorted[i].second /= sum;
        cumProb += sorted[i].second;
        if (cumProb >= topP) { cutoff = i + 1; break; }
    }
    if (cutoff == 0) cutoff = sorted.size();

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    float r = dist(rng);

    float cum = 0.0f;
    for (size_t i = 0; i < cutoff; ++i)
    {
        cum += sorted[i].second;
        if (r <= cum) return sorted[i].first;
    }
    return sorted[0].first;
}

std::string LlamaMgr::DoInference(const std::string& prompt)
{
    int32_t nCtx  = llama_n_ctx(m_ctx);
    int32_t nVocab = llama_vocab_n_tokens(m_vocab);

    std::vector<llama_token> tokens(nCtx);
    int32_t nTokens = llama_tokenize(m_vocab, prompt.c_str(), (int32_t)prompt.size(), tokens.data(), nCtx, true, false);
    if (nTokens < 0)
    {
        nTokens = -nTokens;
        if (nTokens > nCtx) nTokens = nCtx;
    }
    if (nTokens < 1) return "";

    llama_batch batch = llama_batch_get_one(tokens.data(), nTokens);
    if (llama_decode(m_ctx, batch) != 0)
        return "";

    std::string result;
    llama_token eosToken = llama_vocab_eos(m_vocab);

    for (int32_t i = 0; i < sChatLLMConfig.m_maxResponse; ++i)
    {
        const float* logits = llama_get_logits(m_ctx);
        int32_t nextToken = SampleToken(logits, nVocab, sChatLLMConfig.m_temperature, sChatLLMConfig.m_topP);

        if (nextToken == eosToken)
            break;

        char buf[8];
        int32_t len = llama_token_to_piece(m_vocab, nextToken, buf, sizeof(buf), 0, true);
        if (len > 0)
            result.append(buf, len);

        if (result.size() >= 6 && result.compare(result.size() - 6, 6, "\nUser:") == 0)
        {
            result.erase(result.size() - 6);
            break;
        }

        llama_batch singleBatch = llama_batch_get_one(&nextToken, 1);
        if (llama_decode(m_ctx, singleBatch) != 0)
            break;
    }

    return result;
}
