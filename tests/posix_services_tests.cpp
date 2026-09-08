#include <pcsx5/runtime/posix.h>
#include <cstdio>
namespace rt=pcsx5::runtime;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
int main() {
    CHECK(!rt::start_posix_worker(nullptr,nullptr));
    unsigned value{};
    auto worker=rt::start_posix_worker([](void* context) { *static_cast<unsigned*>(context)=42; },&value);
    CHECK(worker && *worker);
    auto joined=(*worker)->join();
    CHECK(joined && *joined==rt::worker_completion::returned && value==42);
    CHECK((*worker)->join()==joined);
    auto throwing=rt::start_posix_worker([](void*) { throw 7; },nullptr);
    CHECK(throwing && *throwing);
    auto caught=(*throwing)->join();
    CHECK(caught && *caught==rt::worker_completion::callback_threw);
    auto frequency=rt::posix_counter_frequency(); CHECK(frequency);
    auto previous=rt::posix_counter_now(); CHECK(previous);
    for (unsigned i=0;i<4096;++i) {
        auto current=rt::posix_counter_now(); CHECK(current && current->count>=previous->count);
        CHECK(rt::elapsed_nanoseconds(*previous,*current,*frequency)); previous=current;
    }
    std::puts("POSIX services: worker publication/repeated join/exception and 4096 monotonic samples PASS");
}
