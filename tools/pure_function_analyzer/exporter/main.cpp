#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>

namespace fs = std::filesystem;

static uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

static std::string hex32(uint32_t value) {
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
  return out.str();
}

static std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (unsigned char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
              << std::dec;
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  return out.str();
}

static bool set_cvar(std::string_view name, std::string_view value) {
  return rex::cvar::SetFlagByName(name, value);
}

struct SectionRecord {
  std::string name;
  uint32_t address = 0;
  uint32_t size = 0;
  uint32_t raw_address = 0;
  uint32_t raw_size = 0;
  uint32_t flags = 0;
  bool executable = false;
  bool writable = false;
  const uint8_t* bytes = nullptr;
};

struct PdataRecord {
  uint32_t begin = 0;
  uint32_t size = 0;
  uint32_t data = 0;
  uint32_t prolog_length = 0;
  bool exception_flag = false;
  bool thirty_two_bit = false;
};

static int fail(const std::string& message) {
  std::cerr << "pure_xex_exporter: " << message << "\n";
  return 1;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    return fail("usage: pure_xex_exporter <default.xex> <output-directory>");
  }

  const fs::path xex_path = fs::absolute(argv[1]);
  const fs::path output_dir = fs::absolute(argv[2]);
  if (!fs::is_regular_file(xex_path))
    return fail("input XEX does not exist: " + xex_path.string());
  std::error_code ec;
  fs::create_directories(output_dir, ec);
  if (ec)
    return fail("cannot create output directory: " + ec.message());

  // Runtime's host-path VFS normally saves its directory index beside the game
  // folder. Disable all pack/index writes before Runtime::Setup.
  if (!set_cvar("vfs_index", "false") || !set_cvar("asset_pack", "false") ||
      !set_cvar("asset_pack_build", "false")) {
    return fail("could not disable VFS index/asset-pack writes through SDK cvars");
  }

  rex::InitLoggingEarly();
  rex::Runtime runtime(xex_path.parent_path());
  rex::RuntimeConfig runtime_config;
  runtime_config.kernel_init = rex::kernel::InitializeKernel;
  runtime_config.tool_mode = true;
  const rex::X_STATUS setup_status = runtime.Setup(std::move(runtime_config));
  if (setup_status != static_cast<rex::X_STATUS>(0))
    return fail("ReXGlue Runtime::Setup failed: " + hex32(static_cast<uint32_t>(setup_status)));

  const std::string module_path = "game:\\" + xex_path.filename().string();
  const rex::X_STATUS load_status = runtime.LoadXexImage(module_path);
  if (load_status != static_cast<rex::X_STATUS>(0))
    return fail("ReXGlue Runtime::LoadXexImage failed: " + hex32(static_cast<uint32_t>(load_status)));

  auto user_module = runtime.kernel_state()->GetExecutableModule();
  if (!user_module || !user_module->xex_module())
    return fail("SDK did not expose the loaded executable XexModule");
  auto* module = user_module->xex_module();

  const uint32_t base = module->base_address();
  const uint32_t image_size = module->image_size();
  const uint32_t entry_point = module->entry_point();
  if (!base || !image_size)
    return fail("SDK returned an empty image base or size");

  std::vector<SectionRecord> sections;
  const auto binary_sections = module->binary_sections();
  const auto& pe_sections = module->pe_sections();
  sections.reserve(binary_sections.size());
  for (const auto& section : binary_sections) {
    if (!section.host_data || section.virtual_size == 0)
      continue;
    if (section.virtual_address < base ||
        static_cast<uint64_t>(section.virtual_address) + section.virtual_size >
            static_cast<uint64_t>(base) + image_size) {
      return fail("SDK section lies outside loaded image: " + section.name);
    }
    SectionRecord record;
    record.name = section.name;
    record.address = section.virtual_address;
    record.size = section.virtual_size;
    record.executable = section.executable;
    record.writable = section.writable;
    record.bytes = section.host_data;
    auto pe = std::find_if(pe_sections.begin(), pe_sections.end(), [&](const auto& candidate) {
      return std::string(candidate.name) == record.name && candidate.address == record.address;
    });
    if (pe != pe_sections.end()) {
      record.raw_address = pe->raw_address;
      record.raw_size = pe->raw_size;
      record.flags = pe->flags;
    }
    sections.push_back(std::move(record));
  }
  std::sort(sections.begin(), sections.end(), [](const auto& a, const auto& b) {
    return a.address < b.address;
  });
  for (size_t i = 1; i < sections.size(); ++i) {
    const uint64_t prev_end = static_cast<uint64_t>(sections[i - 1].address) + sections[i - 1].size;
    if (prev_end > sections[i].address) {
      return fail("overlapping mapped sections prevent an unambiguous flat image: " +
                  sections[i - 1].name + " / " + sections[i].name);
    }
  }

  // BinaryView (used by codegen) copies precisely these mapped virtual section
  // bytes. Flatten them at their guest VAs; holes remain zero and are described
  // as unmapped by image-map.json.
  std::vector<uint8_t> image(image_size, 0);
  for (const auto& section : sections) {
    const size_t offset = section.address - base;
    std::copy(section.bytes, section.bytes + section.size, image.begin() + offset);
  }

  const uint32_t pdata_address = module->exception_directory_address();
  const uint32_t pdata_size = module->exception_directory_size();
  std::vector<PdataRecord> pdata;
  if (pdata_size) {
    if (pdata_size % 8 != 0)
      return fail("PDATA byte size is not a multiple of the SDK's 8-byte runtime-function record");
    if (pdata_address < base || static_cast<uint64_t>(pdata_address) + pdata_size >
                                   static_cast<uint64_t>(base) + image_size) {
      return fail("PDATA range lies outside the loaded image");
    }
    const uint8_t* raw_pdata = runtime.memory()->TranslateVirtual(pdata_address);
    if (!raw_pdata)
      return fail("SDK memory could not translate the PDATA directory");
    pdata.reserve(pdata_size / 8);
    for (uint32_t offset = 0; offset < pdata_size; offset += 8) {
      const uint32_t begin = read_be32(raw_pdata + offset);
      const uint32_t data = read_be32(raw_pdata + offset + 4);
      // Mirrors phase_register.cpp's packed IMAGE_CE_RUNTIME_FUNCTION fields
      // after byte_swap on the little-endian host.
      const uint32_t function_length = (data >> 8) & 0x003FFFFF;
      pdata.push_back(PdataRecord{
          .begin = begin,
          .size = function_length ? function_length * 4 : 4,
          .data = data,
          .prolog_length = data & 0xFF,
          .exception_flag = (data & 0x80000000) != 0,
          .thirty_two_bit = (data & 0x40000000) != 0,
      });
    }
  }

  const fs::path image_path = output_dir / "image.bin";
  const fs::path map_path = output_dir / "image-map.json";
  {
    std::ofstream out(image_path, std::ios::binary | std::ios::trunc);
    if (!out)
      return fail("cannot open image.bin for writing");
    out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    if (!out)
      return fail("failed while writing image.bin");
  }

  std::ofstream json(map_path, std::ios::binary | std::ios::trunc);
  if (!json)
    return fail("cannot open image-map.json for writing");
  json << "{\n  \"format\": \"rexglue-loaded-xex-image-v1\",\n"
       << "  \"source_xex\": \"" << json_escape(xex_path.string()) << "\",\n"
       << "  \"image_base\": \"" << hex32(base) << "\",\n"
       << "  \"image_size\": " << image_size << ",\n"
       << "  \"entry_point\": \"" << hex32(entry_point) << "\",\n"
       << "  \"image_representation\": \"zero-filled image span with SDK-mapped virtual sections copied at VA-base; gaps are unmapped\",\n"
       << "  \"sections\": [\n";
  for (size_t i = 0; i < sections.size(); ++i) {
    const auto& section = sections[i];
    json << "    {\"name\": \"" << json_escape(section.name) << "\", \"start\": \""
         << hex32(section.address) << "\", \"end\": \"" << hex32(section.address + section.size)
         << "\", \"virtual_size\": " << section.size << ", \"raw_address\": " << section.raw_address
         << ", \"raw_size\": " << section.raw_size << ", \"characteristics\": \""
         << hex32(section.flags) << "\", \"executable\": " << (section.executable ? "true" : "false")
         << ", \"writable\": " << (section.writable ? "true" : "false") << "}"
         << (i + 1 == sections.size() ? "\n" : ",\n");
  }
  json << "  ],\n  \"executable_regions\": [\n";
  std::vector<const SectionRecord*> executable_sections;
  for (const auto& section : sections) {
    if (section.executable)
      executable_sections.push_back(&section);
  }
  for (size_t i = 0; i < executable_sections.size(); ++i) {
    const auto& section = *executable_sections[i];
    json << "    {\"name\": \"" << json_escape(section.name) << "\", \"start\": \""
         << hex32(section.address) << "\", \"end\": \"" << hex32(section.address + section.size)
         << "\"}" << (i + 1 == executable_sections.size() ? "\n" : ",\n");
  }
  json << "  ],\n  \"pdata_directory\": {\"start\": \"" << hex32(pdata_address)
       << "\", \"size\": " << pdata_size << "},\n  \"pdata\": [\n";
  for (size_t i = 0; i < pdata.size(); ++i) {
    const auto& fn = pdata[i];
    json << "    {\"start\": \"" << hex32(fn.begin) << "\", \"end\": \""
         << hex32(fn.begin + fn.size) << "\", \"size\": " << fn.size << ", \"prolog_length\": "
         << fn.prolog_length << ", \"exception_flag\": " << (fn.exception_flag ? "true" : "false")
         << ", \"thirty_two_bit\": " << (fn.thirty_two_bit ? "true" : "false")
         << ", \"raw_data\": \"" << hex32(fn.data) << "\"}"
         << (i + 1 == pdata.size() ? "\n" : ",\n");
  }
  json << "  ],\n  \"imports\": [\n";
  std::vector<std::string> imports;
  for (const auto& library : *module->import_libraries()) {
    for (const auto& import : library.imports) {
      std::ostringstream row;
      row << "    {\"library\": \"" << json_escape(library.name) << "\", \"ordinal\": "
          << import.ordinal << ", \"name\": null, \"address\": \""
          << hex32(import.thunk_address ? import.thunk_address : import.value_address)
          << "\", \"thunk_address\": \"" << hex32(import.thunk_address)
          << "\", \"value_address\": \"" << hex32(import.value_address)
          << "\", \"is_function_thunk\": " << (import.thunk_address ? "true" : "false") << "}";
      imports.push_back(row.str());
    }
  }
  for (size_t i = 0; i < imports.size(); ++i)
    json << imports[i] << (i + 1 == imports.size() ? "\n" : ",\n");
  json << "  ],\n  \"relocations\": {\"available\": false, \"reason\": "
       << "\"XexModule exposes no relocation enumeration API; codegen's BinaryView also carries no relocation records.\"},\n"
       << "  \"exporter_notes\": [\"XEX decryption, decompression, mapping, and import setup were performed by ReXGlue Runtime/XexModule.\", "
       << "\"PDATA records use the same 8-byte big-endian fields and FunctionLength*4 rule as codegen phase_register.cpp.\", "
       << "\"Import names are not supplied by XexModule import metadata; library plus ordinal are preserved.\"]\n}\n";
  if (!json)
    return fail("failed while writing image-map.json");

  std::cout << "Loaded by ReXGlue: base=" << hex32(base) << " size=" << hex32(image_size)
            << " entry=" << hex32(entry_point) << "\n"
            << "Sections: " << sections.size() << "; executable: " << executable_sections.size()
            << "; PDATA records: " << pdata.size() << "; imports: " << imports.size() << "\n"
            << "Wrote " << image_path.string() << " and " << map_path.string() << "\n";
  return 0;
}
