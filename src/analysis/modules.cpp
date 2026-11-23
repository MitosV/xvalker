#include "analysis/modules.hpp"
#include "analysis/xbdepends_data.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <string>

namespace xv::analysis {
namespace {
using json = nlohmann::json;
template <std::size_t N>
bool contains(const std::array<std::string_view, N> &sorted, std::string_view value) {
    return std::binary_search(sorted.begin(), sorted.end(), value);
}
// regexes taken from xbdepends.cpp, matched against the lowercase name
bool matches(const std::string &name, const std::regex &pattern) {
    return std::regex_match(name, pattern);
}
const std::regex api_set{"(api|ext)-ms-win-([a-z0-9]*-)*l[0-9]+-[0-9]+-[0-9]+\\.dll"};
const std::regex os_crt{
    "((api-ms-win-crt-([a-z0-9]*-)*l[0-9]+-[0-9]+-[0-9]+)|msvcrt|ucrtbase)\\.dll"};
const std::regex crt_debug{
    "(((concrt|msvcp|vccorlib|vcruntime|vcamp|vcomp)[0-9]+(_[0-9])?d(_[a-z_]*)?)|(libomp[0-9]+d"
    "\\.x86_64)|ucrtbased|vcruntime140_threadsd|clang_rt\\.asan_dbg_dynamic-x86_64)\\.dll"};
const std::regex crt_release{
    "(((concrt|msvcp|msvcr|vccorlib|vcruntime|vcamp|vcomp)[0-9]+(_[0-9])?(_[a-z_]+)?)|(libomp[0-9]+"
    "\\.x86_64)|clang_rt\\.asan_dynamic-x86_64)\\.dll"};
const std::regex mfc_debug{"mfcm?[0-9]+u?d\\.dll"};
const std::regex mfc_release{"((mfcm?[0-9]+u?)|(mfc[0-9]+[a-z]{3}))\\.dll"};
const std::regex xtf{"xtf[a-z]+\\.dll"};
const std::regex windows_component{"windows\\.[a-z\\.]+\\.dll"};

// Not in the xbdepends tables, which only cover the GDK. Names from the import tables of
// Xbox One ERA titles.
constexpr std::string_view era_d3d = "d3d11_x.dll", era_tracing = "etwplus.dll";

void append(json &list, const std::set<std::string> &names) {
    for (const auto &name : names)
        list.push_back(name);
}
} // namespace

ModuleClass classify_module(std::string_view file_name) {
    std::string name(file_name);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
    });
    using namespace data;
    if (matches(name, api_set))
        return {matches(name, os_crt) ? "os_crt" : "api_set", !matches(name, os_crt), false};
    if (matches(name, crt_debug))
        return {"crt", false, true};
    if (matches(name, crt_release))
        return {"crt", false, false};
    if (matches(name, mfc_debug) || matches(name, mfc_release))
        return {"mfc", false, matches(name, mfc_debug)};
    if (matches(name, xtf))
        return {"xtf", false, true};
    if (matches(name, windows_component))
        return {"windows_component", true, false};
    if (contains(core_os, name))
        return {matches(name, os_crt) ? "os_crt" : "core_os", !matches(name, os_crt), false};
    if (contains(win32_legacy, name))
        return {"win32_legacy", true, false};
    if (contains(system_os, name))
        return {"system_os", true, false};
    if (contains(game_os_only, name))
        return {"game_os_only", false, false};
    if (contains(pc_only, name))
        return {"pc_only", true, false};
    if (contains(pc_vendor, name))
        return {"pc_vendor", false, false};
    if (contains(legacy_dxsdk_debug, name))
        return {"legacy_dxsdk", false, true};
    if (contains(legacy_dxsdk, name))
        return {"legacy_dxsdk", false, false};
    if (contains(gdk, name))
        return {"gdk", false, false};
    if (contains(dev_only_gdk, name))
        return {"gdk", false, true};
    if (contains(d3d_legacy, name))
        return {"d3d_legacy", false, false};
    if (contains(d3d_stock, name))
        return {"d3d_stock", false, false};
    if (contains(d3d_xbox_one, name))
        return {"d3d_xbox_one", false, false};
    if (contains(d3d_scarlett, name))
        return {"d3d_scarlett", false, false};
    if (contains(legacy_era, name))
        return {"legacy_era", true, false};
    if (name == era_d3d)
        return {"d3d_xbox_one", false, false};
    if (name == era_tracing)
        return {"legacy_era", false, false};
    return {"unknown", false, false};
}

