#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <guiddef.h>

namespace xv::runtime::durango::winrt {
// What a method of a console interface does when the feature behind it is absent.
// Signatures come from how titles call them: the arguments passed and the size of the
// result read back.
enum class Inert : std::uint8_t {
    accept,         // takes its arguments and succeeds: setters, event removal
    zero8,          // HRESULT get(T *result) with a 1-byte result
    true8,          // same, result is true
    zero32,         // same, 4 bytes
    zero64,         // same, 8 bytes: null object, empty string
    object,         // HRESULT get(T **result) returning an inert object of type `result`
    lookup,         // HRESULT get(key, T **result) returning an inert object of type `result`
    lookup2,        // HRESULT get(key, key, T **result), likewise
    operation2,     // HRESULT start(argument, IAsyncOperation<T> **), finished at once likewise
    action2,        // HRESULT start(argument, IAsyncAction **), finished at once
    list_operation, // HRESULT start(IAsyncOperation<IVectorView<T>> **): finished, nothing found
    offline,        // HRESULT start(..., IAsyncOperation<T> **) of an online service: the operation
                    // fails at once. The result is argument number `argument`.
    action_at,      // HRESULT start(..., IAsyncAction **), finished at once; result in `argument`
    operation_at,   // like operation, with the result in argument number `argument`
    operation,      // HRESULT start(IAsyncOperation<T> **): finished at once with an inert `result`
    action,         // HRESULT start(IAsyncAction **): finished at once
    one32,          // HRESULT get(T *result) with a 4-byte result of 1
    one64,          // same, 8 bytes
    string,         // HRESULT get(HSTRING *result): the text in `result`
    title_string,   // same, the title_info() named `result`
    title_version,  // HRESULT get(PackageVersion *result): the package's
    shared,         // HRESULT get(T **result): the one object of type `result` there is
    shared2,        // HRESULT get(key, T **result): likewise, whatever the key
    shared_list,    // HRESULT get(IVectorView<T> **): a view with that one object
    empty_list,     // HRESULT get(IVectorView<T> **): an empty view
    empty_list2,    // HRESULT find(argument, IVectorView<T> **): nothing found
    one_item,       // HRESULT get(IVectorView<T> **): a view with one inert object of type `result`
    no_items,       // IVectorView::GetMany of an empty collection
    event_add,      // HRESULT add(handler, EventRegistrationToken *token); never raised
    controllers,    // HRESULT get(IVectorView<IController> **): the one gamepad; in
                    // argument `argument` when that is set
    object_at,      // HRESULT make(..., T **) returning an inert `result` in argument `argument`
    vector,         // HRESULT get(IVector<T> **): a vector of the object's own, empty at first
    zero_operation_at,  // HRESULT start(..., IAsyncOperation<enum> **), finished at once with 0;
                        // result in `argument`
    null_at,            // HRESULT get(..., T **): nothing, in argument `argument`
    true8_at,           // HRESULT get(..., bool *): true, in argument `argument`
    never,              // HRESULT get(DateTime *result): the latest time there is
    zero32_at,          // HRESULT get(..., T *): 4 bytes of zero, in argument `argument`
    one32_at,           // the same, 1
    controller_at,      // HRESULT get(..., IController **): the one gamepad, in `argument`
    list_operation_at,  // like list_operation, with the result in argument `argument`
    failed_action_at,   // HRESULT start(..., IAsyncAction **) that fails with `error` at once
    failed_progress_at, // likewise, IAsyncOperationWithProgress<T, P> **
    number32,           // HRESULT get(T *result) with the 4-byte value `argument`
    console_string,     // HRESULT get(HSTRING *result): the console_string() named `result`
    console32,          // HRESULT get(T *result): the 4-byte console_number() named `result`
    audio_devices,      // HRESULT get(IVectorView<AudioDeviceInfo> **): the user's headset
    keyboard, // ShowVirtualKeyboardAsync(text, title, description, scope, IAsyncOperation **)
    keyboard_options, // ShowVirtualKeyboardWithOptionsAsync(options, IAsyncOperation **)
};
struct InertMethod {
    Inert kind;
    const wchar_t *result = nullptr; // type or text, for the kinds that have one
    unsigned argument = 0;           // the kinds `..._at` and offline: which argument is the
                                     // result, from 1; number32: the value
    long error = 0;                  // the failed kinds: the HRESULT
};
// The interface a C++/CX caller uses without asking for it: the default interface of an
// object it was handed.
inline constexpr GUID direct{
    0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
// null if nothing is known about this method of `type`. Methods are numbered from
// 0 = QueryInterface.
const InertMethod *inert_method(const wchar_t *type, const GUID &iid, std::size_t method);
} // namespace xv::runtime::durango::winrt
