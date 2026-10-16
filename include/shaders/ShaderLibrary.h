#pragma once

#include "core/Result.h"
#include "core/VkTypes.h"
#include "core/VkUtil.h"
#include "core/VulkanContext.h"
#include "shaders/SpirvReflection.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/*
 Shaders loaded from SPIR-V on disk, which can be reloaded while the program runs.

 The library doesn't compile GLSL: compile shaders to SPIR-V separately (the Makefile's shader
 rules use glslc) and the library loads <name>.spv from its search paths. A shader's stage comes
 from its name's extension (.vert, .frag, .comp, .geom, .tesc, .tese).

    TRY_ASSIGN(auto vert, context.shaders().get("shader.vert"));   // loads shader.vert.spv
    ...
    auto result = context.shaders().reload();             // after rebuilding the SPIR-V
    for (auto& error : result.errors) std::cerr << error << "\n";

 get() fails if the shader can't be found (ErrorCode::NotFound) or loaded (InvalidData, or a
 Vulkan error). Reloading reads every loaded shader's file and, for those that changed, creates a
 new module and notifies listeners (materials rebuild their pipelines). If a file can't be read or
 isn't valid SPIR-V, the shader keeps its current module and the error is reported. Old modules are
 destroyed once in-flight work has finished.

 Thread safe, but reload() runs listeners on the calling thread: call it between frames.
 */

// The stage a shader's name's extension says; std::nullopt for unknown extensions
inline std::optional<VkShaderStageFlagBits> shaderStageFromName(const std::string& name) {
    const auto extension = std::filesystem::path(name).extension().string();
    if (extension == ".vert") return VK_SHADER_STAGE_VERTEX_BIT;
    if (extension == ".frag") return VK_SHADER_STAGE_FRAGMENT_BIT;
    if (extension == ".comp") return VK_SHADER_STAGE_COMPUTE_BIT;
    if (extension == ".geom") return VK_SHADER_STAGE_GEOMETRY_BIT;
    if (extension == ".tesc") return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (extension == ".tese") return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    return std::nullopt;
}

// Checks bytes could be a SPIR-V module; returns an error message, or empty if they could
inline std::string checkSpirv(const std::vector<char>& code) {
    constexpr uint32_t kMagic = 0x07230203;
    if (code.size() < 20 || code.size() % 4 != 0) {
        return "not SPIR-V (size " + std::to_string(code.size()) + " isn't a whole number of words of a module)";
    }
    uint32_t magic = 0;
    memcpy(&magic, code.data(), sizeof(magic));
    if (magic != kMagic) {
        return "not SPIR-V (wrong magic number)";
    }
    return {};
}

class Shader {
public:
    // Called after a reload with new code; an error is reported by the reload
    using Listener = std::function<Result<void>(Shader&)>;
    using ListenerId = uint64_t;

    const std::string& name() const { return name_; }
    const std::filesystem::path& path() const { return path_; }
    VkShaderStageFlagBits stage() const { return stage_; }
    const char* entryPoint() const { return "main"; }

    // The current module. Replaced (and the old one destroyed after in-flight work) on reload.
    VkShaderModule module() {
        std::lock_guard<std::mutex> lock(mutex_);
        return **module_;
    }

    // The bindings of the universal descriptor set the current code writes through
    SpirvDescriptorWrites descriptorWrites() {
        std::lock_guard<std::mutex> lock(mutex_);
        return descriptorWrites_;
    }

    // Incremented each time the shader is reloaded with new code
    uint64_t version() {
        std::lock_guard<std::mutex> lock(mutex_);
        return version_;
    }

    // Calls listener after each reload with new code
    ListenerId addListener(Listener listener) {
        std::lock_guard<std::mutex> lock(mutex_);
        const ListenerId id = nextListener_++;
        listeners_.emplace(id, std::move(listener));
        return id;
    }

    void removeListener(ListenerId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        listeners_.erase(id);
    }

private:
    friend class ShaderLibrary;

    Shader(VulkanContext& context, std::string name, std::filesystem::path path, VkShaderStageFlagBits stage)
    : context_(&context), name_(std::move(name)), path_(std::move(path)), stage_(stage) {}

    // Loads the file. Unchanged code is a success that changes nothing.
    Result<void> load(bool& changed) {
        changed = false;
        // Remember this version of the file even if it fails to load, so polling reports a broken
        // file once rather than on every check
        {
            auto modified = lastModified();
            std::lock_guard<std::mutex> lock(mutex_);
            modified_ = modified;
        }
        std::vector<char> code;
        {
            std::ifstream file(path_, std::ios::ate | std::ios::binary);
            if (!file.is_open()) {
                return Error(ErrorCode::NotFound, "Shader '" + name_ + "': can't open " + path_.string());
            }
            code.resize(static_cast<size_t>(file.tellg()));
            file.seekg(0);
            file.read(code.data(), static_cast<std::streamsize>(code.size()));
        }
        if (auto error = checkSpirv(code); !error.empty()) {
            return Error(ErrorCode::InvalidData, "Shader '" + name_ + "' (" + path_.string() + ") is " + error);
        }
        TRY_ASSIGN(SpirvDescriptorWrites writes,
                   reflectDescriptorWrites(code).withContext("Shader '" + name_ + "' (" + path_.string() + ")"));

        std::lock_guard<std::mutex> lock(mutex_);
        if (module_ && code == code_) {
            return {};
        }
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        std::unique_ptr<VulkanShaderModule> module;
        VK_TRY(VulkanShaderModule::create(module, context_->device(), createInfo),
               "Shader '" + name_ + "': failed to create module");
        if (module_) {
            // Destroyed once in-flight work has finished, like the pipelines built from it
            std::shared_ptr<VulkanShaderModule> old(module_.release());
            context_->retire([old] {});
            ++version_;
        }
        module_ = std::move(module);
        code_ = std::move(code);
        descriptorWrites_ = std::move(writes);
        changed = true;
        return {};
    }

