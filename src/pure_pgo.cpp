// pure-nx (2026-11-07): profile collection for profile-guided optimisation (PGO). From renut-nx's
// app/src/banjo_pgo.cpp (sa2-nx's sa2_pgo.cpp, same project family): the stream API and the dump are theirs; the
// dump thread is a libnx thread here (std::thread is "Function not implemented" in this NRO: rex_crash of 2026-11-07)
// and dumps every minute (there is no focus-loss or exit hook in Pure's app).
//
// The instrumented Switch build (build_switch_pgo.cmd generate: -DPURE_PGO=generate) counts every branch of the game
// code, the app and Renut's renderer. libgcov's own way to save them (a .gcda per object at exit, at the build PC's
// paths) does not fit the Switch, so the build registers each object's counts in a section
// (-fprofile-info-section=gcov_info) and this file streams them all into one file, pgo/pure_pgo_<N>.stream next to the
// NRO (N = this session), the whole session each time through a .tmp file (a dump cut short leaves the previous one).
// On the PC, tools/pgo_merge.py turns every session's stream into .gcda files in pgo\ (gcov-tool merge-stream), and
// build_switch_pgo.cmd use reads them.

#include "pure_pgo.h"

#if defined(PURE_PGO_GENERATE)

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <switch.h>

extern "C" {  // GCC's gcov.h has no C++ guard of its own
#include <gcov.h>
}

#include <rex/filesystem.h>
#include <rex/logging.h>

// One pointer per instrumented object, in the section GNU ld brackets with these two symbols.
extern "C" const struct gcov_info *const __start_gcov_info[];
extern "C" const struct gcov_info *const __stop_gcov_info[];

namespace pure::pgo {
namespace {

std::mutex g_dump_mutex;
std::filesystem::path g_file;

struct Stream {
  std::FILE *file = nullptr;
  bool ok = true;
  std::vector<std::unique_ptr<unsigned char[]>> allocations;  // libgcov's scratch buffers for one dump
};

void Write(const void *data, unsigned length, void *arg) {
  auto *s = static_cast<Stream *>(arg);
  if (s->ok && length && std::fwrite(data, 1, length, s->file) != length)
    s->ok = false;
}

void Filename(const char *name, void *arg) { __gcov_filename_to_gcfn(name, Write, arg); }

void *Allocate(unsigned length, void *arg) {
  auto *s = static_cast<Stream *>(arg);
  s->allocations.push_back(std::make_unique<unsigned char[]>(length ? length : 1));
  return s->allocations.back().get();
}

void DumpThread(void *) {
  for (;;) {
    svcSleepThread(60'000'000'000ll);
    DumpNow();
  }
}

}  // namespace

void DumpNow() {
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  if (g_file.empty())
    return;
  std::filesystem::path temporary = g_file;
  temporary += ".tmp";
  Stream stream;
  stream.file = std::fopen(temporary.string().c_str(), "wb");
  if (!stream.file)
    return;
  static std::vector<char> buffer(1 << 20);  // one SD write per MB instead of per record
  std::setvbuf(stream.file, buffer.data(), _IOFBF, buffer.size());
  const gcov_info *const *info = __start_gcov_info;
  const gcov_info *const *const end = __stop_gcov_info;
  __asm__("" : "+r"(info));  // as in GCC's manual: the compiler must not reason about the linker's symbols
  for (; info != end && stream.ok; ++info) {
    if (*info)
      __gcov_info_to_gcda(*info, Filename, Write, Allocate, &stream);
  }
  const bool closed = std::fclose(stream.file) == 0;
  if (!stream.ok || !closed)
    return;  // the previous complete dump stays
  std::error_code ec;
  std::filesystem::rename(temporary, g_file, ec);
  if (ec) {  // a file system that does not replace on rename
    std::filesystem::remove(g_file, ec);
    std::filesystem::rename(temporary, g_file, ec);
  }
}

void Start() {
  const std::filesystem::path folder = rex::filesystem::GetExecutableFolder() / "pgo";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  for (int n = 1; n < 100000; ++n) {
    const std::filesystem::path candidate = folder / ("pure_pgo_" + std::to_string(n) + ".stream");
    std::filesystem::path temporary = candidate;
    temporary += ".tmp";
    if (!std::filesystem::exists(candidate, ec) && !std::filesystem::exists(temporary, ec)) {
      g_file = candidate;
      break;
    }
  }
  if (g_file.empty())
    return;
  static Thread thread;
  if (R_SUCCEEDED(threadCreate(&thread, DumpThread, nullptr, nullptr, 0x10000, 0x2E, 2)) &&
      R_SUCCEEDED(threadStart(&thread)))
    REXLOG_INFO("[pgo] profile collection on: {} every minute", g_file.string());
  else
    REXLOG_WARN("[pgo] the dump thread could not start");
}

}  // namespace pure::pgo

#endif
