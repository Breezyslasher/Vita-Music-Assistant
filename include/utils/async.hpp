#pragma once
#include <functional>
#include <thread>
#include <borealis.hpp>
#ifdef __vita__
#include <pthread.h>
#endif

namespace vita_ma {

template<typename T>
inline void asyncTask(std::function<T()> task, std::function<void(T)> callback) {
    std::thread([task, callback]() {
        T result = task();
        brls::sync([callback, result]() { callback(result); });
    }).detach();
}

inline void asyncTask(std::function<void()> task, std::function<void()> callback) {
    std::thread([task, callback]() {
        task();
        brls::sync([callback]() { callback(); });
    }).detach();
}

inline void asyncRun(std::function<void()> task) {
    std::thread([task]() { task(); }).detach();
}

inline void asyncRunLargeStack(std::function<void()> task, size_t stackSize = 512 * 1024) {
#ifdef __vita__
    auto* taskCopy = new std::function<void()>(task);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackSize);
    pthread_create(&thread, &attr, [](void* arg) -> void* {
        auto* fn = static_cast<std::function<void()>*>(arg);
        (*fn)();
        delete fn;
        return nullptr;
    }, taskCopy);
    pthread_attr_destroy(&attr);
    pthread_detach(thread);
#else
    std::thread([task]() { task(); }).detach();
#endif
}

} // namespace vita_ma
