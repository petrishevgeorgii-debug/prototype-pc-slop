// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <mutex>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include "port_toggles.h"
#include "common/assert.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/frontend/opcodes.h"

namespace Shader::Gcn {

namespace {
thread_local const FetchShaderSnapshot* compiler_snapshot = nullptr;
}

namespace {
/// port: where the vertex shader's SGPR pairs point when it calls the fetch shader. PROTOTYPE 2's
/// vertex shaders load the V# table pointer themselves (s_load_dwordx2 from a user data
/// pointer, or further), and the fetch shader's s_load_dwordx4 then reads through that pair:
/// read as user data, the pair was out of range (or another value) and the V# was garbage.
struct CallerPointers {
    struct Node {
        enum Kind : u8 { UserData, Load } kind;
        u8 reg;          ///< UserData: the user data SGPR of the pointer
        u16 dword_offset; ///< Load: dwords past the pointer of `child`
        s32 child;       ///< Load: node of the pointer read from
    };
    std::vector<Node> nodes;
    std::array<s32, IR::NumScalarRegs> regs{}; ///< node of the pair starting here; -1 none, -2 unknown
    bool scanned{};

    s32 NodeFor(u32 reg) {
        if (reg >= regs.size()) {
            return -2;
        }
        if (regs[reg] == -1 && reg + 1 < 16) {
            nodes.push_back({Node::UserData, u8(reg), 0, -1});
            regs[reg] = s32(nodes.size() - 1);
        }
        return regs[reg];
    }
    void Clobber(u32 first, u32 count) {
        for (u32 r = first >= 1 ? first - 1 : 0; r < first + count && r < regs.size(); ++r) {
            regs[r] = -2;
        }
    }
    void Scan(const u32* code) {
        regs.fill(-1);
        GcnCodeSlice slice(code, code + 0x4000);
        GcnDecodeContext decoder;
        for (u32 n = 0; n < 2048 && !slice.atEnd(); ++n) {
            const auto inst = decoder.decodeInstruction(slice);
            if (inst.opcode == Opcode::S_SWAPPC_B64) {
                scanned = true;
                return;
            }
            if (inst.opcode == Opcode::S_ENDPGM) {
                return;
            }
            if (inst.dst[0].field != OperandField::ScalarGPR) {
                continue;
            }
            const u32 dst = inst.dst[0].code;
            if (inst.opcode == Opcode::S_LOAD_DWORDX2 && inst.control.smrd.imm) {
                const s32 child = NodeFor(inst.src[0].code * 2u);
                Clobber(dst, 2);
                if (child >= 0 && dst < regs.size()) {
                    nodes.push_back({Node::Load, 0, u16(inst.control.smrd.offset), child});
                    regs[dst] = s32(nodes.size() - 1);
                }
            } else if (inst.opcode == Opcode::S_MOV_B64 &&
                       inst.src[0].field == OperandField::ScalarGPR) {
                const s32 source = NodeFor(inst.src[0].code);
                Clobber(dst, 2);
                if (dst < regs.size()) {
                    regs[dst] = source;
                }
            } else {
                Clobber(dst, inst.category == InstCategory::ScalarMemory ? 16 : 2);
            }
        }
    }
    /// The 64-bit pointer node `index` holds for this draw's user data; nullopt: unknown.
    std::optional<u64> Eval(const Shader::Info& info, s32 index, u32 depth = 0) const {
        if (index < 0 || depth > 8) {
            return std::nullopt;
        }
        const Node& node = nodes[index];
        if (node.kind == Node::UserData) {
            const auto ud = info.UserData();
            if (node.reg + 1u >= ud.size()) {
                return std::nullopt;
            }
            return (u64(ud[node.reg]) | (u64(ud[node.reg + 1]) << 32)) & 0xFFFFFFFFFFFFULL;
        }
        const auto base = Eval(info, node.child, depth + 1);
        if (!base || *base < 0x10000) {
            return std::nullopt;
        }
        u64 value;
        std::memcpy(&value, reinterpret_cast<const u32*>(*base) + node.dword_offset, sizeof(value));
        return value & 0xFFFFFFFFFFFFULL;
    }
};

const CallerPointers& CallerPointersFor(const Shader::Info& info) {
    thread_local std::unordered_map<u64, CallerPointers> cache;
    const VAddr pgm = info.ProgramBase();
    const u64 key = pgm ^ (u64(info.pgm_hash) << 1);
    auto [it, inserted] = cache.try_emplace(key);
    if (inserted) {
        if (cache.size() > 8192) {
            cache.clear();
            std::tie(it, inserted) = cache.try_emplace(key);
        }
        if (pgm) {
            it->second.Scan(reinterpret_cast<const u32*>(pgm));
        }
    }
    return it->second;
}
} // namespace

AmdGpu::Buffer VertexAttribute::GetSharp(const Shader::Info& info) const noexcept {
    if (compiler_snapshot && compiler_snapshot->info == &info && compiler_snapshot->fetch) {
        const auto& attributes = compiler_snapshot->fetch->attributes;
        for (size_t i = 0; i < attributes.size(); ++i) {
            if (attributes[i] == *this) {
                return compiler_snapshot->buffers[i];
            }
        }
        UNREACHABLE_MSG("Fetch attribute missing from compiler snapshot");
    }
    AmdGpu::Buffer buffer{};
    bool read = false;
    if (sgpr_base < IR::NumScalarRegs) {
        const auto& caller = CallerPointersFor(info);
        const s32 node = caller.scanned ? caller.regs[sgpr_base] : -1;
        const bool plain = node == -1 ||
                           (node >= 0 && caller.nodes[node].kind == CallerPointers::Node::UserData &&
                            caller.nodes[node].reg == sgpr_base);
        if (!plain) {
            // The vertex shader set this pair before calling the fetch shader.
            if (const auto table = caller.Eval(info, node); table && *table >= 0x10000) {
                std::memcpy(&buffer, reinterpret_cast<const u32*>(*table) + dword_offset,
                            sizeof(buffer));
            }
            read = true;
            static std::mutex noted_mutex;
            static std::unordered_map<u64, s32> noted;
            std::scoped_lock lock{noted_mutex};
            if (noted.size() < 256 && noted.emplace(info.pgm_hash ^ (u64(sgpr_base) << 56), node).second) {
                LOG_WARNING(Render_Recompiler,
                            "Vertex shader {:#x}: the fetch shader's V# table s[{}:{}] is {} "
                            "(V# at {:#x})",
                            info.pgm_hash, sgpr_base, sgpr_base + 1,
                            node >= 0 ? "a pointer the vertex shader loaded" : "not known",
                            u64(buffer.base_address));
            }
        }
    }
    if (!read) {
        buffer = info.ReadUdReg<AmdGpu::Buffer>(sgpr_base, dword_offset);
    }
    buffer.base_address += inst_offset;
    if (data_format) {
        buffer.data_format = data_format;
        buffer.num_format = num_format;
    }
    return buffer;
}

void FetchShaderSnapshot::Capture(const Info& source) {
    info = &source;
    fetch.reset();
    if (FetchShaderData data; ParseFetchShader(source, data)) {
        fetch = std::move(data);
    }
    buffers.clear();
    if (fetch) {
        for (const auto& attribute : fetch->attributes) {
            buffers.push_back(attribute.GetSharp(source));
        }
    }
}

ScopedFetchShaderSnapshot::ScopedFetchShaderSnapshot(const FetchShaderSnapshot& snapshot)
    : previous{compiler_snapshot} {
    compiler_snapshot = &snapshot;
}

ScopedFetchShaderSnapshot::~ScopedFetchShaderSnapshot() {
    compiler_snapshot = previous;
}

/**
 * s_load_dwordx4 s[8:11], s[2:3], 0x00
 * s_load_dwordx4 s[12:15], s[2:3], 0x04
 * s_load_dwordx4 s[16:19], s[2:3], 0x08
 * s_waitcnt     lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen
 * buffer_load_format_xyz v[8:10], v0, s[12:15], 0 idxen
 * buffer_load_format_xy v[12:13], v0, s[16:19], 0 idxen
 * s_waitcnt     0
 * s_setpc_b64   s[0:1]

 * s_load_dwordx4  s[4:7], s[2:3], 0x0
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[4:7], 0 idxen
 * s_load_dwordx4  s[4:7], s[2:3], 0x8
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[8:11], v0, s[4:7], 0 idxen
 * s_waitcnt       vmcnt(0) & expcnt(0) & lgkmcnt(0)
 * s_setpc_b64     s[0:1]

 * A normal fetch shader looks like the above, the instructions are generated
 * using input semantics on cpu side. Load instructions can either be separate or interleaved
 * We take the reverse way, extract the original input semantics from these instructions.
 **/

const u32* GetFetchShaderCode(const Info& info, u32 sgpr_base) {
    const u32* code;
    std::memcpy(&code, &info.UserData()[sgpr_base], sizeof(code));
    return code;
}

/// port: the V# in SGPRs srsrc..srsrc+3 that the fetch shader uses without loading it. The
/// vertex shader's code up to the fetch call (s_swappc_b64) is scanned for the s_load_dwordx4
/// that filled them from a user data pointer; without one, SGPRs below 16 are user data.
static void ResolveCallerVsharp(const Shader::Info& info, const u32* fetch_code, u32 srsrc,
                                VertexAttribute& attrib) {
    struct Load {
        u32 dword_offset{};
        u32 base_sgpr{};
        bool loaded{};
    };
    std::array<Load, 104> loads{};
    bool scanned = false;
    if (const auto* pgm = reinterpret_cast<const u32*>(info.ProgramBase()); pgm) {
        GcnCodeSlice slice(pgm, pgm + 0x4000);
        GcnDecodeContext decoder;
        for (u32 n = 0; n < 1024 && !slice.atEnd(); ++n) {
            const auto inst = decoder.decodeInstruction(slice);
            if (inst.opcode == Opcode::S_SWAPPC_B64 || inst.opcode == Opcode::S_ENDPGM) {
                scanned = inst.opcode == Opcode::S_SWAPPC_B64;
                break;
            }
            if (inst.opcode == Opcode::S_LOAD_DWORDX4 && inst.control.smrd.imm &&
                inst.dst[0].code + 3 < loads.size()) {
                loads[inst.dst[0].code] = Load{inst.control.smrd.offset, inst.src[0].code * 2u, true};
            } else if (inst.dst[0].field == OperandField::ScalarGPR) {
                // Anything else writing these SGPRs: the V# is no longer the loaded one.
                // Conservative widths: any scalar load up to 16 SGPRs, other writes up to two.
                const u32 first = inst.dst[0].code, count = inst.category == InstCategory::ScalarMemory ? 16 : 2;
                for (u32 r = first >= 3 ? first - 3 : 0; r < first + count && r < loads.size(); ++r) {
                    loads[r].loaded = false;
                }
            }
        }
    }
    const char* how = "unresolved";
    if (scanned && srsrc < loads.size() && loads[srsrc].loaded && loads[srsrc].base_sgpr + 1 < 16) {
        attrib.sgpr_base = static_cast<u8>(loads[srsrc].base_sgpr);
        attrib.dword_offset = static_cast<u8>(loads[srsrc].dword_offset);
        how = "loaded by the vertex shader";
    } else if (srsrc + 3 < 16) {
        attrib.sgpr_base = static_cast<u8>(IR::NumScalarRegs);
        attrib.dword_offset = static_cast<u8>(srsrc);
        how = "in the user data";
    } else {
        attrib.sgpr_base = static_cast<u8>(IR::NumScalarRegs);
        attrib.dword_offset = 0;
    }
    static std::mutex noted_mutex;
    static std::unordered_map<const u32*, u32> noted;
    std::scoped_lock lock{noted_mutex};
    if (noted.size() < 256 && noted.emplace(fetch_code, srsrc).second) {
        LOG_WARNING(Render_Recompiler,
                    "Fetch shader {} reads V# s[{}:{}] it does not load: {} (vs {:#x})",
                    fmt::ptr(fetch_code), srsrc, srsrc + 3, how, info.pgm_hash);
    }
}

bool ParseFetchShader(const Shader::Info& info, FetchShaderData& out_fetch_data) {
    if (compiler_snapshot && compiler_snapshot->info == &info) {
        if (!compiler_snapshot->fetch) {
            return false;
        }
        out_fetch_data = *compiler_snapshot->fetch;
        return true;
    }
    if (!info.has_fetch_shader) {
        return false;
    }

    // port: the pipeline cache parses the fetch shader on every draw. Results are cached
    // per thread by code address and revalidated against a copy of the code, which is
    // much cheaper than decoding it again.
    struct CachedParse {
        std::vector<u32> code;
        FetchShaderData data;
    };
    thread_local std::unordered_map<const u32*, CachedParse> cache;
    if (const auto it = cache.find(GetFetchShaderCode(info, info.fetch_shader_sgpr_base));
        !PortToggle::Disabled(PortToggle::FetchShaderCache) && it != cache.end() &&
        std::memcmp(it->first, it->second.code.data(), it->second.code.size() * sizeof(u32)) == 0) {
        out_fetch_data = it->second.data;
        return true;
    }

    out_fetch_data.attributes.clear();

    struct VsharpLoad {
        u32 dword_offset{};
        u32 base_sgpr{};
        bool loaded{};
    };
    std::array<VsharpLoad, 104> loads{};

    const auto* code = GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
    const u32* ptr = code;
    while (true) {
        const u32 word0 = *(ptr++);
        const auto encoding = GetInstructionEncoding(word0);

        if (encoding == InstEncoding::SOP1) {
            const auto opcode = OpcodeSOP1((word0 >> 8) & 0xff);
            ASSERT(opcode == OpcodeSOP1::S_SETPC_B64);
            break;
        } else if (encoding == InstEncoding::SMRD) {
            const auto opcode = OpcodeSMRD((word0 >> 22) & 0x1f);
            const u32 sdst = (word0 >> 15) & 0x7f;
            const u32 sbase = (word0 >> 9) & 0x3f;
            const u32 offset = word0 & 0xff;
            ASSERT(opcode == OpcodeSMRD::S_LOAD_DWORDX4);
            loads[sdst] = VsharpLoad{offset, sbase * 2, true};
        } else if (encoding == InstEncoding::VOP2) {
            const auto opcode = OpcodeVOP2((word0 >> 25) & 0x3f);
            const u32 src0 = word0 & 0x1ff;
            const u32 vsrc1 = (word0 >> 9) & 0xff;
            const u32 vdst = (word0 >> 17) & 0xff;
            ASSERT(opcode == OpcodeVOP2::V_ADD_I32 && src0 < 16 && vsrc1 == vdst);
            if (vdst == 0) {
                // V0 is always the vertex offset
                out_fetch_data.vertex_offset_sgpr = src0;
            } else if (vdst == 3) {
                // V3 is always the instance offset
                out_fetch_data.instance_offset_sgpr = src0;
            } else {
                UNREACHABLE();
            }
        } else if (encoding == InstEncoding::MUBUF || encoding == InstEncoding::MTBUF) {
            // SRSRC is in units of 4 SPGRs while SBASE is in pairs of SGPRs
            const u32 word1 = *(ptr++);
            const u32 srsrc = ((word1 >> 16) & 0x1f) * 4;
            auto& attrib = out_fetch_data.attributes.emplace_back();
            attrib.dest_vgpr = (word1 >> 8) & 0xff;
            if (encoding == InstEncoding::MUBUF) {
                const auto opcode = OpcodeMUBUF((word0 >> 18) & 0x7f);
                ASSERT(opcode >= OpcodeMUBUF::BUFFER_LOAD_FORMAT_X &&
                       opcode <= OpcodeMUBUF::BUFFER_LOAD_FORMAT_XYZW);
                attrib.num_elements = (std::to_underlying(opcode) -
                                       std::to_underlying(OpcodeMUBUF::BUFFER_LOAD_FORMAT_X)) +
                                      1;
            } else {
                const auto opcode = OpcodeMTBUF((word0 >> 16) & 0x7);
                ASSERT(opcode >= OpcodeMTBUF::TBUFFER_LOAD_FORMAT_X &&
                       opcode <= OpcodeMTBUF::TBUFFER_LOAD_FORMAT_XYZW);
                attrib.num_elements = (std::to_underlying(opcode) -
                                       std::to_underlying(OpcodeMTBUF::TBUFFER_LOAD_FORMAT_X)) +
                                      1;
            }
            if (loads[srsrc].loaded) {
                attrib.sgpr_base = loads[srsrc].base_sgpr;
                attrib.dword_offset = loads[srsrc].dword_offset;
            } else {
                // port: a V# the fetch shader does not load itself (PROTOTYPE 2): the vertex
                // shader loaded it before calling the fetch shader, or it is in the user data.
                // Read as before, the V# came from the fetch shader's own code.
                ResolveCallerVsharp(info, code, srsrc, attrib);
            }
            attrib.inst_offset = word0 & 0xfff;
            if ((word0 & 0xfff) > 0xff) {
                // port: VertexAttribute keeps 8 bits of the 12-bit offset (noted, not seen yet).
                LOG_ERROR(Render_Recompiler, "Fetch shader attribute offset {} does not fit 8 bits",
                          word0 & 0xfff);
            }
            attrib.instance_data = word1 & 0xff;
            if (encoding == InstEncoding::MTBUF) {
                attrib.data_format = (word0 >> 19) & 0xf;
                attrib.num_format = (word0 >> 23) & 0x7;
            } else {
                attrib.data_format = 0;
                attrib.num_format = 0;
            }
        } else if (encoding == InstEncoding::SOPP) {
            const auto opcode = OpcodeSOPP((word0 >> 16) & 0x7f);
            ASSERT(opcode == OpcodeSOPP::S_WAITCNT);
        } else if (encoding == InstEncoding::VOP1) {
            const auto opcode = OpcodeVOP1((word0 >> 9) & 0xff);
            const u32 src0 = word0 & 0x1ff;
            const u32 vdst = (word0 >> 17) & 0xff;
            ASSERT(opcode == OpcodeVOP1::V_MOV_B32 && src0 == 242);
            LOG_WARNING(Render_Recompiler, "Fetch shader has V{} = 1.0 which is ignored", vdst);
        } else {
            UNREACHABLE_MSG("Unexpected instruction encoding in fetch shader {}", u32(encoding));
        }
    }

    out_fetch_data.size = (ptr - code) * sizeof(u32);
    if (cache.size() >= 4096) {
        cache.clear();
    }
    cache[code] = CachedParse{std::vector<u32>(code, ptr), out_fetch_data};
    return true;
}

} // namespace Shader::Gcn
