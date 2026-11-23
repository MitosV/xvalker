#include "runtime/durango/winrt/placeholder.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/winrt/async.hpp"
#include "runtime/durango/winrt/collection.hpp"
#include "runtime/durango/winrt/gamepad.hpp"
#include "runtime/durango/winrt/inert.hpp"
#include "runtime/durango/winrt/system_ui.hpp"
#include "runtime/durango/winrt/title_info.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <inspectable.h>
#include <intrin.h>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <weakreference.h>
#include <windows.h>
#include <winstring.h>

namespace xv::runtime::durango::winrt {
namespace {
constexpr std::size_t method_count = 96;
struct Placeholder {
    void *const *vtable;
    std::atomic<unsigned long> references{1};
    std::wstring name;
    GUID iid;
    std::mutex mutex;
    std::map<std::size_t, void *> members; // by method: what Inert::vector handed out
};
std::string text(const GUID &iid) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2],
                  iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    return buffer;
}
constexpr GUID inspectable{
    0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
constexpr GUID activation_factory{0x00000035, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
// IMarshal: saying yes to it would make COM call into the placeholder to marshal it
constexpr GUID marshal{0x00000003, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
// IReferenceTracker: what XAML's own objects have. A managed runtime asks every object for
// it, and takes one that answers for part of XAML's graph of references.
constexpr GUID reference_tracker{
    0x11d3b13a, 0x180e, 0x4789, {0xa8, 0xbe, 0x77, 0x12, 0x88, 0x28, 0x93, 0xe6}};

std::atomic<std::int64_t> tokens{0};

class Weak final : public IWeakReference {
  public:
    explicit Weak(IUnknown *target) : target_(target) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IWeakReference))
            return E_NOINTERFACE;
        *object = this;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }
    STDMETHODIMP Resolve(REFIID iid, IInspectable **object) override {
        return target_->QueryInterface(iid, reinterpret_cast<void **>(object));
    }

  private:
    std::atomic<ULONG> references_{1};
    IUnknown *target_;
};
class WeakSource final : public IWeakReferenceSource {
  public:
    explicit WeakSource(IUnknown *target) : target_(target) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid != __uuidof(IWeakReferenceSource))
            return target_->QueryInterface(iid, object);
        *object = this;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }
    STDMETHODIMP GetWeakReference(IWeakReference **reference) override {
        if (!reference)
            return E_POINTER;
        *reference = new Weak(target_);
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1};
    IUnknown *target_;
};

