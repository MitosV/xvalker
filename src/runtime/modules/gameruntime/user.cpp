#include "runtime/common/runtime.hpp"
#include "runtime/modules/gameinput.hpp"
#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <thread>

// XUser: one user, signed in, who never signs out: the one of Options > Profiles. Its handle
// is the same object whoever asks, and nothing of it comes from Xbox Live.
namespace xv::runtime::modules::gameruntime {
namespace {
struct User {
} user;
constexpr std::uint64_t local_id = 1;
const char add_identity[] = "XUserAddAsync";
const char privilege_identity[] = "XUserResolvePrivilegeWithUiAsync";
const char token_identity[] = "XUserGetTokenAndSignatureAsync";
const char picture_identity[] = "XUserGetGamerPictureAsync";
const char issue_identity[] = "XUserResolveIssueWithUiAsync";
const char sign_out_identity[] = "XUserSignOutAsync";
const char controller_identity[] = "XUserFindControllerForUserWithUiAsync";
constexpr HRESULT e_user_not_found = static_cast<HRESULT>(0x89245104);
std::atomic<std::uint64_t> tokens{0};

// XV_LOCAL_USER=1: the user is a made-up local one (see xbox_user_id, gamertag, token_async)
bool local_user() {
    static const bool enabled = [] {
        const auto *const text = std::getenv("XV_LOCAL_USER");
        return text && *text == '1';
    }();
    return enabled;
}
constexpr std::uint64_t local_xuid = 2535400000000001;
constexpr HRESULT e_offline = static_cast<HRESULT>(0x80072EFD); // WSAECONNREFUSED-like: no network

std::uint64_t xbox_user_id() {
    if (local_user())
        return local_xuid;
    return std::wcstoull(console_settings().xbox_user_id.c_str(), nullptr, 10);
}
std::string gamertag() {
    if (local_user()) {
        const auto *const name = std::getenv("XV_GAMERTAG");
        return name && *name ? name : "Player";
    }
    const auto &wide = console_settings().gamertag;
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), text.data(), size,
                        nullptr, nullptr);
    return text;
}
HRESULT hand_out(User **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &user;
    return S_OK;
}

