// SPDX-License-Identifier: GPL-2.0-or-later
#include "port_shader_mods.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/path_util.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace PortShaderMods {

namespace fs = std::filesystem;

namespace {

constexpr size_t AllVersions = ~size_t{0};
constexpr u32 SpirvMagic = 0x07230203;

struct ModFile {
    fs::path spv;          ///< the SPIR-V to load (a prebuilt .spv or the compiled source)
    std::string name;      ///< for the log
    std::vector<u32> code; ///< read on first use
    bool read = false;
};

std::once_flag init_once;
std::mutex mods_mutex;
std::map<std::pair<u64, size_t>, ModFile> mods;
std::unordered_set<u64> modded_hashes;
fs::path mods_dir;

std::mutex status_mutex;
std::string status_text;

void SetStatus(std::string text) {
    std::printf("Shader mods: %s\n", text.c_str());
    std::scoped_lock lk{status_mutex};
    status_text = std::move(text);
}

fs::path ExeDir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return n ? fs::path(std::wstring(buffer, n)).parent_path() : fs::current_path();
#else
    std::error_code ec;
    const auto exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : exe.parent_path();
#endif
}

/// A tool from the port folder (tools/), an environment override, MSYS2 or the PATH.
std::string FindTool(const char* env_name, const char* exe_name) {
    if (const char* env = std::getenv(env_name); env && env[0]) {
        return env;
    }
    std::error_code ec;
#ifdef _WIN32
    const std::string file = std::string(exe_name) + ".exe";
#else
    const std::string file = exe_name;
#endif
    for (const auto& dir : {ExeDir() / "tools", ExeDir(), ExeDir().parent_path() / "tools",
                            fs::path("C:/msys64/clang64/bin"), fs::path("C:/msys64/mingw64/bin")}) {
        if (fs::is_regular_file(dir / file, ec)) {
            return (dir / file).string();
        }
    }
    return exe_name; // the PATH
}

/// Runs `arguments` (the tool first), its output into `log`; true when it exits with 0.
bool Run(const std::vector<std::string>& arguments, const fs::path& log) {
    std::string command;
    for (const auto& argument : arguments) {
        if (!command.empty()) {
            command += ' ';
        }
        command += '"' + argument + '"';
    }
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<char> line(command.begin(), command.end());
    line.push_back('\0');
    const BOOL started = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) {
        CloseHandle(out);
    }
    if (!started) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
#else
    command += " > \"" + log.string() + "\" 2>&1";
    return std::system(command.c_str()) == 0;
#endif
}

std::string ReadText(const fs::path& path, size_t limit = 600) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (auto& c : text) {
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return text.size() > limit ? text.substr(0, limit) + "..." : text;
}

std::vector<u32> ReadSpirv(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<u32> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    if (words.size() < 5 || words[0] != SpirvMagic) {
        return {};
    }
    return words;
}

bool IsSource(const fs::path& path) {
    static const char* const extensions[] = {".frag", ".vert", ".comp", ".geom", ".tesc",
                                             ".tese", ".glsl"};
    const auto ext = path.extension().string();
    return std::any_of(std::begin(extensions), std::end(extensions),
                       [&](const char* e) { return ext == e; });
}

/// "<hex hash>_p<version>" or "<hex hash>" (every version).
bool ParseName(const std::string& stem, u64& hash, size_t& perm) {
    const auto mark = stem.find("_p");
    const std::string hex = mark == std::string::npos ? stem : stem.substr(0, mark);
    if (hex.empty() || hex.size() > 16 ||
        hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return false;
    }
    hash = std::strtoull(hex.c_str(), nullptr, 16);
    perm = AllVersions;
    if (mark != std::string::npos) {
        const std::string number = stem.substr(mark + 2);
        if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        perm = std::strtoull(number.c_str(), nullptr, 10);
    }
    return true;
}

const char* const Readme =
    "PROTOTYPE PC port - shader mods (docs/SHADER_MODS.md)\n"
    "\n"
    "Find a shader: in game press Insert, go to SHADERS. The list shows the pixel shaders of the\n"
    "last frame, most draws first. HIDE skips its draws: what disappears is what it draws.\n"
    "EXPORT FOR EDITING writes its versions here as GLSL (<hash>_p<version>.frag) and the\n"
    "originals to _originals.\n"
    "\n"
    "Edit a .frag (or .vert, .comp) and restart the game: the port compiles it with\n"
    "glslangValidator (.compiled/<file>.log has any errors) and uses it in place of the game's.\n"
    "Delete or rename a file to go back to the original. Files and folders starting with _ or .\n"
    "are ignored. A named mod can also live in mods/<name>/shaders.\n"
    "\n"
    "<hash>.frag without _p<version> replaces every version of that shader: only safe when\n"
    "the versions have the same inputs (compare the exported ones first).\n";

