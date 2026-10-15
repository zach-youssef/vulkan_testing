#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

/*
 What a SPIR-V module does with the universal descriptor set (bindless.glsl), found by reading its
 instructions rather than trusting declarations.

 Synchronization validation can't follow accesses through dynamically indexed descriptor arrays,
 so a material binding that declares a read for a resource its shader writes gets no barrier and
 no validation message. Materials compare this reflection with their bindings to catch that.

 Writes are traced back to the variables they write through: access chains, loads (of image
 handles), copies, selects, phis and function parameters. A write is any store, image store,
 atomic other than a load, or memory copy into a variable of the given set and binding.
 */
struct SpirvDescriptorWrites {
    std::set<uint32_t> bindings;   // Bindings of descriptor set 0 the module writes through

    bool writes(uint32_t binding) const {
        return bindings.count(binding) != 0;
    }
};

namespace spirv_reflection_detail {

constexpr uint32_t kMagic = 0x07230203;

enum Op : uint32_t {
    OpFunctionParameter = 55,
    OpFunctionEnd = 56,
    OpFunctionCall = 57,
    OpFunction = 54,
    OpVariable = 59,
    OpImageTexelPointer = 60,
    OpLoad = 61,
    OpStore = 62,
    OpCopyMemory = 63,
    OpCopyMemorySized = 64,
    OpAccessChain = 65,
    OpInBoundsAccessChain = 66,
    OpPtrAccessChain = 67,
    OpInBoundsPtrAccessChain = 70,
    OpDecorate = 71,
    OpCopyObject = 83,
    OpImageWrite = 99,
    OpSelect = 169,
    OpPhi = 245,
    OpAtomicStore = 228,
    OpAtomicExchange = 229,
    OpAtomicCompareExchange = 230,
    OpAtomicCompareExchangeWeak = 231,
    OpAtomicIIncrement = 232,
    OpAtomicIDecrement = 233,
    OpAtomicIAdd = 234,
    OpAtomicISub = 235,
    OpAtomicSMin = 236,
    OpAtomicUMin = 237,
    OpAtomicSMax = 238,
    OpAtomicUMax = 239,
    OpAtomicAnd = 240,
    OpAtomicOr = 241,
    OpAtomicXor = 242,
    OpAtomicFlagTestAndSet = 318,
    OpAtomicFlagClear = 319,
    OpAtomicFMinEXT = 5614,
    OpAtomicFMaxEXT = 5615,
    OpAtomicFAddEXT = 6035,
};

constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;

} // namespace spirv_reflection_detail

// Throws std::invalid_argument for something that isn't a SPIR-V module
inline SpirvDescriptorWrites reflectDescriptorWrites(const std::vector<uint32_t>& words) {
    using namespace spirv_reflection_detail;
    if (words.size() < 5 || words[0] != kMagic) {
        throw std::invalid_argument("Not a SPIR-V module");
    }

    std::map<uint32_t, uint32_t> bindingOf;
    std::map<uint32_t, uint32_t> setOf;
    std::set<uint32_t> variables;
    // Where each id's value or pointer comes from
    std::map<uint32_t, std::vector<uint32_t>> sources;
    // Ids written through
    std::vector<uint32_t> written;
    // Function parameters in order, and calls' arguments, to connect the two
    std::map<uint32_t, std::vector<uint32_t>> parameters;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> calls;
    uint32_t currentFunction = 0;

    for (size_t at = 5; at < words.size();) {
        const uint32_t count = words[at] >> 16;
        const uint32_t opcode = words[at] & 0xffff;
        if (count == 0 || at + count > words.size()) {
            throw std::invalid_argument("Malformed SPIR-V instruction");
        }
        const uint32_t* operand = &words[at + 1];
        const uint32_t operands = count - 1;
        auto from = [&](uint32_t result, uint32_t source) { sources[result].push_back(source); };

        switch (opcode) {
            case OpDecorate:
                if (operands >= 3 && operand[1] == kDecorationBinding) bindingOf[operand[0]] = operand[2];
                if (operands >= 3 && operand[1] == kDecorationDescriptorSet) setOf[operand[0]] = operand[2];
                break;
            case OpVariable:
                variables.insert(operand[1]);
                break;
            case OpFunction:
                currentFunction = operand[1];
                break;
            case OpFunctionEnd:
                currentFunction = 0;
                break;
            case OpFunctionParameter:
                parameters[currentFunction].push_back(operand[1]);
                break;
            case OpFunctionCall:
                calls.emplace_back(operand[2], std::vector<uint32_t>(operand + 3, operand + operands));
                break;
            case OpAccessChain: case OpInBoundsAccessChain: case OpPtrAccessChain: case OpInBoundsPtrAccessChain:
            case OpLoad: case OpCopyObject: case OpImageTexelPointer:
                from(operand[1], operand[2]);
                break;
            case OpSelect:
                from(operand[1], operand[3]);
                from(operand[1], operand[4]);
                break;
            case OpPhi:
                for (uint32_t i = 2; i + 1 < operands; i += 2) {
                    from(operand[1], operand[i]);
                }
                break;
            case OpStore: case OpCopyMemory: case OpCopyMemorySized: case OpImageWrite:
            case OpAtomicStore: case OpAtomicFlagClear:
                written.push_back(operand[0]);
                break;
            case OpAtomicExchange: case OpAtomicCompareExchange: case OpAtomicCompareExchangeWeak:
            case OpAtomicIIncrement: case OpAtomicIDecrement: case OpAtomicIAdd: case OpAtomicISub:
            case OpAtomicSMin: case OpAtomicUMin: case OpAtomicSMax: case OpAtomicUMax:
            case OpAtomicAnd: case OpAtomicOr: case OpAtomicXor: case OpAtomicFlagTestAndSet:
            case OpAtomicFMinEXT: case OpAtomicFMaxEXT: case OpAtomicFAddEXT:
                written.push_back(operand[2]);
                break;
            default:
                break;
        }
        at += count;
    }
    for (const auto& [function, arguments] : calls) {
        const auto& params = parameters[function];
        for (size_t i = 0; i < arguments.size() && i < params.size(); ++i) {
            sources[params[i]].push_back(arguments[i]);
        }
    }

    SpirvDescriptorWrites result;
    std::set<uint32_t> visited;
    std::vector<uint32_t> pending = written;
    while (!pending.empty()) {
        const uint32_t id = pending.back();
        pending.pop_back();
        if (!visited.insert(id).second) {
            continue;
        }
        if (variables.count(id)) {
            auto set = setOf.find(id);
            auto binding = bindingOf.find(id);
            if (set != setOf.end() && set->second == 0 && binding != bindingOf.end()) {
                result.bindings.insert(binding->second);
            }
            continue;
        }
        auto it = sources.find(id);
        if (it != sources.end()) {
            pending.insert(pending.end(), it->second.begin(), it->second.end());
        }
    }
    return result;
}

inline SpirvDescriptorWrites reflectDescriptorWrites(const std::vector<char>& code) {
    if (code.size() % 4 != 0) {
        throw std::invalid_argument("Not a SPIR-V module");
    }
    std::vector<uint32_t> words(code.size() / 4);
    memcpy(words.data(), code.data(), code.size());
    return reflectDescriptorWrites(words);
}
