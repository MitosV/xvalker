#include "runtime/durango/winrt/title_info.hpp"
#include "formats/appx/manifest.hpp"
#include "host/files.hpp"
#include "runtime/common/runtime.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <windows.h>

#include <bcrypt.h>

namespace xv::runtime::durango::winrt {
namespace {
struct Info {
    std::wstring title_id, service_config_id;
    std::wstring name, publisher, publisher_id, architecture;
    std::uint64_t version = 0;
};
std::wstring wide(const std::string &text) { return {text.begin(), text.end()}; } // ASCII
std::wstring wide_utf8(const std::string &text) {
    std::wstring result(text.size(), L' ');
    result.resize(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      result.data(), static_cast<int>(result.size())));
    return result;
}
// How Windows shortens a publisher into the 13 characters of a package's family name: the
// first 8 bytes of the SHA-256 of its UTF-16 text, five bits a character
std::wstring publisher_id(const std::wstring &publisher) {
    std::uint8_t digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                   reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(publisher.data())),
                   static_cast<ULONG>(publisher.size() * sizeof(wchar_t)), digest,
                   sizeof digest) < 0)
        return {};
    std::wstring id;
    for (unsigned bit = 0; bit < 65; bit += 5) {
        unsigned value = 0;
        for (unsigned i = 0; i < 5; ++i) {
            const unsigned at = bit + i;
            value = value << 1 | (at < 64 ? digest[at / 8] >> (7 - at % 8) & 1 : 0);
        }
        id += L"0123456789abcdefghjkmnpqrstvwxyz"[value];
    }
    return id;
}
std::uint64_t packed_version(const std::string &text) {
    std::uint64_t version = 0;
    const char *at = text.c_str();
    for (unsigned part = 0; part < 4; ++part) {
        char *end = nullptr;
        version |= (std::strtoull(at, &end, 10) & 0xffff) << (16 * part);
        at = *end == '.' ? end + 1 : end;
    }
    return version;
}
Info read() {
    Info info;
    try {
        const auto manifest = appx::inspect_manifest(host::read_snapshot(
            package_directory(std::filesystem::current_path()) / "AppxManifest.xml",
            appx::manifest_limit));
        if (manifest.at("xbox_live_title_id").is_string())
            info.title_id = std::to_wstring(
                std::stoul(manifest.at("xbox_live_title_id").get<std::string>(), nullptr, 16));
        if (manifest.at("service_config_id").is_string())
            info.service_config_id = wide(manifest.at("service_config_id").get<std::string>());
        const auto field = [&](const char *key) {
            return manifest.at(key).is_string() ? manifest.at(key).get<std::string>()
                                                : std::string();
        };
        info.name = wide_utf8(field("name"));
        info.publisher = wide_utf8(field("publisher"));
        info.publisher_id = publisher_id(info.publisher);
        info.architecture = wide_utf8(field("architecture"));
        info.version = packed_version(field("version"));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Runtime] no Xbox Live identity from the manifest: %s\n", e.what());
    }
    std::fprintf(stderr, "[Runtime] Xbox Live title %ls, service configuration %ls\n",
                 info.title_id.c_str(), info.service_config_id.c_str());
    return info;
}
const Info &info_once() {
    static const auto info = read();
    return info;
}
} // namespace

std::uint64_t title_version() { return info_once().version; }

std::wstring title_info(std::wstring_view key) {
    const auto &info = info_once();
    if (key == L"title_id")
        return info.title_id;
    if (key == L"service_config_id")
        return info.service_config_id;
    if (key == L"sandbox_id")
        return L"RETAIL";
    if (key == L"package_name")
        return info.name;
    if (key == L"package_publisher")
        return info.publisher;
    if (key == L"package_publisher_id")
        return info.publisher_id;
    if (key == L"package_family_name")
        return info.name + L"_" + info.publisher_id;
    if (key == L"package_full_name") {
        std::wstring version;
        for (unsigned part = 0; part < 4; ++part)
            version += (part ? L"." : L"") + std::to_wstring(info.version >> (16 * part) & 0xffff);
        return info.name + L"_" + version + L"_" +
               (info.architecture.empty() ? L"x64" : info.architecture) + L"__" + info.publisher_id;
    }
    return {};
}
} // namespace xv::runtime::durango::winrt