HRESULT WINAPI duplicate_handle(void *, User *, User **duplicated) { return hand_out(duplicated); }
void WINAPI close_handle(void *, User *) {}
std::int32_t WINAPI compare(void *, User *first, User *second) {
    return first == second ? 0 : first < second ? -1 : 1;
}
HRESULT WINAPI get_max_users(void *, std::uint32_t *count) {
    if (!count)
        return E_POINTER;
    *count = 1;
    return S_OK;
}
HRESULT WINAPI add_async(void *, std::uint32_t, XAsyncBlock *block) {
    std::vector<std::uint8_t> payload(sizeof(User *));
    User *const handle = &user;
    std::memcpy(payload.data(), &handle, sizeof handle);
    return finish(block, add_identity, S_OK, std::move(payload));
}
HRESULT WINAPI add_result(void *, XAsyncBlock *block, User **handle) {
    return result(block, add_identity, sizeof *handle, handle, nullptr);
}
HRESULT WINAPI add_by_id_async(void *, std::uint64_t id, XAsyncBlock *block) {
    return id == xbox_user_id() ? add_async(nullptr, 0, block)
                                : finish(block, add_identity, e_user_not_found);
}
// XUserIsSignOutPresent is false
HRESULT WINAPI sign_out_async(void *, User *, XAsyncBlock *block) {
    return finish(block, sign_out_identity, E_NOTIMPL);
}
HRESULT WINAPI sign_out_result(void *, XAsyncBlock *block) {
    return result(block, sign_out_identity, 0, nullptr, nullptr);
}
// remote connect and SPOP prompts are never raised: no operation to cancel or complete
HRESULT WINAPI prompt_handlers(void *, Queue *, void *, void *) { return S_OK; }
HRESULT WINAPI prompt_operation(void *, void *, std::uint32_t) { return E_INVALIDARG; }
// APP_LOCAL_DEVICE_ID (32 bytes) of the GameInput gamepad
HRESULT WINAPI find_controller_async(void *, User *, XAsyncBlock *block) {
    return finish(
        block, controller_identity, S_OK,
        std::vector<std::uint8_t>(std::begin(gamepad_device_id), std::end(gamepad_device_id)));
}
HRESULT WINAPI find_controller_result(void *, XAsyncBlock *block, void *device) {
    return result(block, controller_identity, 32, device, nullptr);
}
HRESULT WINAPI get_local_id(void *, User *, std::uint64_t *id) {
    if (!id)
        return E_POINTER;
    *id = local_id;
    return S_OK;
}
HRESULT WINAPI find_by_local_id(void *, std::uint64_t id, User **handle) {
    return id == local_id ? hand_out(handle) : E_INVALIDARG;
}
HRESULT WINAPI get_id(void *, User *, std::uint64_t *id) {
    if (!id)
        return E_POINTER;
    *id = xbox_user_id();
    return S_OK;
}
HRESULT WINAPI find_by_id(void *, std::uint64_t id, User **handle) {
    return id == xbox_user_id() ? hand_out(handle) : E_INVALIDARG;
}
HRESULT WINAPI get_is_guest(void *, User *, bool *guest) {
    if (!guest)
        return E_POINTER;
    *guest = false;
    return S_OK;
}
HRESULT WINAPI get_state(void *, User *, std::uint32_t *state) {
    if (!state)
        return E_POINTER;
    *state = 0; // signed in
    return S_OK;
}
// the first kits' form, without a component: the classic gamertag
HRESULT WINAPI get_gamertag_classic(void *, User *, std::size_t size, char *buffer,
                                    std::size_t *used) {
    return copy_text(gamertag().substr(0, 15), size, buffer, used);
}
// classic, modern, the modern one's suffix, modern with its suffix
HRESULT WINAPI get_gamertag(void *, User *, std::uint32_t component, std::size_t size, char *buffer,
                            std::size_t *used) {
    if (component > 3)
        return E_INVALIDARG;
    return copy_text(component == 2 ? std::string() : gamertag(), size, buffer, used);
}
HRESULT WINAPI get_age_group(void *, User *, std::uint32_t *group) {
    if (!group)
        return E_POINTER;
    *group = console_settings().age_group;
    return S_OK;
}
HRESULT WINAPI check_privilege(void *, User *, std::uint32_t, std::uint32_t, bool *has,
                               std::uint32_t *reason) {
    if (!has)
        return E_POINTER;
    *has = true;
    if (reason)
        *reason = 0;
    return S_OK;
}
HRESULT WINAPI resolve_privilege_async(void *, User *, std::uint32_t, std::uint32_t,
                                       XAsyncBlock *block) {
    return finish(block, privilege_identity, S_OK);
}
HRESULT WINAPI resolve_privilege_result(void *, XAsyncBlock *block) {
    return result(block, privilege_identity, 0, nullptr, nullptr);
}
// Xbox Live tokens: there is no service to give one
HRESULT WINAPI token_async(void *, User *, std::uint32_t, const void *, const void *, std::size_t,
                           const void *, std::size_t, const void *, XAsyncBlock *block) {
    return finish(block, token_identity, local_user() ? e_offline : E_FAIL);
}
HRESULT WINAPI token_result_size(void *, XAsyncBlock *block, std::size_t *size) {
    return result_size(block, size);
}
HRESULT WINAPI token_result(void *, XAsyncBlock *block, std::size_t size, void *buffer, void **data,
                            std::size_t *used) {
    if (data)
        *data = nullptr;
    return result(block, token_identity, size, buffer, used);
}
HRESULT WINAPI picture_async(void *, User *, std::uint32_t, XAsyncBlock *block) {
    return finish(block, picture_identity, E_FAIL);
}
HRESULT WINAPI picture_result(void *, XAsyncBlock *block, std::size_t size, void *buffer,
                              std::size_t *used) {
    return result(block, picture_identity, size, buffer, used);
}
HRESULT WINAPI resolve_issue_async(void *, User *, const void *, XAsyncBlock *block) {
    return finish(block, issue_identity, S_OK);
}
HRESULT WINAPI resolve_issue_result(void *, XAsyncBlock *block) {
    return result(block, issue_identity, 0, nullptr, nullptr);
}
// events of a user who never changes: a token, and no event
HRESULT WINAPI register_event(void *, Queue *, void *, void *, std::uint64_t *token) {
    if (!token)
        return E_POINTER;
    *token = ++tokens;
    return S_OK;
}
bool WINAPI unregister_event(void *, std::uint64_t, bool) { return true; }
// XUserDeviceAssociationChange
struct Association {
    std::uint8_t device_id[32];
    std::uint64_t old_user, new_user;
};
struct AssociationEvent {
    void *context;
    void(CALLBACK *callback)(void *context, const Association *change);
};
void CALLBACK association_replayed(void *context, bool canceled) {
    const std::unique_ptr<AssociationEvent> event{static_cast<AssociationEvent *>(context)};
    if (!canceled) {
        Association change{{}, 0, local_id};
        std::memcpy(change.device_id, gamepad_device_id, sizeof gamepad_device_id);
        event->callback(event->context, &change);
    }
    gamepad_associated();
}
// the one association there is, the gamepad with the user, is replayed on registration
HRESULT WINAPI register_association(void *, Queue *queue, void *context,
                                    void(CALLBACK *callback)(void *, const Association *),
                                    std::uint64_t *token) {
    if (!token)
        return E_POINTER;
    *token = ++tokens;
    if (callback) {
        auto *const event = new AssociationEvent{context, callback};
        if (FAILED(submit(queue, Port::completion, 0, event, &association_replayed)))
            delete event;
    }
    return S_OK;
}
HRESULT WINAPI sign_out_deferral(void *, void **deferral) {
    if (!deferral)
        return E_POINTER;
    *deferral = &user;
    return S_OK;
}
void WINAPI close_deferral(void *, void *) {}
// The user's headset is the host's communications device. On a thread of its own for COM.
std::wstring communications_endpoint(bool capture) {
    std::wstring id;
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            return;
        IMMDeviceEnumerator *devices = nullptr;
        IMMDevice *device = nullptr;
        wchar_t *name = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&devices))) &&
            SUCCEEDED(devices->GetDefaultAudioEndpoint(capture ? eCapture : eRender,
                                                       eCommunications, &device)) &&
            SUCCEEDED(device->GetId(&name)))
            id = name;
        CoTaskMemFree(name);
        if (device)
            device->Release();
        if (devices)
            devices->Release();
        CoUninitialize();
    }).join();
    return id;
}
// XUserDefaultAudioEndpointKind: communication render, communication capture
HRESULT WINAPI default_audio_endpoint(void *, std::uint64_t, std::uint32_t kind, std::size_t size,
                                      wchar_t *buffer, std::size_t *used) {
    if (kind > 1)
        return E_INVALIDARG;
    const auto id = communications_endpoint(kind == 1);
    if (id.empty())
        return static_cast<HRESULT>(0x80070490); // ERROR_NOT_FOUND: no such device
    if (used)
        *used = id.size() + 1;
    if (!buffer || size < id.size() + 1)
        return e_not_sufficient_buffer;
    std::memcpy(buffer, id.c_str(), (id.size() + 1) * sizeof(wchar_t));
    return S_OK;
}
// every input device is the one user's
HRESULT WINAPI find_for_device(void *, const void *, User **handle) { return hand_out(handle); }
bool WINAPI is_store_user(void *, User *) { return true; }
} // namespace

