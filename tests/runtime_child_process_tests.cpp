#include <pcsx5/runtime/child_process.h>
#include "process_test_signal.h"
#include <array>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>
#if defined(PCSX5_TEST_LINUX)
#include <csignal>
#include <sys/wait.h>
#endif
namespace rt = pcsx5::runtime;
#if defined(PCSX5_TEST_LINUX)
constexpr auto run_child = rt::run_linux_child;
#else
constexpr auto run_child = rt::run_windows_child;
#endif
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
bool error(const rt::process_run_result& r,rt::process_error e) { return !r && r.error() == e; }
int main(int argc,char** argv) {
    CHECK(argc == 2);
    const std::filesystem::path helper(argv[1]);
    const std::array exit_args{std::string{"exit"}};
    const std::array hang_args{std::string{"hang"}};
    auto nul_path = helper.native(); nul_path.push_back(std::filesystem::path::value_type{});
    nul_path += helper.native();
    CHECK(error(run_child(std::filesystem::path(nul_path),exit_args,1000,{}),rt::process_error::invalid_argument));
    CHECK(error(run_child({},exit_args,1000,{}),rt::process_error::invalid_argument));
    CHECK(error(run_child("relative",exit_args,1000,{}),rt::process_error::invalid_argument));
    CHECK(error(run_child(helper,exit_args,0,{}),rt::process_error::invalid_argument));
    CHECK(error(run_child(helper,exit_args,60001,{}),rt::process_error::invalid_argument));
    const std::array nul{std::string("a\0b",3)};
    CHECK(error(run_child(helper,nul,1000,{}),rt::process_error::invalid_argument));
    const std::vector<std::string> too_many(33,"x");
    CHECK(error(run_child(helper,too_many,1000,{}),rt::process_error::invalid_argument));
    const std::array too_long{std::string(8193,'x')};
    CHECK(error(run_child(helper,too_long,1000,{}),rt::process_error::invalid_argument));
    CHECK(error(run_child(helper / "absent-child",exit_args,1000,{}),rt::process_error::unavailable));
    std::stop_source already;
    CHECK(already.request_stop());
    CHECK((run_child(helper,exit_args,1000,already.get_token()) == rt::process_result{rt::process_outcome::cancelled,0}));
    CHECK(error(run_child({},exit_args,1000,already.get_token()),rt::process_error::invalid_argument));
#if defined(PCSX5_TEST_LINUX)
    // This dedicated test process has no live children or threads at this point.
    struct sigaction original{}, ignored{};
    CHECK(sigaction(SIGCHLD,nullptr,&original) == 0);
    ignored.sa_handler = SIG_IGN; sigemptyset(&ignored.sa_mask);
    CHECK(sigaction(SIGCHLD,&ignored,nullptr) == 0);
    const auto rejected_ignore = run_child(helper,exit_args,1000,{});
    CHECK(sigaction(SIGCHLD,&original,nullptr) == 0);
    CHECK(error(rejected_ignore,rt::process_error::host_failure));
    ignored.sa_handler = SIG_DFL; ignored.sa_flags = SA_NOCLDWAIT;
    CHECK(sigaction(SIGCHLD,&ignored,nullptr) == 0);
    const auto rejected_reap = run_child(helper,exit_args,1000,{});
    CHECK(sigaction(SIGCHLD,&original,nullptr) == 0);
    CHECK(error(rejected_reap,rt::process_error::host_failure));
#else
    const std::array malformed_utf8{std::string("\xc0\xaf",2)};
    CHECK(error(run_child(helper,malformed_utf8,1000,{}),rt::process_error::invalid_argument));
    CHECK(error(run_child(helper,malformed_utf8,1000,already.get_token()),rt::process_error::invalid_argument));
#endif
    CHECK((run_child(helper,exit_args,1000,{}) == rt::process_result{rt::process_outcome::exited,7}));
    const std::array arguments{std::string{"args"},std::string{},std::string{"space value"},std::string{"quote\"value"},
        std::string{"trailing\\"},std::string{"a\\\"b"},std::string{"&|<>$()"}};
    CHECK((run_child(helper,arguments,1000,{}) == rt::process_result{rt::process_outcome::exited,23}));
    const std::array fault_args{std::string{"fault"}};
    const auto fault = run_child(helper,fault_args,5000,{});
#if defined(PCSX5_TEST_LINUX)
    CHECK((fault == rt::process_result{rt::process_outcome::signaled,SIGSEGV}));
#else
    CHECK((fault == rt::process_result{rt::process_outcome::exited,EXCEPTION_ACCESS_VIOLATION}));
#endif
    CHECK((run_child(helper,hang_args,40,{}) == rt::process_result{rt::process_outcome::timed_out,0}));
    {
        process_test_signal started;
        const std::array ready_args{std::string{"ready"},started.name};
        std::stop_source stop;
        std::promise<rt::process_run_result> promise;
        auto future = promise.get_future();
        std::jthread runner([&] { promise.set_value(run_child(helper,ready_args,10000,stop.get_token())); });
        const bool ready = started.ready(); // A real child signaled, not a guessed delay.
        stop.request_stop();
        runner.join();
        CHECK(ready);
        CHECK((future.get() == rt::process_result{rt::process_outcome::cancelled,0}));
    }
    for (unsigned repetition=0; repetition<6; ++repetition) {
        auto first = std::async(std::launch::async,[&] { return run_child(helper,exit_args,1000,{}); });
        auto second = std::async(std::launch::async,[&] { return run_child(helper,arguments,1000,{}); });
        CHECK((first.get() == rt::process_result{rt::process_outcome::exited,7}));
        CHECK((second.get() == rt::process_result{rt::process_outcome::exited,23}));
    }
#if !defined(PCSX5_TEST_LINUX)
    DWORD before{}; CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
#endif
    for (unsigned repetition=0; repetition<12; ++repetition) {
        CHECK((run_child(helper,exit_args,1000,{}) == rt::process_result{rt::process_outcome::exited,7}));
        CHECK((run_child(helper,hang_args,10,{}) == rt::process_result{rt::process_outcome::timed_out,0}));
    }
#if defined(PCSX5_TEST_LINUX)
    int status{}; errno = 0;
    CHECK(waitpid(-1,&status,WNOHANG) == -1 && errno == ECHILD); // This fixture owns all its children.
#else
    DWORD after{}; CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)); CHECK(after == before);
#endif
    std::puts("child-process-v1: literal-args exit fault timeout pre-cancel running-cancel repeated-cleanup PASS");
}
