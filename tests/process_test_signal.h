#pragma once
#include <string>
#if defined(PCSX5_TEST_LINUX)
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
// Test-only readiness handshake: no timing guess that a child has started.
class process_test_signal {
public:
    process_test_signal() {
#if defined(PCSX5_TEST_LINUX)
        name = "/pcsx5-phase6-" + std::to_string(getpid());
        handle_ = sem_open(name.c_str(),O_CREAT|O_EXCL,0600,0);
#else
        name = "Local\\pcsx5-phase6-" + std::to_string(GetCurrentProcessId());
        const std::wstring wide(name.begin(),name.end());
        handle_ = CreateEventW(nullptr,TRUE,FALSE,wide.c_str());
        if (handle_ && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(handle_); handle_ = nullptr; }
#endif
    }
    ~process_test_signal() {
#if defined(PCSX5_TEST_LINUX)
        if (handle_ != SEM_FAILED) { sem_close(handle_); sem_unlink(name.c_str()); }
#else
        if (handle_) CloseHandle(handle_);
#endif
    }
    process_test_signal(const process_test_signal&) = delete;
    process_test_signal& operator=(const process_test_signal&) = delete;
    process_test_signal(process_test_signal&&) = delete;
    process_test_signal& operator=(process_test_signal&&) = delete;
    bool ready() const {
#if defined(PCSX5_TEST_LINUX)
        if (handle_ == SEM_FAILED) return false;
        timespec deadline{};
        if (clock_gettime(CLOCK_REALTIME,&deadline) != 0) return false;
        deadline.tv_sec += 5;
        int result;
        do { result = sem_timedwait(handle_,&deadline); } while(result == -1 && errno == EINTR);
        return result == 0;
#else
        return handle_ && WaitForSingleObject(handle_,5000) == WAIT_OBJECT_0;
#endif
    }
    static bool notify(const std::string& name) {
#if defined(PCSX5_TEST_LINUX)
        auto signal = sem_open(name.c_str(),0);
        if (signal == SEM_FAILED) return false;
        const bool success = sem_post(signal) == 0;
        sem_close(signal); return success;
#else
        const std::wstring wide(name.begin(),name.end());
        const auto signal = OpenEventW(EVENT_MODIFY_STATE,FALSE,wide.c_str());
        if (!signal) return false;
        const bool success = SetEvent(signal) != FALSE;
        CloseHandle(signal); return success;
#endif
    }
    std::string name;
private:
#if defined(PCSX5_TEST_LINUX)
    sem_t* handle_{SEM_FAILED};
#else
    HANDLE handle_{};
#endif
};
