#include "../runtime/src/windows_memory_api.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>

using namespace pcsx5::runtime;
namespace {
enum class operation { none, reserve, commit, protect, decommit, release, query, foreign_query };
struct fixture {
    operation fail{};
    DWORD error{ERROR_INVALID_FUNCTION};
    void* base{};
    unsigned allocations{}, releases{}, queries{}, mutations{};
    bool freed{};
    bool mark_release_failure{};
};

bool injected(fixture& state, operation op) noexcept {
    if (state.fail != op) return false;
    SetLastError(state.error);
    return true;
}
void* allocate(void* context, void* address, SIZE_T size, DWORD kind, DWORD access) noexcept {
    auto& state = *static_cast<fixture*>(context);
    ++state.allocations;
    if (injected(state, kind == MEM_RESERVE ? operation::reserve : operation::commit)) return nullptr;
    const auto result = VirtualAlloc(address, size, kind, access);
    if (kind == MEM_RESERVE) state.base = result;
    return result;
}
BOOL free_memory(void* context, void* address, SIZE_T size, DWORD kind) noexcept {
    auto& state = *static_cast<fixture*>(context);
    if (kind == MEM_RELEASE) ++state.releases;
    else ++state.mutations;
    if (injected(state, kind == MEM_RELEASE ? operation::release : operation::decommit)) {
        if (kind == MEM_RELEASE && state.mark_release_failure) {
            std::fputs("INJECTED_RELEASE_FAILURE\n", stdout);
            std::fflush(stdout);
        }
        return FALSE;
    }
    const auto result = VirtualFree(address, size, kind);
    if (result && kind == MEM_RELEASE) {
        MEMORY_BASIC_INFORMATION info{};
        state.freed = VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE;
    }
    return result;
}
BOOL protect(void* context, void* address, SIZE_T size, DWORD access, DWORD* old) noexcept {
    auto& state = *static_cast<fixture*>(context);
    ++state.mutations;
    if (injected(state, operation::protect)) return FALSE;
    return VirtualProtect(address, size, access, old);
}
SIZE_T query(void* context, const void* address, MEMORY_BASIC_INFORMATION* info, SIZE_T size) noexcept {
    auto& state = *static_cast<fixture*>(context);
    ++state.queries;
    if (injected(state, operation::query)) return 0;
    const auto result = VirtualQuery(address, info, size);
    if (result && state.fail == operation::foreign_query) info->AllocationBase = nullptr;
    return result;
}
detail::windows_memory_api api(fixture& state) {
    return {&state, allocate, free_memory, protect, query};
}
template<class T> bool fails(const memory_result<T>& result, memory_error error) {
    return !result && result.error() == error;
}
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #expression); return 1; } } while (false)
} // namespace

