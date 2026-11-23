#pragma once
#include <guiddef.h>
#include <string_view>

struct IInspectable;

namespace xv::runtime::durango::winrt {
// Activation factory of Windows.Xbox.Input.Gamepad, null for other names. There is one
// gamepad, always connected, whose readings come from xv::input.
IInspectable *input_class(std::wstring_view name);
// An IVectorView of the one gamepad, with a reference to it.
void *gamepad_list();
// The same gamepad as the IController that Controller::Controllers lists.
void *controller_list();
// and alone, with a reference
void *controller();
// An interface asked of one of the gamepad's facets: IGamepad is the gamepad itself, and the
// interface titles ask for to leave a controller out is not there. False for any other.
bool gamepad_interface(std::wstring_view name, const GUID &iid, void **object, long &result);
} // namespace xv::runtime::durango::winrt
