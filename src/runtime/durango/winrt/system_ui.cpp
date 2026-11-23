#include "runtime/durango/winrt/system_ui.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/winrt/async.hpp"
#include "runtime/durango/winrt/collection.hpp"
#include "runtime/durango/winrt/object.hpp"
#include "runtime/durango/winrt/placeholder.hpp"
#include <activation.h>
#include <cstdio>
#include <filesystem>
#include <mmdeviceapi.h>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <windows.h>
#include <windows.ui.popups.h>
#include <winstring.h>
#include <wrl/wrappers/corewrappers.h>

namespace xv::runtime::durango::winrt {
namespace {
using Microsoft::WRL::Make;
using Microsoft::WRL::Wrappers::HString;

MIDL_INTERFACE("74782F58-DA5D-4E31-9E6E-BD10C91F1BE7")
IKeyboardOptions : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DefaultText(HSTRING * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DefaultText(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TitleText(HSTRING * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_TitleText(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DescriptionText(HSTRING * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DescriptionText(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxLength(UINT32 * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MaxLength(UINT32 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LengthIndicatorVisibility(boolean * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_LengthIndicatorVisibility(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LengthIndicatorThreshold(UINT32 * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_LengthIndicatorThreshold(UINT32 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InputScope(UINT32 * value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_InputScope(UINT32 value) = 0;
};

class KeyboardOptions : public Class<KeyboardOptions, IKeyboardOptions> {
  public:
    static constexpr wchar_t name[] = L"Windows.Xbox.UI.KeyboardOptions";
    STDMETHODIMP get_DefaultText(HSTRING *value) override { return text_.CopyTo(value); }
    STDMETHODIMP put_DefaultText(HSTRING value) override { return text_.Set(value); }
    STDMETHODIMP get_TitleText(HSTRING *value) override { return title_.CopyTo(value); }
    STDMETHODIMP put_TitleText(HSTRING value) override { return title_.Set(value); }
    STDMETHODIMP get_DescriptionText(HSTRING *value) override { return description_.CopyTo(value); }
    STDMETHODIMP put_DescriptionText(HSTRING value) override { return description_.Set(value); }
    STDMETHODIMP get_MaxLength(UINT32 *value) override { return get(max_length_, value); }
    STDMETHODIMP put_MaxLength(UINT32 value) override { return put(max_length_, value); }
    STDMETHODIMP get_LengthIndicatorVisibility(boolean *value) override {
        return get(indicator_, value);
    }
    STDMETHODIMP put_LengthIndicatorVisibility(boolean value) override {
        return put(indicator_, value);
    }
    STDMETHODIMP get_LengthIndicatorThreshold(UINT32 *value) override {
        return get(threshold_, value);
    }
    STDMETHODIMP put_LengthIndicatorThreshold(UINT32 value) override {
        return put(threshold_, value);
    }
    STDMETHODIMP get_InputScope(UINT32 *value) override { return get(scope_, value); }
    STDMETHODIMP put_InputScope(UINT32 value) override { return put(scope_, value); }

  private:
    template <class T> static HRESULT get(T field, T *value) {
        if (!value)
            return E_POINTER;
        *value = field;
        return S_OK;
    }
    template <class T> static HRESULT put(T &field, T value) {
        field = value;
        return S_OK;
    }
    HString text_, title_, description_;
    UINT32 max_length_ = 0, threshold_ = 0, scope_ = 0;
    boolean indicator_ = false;
};
class KeyboardOptionsFactory : public Class<KeyboardOptionsFactory, IActivationFactory> {
  public:
    static constexpr wchar_t name[] = L"Windows.Xbox.UI.KeyboardOptions";
    STDMETHODIMP ActivateInstance(IInspectable **instance) override {
        if (!instance)
            return E_POINTER;
        *instance = Make<KeyboardOptions>().Detach();
        return S_OK;
    }
};

std::wstring text(HSTRING value) {
    UINT32 length = 0;
    const auto *raw = WindowsGetStringRawBuffer(value, &length);
    return {raw, length};
}
// Windows.UI.Popups.MessageDialog: what a title has to say is logged, and the dialog is
// answered at once with no command.
namespace popups = ABI::Windows::UI::Popups;
class MessageDialog : public Class<MessageDialog, popups::IMessageDialog> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Popups.MessageDialog";
    MessageDialog(HSTRING content, HSTRING title) {
        content_.Set(content);
        title_.Set(title);
    }
    STDMETHODIMP get_Title(HSTRING *value) override { return title_.CopyTo(value); }
    STDMETHODIMP put_Title(HSTRING value) override { return title_.Set(value); }
    STDMETHODIMP get_Commands(
        ABI::Windows::Foundation::Collections::IVector<popups::IUICommand *> **value) override {
        if (!commands_)
            commands_ = static_cast<IUnknown *>(make_vector());
        commands_->AddRef();
        *value =
            static_cast<ABI::Windows::Foundation::Collections::IVector<popups::IUICommand *> *>(
                static_cast<void *>(commands_));
        return S_OK;
    }
    STDMETHODIMP get_DefaultCommandIndex(UINT32 *value) override {
        *value = default_;
        return S_OK;
    }
    STDMETHODIMP put_DefaultCommandIndex(UINT32 value) override {
        default_ = value;
        return S_OK;
    }
    STDMETHODIMP get_CancelCommandIndex(UINT32 *value) override {
        *value = cancel_;
        return S_OK;
    }
    STDMETHODIMP put_CancelCommandIndex(UINT32 value) override {
        cancel_ = value;
        return S_OK;
    }
    STDMETHODIMP get_Content(HSTRING *value) override { return content_.CopyTo(value); }
    STDMETHODIMP put_Content(HSTRING value) override { return content_.Set(value); }
    STDMETHODIMP
    ShowAsync(
        ABI::Windows::Foundation::IAsyncOperation<popups::IUICommand *> **operation) override {
        std::fprintf(stderr, "[Runtime] message dialog: \"%ls\": \"%ls\", answered at once\n",
                     text(title_.Get()).c_str(), text(content_.Get()).c_str());
        std::fflush(stderr);
        *operation = static_cast<ABI::Windows::Foundation::IAsyncOperation<popups::IUICommand *> *>(
            make_completed_operation(nullptr));
        return S_OK;
    }
    STDMETHODIMP get_Options(popups::MessageDialogOptions *value) override {
        *value = options_;
        return S_OK;
    }
    STDMETHODIMP put_Options(popups::MessageDialogOptions value) override {
        options_ = value;
        return S_OK;
    }

  private:
    HString content_, title_;
    IUnknown *commands_ = nullptr;
    UINT32 default_ = 0, cancel_ = 0xffffffff;
    popups::MessageDialogOptions options_{};
};
class UICommand : public Class<UICommand, popups::IUICommand> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Popups.UICommand";
    UICommand(HSTRING label, popups::IUICommandInvokedHandler *handler, IInspectable *id)
        : handler_(handler), id_(id) {
        label_.Set(label);
    }
    STDMETHODIMP get_Label(HSTRING *value) override { return label_.CopyTo(value); }
    STDMETHODIMP put_Label(HSTRING value) override { return label_.Set(value); }
    STDMETHODIMP get_Invoked(popups::IUICommandInvokedHandler **value) override {
        return handler_.CopyTo(value);
    }
    STDMETHODIMP put_Invoked(popups::IUICommandInvokedHandler *value) override {
        handler_ = value;
        return S_OK;
    }
    STDMETHODIMP get_Id(IInspectable **value) override { return id_.CopyTo(value); }
    STDMETHODIMP put_Id(IInspectable *value) override {
        id_ = value;
        return S_OK;
    }

  private:
    HString label_;
    Microsoft::WRL::ComPtr<popups::IUICommandInvokedHandler> handler_;
    Microsoft::WRL::ComPtr<IInspectable> id_;
};
class UICommandFactory
    : public Class<UICommandFactory, IActivationFactory, popups::IUICommandFactory> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Popups.UICommand";
    STDMETHODIMP ActivateInstance(IInspectable **instance) override {
        if (!instance)
            return E_POINTER;
        *instance = Make<UICommand>(nullptr, nullptr, nullptr).Detach();
        return S_OK;
    }
    STDMETHODIMP Create(HSTRING label, popups::IUICommand **command) override {
        return CreateWithHandlerAndId(label, nullptr, nullptr, command);
    }
    STDMETHODIMP CreateWithHandler(HSTRING label, popups::IUICommandInvokedHandler *handler,
                                   popups::IUICommand **command) override {
        return CreateWithHandlerAndId(label, handler, nullptr, command);
    }
    STDMETHODIMP CreateWithHandlerAndId(HSTRING label, popups::IUICommandInvokedHandler *handler,
                                        IInspectable *id, popups::IUICommand **command) override {
        if (!command)
            return E_POINTER;
        *command = Make<UICommand>(label, handler, id).Detach();
        return S_OK;
    }
};
class MessageDialogFactory
    : public Class<MessageDialogFactory, IActivationFactory, popups::IMessageDialogFactory> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Popups.MessageDialog";
    STDMETHODIMP ActivateInstance(IInspectable **) override { return E_NOTIMPL; }
    STDMETHODIMP Create(HSTRING content, popups::IMessageDialog **dialog) override {
        return CreateWithTitle(content, nullptr, dialog);
    }
    STDMETHODIMP CreateWithTitle(HSTRING content, HSTRING title,
                                 popups::IMessageDialog **dialog) override {
        if (!dialog)
            return E_POINTER;
        *dialog = Make<MessageDialog>(content, title).Detach();
        return S_OK;
    }
};
// input scopes: number 0x1d, password 0x1f, telephone number 0x20
void *keyboard(surface::TextRequest request, std::uint32_t scope) {
    request.password = scope == 0x1f;
    request.digits = scope == 0x1d || scope == 0x20;
    const bool ask = console_settings().keyboard_dialog;
    std::fprintf(stderr, "[Runtime] system keyboard: \"%ls\"%s\n", request.title.c_str(),
                 ask ? "" : ", answered with the default text");
    std::fflush(stderr);
    return make_text_operation([request = std::move(request), ask](auto finish) mutable {
        if (!ask)
            return finish(request.text);
        auto fallback = request.text;
        // cancelled: the suggested text
        surface::ask_text(std::move(request),
                          [finish, fallback](std::optional<std::wstring> entered) {
                              finish(entered.value_or(fallback));
                          });
    });
}
// on a thread of its own for COM
std::wstring communications_output() {
    std::wstring id;
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            return;
        IMMDeviceEnumerator *devices = nullptr;
        IMMDevice *device = nullptr;
        wchar_t *name = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&devices))) &&
            SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eCommunications, &device)) &&
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
} // namespace

std::wstring console_string(std::wstring_view key) {
    const auto &settings = console_settings();
    if (key == L"gamertag")
        return settings.gamertag;
    if (key == L"xbox_user_id")
        return settings.xbox_user_id;
    // ApplicationData's folders, inside the title's save folder
    for (const auto &[name, folder] :
         {std::pair{L"local_folder", L"LocalState"}, std::pair{L"roaming_folder", L"RoamingState"},
          std::pair{L"temporary_folder", L"TempState"}})
        if (key == name) {
            const auto path = save_directory() / folder;
            std::error_code error;
            std::filesystem::create_directories(path, error);
            return path.wstring();
        }
    if (key == L"local_folder_name")
        return L"LocalState";
    if (key == L"package_folder" || key == L"package_folder_name") {
        std::error_code error;
        const auto folder = package_directory(std::filesystem::current_path(error));
        return key == L"package_folder" ? folder.wstring() : folder.filename().wstring();
    }
    if (key == L"audio_device_id") {
        static const auto id = communications_output();
        return id;
    }
    return {};
}
std::uint32_t console_number(std::wstring_view key) {
    const auto &settings = console_settings();
    return key == L"gamerscore"  ? settings.gamerscore
           : key == L"age_group" ? settings.age_group
                                 : 0;
}
void *audio_devices() {
    if (!console_settings().headset)
        return make_vector_view({});
    return make_vector_view(
        {static_cast<IInspectable *>(shared_placeholder(L"Windows.Xbox.System.AudioDeviceInfo"))});
}
void *show_keyboard(HSTRING value, HSTRING title, HSTRING description, std::uint32_t scope) {
    return keyboard({text(title), text(description), text(value)}, scope);
}
void *show_keyboard_with_options(void *object) {
    auto *const unknown = static_cast<IInspectable *>(object);
    IKeyboardOptions *options = nullptr;
    if (!unknown || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&options))))
        return make_failed_operation(E_INVALIDARG);
    HString value, title, description;
    UINT32 scope = 0, max_length = 0;
    options->get_DefaultText(value.GetAddressOf());
    options->get_TitleText(title.GetAddressOf());
    options->get_DescriptionText(description.GetAddressOf());
    options->get_InputScope(&scope);
    options->get_MaxLength(&max_length);
    options->Release();
    surface::TextRequest request{text(title.Get()), text(description.Get()), text(value.Get())};
    request.max_length = max_length;
    return keyboard(std::move(request), scope);
}
IInspectable *system_ui_class(std::wstring_view name) {
    static const auto options = Make<KeyboardOptionsFactory>();
    static const auto dialogs = Make<MessageDialogFactory>();
    if (name == MessageDialogFactory::name)
        return static_cast<IActivationFactory *>(dialogs.Get());
    static const auto commands = Make<UICommandFactory>();
    if (name == UICommandFactory::name)
        return static_cast<IActivationFactory *>(commands.Get());
    return name == KeyboardOptionsFactory::name ? static_cast<IActivationFactory *>(options.Get())
                                                : nullptr;
}
} // namespace xv::runtime::durango::winrt