int main(int argc, char** argv) {
    const auto geometry = windows_memory_geometry();
    CHECK(geometry);
    const auto page = geometry->page_size();
    fixture state;
    if (argc == 2 && std::string_view(argv[1]) == "destructor-failure") {
        auto owner = detail::reserve_windows_memory_with_api(page, api(state));
        CHECK(owner && *owner);
        std::set_terminate([] {
            std::fputs("EXPECTED_DESTRUCTOR_TERMINATION\n", stdout);
            std::fflush(stdout);
            std::_Exit(73);
        });
        state.fail = operation::release;
        state.mark_release_failure = true;
        owner->reset();
        return 2; // Must not return normally.
    }
    CHECK(argc == 1);
    CHECK(fails(detail::reserve_windows_memory_with_api(page, {}), memory_error::unsupported));
    state.fail = operation::reserve;
    for (const auto error : {ERROR_NOT_ENOUGH_MEMORY, ERROR_OUTOFMEMORY, ERROR_COMMITMENT_LIMIT}) {
        state.error = error;
        CHECK(fails(detail::reserve_windows_memory_with_api(page, api(state)), memory_error::out_of_memory));
        CHECK(state.base == nullptr && state.releases == 0);
    }
    state.error = ERROR_INVALID_FUNCTION;
    CHECK(fails(detail::reserve_windows_memory_with_api(page, api(state)), memory_error::host_failure));
    state.fail = operation::none;
    auto owner = detail::reserve_windows_memory_with_api(page, api(state));
    CHECK(owner && *owner && state.base);
    state.fail = operation::commit;
    CHECK(fails((*owner)->commit(0, page, memory_access::read_write), memory_error::host_failure));
    state.fail = operation::none;
    auto info = (*owner)->query(0);
    CHECK(info && info->state == page_state::reserved);
    CHECK((*owner)->commit(0, page, memory_access::read_write));
    const std::array<std::byte, 1> pattern{std::byte{42}};
    std::array<std::byte, 1> output{};
    CHECK((*owner)->write(0, pattern));
    state.fail = operation::protect;
    CHECK(fails((*owner)->protect(0, page, memory_access::none), memory_error::host_failure));
    state.fail = operation::decommit;
    CHECK(fails((*owner)->decommit(0, page), memory_error::host_failure));
    state.fail = operation::none;
    CHECK((*owner)->read(0, output) && output == pattern);
    for (const auto failure : {operation::query, operation::foreign_query}) {
        state.fail = failure;
        const auto mutations = state.mutations;
        CHECK(fails((*owner)->protect(0, page, memory_access::none), memory_error::host_failure));
        CHECK(fails((*owner)->decommit(0, page), memory_error::host_failure));
        CHECK(state.mutations == mutations);
        output[0] = std::byte{9};
        CHECK(fails((*owner)->read(0, output), memory_error::host_failure));
        CHECK(output[0] == std::byte{9});
        CHECK(fails((*owner)->write(0, output), memory_error::host_failure));
    }
    state.fail = operation::none;
    CHECK((*owner)->read(0, output) && output == pattern);
    const auto queries = state.queries;
    CHECK(fails((*owner)->protect(page, page, memory_access::none), memory_error::invalid_range));
    CHECK(fails((*owner)->read(page, output), memory_error::invalid_range));
    CHECK(state.queries == queries); // Rejected before any native lookup.
    state.fail = operation::release;
    CHECK(fails((*owner)->release(), memory_error::host_failure));
    CHECK((*owner)->read(0, output) && output == pattern); // Ownership retained.
    state.fail = operation::none;
    owner->reset(); // Destructor retries the failed explicit release.
    CHECK(state.releases == 2 && state.freed);

    fixture other;
    {
        auto automatic = detail::reserve_windows_memory_with_api(page, api(other));
        CHECK(automatic && *automatic);
        CHECK((*automatic)->commit(0, page, memory_access::read_write));
    }
    CHECK(other.releases == 1 && other.freed);
    fixture explicit_close;
    {
        auto explicit_owner = detail::reserve_windows_memory_with_api(page, api(explicit_close));
        CHECK(explicit_owner && *explicit_owner);
        CHECK((*explicit_owner)->release());
        CHECK((*explicit_owner)->release());
    }
    CHECK(explicit_close.releases == 1 && explicit_close.freed);
    fixture left, right;
    {
        auto left_owner = detail::reserve_windows_memory_with_api(page, api(left));
        auto right_owner = detail::reserve_windows_memory_with_api(page, api(right));
        CHECK(left_owner && *left_owner && right_owner && *right_owner);
        CHECK(left.base != right.base);
        left.fail = operation::commit;
        CHECK(fails((*left_owner)->commit(0, page, memory_access::read_write), memory_error::host_failure));
        CHECK((*right_owner)->commit(0, page, memory_access::read_write));
        CHECK((*right_owner)->write(0, pattern));
        left.fail = operation::release;
        CHECK(fails((*left_owner)->release(), memory_error::host_failure));
        CHECK((*right_owner)->read(0, output) && output == pattern);
        CHECK((*right_owner)->release());
        CHECK(right.releases == 1 && right.freed && !left.freed);
        left.fail = operation::none;
    }
    CHECK(left.releases == 2 && left.freed && right.releases == 1);
    std::puts("native failure response and observed cleanup checks passed");
    return 0;
}
