#pragma once
#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace xv::runtime {
// Gives a manually mapped image the static thread-local storage the system loader would
// give it: a block for every thread, those that exist and those to come, at the index the
// image reads from its _tls_index. `tls` is the "tls" entry of its pe::inspect report,
// whose addresses are for `image_base`; the image is mapped and relocated at `base`.
// The system does the work: it loads a small DLL made here that has the same TLS template
// (and the image's TLS callbacks), and the image gets that DLL's index. Throws on failure.
// An `executable` gets index 0 instead (see static_tls.cpp).
void install_static_tls(const nlohmann::json &tls, std::uint64_t base, std::uint64_t image_base,
                        bool executable = false);
// On the thread about to run the executable's entry point: its TLS callbacks are told of
// the process there.
void start_static_tls();
} // namespace xv::runtime