void Init() {
    const char* env = std::getenv("THEORDER_SHADER_MODS_DIR");
    mods_dir = env && env[0] ? fs::path(env) : fs::path("mods");
    std::error_code ec;
    const fs::path shaders = mods_dir / "shaders";
    fs::create_directories(shaders, ec);
    if (!fs::exists(shaders / "README.txt", ec)) {
        std::ofstream(shaders / "README.txt") << Readme;
    }
    // Named mods first (alphabetical), mods/shaders last: a later file wins.
    std::vector<fs::path> folders;
    for (const auto& entry : fs::directory_iterator(mods_dir, ec)) {
        const auto name = entry.path().filename().string();
        if (entry.is_directory() && name != "shaders" && !name.starts_with('_') &&
            !name.starts_with('.') && fs::is_directory(entry.path() / "shaders", ec)) {
            folders.push_back(entry.path() / "shaders");
        }
    }
    std::sort(folders.begin(), folders.end());
    folders.push_back(shaders);
    std::string glslang;
    size_t compiled = 0, failed = 0;
    std::string first_error;
    for (const auto& folder : folders) {
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            const auto name = file.filename().string();
            if (name.starts_with('_') || name.starts_with('.')) {
                continue;
            }
            u64 hash{};
            size_t perm{};
            if (!ParseName(file.stem().string(), hash, perm)) {
                continue;
            }
            fs::path spv;
            if (file.extension() == ".spv") {
                spv = file;
            } else if (IsSource(file)) {
                const fs::path out_dir = folder / ".compiled";
                fs::create_directories(out_dir, ec);
                spv = out_dir / (name + ".spv");
                const fs::path log = out_dir / (name + ".log");
                const bool stale = !fs::exists(spv, ec) ||
                                   fs::last_write_time(spv, ec) < fs::last_write_time(file, ec);
                if (stale) {
                    if (glslang.empty()) {
                        glslang = FindTool("THEORDER_GLSLANG", "glslangValidator");
                    }
                    fs::remove(spv, ec);
                    if (!Run({glslang, "-V", "--target-env", "vulkan1.3", "-o", spv.string(),
                              file.string()},
                             log) ||
                        !fs::exists(spv, ec)) {
                        ++failed;
                        if (first_error.empty()) {
                            first_error = name + ": " + ReadText(log);
                        }
                        fs::remove(spv, ec);
                        continue;
                    }
                    ++compiled;
                }
            } else {
                continue;
            }
            auto& mod = mods[{hash, perm}];
            mod = ModFile{.spv = spv, .name = (folder.parent_path().filename() / name).string()};
            modded_hashes.insert(hash);
            std::printf("Shader mods: %s replaces shader %llx %s\n", mod.name.c_str(),
                        static_cast<unsigned long long>(hash),
                        perm == AllVersions ? "(every version)"
                                            : ("version " + std::to_string(perm)).c_str());
        }
    }
    if (failed) {
        SetStatus(std::to_string(failed) + " failed to compile: " + first_error);
    } else if (!mods.empty()) {
        SetStatus(std::to_string(mods.size()) + " loaded" +
                  (compiled ? " (" + std::to_string(compiled) + " compiled)" : std::string{}));
    } else {
        SetStatus("none in " + shaders.string());
    }
}

// Shader browser.
std::unordered_map<u64, u32> frame_draws; // GPU thread
std::mutex browser_mutex;
std::vector<ShaderUse> last_frame;
std::unordered_set<u64> hidden;
std::atomic<bool> any_hidden{false};

} // namespace

std::optional<std::vector<u32>> Find(u64 hash, size_t perm) {
    std::call_once(init_once, Init);
    std::scoped_lock lk{mods_mutex};
    if (!modded_hashes.contains(hash)) {
        return std::nullopt;
    }
    auto it = mods.find({hash, perm});
    if (it == mods.end()) {
        it = mods.find({hash, AllVersions});
    }
    if (it == mods.end()) {
        return std::nullopt;
    }
    auto& mod = it->second;
    if (!mod.read) {
        mod.read = true;
        mod.code = ReadSpirv(mod.spv);
        if (mod.code.empty()) {
            SetStatus(mod.name + ": not valid SPIR-V");
        } else {
            std::printf("Shader mods: %s applied to shader %llx version %zu\n", mod.name.c_str(),
                        static_cast<unsigned long long>(hash), perm);
        }
    }
    if (mod.code.empty()) {
        return std::nullopt;
    }
    return mod.code;
}

size_t Loaded() {
    std::call_once(init_once, Init);
    std::scoped_lock lk{mods_mutex};
    return mods.size();
}

void NoteDraw(u64 ps_hash) {
    if (ps_hash) {
        ++frame_draws[ps_hash];
    }
}