HRESULT STDMETHODCALLTYPE query(Placeholder *self, const GUID &iid, void **object) {
    if (!object)
        return E_POINTER;
    *object = nullptr;
    constexpr GUID unknown{0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    constexpr GUID agile{
        0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
    if (iid == unknown || iid == inspectable || iid == agile || iid == self->iid) {
        ++self->references;
        *object = self;
        return S_OK;
    }
    if (iid == __uuidof(IWeakReferenceSource))
        return weak_reference_source(self, object);
    // another interface of the same object: its methods are told apart by the IID
    return placeholder_interface(self->name.c_str(), iid, object);
}
ULONG STDMETHODCALLTYPE add_ref(Placeholder *self) { return ++self->references; }
// never freed: a placeholder is a diagnostic, and its facets do not share a lifetime
ULONG STDMETHODCALLTYPE release(Placeholder *self) { return --self->references; }
HRESULT STDMETHODCALLTYPE get_iids(Placeholder *, ULONG *count, GUID **iids) {
    *count = 0;
    *iids = nullptr;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE get_class_name(Placeholder *self, HSTRING *name) {
    return WindowsCreateString(self->name.c_str(), static_cast<UINT32>(self->name.size()), name);
}
HRESULT STDMETHODCALLTYPE get_trust_level(Placeholder *, int *level) {
    *level = 0;
    return S_OK;
}
// (delegate, token *): an object whose vtable is in the title, and a stack slot
bool looks_like_event_add(void *first, void *second) {
    const auto *tib = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
    if (second < tib->StackLimit || second >= tib->StackBase)
        return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!first || !VirtualQuery(first, &memory, sizeof memory) || memory.State != MEM_COMMIT ||
        !(memory.Protect & (PAGE_READWRITE | PAGE_READONLY)))
        return false;
    return detail::in_title(*static_cast<void **>(first));
}

// The interface C++/CX compares two objects through ({DE0CBAEC-...}): Equals(Object ^, bool *)
// is its one method. The facets of a class are one object here, so another placeholder of
// the same class is the same.
constexpr GUID equatable{
    0xde0cbaec, 0xe077, 0x4e92, {0x84, 0xbe, 0xc6, 0xd4, 0x8d, 0xca, 0x30, 0x06}};
bool same_object(const Placeholder *self, void *other);
// The one of Object::ToString ({DE0CBAEB-...}): ToString(String ^*), the name of the class
constexpr GUID printable{
    0xde0cbaeb, 0x8065, 0x4a45, {0x96, 0xb1, 0xc9, 0xd4, 0x43, 0xf9, 0xba, 0xb3}};

template <std::size_t Index>
HRESULT STDMETHODCALLTYPE method(Placeholder *self, void *first, void *second, void *third,
                                 void *fourth, void *a5, void *a6, void *a7, void *a8, void *a9,
                                 void *a10, void *a11) {
    if (Index == 6 && self->iid == equatable && second) {
        *static_cast<std::uint8_t *>(second) = same_object(self, first);
        return S_OK;
    }
    if (Index == 6 && self->iid == printable && first)
        return get_class_name(self, static_cast<HSTRING *>(first));
    if (const auto *known = inert_method(self->name.c_str(), self->iid, Index)) {
        void *const arguments[] = {first, second, third, fourth, a5, a6, a7, a8, a9, a10, a11};
        void **const at = known->argument >= 1 && known->argument <= std::size(arguments)
                              ? static_cast<void **>(arguments[known->argument - 1])
                              : nullptr;
        switch (known->kind) {
        case Inert::accept:
            break;
        case Inert::zero8:
            *static_cast<std::uint8_t *>(first) = 0;
            break;
        case Inert::true8_at:
            if (at)
                *reinterpret_cast<std::uint8_t *>(at) = 1;
            break;
        case Inert::never:
            *static_cast<std::int64_t *>(first) = INT64_MAX;
            break;
        case Inert::zero32_at:
            if (at)
                *reinterpret_cast<std::uint32_t *>(at) = 0;
            break;
        case Inert::controller_at:
            if (at)
                *at = controller();
            break;
        case Inert::one32_at:
            if (at)
                *reinterpret_cast<std::uint32_t *>(at) = 1;
            break;
        case Inert::true8:
            *static_cast<std::uint8_t *>(first) = 1;
            break;
        case Inert::zero32:
            *static_cast<std::uint32_t *>(first) = 0;
            break;
        case Inert::zero64:
            *static_cast<std::uint64_t *>(first) = 0;
            break;
        case Inert::object:
            *static_cast<void **>(first) = make_placeholder(known->result, inspectable);
            break;
        case Inert::lookup:
            *static_cast<void **>(second) = make_placeholder(known->result, inspectable);
            break;
        case Inert::lookup2:
            *static_cast<void **>(third) = make_placeholder(known->result, inspectable);
            break;
        case Inert::operation2:
            *static_cast<void **>(second) =
                make_completed_operation(make_placeholder(known->result, inspectable));
            break;
        case Inert::action2:
            *static_cast<void **>(second) = make_completed_action();
            break;
        case Inert::list_operation:
            *static_cast<void **>(first) = make_completed_operation(make_vector_view({}));
            break;
        case Inert::offline: // "the network is not connected", which is the truth
            if (!at)
                return E_NOTIMPL;
            *at = make_failed_operation(HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE));
            break;
        case Inert::action_at:
            if (!at)
                return E_NOTIMPL;
            *at = make_completed_action();
            break;
        case Inert::operation_at:
            if (!at)
                return E_NOTIMPL;
            *at = make_completed_operation(make_placeholder(known->result, inspectable));
            break;
        case Inert::object_at:
            if (!at)
                return E_NOTIMPL;
            *at = make_placeholder(known->result, inspectable);
            break;
        case Inert::zero_operation_at:
            if (!at)
                return E_NOTIMPL;
            *at = make_completed_value(0, 4);
            break;
        case Inert::null_at:
            if (!at)
                return E_NOTIMPL;
            *at = nullptr;
            break;
        case Inert::list_operation_at:
            if (!at)
                return E_NOTIMPL;
            *at = make_completed_operation(make_vector_view({}));
            break;
        case Inert::failed_action_at:
            if (!at)
                return E_NOTIMPL;
            *at = make_failed_action(known->error);
            break;
        case Inert::failed_progress_at:
            if (!at)
                return E_NOTIMPL;
            *at = make_failed_progress_operation(known->error);
            break;
        case Inert::number32:
            *static_cast<std::uint32_t *>(first) = known->argument;
            break;
        case Inert::console_string: {
            const auto value = winrt::console_string(known->result);
            return WindowsCreateString(value.c_str(), static_cast<UINT32>(value.size()),
                                       static_cast<HSTRING *>(first));
        }
        case Inert::console32:
            *static_cast<std::uint32_t *>(first) = console_number(known->result);
            break;
        case Inert::audio_devices:
            *static_cast<void **>(first) = audio_devices();
            break;
        case Inert::keyboard:
            *static_cast<void **>(a5) =
                show_keyboard(static_cast<HSTRING>(first), static_cast<HSTRING>(second),
                              static_cast<HSTRING>(third),
                              static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(fourth)));
            break;
        case Inert::keyboard_options:
            *static_cast<void **>(second) = show_keyboard_with_options(first);
            break;
        case Inert::operation:
            *static_cast<void **>(first) =
                make_completed_operation(make_placeholder(known->result, inspectable));
            break;
        case Inert::action:
            *static_cast<void **>(first) = make_completed_action();
            break;
        case Inert::one32:
            *static_cast<std::uint32_t *>(first) = 1;
            break;
        case Inert::one64:
            *static_cast<std::uint64_t *>(first) = 1;
            break;
        case Inert::string:
            return WindowsCreateString(known->result, static_cast<UINT32>(wcslen(known->result)),
                                       static_cast<HSTRING *>(first));
        case Inert::title_string: {
            const auto value = title_info(known->result);
            return WindowsCreateString(value.c_str(), static_cast<UINT32>(value.size()),
                                       static_cast<HSTRING *>(first));
        }
        case Inert::title_version:
            *static_cast<std::uint64_t *>(first) = title_version();
            break;
        case Inert::shared:
            *static_cast<void **>(first) = shared_placeholder(known->result);
            break;
        case Inert::shared2:
            *static_cast<void **>(second) = shared_placeholder(known->result);
            break;
        case Inert::shared_list:
            *static_cast<void **>(first) =
                make_vector_view({static_cast<IInspectable *>(shared_placeholder(known->result))});
            break;
        case Inert::empty_list:
            *static_cast<void **>(first) = make_vector_view({});
            break;
        case Inert::vector: {
            const std::lock_guard lock(self->mutex);
            auto &member = self->members[Index];
            if (!member)
                member = make_vector();
            static_cast<IUnknown *>(member)->AddRef();
            *static_cast<void **>(first) = member;
            break;
        }
        case Inert::empty_list2:
            *static_cast<void **>(second) = make_vector_view({});
            break;
        case Inert::one_item:
            *static_cast<void **>(first) = make_vector_view(
                {static_cast<IInspectable *>(make_placeholder(known->result, inspectable))});
            break;
        case Inert::no_items:
            *static_cast<std::uint32_t *>(fourth) = 0;
            break;
        case Inert::controllers:
            *(at ? at : static_cast<void **>(first)) = controller_list();
            break;
        case Inert::event_add:
            *static_cast<std::int64_t *>(second) = ++tokens;
            break;
        }
        return S_OK;
    }
    return unknown_method(self->name.c_str(), text(self->iid).c_str(), Index, first, second,
                          _ReturnAddress());
}
// IActivationFactory::ActivateInstance, the one method whose meaning never depends on the class
HRESULT STDMETHODCALLTYPE activate(Placeholder *self, void **instance) {
    *instance = make_placeholder(self->name.c_str(), inspectable);
    return S_OK;
}
template <std::size_t... Index>
constexpr std::array<void *, method_count> make_vtable(std::index_sequence<Index...>) {
    return {reinterpret_cast<void *>(&query),
            reinterpret_cast<void *>(&add_ref),
            reinterpret_cast<void *>(&release),
            reinterpret_cast<void *>(&get_iids),
            reinterpret_cast<void *>(&get_class_name),
            reinterpret_cast<void *>(&get_trust_level),
            reinterpret_cast<void *>(&method<Index + 6>)...};
}
const std::array<void *, method_count> vtable =
    make_vtable(std::make_index_sequence<method_count - 6>{});
