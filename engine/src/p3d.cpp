// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: PROTOTYPE's Pure3D files (docs/formats/pure3d.md; tools/formats/pure3d.py
// is the same reader in Python).

#include "p3d.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
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
                        kColour0 = 0x3898fc04, kUv1 = 0x0036450a, kTangent = 0xa4176245, kColour1 = 0x3898fc05;

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
    float F32() {
        if (!Need(4)) return 0;
        auto v = Get<float>(p);
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

/// The first `text` in [begin, end), or null.
const std::uint8_t* Find(const std::uint8_t* begin, const std::uint8_t* end, const char* text) {
    const std::size_t n = std::strlen(text);
    for (const std::uint8_t* p = begin; p + n <= end; ++p) {
        if (std::memcmp(p, text, n) == 0) return p;
    }
    return nullptr;
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

/// A skinned part's extra data: per vertex, up to four skeleton joints and their weights.
struct SkinData {
    std::vector<std::array<std::uint8_t, 4>>* joints = nullptr;
    std::vector<std::array<float, 4>>* weights = nullptr;
};

constexpr std::uint32_t kWeights = 0x49570cfb, kJoints = 0x73d5cba7;
constexpr std::uint32_t kMatrixPalette = 0x0001000d;

/// The palette's joints: what a skinned vertex's joint numbers stand for.
std::vector<std::uint32_t> Palette(const Chunk& group) {
    std::vector<std::uint32_t> palette;
    if (const Chunk* p = group.First(kMatrixPalette); p && p->size >= 4) {
        const std::uint32_t n = Get<std::uint32_t>(p->data);
        for (std::uint32_t i = 0; i < n && 4 + 4 * (i + 1) <= p->size; ++i) palette.push_back(Get<std::uint32_t>(p->data + 4 + 4 * i));
    }
    return palette;
}

void SetWeights(SkinData& skin, std::size_t i, const float w3[3], const std::uint8_t j4[4], const std::vector<std::uint32_t>& palette) {
    if (!skin.joints) return;
    std::array<float, 4> w = {w3[0], w3[1], w3[2], std::max(0.0f, 1.0f - w3[0] - w3[1] - w3[2])};
    std::array<std::uint8_t, 4> j{};
    for (int k = 0; k < 4; ++k) {
        const std::uint32_t joint = j4[k] < palette.size() ? palette[j4[k]] : 0;
        j[k] = static_cast<std::uint8_t>(std::min<std::uint32_t>(joint, 255));
    }
    (*skin.joints)[i] = j;
    (*skin.weights)[i] = w;
}

/// The older layout: a list per attribute (PositionList, NormalList, UVList, ...).
bool ReadListPart(const Chunk& group, Part& part, SkinData skin) {
    const Chunk* positions = group.First(0x00010005);
    const Chunk* indices = group.First(0x0001000a);
    if (!positions || !indices || positions->size < 4 || indices->size < 4) return false;
    const std::uint32_t count = std::min<std::uint32_t>(Get<std::uint32_t>(positions->data), static_cast<std::uint32_t>((positions->size - 4) / 12));
    part.vertices.assign(count, Vertex{});
    if (skin.joints) skin.joints->assign(count, {}), skin.weights->assign(count, {1, 0, 0, 0});
    const auto list = [&](std::uint32_t id, std::size_t header, std::size_t element) -> const std::uint8_t* {
        const Chunk* c = group.First(id);
        if (!c || c->size < header + element * count) return nullptr;
        return c->data + header;
    };
    const std::uint8_t* normals = list(0x00010006, 4, 12);
    const std::uint8_t* uvs = list(0x00010007, 8, 8);
    const std::uint8_t* colours = list(0x00010008, 4, 4);
    const std::uint8_t* tangents = list(0x00010028, 4, 16);
    const std::uint8_t* matrices = list(0x0001000b, 4, 4);
    const std::uint8_t* weights = list(0x0001000c, 4, 12);
    const auto palette = Palette(group);
    std::fill(part.min, part.min + 3, FLT_MAX);
    std::fill(part.max, part.max + 3, -FLT_MAX);
    for (std::uint32_t i = 0; i < count; ++i) {
        Vertex& v = part.vertices[i];
        std::memcpy(v.position, positions->data + 4 + 12 * i, 12);
        if (normals) std::memcpy(v.normal, normals + 12 * i, 12);
        if (uvs) std::memcpy(v.uv, uvs + 8 * i, 8);
        if (tangents) std::memcpy(v.tangent, tangents + 16 * i, 16);
        v.colour = colours ? Get<std::uint32_t>(colours + 4 * i) : 0xffffffffu;
        if (matrices && weights) SetWeights(skin, i, reinterpret_cast<const float*>(weights + 12 * i), matrices + 4 * i, palette);
        Grow(part.min, part.max, v.position);
    }
    const std::uint32_t n = std::min<std::uint32_t>(Get<std::uint32_t>(indices->data), static_cast<std::uint32_t>((indices->size - 4) / 4)) / 3 * 3;
    part.indices.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t idx = Get<std::uint32_t>(indices->data + 4 + 4 * i);
        part.indices[i] = idx < count ? idx : 0;
    }
    return count > 0 && n > 0;
}

bool ReadPart(const Chunk& group, Part& part, SkinData skin = {}) {
    Reader r{group.data, group.data + group.size};
    r.U32();
    part.shader = r.Str();
    const std::uint32_t kind = r.U32();
    r.U32();
    r.U32();
    const std::uint32_t index_count = r.U32();
    if (!r.ok || kind != 0) return false;
    // Vertex streams: each VertexFormat describes the VertexBuffer in the same place.
    std::vector<const Chunk*> formats, buffers;
    for (const Chunk& c : group.children) {
        if (c.id == kVertexFormat) formats.push_back(&c);
        if (c.id == kVertexBuffer) buffers.push_back(&c);
    }
    const Chunk* ib = group.First(kIndexBuffer);
    if (formats.empty()) return ReadListPart(group, part, skin);
    if (buffers.size() < formats.size() || !ib || ib->size < 12) return false;
    struct Stream {
        const std::uint8_t* data;
        std::uint32_t stride, count;
    };
    struct Found {
        const Element* e = nullptr;
        int stream = -1;
    };
    std::vector<std::vector<Element>> elements(formats.size());
    std::vector<Stream> streams;
    std::uint32_t count = UINT32_MAX;
    for (std::size_t s = 0; s < formats.size(); ++s) {
        Reader f{formats[s]->data, formats[s]->data + formats[s]->size};
        f.U16(), f.U16(), f.U32(), f.U32();
        const std::uint32_t element_bytes = f.U32();
        for (std::uint32_t i = 0; i < element_bytes / 17 && f.ok; ++i) {
            Element e{};
            e.hash = f.U32();
            e.packed = f.U32();
            e.offset = f.U32();
            e.stride = f.U16();
            e.components = f.U16();
            e.kind = f.U8();
            elements[s].push_back(e);
        }
        if (!f.ok || elements[s].empty() || elements[s][0].stride == 0 || buffers[s]->size < 12) return false;
        const std::uint32_t stride = elements[s][0].stride;
        const std::uint32_t size = std::min<std::uint32_t>(Get<std::uint32_t>(buffers[s]->data + 8),
                                                           static_cast<std::uint32_t>(buffers[s]->size - 12));
        streams.push_back({buffers[s]->data + 12, stride, size / stride});
        count = std::min(count, size / stride);
    }
    const auto find = [&](std::uint32_t hash, std::uint32_t bytes) {
        for (std::size_t s = 0; s < elements.size(); ++s) {
            for (const auto& e : elements[s]) {
                if (e.hash == hash && e.offset + bytes <= streams[s].stride) return Found{&e, static_cast<int>(s)};
            }
        }
        return Found{};
    };
    const Found position = find(kPosition, 12), uv = find(kUv0, 8), normal = find(kNormal, 12), colour = find(kColour0, 4),
                uv1 = find(kUv1, 8), tangent = find(kTangent, 16), colour1 = find(kColour1, 4), weights = find(kWeights, 12),
                joints = find(kJoints, 4);
    if (!position.e || count == 0) return false;
    const auto at = [&](const Found& f, std::uint32_t i) { return streams[f.stream].data + std::size_t(i) * streams[f.stream].stride + f.e->offset; };
    const bool skinned = skin.joints && weights.e && joints.e;
    if (skin.joints) skin.joints->assign(count, {}), skin.weights->assign(count, {1, 0, 0, 0});
    const auto palette = skinned ? Palette(group) : std::vector<std::uint32_t>{};
    part.vertices.resize(count);
    std::fill(part.min, part.min + 3, FLT_MAX);
    std::fill(part.max, part.max + 3, -FLT_MAX);
    for (std::uint32_t i = 0; i < count; ++i) {
        Vertex& out = part.vertices[i];
        std::memcpy(out.position, at(position, i), 12);
        if (uv.e) std::memcpy(out.uv, at(uv, i), 8);
        else out.uv[0] = out.uv[1] = 0;
        if (normal.e) std::memcpy(out.normal, at(normal, i), 12);
        else out.normal[0] = out.normal[1] = out.normal[2] = 0;
        out.colour = colour.e ? Get<std::uint32_t>(at(colour, i)) : 0xffffffffu;
        if (uv1.e) std::memcpy(out.uv1, at(uv1, i), 8);
        else out.uv1[0] = out.uv1[1] = 0;
        if (tangent.e) std::memcpy(out.tangent, at(tangent, i), 16);
        else std::fill(out.tangent, out.tangent + 4, 0.0f);
        out.colour1 = colour1.e ? Get<std::uint32_t>(at(colour1, i)) : 0u;
        if (skinned) {
            float w[3];
            std::memcpy(w, at(weights, i), 12);
            SetWeights(skin, i, w, at(joints, i), palette);
        }
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
    return n > 0;
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

void ReadSkins(const File& file, std::vector<Skin>& skins) {
    for (const Chunk& c : file.chunks) {
        if (c.id != 0x00010001) continue;
        Reader r{c.data, c.data + c.size};
        Skin skin;
        skin.name = r.Str();
        r.U32();
        skin.skeleton = r.Str();
        for (const Chunk& g : c.children) {
            if (g.id != kPrimGroup) continue;
            SkinPart sp;
            if (ReadPart(g, sp.part, SkinData{&sp.joints, &sp.weights})) skin.parts.push_back(std::move(sp));
        }
        if (!skin.parts.empty()) skins.push_back(std::move(skin));
    }
}

namespace {

float Half(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    float v;
    if (exponent == 0) v = std::ldexp(float(mantissa), -24);
    else if (exponent == 31) v = mantissa ? NAN : INFINITY;
    else v = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    return sign ? -v : v;
}

std::array<float, 4> FromXyz(float x, float y, float z) {
    const float w2 = 1.0f - (x * x + y * y + z * z);
    return {x, y, z, w2 > 0 ? std::sqrt(w2) : 0.0f};
}

/// One channel's keys: frames, then values, from the chunk itself or (compressed animations)
/// from the decompressed block at the offset its 0x121120 child gives, each run padded to 4.
bool ReadChannel(const Chunk& k, const std::vector<std::uint8_t>& block, AnimationTrack& track) {
    const std::uint8_t* d = k.data;
    const std::uint8_t* end = k.data + k.size;
    if (k.size < 8) return false;
    const std::uint32_t id = k.id;
    const bool reduced = id == 0x00121102 || id == 0x00121103 || id == 0x00121118; // one axis fixed or two
    std::uint16_t axis = 0;
    float base[3] = {0, 0, 0};
    std::size_t at = 8;
    if (reduced) {
        if (k.size < 22) return false;
        axis = Get<std::uint16_t>(d + 8);
        std::memcpy(base, d + 10, 12);
        at = 22;
    }
    const std::uint8_t* src = d;
    const std::uint8_t* src_end = end;
    std::uint32_t n = 0;
    bool padded = false;
    if (const Chunk* c = k.First(0x00121120); c && c->size >= 12) {
        n = Get<std::uint32_t>(c->data + 4);
        at = Get<std::uint32_t>(c->data + 8);
        src = block.data();
        src_end = block.data() + block.size();
        padded = true;
    } else {
        if (d + at + 4 > end) return false;
        n = Get<std::uint32_t>(d + at);
        at += 4;
    }
    std::size_t value_bytes = 0;
    switch (id) {
    case 0x00121112: value_bytes = 6; break;  // quaternion: x y z as s16
    case 0x00121114: value_bytes = 3; break;  // quaternion: x y z as s8
    case 0x00121119: value_bytes = 6; break;  // vector: 3 halves
    case 0x00121104: value_bytes = 12; break; // vector: 3 floats
    case 0x00121102: value_bytes = 4; break;  // one float, the rest from the base
    case 0x00121103: value_bytes = 8; break;  // two floats
    case 0x00121118: value_bytes = 4; break;  // two halves
    default: return false;
    }
    if (n == 0 || n > 100000 || !src) return false;
    if (src + at + 2 * n > src_end) return false;
    track.times.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) track.times[i] = Get<std::uint16_t>(src + at + 2 * i);
    at += 2 * n;
    if (padded) at = (at + 3) & ~std::size_t(3);
    if (src + at + value_bytes * n > src_end) return false;
    track.values.resize(n);
    const std::uint8_t* v = src + at;
    int other[2] = {axis == 0 ? 1 : 0, axis == 2 ? 1 : 2};
    for (std::uint32_t i = 0; i < n; ++i) {
        auto& out = track.values[i];
        const std::uint8_t* p = v + value_bytes * i;
        switch (id) {
        case 0x00121112:
            out = FromXyz(Get<std::int16_t>(p) / 32767.0f, Get<std::int16_t>(p + 2) / 32767.0f, Get<std::int16_t>(p + 4) / 32767.0f);
            break;
        case 0x00121114:
            out = FromXyz(std::int8_t(p[0]) / 127.0f, std::int8_t(p[1]) / 127.0f, std::int8_t(p[2]) / 127.0f);
            break;
        case 0x00121119:
            out = {Half(Get<std::uint16_t>(p)), Half(Get<std::uint16_t>(p + 2)), Half(Get<std::uint16_t>(p + 4)), 0};
            break;
        case 0x00121104: out = {Get<float>(p), Get<float>(p + 4), Get<float>(p + 8), 0}; break;
        case 0x00121102:
            out = {base[0], base[1], base[2], 0};
            if (axis < 3) out[axis] = Get<float>(p);
            break;
        case 0x00121103:
            out = {base[0], base[1], base[2], 0};
            out[other[0]] = Get<float>(p), out[other[1]] = Get<float>(p + 4);
            break;
        case 0x00121118:
            out = {base[0], base[1], base[2], 0};
            out[other[0]] = Half(Get<std::uint16_t>(p)), out[other[1]] = Half(Get<std::uint16_t>(p + 2));
            break;
        }
    }
    return true;
}

} // namespace

void ReadAnimations(const File& file, std::vector<Animation>& animations, const std::vector<std::string>* only) {
    for (const Chunk& c : file.chunks) {
        if (c.id != 0x00121000 || c.size < 8) continue;
        Reader r{c.data + 4, c.data + c.size};
        Animation a;
        a.name = r.Str();
        if (only && std::find(only->begin(), only->end(), a.name) == only->end()) continue;
        r.U32(); // "PTRN"
        a.frames = r.F32();
        a.fps = r.F32();
        if (!r.ok || a.fps <= 0) continue;
        std::vector<std::uint8_t> block;
        if (const Chunk* z = c.First(0x02f00000); z && z->size > 16 && std::memcmp(z->data + 4, "ZLIB", 4) == 0) {
            mz_ulong size = Get<std::uint32_t>(z->data + 8);
            block.resize(size);
            if (mz_uncompress(block.data(), &size, z->data + 16, static_cast<mz_ulong>(z->size - 16)) != MZ_OK) continue;
            block.resize(size);
        }
        const Chunk* groups = c.First(0x00121002);
        if (!groups) continue;
        for (const Chunk& g : groups->children) {
            if (g.id != 0x00121001) continue;
            Reader gr{g.data + 4, g.data + g.size};
            const std::string joint = gr.Str();
            for (const Chunk& k : g.children) {
                if (k.size < 8) continue;
                AnimationTrack t;
                t.joint = joint;
                if (std::memcmp(k.data + 4, "ROT", 3) == 0) t.kind = AnimationTrack::Kind::Rotation;
                else if (std::memcmp(k.data + 4, "TRAN", 4) == 0) t.kind = AnimationTrack::Kind::Translation;
                else continue;
                if (ReadChannel(k, block, t)) a.tracks.push_back(std::move(t));
            }
        }
        animations.push_back(std::move(a));
    }
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

namespace {

/// Chunk data that starts with zero words before its name (the newer chunks: 4 or 8 bytes).
Reader AfterZeros(const Chunk& c, std::size_t words) {
    std::size_t skip = 0;
    while (skip < words * 4 && skip + 4 <= c.size && Get<std::uint32_t>(c.data + skip) == 0) skip += 4;
    return Reader{c.data + skip, c.data + c.size};
}

} // namespace

void ReadSkeletons(const File& file, std::vector<Skeleton>& skeletons) {
    for (const Chunk& c : file.chunks) {
        if (c.id != 0x00023000) continue;
        Skeleton s;
        Reader r{c.data, c.data + c.size};
        s.name = r.Str();
        for (const Chunk& j : c.children) {
            if (j.id != 0x00023001) continue;
            Reader jr{j.data, j.data + j.size};
            Joint joint;
            joint.name = jr.Str();
            joint.parent = jr.U32();
            for (float& f : joint.matrix) f = jr.F32();
            if (!jr.ok) break;
            s.joints.push_back(std::move(joint));
        }
        skeletons.push_back(std::move(s));
    }
}

void ReadComposites(const File& file, std::vector<Composite>& composites) {
    for (const Chunk& c : file.chunks) {
        if (c.id != 0x00123000) continue;
        Composite composite;
        Reader r = AfterZeros(c, 1);
        composite.name = r.Str();
        composite.skeleton = r.Str();
        for (const Chunk& p : c.children) {
            if (p.id != 0x00123001) continue;
            Reader pr = AfterZeros(p, 2);
            Composite::Prop prop;
            prop.mesh = pr.Str();
            pr.U32(); // translucent
            prop.joint = pr.U32();
            if (pr.ok) composite.props.push_back(std::move(prop));
        }
        composites.push_back(std::move(composite));
    }
}

void ReadPalette(const File& file, std::vector<std::array<float, 3>>& colours) {
    // Big-endian attributes ("ATEM"): "color_variation", u32 count, then each entry: u32,
    // "color_variation", r g b and a fourth float.
    const auto be = [](const std::uint8_t* p) {
        const std::uint32_t u = std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    };
    Walk(file.chunks, [&](const Chunk& c) {
        if (c.id != 0x07f00001) return;
        const std::uint8_t* end = c.data + c.size;
        const std::uint8_t* p = Find(c.data, end, "color_variation");
        if (!p) return;
        p += 15; // the list's own name; its entries follow
        while ((p = Find(p, end, "color_variation")) && p + 15 + 12 <= end) {
            p += 15;
            const std::array<float, 3> rgb = {be(p), be(p + 4), be(p + 8)};
            bool sane = true;
            for (float v : rgb) sane = sane && v >= 0.0f && v <= 2.0f;
            if (sane) colours.push_back(rgb);
        }
    });
}

std::string ColourTexture(const Shader& shader) {
    for (const char* param : {"color", "colour", "diffuse", "color_inards", "color_infection", "Diffuse", "Color", "bottom"}) {
        if (auto it = shader.textures.find(param); it != shader.textures.end() && !it->second.empty()) return it->second;
    }
    return {};
}

std::string NormalTexture(const Shader& shader) {
    for (const char* param : {"normal", "normalMap", "normalmap", "normalbottom"}) {
        if (auto it = shader.textures.find(param); it != shader.textures.end() && !it->second.empty()) return it->second;
    }
    return {};
}

namespace {

bool Has(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

} // namespace

Surface SurfaceOf(const std::string& effect) {
    // Light cones and glows, particles, the interface: not surfaces.
    if (Has(effect, "light_decal") || effect.rfind("fx_", 0) == 0 || effect.rfind("hud_", 0) == 0 ||
        Has(effect, "_lights") || Has(effect, "_traffic") || Has(effect, "bridgelights") || Has(effect, "lit_sign") || Has(effect, "PostProc")) {
        return Surface::Skip;
    }
    if (Has(effect, "decal")) return Surface::Decal;
    // The effects named by their inputs (1c1n1a: a colour, a normal and an alpha map) and the
    // ones with "alpha" in the name are cut out.
    if (Has(effect, "alpha") || Has(effect, "1a") || Has(effect, "tree") || Has(effect, "sign_alpha")) return Surface::AlphaTest;
    return Surface::Opaque;
}

bool IsGlass(const std::string& effect) {
    return Has(effect, "vehicle_glass");
}

bool IsPainted(const std::string& effect) {
    return Has(effect, "colorvar");
}

bool TintedByVertices(const std::string& effect) {
    return effect.rfind("zCBV", 0) == 0 || Has(effect, "CBV") || Has(effect, "_vtx");
}

namespace {

void RotateByQuaternion(const float q[4], const float v[3], float out[3]) {
    const float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

} // namespace

void ReadLightRig(const File& file, std::vector<Light>& lights, std::vector<District>& districts) {
    // The world's objects: "GameObject(1):name", attributes "META", then named attributes:
    // transform (MatrixAttribute: u16, position, rotation quaternion x y z w) and behaviours.
    for (const Chunk& c : file.chunks) {
        if (c.id != 0x07f00000) continue;
        Reader r{c.data, c.data + c.size};
        const std::string full = r.Str();
        const std::string prefix = "GameObject(1):";
        if (full.compare(0, prefix.size(), prefix) != 0) continue;
        const Chunk* a = c.First(0x07f00001);
        if (!a) continue;
        const std::uint8_t* end = a->data + a->size;
        const std::uint8_t* m = Find(a->data, end, "MatrixAttribute");
        if (!m || m + 15 + 2 + 28 > end) continue;
        float transform[7];
        std::memcpy(transform, m + 17, sizeof(transform));
        const std::string name = full.substr(prefix.size());
        if (const std::uint8_t* l = Find(m, end, "engine::LightBehaviour"); l && l + 22 + 16 + 16 <= end) {
            Light light;
            light.name = name;
            const float forward[3] = {0, 0, 1};
            RotateByQuaternion(transform + 3, forward, light.direction);
            float rgba[4];
            std::memcpy(rgba, l + 22 + 16, sizeof(rgba));
            bool sane = true;
            for (float v : rgba) sane = sane && v >= 0.0f && v < 16.0f;
            if (!sane) continue;
            std::copy(rgba, rgba + 3, light.colour);
            light.intensity = rgba[3];
            lights.push_back(light);
        } else if (Find(m, end, "proto::DistrictBehaviour")) {
            District d;
            d.name = name;
            std::copy(transform, transform + 3, d.position);
            // The vehicles it spawns: "|name", u32 0, u32 count, then each: u32-length name,
            // f32 weight, 4 u32.
            if (const std::uint8_t* list = Find(m, end, ("|" + name).c_str())) {
                const std::uint8_t* p = list + 1 + name.size() + 4;
                std::uint32_t count = 0;
                if (p + 4 <= end) std::memcpy(&count, p, 4), p += 4;
                for (std::uint32_t i = 0; i < count && i < 64 && p + 4 <= end; ++i) {
                    std::uint32_t n = 0;
                    std::memcpy(&n, p, 4);
                    if (n == 0 || n > 64 || p + 4 + n + 20 > end) break;
                    std::string vehicle(reinterpret_cast<const char*>(p + 4), n);
                    float weight = 0;
                    std::memcpy(&weight, p + 4 + n, 4);
                    p += 4 + n + 20;
                    if (weight > 0 && weight < 1000) d.vehicles.emplace_back(vehicle, weight);
                }
            }
            if (d.position[1] > -1000.0f) districts.push_back(d); // not district_default
        }
    }
}

} // namespace p3d
