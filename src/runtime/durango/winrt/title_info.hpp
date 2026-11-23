#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace xv::runtime::durango::winrt {
// What the console knows about the running title from its package manifest: that of the
// package the current directory (the executable's folder while it runs) is in:
//   title_id           the Xbox Live title ID, in decimal as the console gives it
//   service_config_id  the primary service configuration ID
//   sandbox_id         always RETAIL
//   package_name, package_publisher, package_publisher_id, package_family_name,
//   package_full_name  the identity of the package, as Windows.ApplicationModel.PackageId has it
// Empty for what the manifest does not say.
std::wstring title_info(std::wstring_view key);
// The package's version as PackageVersion has it: major, minor, build and revision, 16 bits
// each from the lowest
std::uint64_t title_version();
} // namespace xv::runtime::durango::winrt
