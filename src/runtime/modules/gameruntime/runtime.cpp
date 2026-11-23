#include "runtime/common/runtime.hpp"
#include "runtime/modules/gameruntime.hpp"
#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string_view>
#include <utility>

// The Gaming Runtime is two halves. The title's half is a static library: every function of
// the GDK (XUserAddAsync, XGameSaveCreateContainer...) loads xgameruntime.dll, asks its
// QueryApiImpl for the table of the function's API, and calls one slot of the table with
// the table first and its own arguments after it. The system's half is that DLL, which is
// what this is.
namespace xv::runtime::modules::gameruntime {
namespace {
constexpr std::size_t table_slots = 96, max_interfaces = 32;
using Slot = void *;
struct Table {
    Slot *vtable;
    const Interface *info;
};
std::array<std::array<Slot, table_slots>, max_interfaces> vtables;
std::array<Table, max_interfaces> tables;

const char *method_name(const Interface &info, std::size_t slot) {
    for (const auto &method : info.methods)
        if (method.slot == slot)
            return method.name;
    return "a method no kit here names";
}
// A slot nothing implements: says which function the title called, once, and fails
template <std::size_t Id>
HRESULT WINAPI unimplemented(Table *table, std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Gaming Runtime call: %s (%s slot %zu) (%llx, %llx, "
                     "%llx) from %s\n",
                     method_name(*table->info, Id % table_slots), table->info->name,
                     Id % table_slots, a, b, c, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
template <std::size_t... Id> void fill_unimplemented(std::index_sequence<Id...>) {
    // a list rather than a fold: clang caps how deep a fold of 3072 terms may nest
    const Slot slots[] = {reinterpret_cast<Slot>(&unimplemented<Id>)...};
    for (std::size_t id = 0; id < sizeof...(Id); ++id)
        vtables[id / table_slots][id % table_slots] = slots[id];
}
// the tables live as long as the process, and one answers for every info of its API
HRESULT WINAPI table_query(Table *table, const GUID *, void **object) {
    if (!object)
        return E_POINTER;
    *object = table;
    return S_OK;
}
ULONG WINAPI table_reference(Table *) { return 1; }

void build_tables() {
    static std::once_flag once;
    std::call_once(once, [] {
        fill_unimplemented(std::make_index_sequence<table_slots * max_interfaces>{});
        const auto interfaces = known_interfaces();
        for (std::size_t i = 0; i < interfaces.size() && i < max_interfaces; ++i) {
            vtables[i][0] = reinterpret_cast<Slot>(&table_query);
            vtables[i][1] = vtables[i][2] = reinterpret_cast<Slot>(&table_reference);
            tables[i] = {vtables[i].data(), &interfaces[i]};
        }
        install_task_queue();
        install_async();
        install_user();
        install_save();
        install_system();
        install_store();
        install_ui();
        install_streaming();
    });
}

std::string guid_text(const GUID &guid) {
    char text[40];
    std::snprintf(text, sizeof text, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
                  guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}
HRESULT WINAPI query_api(const GUID *api, const GUID *iid, void **table) {
    if (!api || !iid || !table)
        return E_POINTER;
    build_tables();
    *table = nullptr;
    const auto interfaces = known_interfaces();
    constexpr GUID any{};
    for (const bool exact : {true, false})
        for (std::size_t i = 0; i < interfaces.size() && i < max_interfaces; ++i)
            if (interfaces[i].api == *api && interfaces[i].iid == (exact ? *iid : any)) {
                *table = &tables[i];
                return S_OK;
            }
    std::fprintf(stderr,
                 "[Runtime] the title asks for a Gaming Runtime API nothing here has: %s, "
                 "info %s\n",
                 guid_text(*api).c_str(), guid_text(*iid).c_str());
    std::fflush(stderr);
    return E_NOINTERFACE;
}
HRESULT WINAPI initialize(std::uint64_t, std::uint64_t) {
    build_tables();
    return S_OK;
}
HRESULT WINAPI initialize_ex(std::uint64_t, std::uint64_t, std::uint64_t) {
    build_tables();
    return S_OK;
}
void WINAPI uninitialize() {}
// what the title's half reports when one of its own checks fails
void WINAPI error_report(HRESULT result, const char *message) {
    std::fprintf(stderr, "[Runtime] Gaming Runtime error 0x%08lx: %s\n",
                 static_cast<unsigned long>(result), message ? message : "");
    std::fflush(stderr);
}
HRESULT WINAPI can_unload() { return S_FALSE; }
} // namespace

void implement(const char *name, void *function) {
    const auto interfaces = known_interfaces();
    for (std::size_t i = 0; i < interfaces.size() && i < max_interfaces; ++i)
        for (const auto &method : interfaces[i].methods)
            if (method.slot < table_slots && std::string_view(method.name) == name)
                vtables[i][method.slot] = function;
}

HRESULT copy_text(const std::string &text, std::size_t size, char *buffer, std::size_t *used) {
    if (used)
        *used = text.size() + 1;
    if (!buffer || size < text.size() + 1)
        return e_not_sufficient_buffer;
    std::memcpy(buffer, text.c_str(), text.size() + 1);
    return S_OK;
}
} // namespace xv::runtime::modules::gameruntime

namespace xv::runtime::modules {
void install_gameruntime(Registry &registry) {
    using namespace gameruntime;
    const char *const module = "xgameruntime.dll";
    registry.add(module, "QueryApiImpl", reinterpret_cast<void *>(&query_api));
    registry.add(module, "InitializeApiImpl", reinterpret_cast<void *>(&initialize));
    registry.add(module, "InitializeApiImplEx", reinterpret_cast<void *>(&initialize_ex));
    registry.add(module, "UninitializeApiImpl", reinterpret_cast<void *>(&uninitialize));
    registry.add(module, "XErrorReport", reinterpret_cast<void *>(&error_report));
    registry.add(module, "DllCanUnloadNow", reinterpret_cast<void *>(&can_unload));
}
} // namespace xv::runtime::modules
