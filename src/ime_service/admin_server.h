#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <future>
#include "../common/admin_protocol.h"
#include "../core/engine/ime_engine.h"

namespace ime {

class AdminServer {
public:
    explicit AdminServer(ImeEngine& engine, std::wstring pipeName = IME_ADMIN_PIPE_NAME);
    ~AdminServer();

    bool Start();
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    std::wstring GetPipeName() const { return m_pipeName; }

private:
    struct ClientWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void ListenerThread(std::promise<bool> readyPromise);
    void ClientHandlerThread(HANDLE hPipe);
    void CleanUpFinishedWorkersLocked();

    std::string HandleRequest(const std::string& requestJson, uint32_t requestId);

    ImeEngine& m_engine;
    std::wstring m_pipeName;

    std::atomic<bool> m_running{false};
    std::thread m_listenerThread;
    std::vector<ClientWorker> m_clientWorkers;
    std::mutex m_threadsMutex;

    std::vector<HANDLE> m_activeClientPipes;
    std::mutex m_pipesMutex;

    HANDLE m_stopEvent = nullptr;
};

} // namespace ime

