#ifndef _LLAMA_MGR_H
#define _LLAMA_MGR_H

#include "Common.h"
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

struct llama_model;
struct llama_context;
struct llama_vocab;

struct ChatLLMRequest
{
    std::string prompt;
    std::function<void(std::string)> callback;
};

class LlamaMgr
{
public:
    static LlamaMgr& instance();

    bool Initialize();
    void Shutdown();

    void InferAsync(const std::string& prompt, std::function<void(std::string)> callback);

    LlamaMgr() : m_model(nullptr), m_ctx(nullptr), m_vocab(nullptr), m_running(false) {}
    ~LlamaMgr() { Shutdown(); }

private:
    void WorkerLoop();
    std::string DoInference(const std::string& prompt);

    llama_model*    m_model;
    llama_context*  m_ctx;
    const llama_vocab* m_vocab;

    std::queue<ChatLLMRequest> m_queue;
    std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::thread     m_worker;
    std::atomic_bool m_running;
};

#define sLlamaMgr LlamaMgr::instance()

#endif
