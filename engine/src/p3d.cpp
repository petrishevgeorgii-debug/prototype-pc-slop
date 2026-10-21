// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: PROTOTYPE's Pure3D files (docs/formats/pure3d.md; tools/formats/pure3d.py
// is the same reader in Python).

#include "p3d.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <fstream>

#include "miniz.h"

namespace p3d {

namespace {

constexpr std::uint32_t kFile = 0xff443350;
constexpr std::uint32_t kMesh = 0x00010000, kPrimGroup = 0x00010020, kVertexFormat = 0x00010014,
                        kVertexBuffer = 0x00010012, kIndexBuffer = 0x00010013;
constexpr std::uint32_t kShader = 0x00011015, kShaderTexture = 0x00011016;
constexpr std::uint32_t kTexture = 0x00019000, kImageData = 0x00019002;

constexpr std::uint32_t kPosition = 0x2c929929, kUv0 = 0x00364509, kNormal = 0xc206bce7,
                        kColour0 = 0x3898fc04;

template <class T>
T Get(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

struct Reader {
    const std::uint8_t* p;
    const std::uint8_t* end;
    bool ok = true;

    bool Need(std::size_t n) {
        if (!ok || static_cast<std::size_t>(end - p) < n) {
            ok = false;
            return false;
        }
        return true;
    }
    std::uint8_t U8() { return Need(1) ? *p++ : 0; }
    std::uint16_t U16() {
        if (!Need(2)) return 0;
        auto v = Get<std::uint16_t>(p);
        p += 2;
        return v;
    }
    std::uint32_t U32() {
        if (!Need(4)) return 0;
        auto v = Get<std::uint32_t>(p);
        p += 4;
        return v;
    }
    /// A string: length byte, then that many bytes, zero-padded.
    std::string Str() {
        const std::size_t n = U8();
        if (!Need(n)) return {};
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        s.resize(std::strlen(s.c_str()));
        return s;
    }
};

bool Parse(const std::uint8_t* base, std::size_t offset, std::size_t end, std::vector<Chunk>& out) {
    while (offset + 12 <= end) {
        const auto id = Get<std::uint32_t>(base + offset);
        const auto data_size = Get<std::uint32_t>(base + offset + 4);
        const auto total = Get<std::uint32_t>(base + offset + 8);
        if (data_size < 12 || total < data_size || offset + total > end) {
            return false;
        }
        Chunk c;
        c.id = id;
        c.data = base + offset + 12;
        c.size = data_size - 12;
        if (!Parse(base, offset + data_size, offset + total, c.children)) {
            return false;
        }
        out.push_back(std::move(c));
        offset += total;
    }
    return true;
}

template <class F>
void Walk(const std::vector<Chunk>& chunks, const F& f) {
    for (const auto& c : chunks) {
        f(c);
        Walk(c.children, f);
    }
}

void Grow(float* lo, float* hi, const float* p) {
    for (int k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], p[k]);
        hi[k] = std::max(hi[k], p[k]);
    }
}

struct Element {
    std::uint32_t hash, packed, offset;
    std::uint16_t stride, components;
    std::uint8_t kind;
};

bool ReadPart(const Chunk& group, Part& part) {
    Reader r{group.data, group.data + group.size};
    r.U32();
    part.shader = r.Str();
    const std::uint32_t kind = r.U32();
    r.U32();
    r.U32();
    const std::uint32_t index_count = r.U32();
    const Chunk* format = group.First(kVertexFormat);
    const Chunk* vb = group.First(kVertexBuffer);
    const Chunk* ib = group.First(kIndexBuffer);
    if (!r.ok || kind != 0 || !format || !vb || !ib || vb->size < 12 || ib->size < 12) {
        return false;
    }
    Reader f{format->data, format->data + format->size};
    f.U16(), f.U16(), f.U32(), f.U32();
    const std::uint32_t element_bytes = f.U32();
    std::vector<Element> elements;
    for (std::uint32_t i = 0; i < element_bytes / 17 && f.ok; ++i) {
        Element e{};
        e.hash = f.U32();
        e.packed = f.U32();
        e.offset = f.U32();
        e.stride = f.U16();
        e.components = f.U16();
        e.kind = f.U8();
        elements.push_back(e);
    }
    if (!f.ok || elements.empty() || elements[0].stride == 0) {
        return false;
    }
    const std::uint32_t stride = elements[0].stride;
    const auto find = [&](std::uint32_t hash) -> const Element* {
        for (const auto& e : elements) {
            if (e.hash == hash) return &e;
        }
        return nullptr;
    };
    const Element* position = find(kPosition);
    const Element* uv = find(kUv0);
    const Element* normal = find(kNormal);
    const Element* colour = find(kColour0);
    if (!position) {
        return false;
    }
    const std::uint32_t vb_size = std::min<std::uint32_t>(Get<std::uint32_t>(vb->data + 8),
                                                          static_cast<std::uint32_t>(vb->size - 12));
    const std::uint8_t* v = vb->data + 12;
    const std::uint32_t count = vb_size / stride;
    part.vertices.resize(count);
    std::fill(part.min, part.min + 3, FLT_MAX);
    std::fill(part.max, part.max + 3, -FLT_MAX);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* src = v + std::size_t(i) * stride;
        Vertex& out = part.vertices[i];
        std::memcpy(out.position, src + position->offset, 12);
        if (uv && uv->offset + 8 <= stride) std::memcpy(out.uv, src + uv->offset, 8);
        else out.uv[0] = out.uv[1] = 0;
        if (normal && normal->offset + 12 <= stride) std::memcpy(out.normal, src + normal->offset, 12);
        else out.normal[0] = out.normal[1] = out.normal[2] = 0;
        out.colour = colour && colour->offset + 4 <= stride ? Get<std::uint32_t>(src + colour->offset) : 0xffffffffu;
        Grow(part.min, part.max, out.position);
    }
    const std::uint32_t ib_size = std::min<std::uint32_t>(Get<std::uint32_t>(ib->data + 8),
                                                          static_cast<std::uint32_t>(ib->size - 12));
    const bool wide = index_count && ib_size == 4 * index_count;
    const std::uint32_t n = ib_size / (wide ? 4 : 2) / 3 * 3;
    part.indices.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t idx = wide ? Get<std::uint32_t>(ib->data + 12 + 4 * i) : Get<std::uint16_t>(ib->data + 12 + 2 * i);
        part.indices[i] = idx < count ? idx : 0;
    }
    return count > 0 && n > 0;
}