bool is_game_os_api(std::string_view symbol) {
    // not in the kits' lists, and imported by titles of both consoles all the same
    static constexpr std::string_view seen[] = {"RtlAddFunctionTable", "RtlDeleteFunctionTable",
                                                "VerSetConditionMask", "VerifyVersionInfoW"};
    return contains(data::game_os_apis, symbol) ||
           contains(data::additional_game_os_apis, symbol) ||
           std::find(std::begin(seen), std::end(seen), symbol) != std::end(seen);
}

json import_profile(const json &pe_report) {
    const auto &contents = pe_report.at("directory_contents");
    auto modules = json::array();
    std::set<std::string> scarlett, xbox_one, pc, era, pc_modules, dev_only, unknown, outside;
    std::size_t inside = 0;
    for (const auto *key : {"imports", "delay_imports"}) {
        if (!contents.contains(key))
            continue;
        for (const auto &entry : contents.at(key)) {
            const auto &name = entry.at("module").get_ref<const std::string &>();
            const auto kind = classify_module(name);
            const std::string_view category = kind.category;
            modules.push_back({{"module", name},
                               {"category", category},
                               {"delay_load", std::string_view(key) == "delay_imports"},
                               {"development_only", kind.development_only}});
            if (category == "d3d_scarlett")
                scarlett.insert(name);
            else if (category == "d3d_xbox_one")
                xbox_one.insert(name);
            else if (category == "d3d_stock" || category == "d3d_legacy")
                pc.insert(name);
            else if (category == "legacy_era")
                era.insert(name);
            else if (category == "pc_only" || category == "pc_vendor" || category == "system_os" ||
                     category == "mfc")
                pc_modules.insert(name);
            else if (category == "unknown")
                unknown.insert(name);
            if (kind.development_only)
                dev_only.insert(name);
            if (!kind.os_surface || !entry.contains("symbols"))
                continue;
            for (const auto &symbol : entry.at("symbols")) {
                if (!symbol.contains("name"))
                    continue;
                const auto &api = symbol.at("name").get_ref<const std::string &>();
                if (is_game_os_api(api))
                    ++inside;
                else
                    outside.insert(name + "!" + api);
            }
        }
    }
    const bool durango = !xbox_one.empty() || !era.empty();
    // the first Xbox One kits named the console's Direct3D d3d11.dll, as the desktop's is
    const bool desktop = !pc.empty() && era.empty();
    const auto runtimes = int{!scarlett.empty()} + int{durango} + int{desktop};
    auto findings = json::array();
    const auto finding = [&](const char *text, const std::set<std::string> &names) {
        if (names.empty())
            return;
        json item{{"finding", text}, {"names", json::array()}};
        append(item["names"], names);
        findings.push_back(std::move(item));
    };
    if (runtimes > 1)
        findings.push_back({{"finding", "modules of more than one target in one image"}});
    finding("Xbox One ERA module; not part of the GDK application model", era);
    finding("module xbdepends lists as unavailable to Game OS titles", pc_modules);
    finding("development-only module", dev_only);
    finding("module not in the xbdepends tables; a title-local DLL or unrecognized", unknown);
    finding("OS import name outside the xbdepends Game OS API list", outside);
    return {
        {"basis", "microsoft/Xbox-GDK-Samples xbdepends tables; file names and API spellings only"},
        {"target_indication", runtimes > 1        ? "mixed"
                              : !scarlett.empty() ? "scarlett"
                              : durango           ? "xbox_one"
                              : desktop           ? "pc"
                                                  : "unknown"},
        {"modules", std::move(modules)},
        {"game_os_api_names", inside},
        {"findings", std::move(findings)}};
}

std::optional<Target> console_target(const json &profile) {
    const auto &indication = profile.at("target_indication").get_ref<const std::string &>();
    if (indication == "xbox_one")
        return Target::durango;
    if (indication == "scarlett")
        return Target::scarlett;
    return std::nullopt;
}
} // namespace xv::analysis
