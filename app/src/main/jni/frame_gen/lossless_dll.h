#pragma once
#include <filesystem>
#include <map>
#include <vector>
#include <cstdint>

namespace FrameGen {

enum class LosslessStatus : uint32_t {
    Ok = 0,
    NotInstalled,
    UnreadableFile,
    NotPortableExecutable,
    MissingShaders,
    TranslationFailed,
    CacheUnusable,
};

using ShaderResources = std::map<uint32_t, std::vector<uint8_t>>;
using ShaderModules   = std::map<uint32_t, std::vector<uint32_t>>;

// Shader resource IDs inside Lossless.dll (RCDATA resources)
namespace ShaderID {
    constexpr uint32_t MIPMAPS  = 255;
    constexpr uint32_t GENERATE = 256;
    // Optical-flow pipeline stages
    constexpr uint32_t ALPHA_0 = 290, ALPHA_1 = 291, ALPHA_2 = 292, ALPHA_3 = 293;
    constexpr uint32_t BETA_0  = 298, BETA_1  = 299, BETA_2  = 300, BETA_3  = 301, BETA_4 = 302;
    constexpr uint32_t GAMMA_0 = 280, GAMMA_1 = 282, GAMMA_2 = 283, GAMMA_3 = 284, GAMMA_4 = 285;
    constexpr uint32_t DELTA_0 = 286, DELTA_1 = 287, DELTA_2 = 288, DELTA_3 = 289;
    constexpr uint32_t DELTA_4 = 281, DELTA_5 = 294, DELTA_6 = 295, DELTA_7 = 296, DELTA_8 = 297;
    constexpr uint32_t FP16_OFFSET = 49;
} // namespace ShaderID

std::filesystem::path GetLosslessDllPath();
std::filesystem::path GetShaderCachePath();

LosslessStatus ReadShaderResources(const std::filesystem::path& path, ShaderResources& out);
LosslessStatus ValidateLosslessDll(const std::filesystem::path& path);
LosslessStatus GetInstalledLosslessStatus();
LosslessStatus BuildShaderCache();
LosslessStatus LoadShaderModules(ShaderModules& out_modules);
bool           RemoveInstalledLosslessDll();

} // namespace FrameGen