/// Element order inside an 8 x 8 micro tile of a "thin" tile mode: x0 y0 x1 y1 x2 y2.
void ThinMicroTile(int i, int& x, int& y) {
    x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
    y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
}

bool ReadTexture(const Chunk& texture, Texture& out, std::string& why) {
    Reader r{texture.data, texture.data + texture.size};
    out.name = r.Str();
    const Chunk* image = nullptr;
    Walk(texture.children, [&](const Chunk& c) {
        if (c.id == kImageData && !image) image = &c;
    });
    if (!image) {
        why = "no image data";
        return false;
    }
    const std::uint8_t* gnf = nullptr;
    for (std::size_t i = 0; i + 4 <= std::min<std::size_t>(image->size, 64); ++i) {
        if (std::memcmp(image->data + i, "GNF ", 4) == 0) {
            gnf = image->data + i;
            break;
        }
    }
    const std::uint8_t* end = image->data + image->size;
    if (!gnf || end - gnf < 48) {
        why = "not a GNF";
        return false;
    }
    std::uint32_t w[8];
    std::memcpy(w, gnf + 16, sizeof(w));
    const std::uint32_t format = (w[1] >> 20) & 0x3f;
    out.width = (w[2] & 0x3fff) + 1;
    out.height = ((w[2] >> 14) & 0x3fff) + 1;
    const std::uint32_t last_level = (w[3] >> 16) & 0xf;
    const std::uint32_t tiling = (w[3] >> 20) & 0x1f;
    const std::uint32_t pitch = ((w[4] >> 13) & 0x3fff) + 1;
    const std::uint32_t header = Get<std::uint32_t>(gnf + 4);
    const std::uint8_t* pixels = gnf + 8 + header;
    std::size_t block = 0;
    switch (format) {
    case 35: out.format = TextureFormat::BC1; block = 8; break;
    case 36: out.format = TextureFormat::BC2; block = 16; break;
    case 37: out.format = TextureFormat::BC3; block = 16; break;
    default: why = "data format " + std::to_string(format); return false;
    }
    if (tiling != 13) { // Thin1DThin: 8 x 8 micro tiles in rows
        why = "tile mode " + std::to_string(tiling);
        return false;
    }
    // Levels follow each other, each a whole number of micro tiles.
    std::size_t at = 0;
    for (std::uint32_t level = 0; level <= last_level; ++level) {
        const std::uint32_t lw = std::max(1u, out.width >> level), lh = std::max(1u, out.height >> level);
        const std::uint32_t lp = std::max(1u, pitch >> level);
        const std::uint32_t bw = (lw + 3) / 4, bh = (lh + 3) / 4;
        const std::uint32_t tiles_x = ((lp + 3) / 4 + 7) / 8, tiles_y = (bh + 7) / 8;
        const std::size_t size = std::size_t(tiles_x) * tiles_y * 64 * block;
        if (pixels + at + size > end) {
            break;
        }
        std::vector<std::uint8_t> linear(std::size_t(bw) * bh * block);
        for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
            for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
                const std::uint8_t* tile = pixels + at + (std::size_t(ty) * tiles_x + tx) * 64 * block;
                for (int k = 0; k < 64; ++k) {
                    int x, y;
                    ThinMicroTile(k, x, y);
                    const std::uint32_t bx = tx * 8 + x, by = ty * 8 + y;
                    if (bx < bw && by < bh) {
                        std::memcpy(linear.data() + (std::size_t(by) * bw + bx) * block, tile + k * block, block);
                    }
                }
            }
        }
        out.levels.push_back(std::move(linear));
        at += size;
    }
    if (out.levels.empty()) {
        why = "data too short";
        return false;
    }
    return true;
}

} // namespace