void OnFrame() {
    std::vector<ShaderUse> uses;
    uses.reserve(frame_draws.size());
    {
        std::scoped_lock lk{mods_mutex};
        for (const auto& [hash, draws] : frame_draws) {
            uses.push_back({hash, draws, false, modded_hashes.contains(hash)});
        }
    }
    frame_draws.clear();
    std::sort(uses.begin(), uses.end(), [](const auto& a, const auto& b) {
        return a.draws != b.draws ? a.draws > b.draws : a.hash < b.hash;
    });
    std::scoped_lock lk{browser_mutex};
    // Hidden shaders stay in the list (their draws are counted before they are skipped).
    for (auto& use : uses) {
        use.hidden = hidden.contains(use.hash);
    }
    last_frame = std::move(uses);
}

std::vector<ShaderUse> LastFrame() {
    std::scoped_lock lk{browser_mutex};
    return last_frame;
}

bool AnyHidden() {
    return any_hidden.load(std::memory_order_relaxed);
}

bool IsHidden(u64 ps_hash) {
    std::scoped_lock lk{browser_mutex};
    return hidden.contains(ps_hash);
}

void SetHidden(u64 ps_hash, bool hide) {
    std::scoped_lock lk{browser_mutex};
    if (hide) {
        hidden.insert(ps_hash);
    } else {
        hidden.erase(ps_hash);
    }
    any_hidden = !hidden.empty();
}

void ShowAll() {
    std::scoped_lock lk{browser_mutex};
    hidden.clear();
    any_hidden = false;
}

void Export(u64 hash) {
    std::call_once(init_once, Init);
    std::thread([hash] {
        char hex[24];
        std::snprintf(hex, sizeof(hex), "%llx", static_cast<unsigned long long>(hash));
        char cache_prefix[32];
        std::snprintf(cache_prefix, sizeof(cache_prefix), "0x%016llx_",
                      static_cast<unsigned long long>(hash));
        SetStatus(std::string("exporting ") + hex + "...");
        std::error_code ec;
        const fs::path cache = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "cache";
        const fs::path shaders = mods_dir / "shaders";
        const fs::path originals = shaders / "_originals";
        fs::create_directories(originals, ec);
        std::string spirv_cross;
        size_t found = 0, written = 0, kept = 0, failed = 0;
        std::string error;
        for (const auto& game : fs::directory_iterator(cache, ec)) {
            if (!game.is_directory()) {
                continue;
            }
            for (const auto& entry : fs::directory_iterator(game.path(), ec)) {
                const auto name = entry.path().filename().string();
                if (!name.starts_with(cache_prefix) || entry.path().extension() != ".spv") {
                    continue;
                }
                const std::string version = entry.path().stem().string().substr(
                    std::strlen(cache_prefix));
                const auto words = ReadSpirv(entry.path());
                if (words.empty()) {
                    continue;
                }
                ++found;
                // The stage, from the entry point's execution model.
                const char* ext = ".glsl";
                for (size_t i = 5; i < words.size();) {
                    const u32 count = words[i] >> 16, opcode = words[i] & 0xffff;
                    if (count == 0) {
                        break;
                    }
                    if (opcode == 15 && i + 1 < words.size()) { // OpEntryPoint
                        static const char* const by_model[] = {".vert", ".tesc", ".tese",
                                                               ".geom", ".frag", ".comp"};
                        ext = words[i + 1] < 6 ? by_model[words[i + 1]] : ".glsl";
                        break;
                    }
                    i += count;
                }
                const std::string base = std::string(hex) + "_p" + version;
                fs::copy_file(entry.path(), originals / (base + ".spv"),
                              fs::copy_options::overwrite_existing, ec);
                const fs::path source = shaders / (base + ext);
                if (fs::exists(source, ec)) {
                    ++kept; // never over an edited file
                    continue;
                }
                if (spirv_cross.empty()) {
                    spirv_cross = FindTool("THEORDER_SPIRV_CROSS", "spirv-cross");
                }
                const fs::path log = originals / (base + ".log");
                if (!Run({spirv_cross, "--vulkan-semantics", (originals / (base + ".spv")).string(),
                          "--output", source.string()},
                         log) ||
                    !fs::exists(source, ec)) {
                    ++failed;
                    if (error.empty()) {
                        error = ReadText(log);
                    }
                    fs::remove(source, ec);
                    continue;
                }
                ++written;
            }
        }
        if (!found) {
            SetStatus(std::string(hex) + ": not in the shader cache yet (play on, then export)");
        } else if (failed && !written) {
            SetStatus(std::string(hex) + ": decompiling failed (spirv-cross): " + error);
        } else {
            SetStatus(std::string(hex) + ": " + std::to_string(written) + " of " +
                      std::to_string(found) + " versions exported" +
                      (kept ? ", " + std::to_string(kept) + " already there (kept)" : std::string{}) +
                      (failed ? ", " + std::to_string(failed) + " failed" : std::string{}) +
                      ". Edit them in mods\\shaders, then restart the game.");
        }
    }).detach();
}

std::string Status() {
    std::scoped_lock lk{status_mutex};
    return status_text;
}

std::string Folder() {
    std::call_once(init_once, Init);
    return (mods_dir / "shaders").string();
}

} // namespace PortShaderMods
