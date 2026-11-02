#pragma once

#include <cstdint>
#include <chrono>
#include <atomic>

namespace ime {

class IClock {
public:
    virtual ~IClock() = default;
    virtual int64_t Now() const = 0;
};

class SystemClock : public IClock {
public:
    int64_t Now() const override {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};

class MockClock : public IClock {
public:
    explicit MockClock(int64_t initialTime = 1700000000) : m_now(initialTime) {}

    int64_t Now() const override {
        return m_now.load(std::memory_order_relaxed);
    }

    void SetTime(int64_t t) {
        m_now.store(t, std::memory_order_relaxed);
    }

    void Advance(int64_t seconds) {
        m_now.fetch_add(seconds, std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> m_now;
};

} // namespace ime