const std::array<void *, method_count> factory_vtable = [] {
    auto table = vtable;
    table[6] = reinterpret_cast<void *>(&activate);
    return table;
}();
} // namespace

long unknown_method(const wchar_t *type, const char *iid, unsigned method, void *first,
                    void *second, void *caller) {
    const bool event = looks_like_event_add(first, second);
    std::fprintf(stderr, "[Runtime] unimplemented WinRT call %ls {%s} method %u from %s%s\n", type,
                 iid, method, detail::guest_location(caller).c_str(),
                 event ? " (arguments look like an event registration)" : "");
    std::fflush(stderr);
    // XV_WINRT_GUESS: keep going on that guess to collect several unknown methods per run
    static const bool guess = std::getenv("XV_WINRT_GUESS") != nullptr;
    if (event && guess) {
        *static_cast<std::int64_t *>(second) = ++tokens;
        return S_OK;
    }
    return E_NOTIMPL;
}

void *shared_placeholder(const wchar_t *name) {
    static std::mutex mutex;
    static std::map<std::wstring, Placeholder *> objects;
    const std::lock_guard lock(mutex);
    auto &object = objects[name];
    if (!object)
        object = static_cast<Placeholder *>(make_placeholder(name, inspectable));
    ++object->references;
    return object;
}