const Chunk* Chunk::First(std::uint32_t child_id) const {
    for (const auto& c : children) {
        if (c.id == child_id) return &c;
    }
    return nullptr;
}

bool Load(const std::filesystem::path& path, File& file, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() >= 16 && raw[0] == 'R' && raw[1] == 'Z') {
        // "RZ", 6 bytes, the unpacked size (u64), one zlib stream.
        const auto size = Get<std::uint64_t>(raw.data() + 8);
        if (size > (1ull << 31)) {
            error = path.string() + ": unpacked size too large";
            return false;
        }
        file.bytes.resize(size);
        mz_ulong out_size = static_cast<mz_ulong>(size);
        if (mz_uncompress(file.bytes.data(), &out_size, raw.data() + 16, static_cast<mz_ulong>(raw.size() - 16)) != MZ_OK ||
            out_size != size) {
            error = path.string() + ": cannot unpack";
            return false;
        }
    } else {
        file.bytes = std::move(raw);
    }
    if (file.bytes.size() < 12 || Get<std::uint32_t>(file.bytes.data()) != kFile) {
        // An empty cell is an "RZ" of a few bytes with nothing in it.
        if (file.bytes.empty()) return true;
        error = path.string() + ": not a Pure3D file";
        return false;
    }
    const auto total = std::min<std::size_t>(Get<std::uint32_t>(file.bytes.data() + 8), file.bytes.size());
    if (!Parse(file.bytes.data(), 12, total, file.chunks)) {
        error = path.string() + ": bad chunk";
        return false;
    }
    return true;
}

void ReadMeshes(const File& file, std::vector<Mesh>& meshes) {
    Walk(file.chunks, [&](const Chunk& c) {
        if (c.id != kMesh) return;
        Mesh mesh;
        Reader r{c.data, c.data + c.size};
        mesh.name = r.Str();
        std::fill(mesh.min, mesh.min + 3, FLT_MAX);
        std::fill(mesh.max, mesh.max + 3, -FLT_MAX);
        for (const auto& g : c.children) {
            if (g.id != kPrimGroup) continue;
            Part part;
            if (ReadPart(g, part)) {
                Grow(mesh.min, mesh.max, part.min);
                Grow(mesh.min, mesh.max, part.max);
                mesh.parts.push_back(std::move(part));
            }
        }
        if (!mesh.parts.empty()) meshes.push_back(std::move(mesh));
    });
}

void ReadShaders(const File& file, std::vector<Shader>& shaders) {
    Walk(file.chunks, [&](const Chunk& c) {
        if (c.id != kShader) return;
        Shader s;
        Reader r{c.data, c.data + c.size};
        s.name = r.Str();
        r.U16(), r.U16();
        s.effect = r.Str();
        for (const auto& t : c.children) {
            if (t.id != kShaderTexture) continue;
            Reader tr{t.data, t.data + t.size};
            std::string param = tr.Str();
            std::string texture = tr.Str();
            if (tr.ok) s.textures[param] = texture;
        }
        shaders.push_back(std::move(s));
    });
}

void ReadTextures(const File& file, std::vector<Texture>& textures, std::vector<std::string>* skipped) {
    Walk(file.chunks, [&](const Chunk& c) {
        if (c.id != kTexture) return;
        Texture t;
        std::string why;
        if (ReadTexture(c, t, why)) {
            textures.push_back(std::move(t));
        } else if (skipped) {
            skipped->push_back(t.name + ": " + why);
        }
    });
}

void ReadPlacements(const File& file, std::vector<Placement>& placements) {
    Walk(file.chunks, [&](const Chunk& c) {
        if (c.id != 0x07f00000) return;
        Reader r{c.data, c.data + c.size};
        const std::string full = r.Str();
        const auto sep = full.find("(1):");
        if (sep == std::string::npos || full.compare(0, sep, "GameObject") == 0) return;
        const Chunk* attributes = c.First(0x07f00001);
        if (!attributes || attributes->size < 8 + 28 || std::memcmp(attributes->data + 4, "META", 4) != 0) return;
        Placement p;
        p.type = full.substr(0, sep);
        p.name = full.substr(sep + 4);
        std::memcpy(p.position, attributes->data + 8, 12);
        std::memcpy(p.rotation, attributes->data + 20, 16);
        if (p.type == "proto::CloudSpawner") {
            Reader rest{attributes->data + 36, attributes->data + attributes->size};
            p.prop = rest.Str();
            if (!rest.ok) p.prop.clear();
        }
        placements.push_back(std::move(p));
    });
}

std::string ColourTexture(const Shader& shader) {
    for (const char* param : {"color", "colour", "diffuse", "color_inards", "color_infection", "Diffuse"}) {
        if (auto it = shader.textures.find(param); it != shader.textures.end()) return it->second;
    }
    return {};
}

} // namespace p3d