void install_user() {
    implement("XUserDuplicateHandle", &duplicate_handle);
    implement("XUserCloseHandle", &close_handle);
    implement("XUserCompare", &compare);
    implement("XUserGetMaxUsers", &get_max_users);
    implement("XUserAddAsync", &add_async);
    implement("XUserAddResult", &add_result);
    implement("XUserAddByIdWithUiAsync", &add_by_id_async);
    implement("XUserAddByIdWithUiResult", &add_result);
    implement("XUserSignOutAsync", &sign_out_async);
    implement("XUserSignOutResult", &sign_out_result);
    implement("XUserPlatformRemoteConnectSetEventHandlers", &prompt_handlers);
    implement("XUserPlatformSpopPromptSetEventHandlers", &prompt_handlers);
    implement("XUserPlatformRemoteConnectCancelPrompt", &prompt_operation);
    implement("XUserPlatformSpopPromptComplete", &prompt_operation);
    implement("XUserFindControllerForUserWithUiAsync", &find_controller_async);
    implement("XUserFindControllerForUserWithUiResult", &find_controller_result);
    implement("XUserGetLocalId", &get_local_id);
    implement("XUserFindUserByLocalId", &find_by_local_id);
    implement("XUserGetId", &get_id);
    implement("XUserFindUserById", &find_by_id);
    implement("XUserGetIsGuest", &get_is_guest);
    implement("XUserGetState", &get_state);
    implement("XUserGetGamertagClassic", &get_gamertag_classic);
    implement("XUserGetGamertag", &get_gamertag);
    implement("XUserGetAgeGroup", &get_age_group);
    implement("XUserCheckPrivilege", &check_privilege);
    implement("XUserResolvePrivilegeWithUiAsync", &resolve_privilege_async);
    implement("XUserResolvePrivilegeWithUiResult", &resolve_privilege_result);
    implement("XUserGetTokenAndSignatureAsync", &token_async);
    implement("XUserGetTokenAndSignatureUtf16Async", &token_async);
    implement("XUserGetTokenAndSignatureResultSize", &token_result_size);
    implement("XUserGetTokenAndSignatureUtf16ResultSize", &token_result_size);
    implement("XUserGetTokenAndSignatureResult", &token_result);
    implement("XUserGetTokenAndSignatureUtf16Result", &token_result);
    implement("XUserGetGamerPictureAsync", &picture_async);
    implement("XUserGetGamerPictureResultSize", &token_result_size);
    implement("XUserGetGamerPictureResult", &picture_result);
    implement("XUserResolveIssueWithUiAsync", &resolve_issue_async);
    implement("XUserResolveIssueWithUiUtf16Async", &resolve_issue_async);
    implement("XUserResolveIssueWithUiResult", &resolve_issue_result);
    implement("XUserResolveIssueWithUiUtf16Result", &resolve_issue_result);
    implement("XUserRegisterForChangeEvent", &register_event);
    implement("XUserUnregisterForChangeEvent", &unregister_event);
    implement("XUserRegisterForDeviceAssociationChanged", &register_association);
    implement("XUserGetDefaultAudioEndpointUtf16", &default_audio_endpoint);
    // the default audio endpoint never changes here
    implement("XUserRegisterForDefaultAudioEndpointUtf16Changed", &register_event);
    implement("XUserUnregisterForDefaultAudioEndpointUtf16Changed", &unregister_event);
    implement("XUserUnregisterForDeviceAssociationChanged", &unregister_event);
    implement("XUserGetSignOutDeferral", &sign_out_deferral);
    implement("XUserCloseSignOutDeferralHandle", &close_deferral);
    implement("XUserFindForDevice", &find_for_device);
    implement("XUserIsStoreUser", &is_store_user);
}
} // namespace xv::runtime::modules::gameruntime