long placeholder_interface(const wchar_t *name, const GUID &iid, void **object) {
    *object = nullptr;
    if (iid == marshal || iid == reference_tracker)
        return E_NOINTERFACE;
    if (long result; gamepad_interface(name, iid, object, result))
        return result;
    if (iid == __uuidof(IWeakReferenceSource)) {
        void *target = nullptr;
        placeholder_interface(name, inspectable, &target);
        return weak_reference_source(target, object);
    }
    // A facet is only its class and interface: titles that ask every frame get the same one.
    static std::mutex mutex;
    static std::map<std::pair<std::wstring, std::string>, Placeholder *> facets;
    const std::lock_guard lock(mutex);
    auto &facet = facets[{name, text(iid)}];
    if (!facet)
        facet = static_cast<Placeholder *>(make_placeholder(name, iid));
    ++facet->references;
    *object = facet;
    return S_OK;
}

long weak_reference_source(void *target, void **object) {
    if (!target || !object)
        return E_POINTER;
    *object = new WeakSource(static_cast<IUnknown *>(target));
    return S_OK;
}

namespace {
bool same_object(const Placeholder *self, void *other) {
    if (other == self)
        return true;
    MEMORY_BASIC_INFORMATION memory{};
    if (!other || !VirtualQuery(other, &memory, sizeof memory) || memory.State != MEM_COMMIT)
        return false;
    const auto *const table = *static_cast<void *const *const *>(other);
    return (table == vtable.data() || table == factory_vtable.data()) &&
           static_cast<const Placeholder *>(other)->name == self->name;
}
} // namespace

void *make_placeholder(const wchar_t *name, const GUID &iid) {
    // once per class and interface: titles may make the same one every frame
    static std::mutex mutex;
    static std::set<std::pair<std::wstring, std::string>> reported;
    if (const std::lock_guard lock(mutex); reported.emplace(name, text(iid)).second) {
        std::fprintf(stderr, "[Runtime] placeholder for %ls {%s}\n", name, text(iid).c_str());
        std::fflush(stderr);
    }
    return new Placeholder{(iid == activation_factory ? factory_vtable : vtable).data(), 1, name,
                           iid};
}
} // namespace xv::runtime::durango::winrt
