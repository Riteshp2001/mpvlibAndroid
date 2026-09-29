/**
 * lossless_dll.cpp
 *
 * Parses the Lossless Scaling Windows PE binary (Lossless.dll) and extracts
 * embedded SPIR-V shader blobs from its RCDATA resources.  Builds a local
 * shader cache so subsequent launches avoid re-parsing the PE.
 *
 * Ported from Eden Emulator's video_core/frame_gen/lossless_dll.cpp.
 * Eden © 2026 Eden Emulator Project, GPL-3.0-or-later.
 * lsfg-vk © 2025 lsfg-vk, GPL-3.0-or-later.
 */

#include "lossless_dll.h"
#include <android/log.h>
#include <cstring>
#include <fstream>
#include <span>
#include <optional>
#include <algorithm>

#define TAG "LSFG_DLL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Internal storage root is set once by the JNI layer calling SetStorageRoot().
static std::filesystem::path g_storage_root;

void FrameGen_SetStorageRoot(const std::string& root) {
    g_storage_root = root;
}

namespace FrameGen {

// ── PE constants (same as Eden) ──────────────────────────────────────────────
constexpr uint16_t DOS_MAGIC       = 0x5A4D;
constexpr uint32_t PE_SIGNATURE    = 0x00004550;
constexpr uint16_t PE32_PLUS_MAGIC = 0x020B;
constexpr size_t   DOS_LFANEW_OFFSET                = 0x3C;
constexpr size_t   COFF_HEADER_SIZE                 = 20;
constexpr size_t   OPTIONAL_HEADER_SIZE_OFFSET      = 16;
constexpr size_t   SECTION_HEADER_SIZE              = 40;
constexpr size_t   DATA_DIRECTORY_ENTRY_SIZE        = 8;
constexpr size_t   DATA_DIRECTORY_OFFSET_PE32_PLUS  = 112;
constexpr size_t   RESOURCE_DATA_DIRECTORY_INDEX    = 2;
constexpr size_t   RESOURCE_DIRECTORY_SIZE          = 16;
constexpr size_t   RESOURCE_NAMED_COUNT_OFFSET      = 12;
constexpr size_t   RESOURCE_ID_COUNT_OFFSET         = 14;
constexpr size_t   RESOURCE_ENTRY_SIZE              = 8;
constexpr uint32_t RESOURCE_SUBDIRECTORY_FLAG       = 0x80000000;
constexpr uint32_t RESOURCE_TYPE_RCDATA             = 10;
constexpr uint32_t MIPMAPS_SHADER_ID                = 255;
constexpr uint32_t GENERATE_SHADER_ID               = 256;
constexpr uint32_t PERF_SHADER_ID_FIRST             = 280;
constexpr uint32_t PERF_SHADER_ID_LAST              = 302;
constexpr uint32_t CACHE_MAGIC   = 0x4746534C; // "LSFG"
constexpr uint32_t CACHE_VERSION = 3;

struct CacheHeader { uint32_t magic, version; uint64_t src_size, src_hash; uint32_t count; };
struct Section     { uint32_t va, vs, ra, rs; };
struct ResourceEntry { uint32_t id, offset; bool is_dir, is_named; };

// ── Minimal PE reader ────────────────────────────────────────────────────────
class ImageReader {
    std::span<const uint8_t> img;
public:
    explicit ImageReader(std::span<const uint8_t> s) : img(s) {}
    template<typename T>
    bool Read(size_t off, T& v) const {
        if (off + sizeof(T) > img.size()) return false;
        memcpy(&v, img.data() + off, sizeof(T));
        return true;
    }
    bool ReadBytes(size_t off, size_t len, std::vector<uint8_t>& out) const {
        if (off + len > img.size()) return false;
        out.assign(img.begin() + off, img.begin() + off + len);
        return true;
    }
};

static uint64_t SimpleHash(const std::vector<uint8_t>& data) {
    uint64_t h = 14695981039346656037ULL;
    for (auto b : data) { h ^= b; h *= 1099511628211ULL; }
    return h;
}

static bool FindSection(const ImageReader& r, uint32_t rva,
                        const std::vector<Section>& sections, size_t& file_off) {
    for (auto& s : sections) {
        if (rva >= s.va && rva < s.va + s.vs) { file_off = s.ra + (rva - s.va); return true; }
    }
    return false;
}

static std::optional<std::vector<ResourceEntry>>
ReadResourceDir(const ImageReader& r, size_t dir_off, size_t res_base) {
    uint16_t named_count = 0, id_count = 0;
    if (!r.Read(dir_off + RESOURCE_NAMED_COUNT_OFFSET, named_count)) return {};
    if (!r.Read(dir_off + RESOURCE_ID_COUNT_OFFSET,    id_count))    return {};
    std::vector<ResourceEntry> entries;
    size_t entry_off = dir_off + RESOURCE_DIRECTORY_SIZE;
    for (int i = 0; i < named_count + id_count; ++i, entry_off += RESOURCE_ENTRY_SIZE) {
        uint32_t id_field = 0, off_field = 0;
        if (!r.Read(entry_off, id_field) || !r.Read(entry_off + 4, off_field)) return {};
        ResourceEntry e;
        e.is_named  = (id_field  & RESOURCE_SUBDIRECTORY_FLAG) != 0;
        e.is_dir    = (off_field & RESOURCE_SUBDIRECTORY_FLAG) != 0;
        e.id        = id_field  & ~RESOURCE_SUBDIRECTORY_FLAG;
        e.offset    = (off_field & ~RESOURCE_SUBDIRECTORY_FLAG) + res_base;
        entries.push_back(e);
    }
    return entries;
}

// ── Public API ───────────────────────────────────────────────────────────────
std::filesystem::path GetLosslessDllPath() {
    return g_storage_root / "lossless" / "Lossless.dll";
}

std::filesystem::path GetShaderCachePath() {
    return g_storage_root / "lossless" / "shader_cache.bin";
}

LosslessStatus ReadShaderResources(const std::filesystem::path& path, ShaderResources& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return LosslessStatus::UnreadableFile;
    size_t sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (!f) return LosslessStatus::UnreadableFile;

    ImageReader r({buf.data(), buf.size()});
    uint16_t dos_magic = 0;
    if (!r.Read(0, dos_magic) || dos_magic != DOS_MAGIC) return LosslessStatus::NotPortableExecutable;
    uint32_t pe_off = 0;
    if (!r.Read(DOS_LFANEW_OFFSET, pe_off)) return LosslessStatus::NotPortableExecutable;
    uint32_t pe_sig = 0;
    if (!r.Read(pe_off, pe_sig) || pe_sig != PE_SIGNATURE) return LosslessStatus::NotPortableExecutable;
    size_t opt_hdr = pe_off + 4 + COFF_HEADER_SIZE;
    uint16_t magic = 0;
    if (!r.Read(opt_hdr, magic) || magic != PE32_PLUS_MAGIC) return LosslessStatus::NotPortableExecutable;
    uint16_t opt_sz = 0;
    if (!r.Read(pe_off + 4 + OPTIONAL_HEADER_SIZE_OFFSET, opt_sz)) return LosslessStatus::NotPortableExecutable;

    // Read section headers
    size_t sections_off = opt_hdr + opt_sz;
    uint16_t num_sections = 0;
    if (!r.Read(pe_off + 4 + 2, num_sections)) return LosslessStatus::NotPortableExecutable;
    std::vector<Section> sections(num_sections);
    for (int i = 0; i < num_sections; ++i) {
        size_t sh = sections_off + i * SECTION_HEADER_SIZE;
        if (!r.Read(sh + 12, sections[i].va) || !r.Read(sh + 8,  sections[i].vs) ||
            !r.Read(sh + 20, sections[i].ra) || !r.Read(sh + 16, sections[i].rs))
            return LosslessStatus::NotPortableExecutable;
    }

    // Find .rsrc section via Data Directory
    size_t dd_off = opt_hdr + DATA_DIRECTORY_OFFSET_PE32_PLUS +
                    RESOURCE_DATA_DIRECTORY_INDEX * DATA_DIRECTORY_ENTRY_SIZE;
    uint32_t rsrc_rva = 0;
    if (!r.Read(dd_off, rsrc_rva) || rsrc_rva == 0) return LosslessStatus::MissingShaders;
    size_t rsrc_file_off = 0;
    if (!FindSection(r, rsrc_rva, sections, rsrc_file_off)) return LosslessStatus::MissingShaders;

    // Walk resource tree: Type → Name → Language
    auto type_entries = ReadResourceDir(r, rsrc_file_off, rsrc_file_off);
    if (!type_entries) return LosslessStatus::MissingShaders;
    size_t rcdata_dir_off = 0;
    for (auto& e : *type_entries) {
        if (!e.is_named && e.id == RESOURCE_TYPE_RCDATA && e.is_dir) { rcdata_dir_off = e.offset; break; }
    }
    if (!rcdata_dir_off) return LosslessStatus::MissingShaders;

    auto name_entries = ReadResourceDir(r, rcdata_dir_off, rsrc_file_off);
    if (!name_entries) return LosslessStatus::MissingShaders;

    bool found_any = false;
    for (auto& ne : *name_entries) {
        uint32_t shader_id = ne.id;
        bool is_shader = (shader_id == MIPMAPS_SHADER_ID) ||
                         (shader_id == GENERATE_SHADER_ID) ||
                         (shader_id >= PERF_SHADER_ID_FIRST && shader_id <= PERF_SHADER_ID_LAST);
        if (!is_shader || !ne.is_dir) continue;
        // Pick first language entry
        auto lang_entries = ReadResourceDir(r, ne.offset, rsrc_file_off);
        if (!lang_entries || lang_entries->empty()) continue;
        auto& le = (*lang_entries)[0];
        // le.offset points to RESOURCE_DATA_ENTRY (RVA + size + codepage + reserved)
        uint32_t data_rva = 0, data_size = 0;
        if (!r.Read(le.offset, data_rva) || !r.Read(le.offset + 4, data_size)) continue;
        size_t data_file_off = 0;
        if (!FindSection(r, data_rva, sections, data_file_off)) continue;
        std::vector<uint8_t> blob;
        if (!r.ReadBytes(data_file_off, data_size, blob)) continue;
        out[shader_id] = std::move(blob);
        found_any = true;
    }
    return found_any ? LosslessStatus::Ok : LosslessStatus::MissingShaders;
}

LosslessStatus ValidateLosslessDll(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return LosslessStatus::NotInstalled;
    ShaderResources dummy;
    return ReadShaderResources(path, dummy);
}

LosslessStatus GetInstalledLosslessStatus() {
    return ValidateLosslessDll(GetLosslessDllPath());
}

LosslessStatus BuildShaderCache() {
    ShaderResources resources;
    auto status = ReadShaderResources(GetLosslessDllPath(), resources);
    if (status != LosslessStatus::Ok) return status;

    std::vector<uint8_t> file_data;
    { std::ifstream f(GetLosslessDllPath(), std::ios::binary | std::ios::ate);
      size_t sz = f.tellg(); f.seekg(0); file_data.resize(sz); f.read((char*)file_data.data(), sz); }

    CacheHeader hdr{CACHE_MAGIC, CACHE_VERSION, file_data.size(), SimpleHash(file_data),
                    static_cast<uint32_t>(resources.size())};

    std::filesystem::create_directories(GetShaderCachePath().parent_path());
    std::ofstream out(GetShaderCachePath(), std::ios::binary);
    if (!out) return LosslessStatus::CacheUnusable;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    for (auto& [id, blob] : resources) {
        uint32_t sz = blob.size();
        out.write(reinterpret_cast<const char*>(&id), sizeof(id));
        out.write(reinterpret_cast<const char*>(&sz), sizeof(sz));
        out.write(reinterpret_cast<const char*>(blob.data()), sz);
    }
    LOGI("Shader cache built: %zu shaders", resources.size());
    return LosslessStatus::Ok;
}

LosslessStatus LoadShaderModules(ShaderModules& out_modules) {
    std::ifstream f(GetShaderCachePath(), std::ios::binary);
    if (!f) return LosslessStatus::CacheUnusable;
    CacheHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (hdr.magic != CACHE_MAGIC || hdr.version != CACHE_VERSION) return LosslessStatus::CacheUnusable;
    for (uint32_t i = 0; i < hdr.count; ++i) {
        uint32_t id = 0, sz = 0;
        f.read(reinterpret_cast<char*>(&id), sizeof(id));
        f.read(reinterpret_cast<char*>(&sz), sizeof(sz));
        std::vector<uint32_t> spirv(sz / sizeof(uint32_t));
        f.read(reinterpret_cast<char*>(spirv.data()), sz);
        out_modules[id] = std::move(spirv);
    }
    return f ? LosslessStatus::Ok : LosslessStatus::CacheUnusable;
}

bool RemoveInstalledLosslessDll() {
    bool ok = true;
    auto dll  = GetLosslessDllPath();
    auto cache = GetShaderCachePath();
    if (std::filesystem::exists(dll))  { std::error_code ec; std::filesystem::remove(dll,  ec); ok &= !ec; }
    if (std::filesystem::exists(cache)) { std::error_code ec; std::filesystem::remove(cache, ec); ok &= !ec; }
    return ok;
}

} // namespace FrameGen