    std::optional<std::filesystem::file_time_type> lastModified() {
        std::error_code error;
        auto time = std::filesystem::last_write_time(path_, error);
        if (error) {
            return std::nullopt;
        }
        return time;
    }

    bool modifiedSinceLoad() {
        auto time = lastModified();
        std::lock_guard<std::mutex> lock(mutex_);
        return time != modified_;
    }

    // Runs listeners, collecting their errors
    void notify(std::vector<Error>& errors) {
        std::vector<Listener> listeners;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, listener] : listeners_) {
                listeners.push_back(listener);
            }
        }
        for (auto& listener : listeners) {
            if (auto result = listener(*this); !result) {
                errors.push_back(std::move(result).error().withContext("reloading shader '" + name_ + "'"));
            }
        }
    }

    VulkanContext* context_;
    std::string name_;
    std::filesystem::path path_;
    VkShaderStageFlagBits stage_;

    std::mutex mutex_;
    std::unique_ptr<VulkanShaderModule> module_;
    std::vector<char> code_;
    SpirvDescriptorWrites descriptorWrites_;
    std::optional<std::filesystem::file_time_type> modified_;
    uint64_t version_ = 0;
    std::map<ListenerId, Listener> listeners_;
    ListenerId nextListener_ = 0;
};

class ShaderLibrary {
public:
    struct ReloadResult {
        std::vector<std::string> reloaded;   // Names of shaders whose code changed
        std::vector<Error> errors;           // Shaders kept their previous code, or dependents failed
    };

    ShaderLibrary(VulkanContext& context, std::vector<std::filesystem::path> searchPaths)
    : context_(&context), searchPaths_(std::move(searchPaths)) {}

    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    void addSearchPath(std::filesystem::path path) {
        std::lock_guard<std::mutex> lock(mutex_);
        searchPaths_.push_back(std::move(path));
    }

    // The shader `name` (e.g. "shader.vert"), loaded from <search path>/<name>.spv on first use and
    // shared afterwards. A name without a shader stage extension is a contract violation.
    Result<Handle<Shader>> get(const std::string& name) {
        const auto stage = shaderStageFromName(name);
        if (!stage) {
            contractViolation("Can't tell the stage of shader '" + name
                              + "' (expected .vert, .frag, .comp, .geom, .tesc or .tese)");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = shaders_.find(name);
        if (it != shaders_.end()) {
            return it->second;
        }
        std::string searched;
        for (const auto& directory : searchPaths_) {
            auto path = directory / (name + ".spv");
            if (std::filesystem::exists(path)) {
                auto shader = context_->resources().adopt(std::unique_ptr<Shader>(new Shader(*context_, name, path, *stage)));
                bool changed = false;
                TRY(shader->load(changed));
                shaders_.emplace(name, shader);
                return shader;
            }
            searched += "\n  " + path.string();
        }
        return Error(ErrorCode::NotFound, "Shader '" + name + "' not found; searched:" + searched);
    }

    // Reloads every loaded shader whose file changed, and notifies their listeners
    ReloadResult reload() {
        return reload(false);
    }

    // Reloads shaders whose files' modification times changed since they were loaded. Cheaper than
    // reload() (which reads every file), for calling every frame.
    ReloadResult reloadModified() {
        return reload(true);
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return shaders_.size();
    }

    // Stops tracking a shader: it is no longer reloaded, and get() loads it afresh. Shaders still
    // used elsewhere stay alive.
    void forget(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        shaders_.erase(name);
    }

    // Releases the library's references to its shaders (shaders still used elsewhere stay alive)
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        shaders_.clear();
    }

private:
    ReloadResult reload(bool onlyModified) {
        std::vector<Handle<Shader>> shaders;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [name, shader] : shaders_) {
                shaders.push_back(shader);
            }
        }
        ReloadResult result;
        for (auto& shader : shaders) {
            if (onlyModified && !shader->modifiedSinceLoad()) {
                continue;
            }
            bool changed = false;
            if (auto loaded = shader->load(changed); !loaded) {
                result.errors.push_back(std::move(loaded).error());
            } else if (changed) {
                result.reloaded.push_back(shader->name());
                shader->notify(result.errors);
            }
        }
        return result;
    }

    VulkanContext* context_;
    std::mutex mutex_;
    std::vector<std::filesystem::path> searchPaths_;
    std::map<std::string, Handle<Shader>> shaders_;
};

// VulkanContext::shaders, defined here since VulkanContext.h cannot depend on ShaderLibrary
inline ShaderLibrary& VulkanContext::shaders() {
    std::call_once(shadersCreated_, [this] {
        std::vector<std::filesystem::path> paths(config_.shaderSearchPaths.begin(), config_.shaderSearchPaths.end());
        shaders_ = std::unique_ptr<ShaderLibrary, void (*)(ShaderLibrary*)>(
            new ShaderLibrary(*this, std::move(paths)), [](ShaderLibrary* library) { delete library; });
    });
    return *shaders_;
}
