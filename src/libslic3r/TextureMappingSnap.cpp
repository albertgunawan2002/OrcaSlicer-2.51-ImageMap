#include "TextureMappingSnap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>

#include "Config.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"

namespace Slic3r {

namespace {

using RGB = std::array<float, 3>;
using Lab = std::array<float, 3>;

float clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; }

float srgb_to_linear(float c)
{
    c = clamp01(c);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

Lab to_oklab(const RGB &rgb)
{
    const float r = srgb_to_linear(rgb[0]);
    const float g = srgb_to_linear(rgb[1]);
    const float b = srgb_to_linear(rgb[2]);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return { 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
             1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
             0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s };
}

float lab_distance(const Lab &a, const Lab &b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

std::optional<RGB> parse_hex_rgb(const std::string &text)
{
    std::string hex = text;
    if (!hex.empty() && hex.front() == '#')
        hex.erase(hex.begin());
    if (hex.size() != 6 && hex.size() != 8)
        return std::nullopt;
    RGB out {};
    for (size_t c = 0; c < 3; ++c) {
        char *end = nullptr;
        const std::string part = hex.substr(c * 2, 2);
        const long v = std::strtol(part.c_str(), &end, 16);
        if (end == nullptr || *end != '\0')
            return std::nullopt;
        out[c] = float(v) / 255.f;
    }
    return out;
}

RGB background_rgb(const ModelVolume &volume)
{
    static constexpr const char *key = "texture_mapping_background_color";
    const ModelConfigObject *configs[] = { &volume.config, volume.get_object() != nullptr ? &volume.get_object()->config : nullptr };
    for (const ModelConfigObject *config : configs) {
        if (config == nullptr || !config->has(key))
            continue;
        if (const ConfigOptionString *opt = dynamic_cast<const ConfigOptionString *>(config->option(key)))
            if (std::optional<RGB> rgb = parse_hex_rgb(opt->value))
                return *rgb;
    }
    return { 1.f, 1.f, 1.f };
}

RGB composite(const std::array<float, 4> &rgba, const RGB &background)
{
    const float a = clamp01(rgba[3]);
    return { clamp01(rgba[0]) * a + background[0] * (1.f - a),
             clamp01(rgba[1]) * a + background[1] * (1.f - a),
             clamp01(rgba[2]) * a + background[2] * (1.f - a) };
}

std::array<float, 4> unpack_rgba(uint32_t packed)
{
    return { float((packed >> 24) & 0xFFu) / 255.f, float((packed >> 16) & 0xFFu) / 255.f,
             float((packed >> 8) & 0xFFu) / 255.f, float(packed & 0xFFu) / 255.f };
}

float wrap01(float v)
{
    if (!std::isfinite(v))
        return 0.f;
    if (v >= 0.f && v <= 1.f)
        return v;
    v -= std::floor(v);
    return v < 0.f ? v + 1.f : v;
}

std::array<float, 4> sample_texture(const ModelVolume &volume, float u, float v)
{
    const uint32_t w = volume.imported_texture_width;
    const uint32_t h = volume.imported_texture_height;
    const auto    &px = volume.imported_texture_rgba;
    const float x = wrap01(u) * float(w > 1 ? w - 1 : 0);
    const float y = wrap01(v) * float(h > 1 ? h - 1 : 0);
    const size_t x0 = std::min<size_t>(size_t(std::floor(x)), w - 1);
    const size_t y0 = std::min<size_t>(size_t(std::floor(y)), h - 1);
    const size_t x1 = std::min<size_t>(x0 + 1, w - 1);
    const size_t y1 = std::min<size_t>(y0 + 1, h - 1);
    const float tx = x - float(x0);
    const float ty = y - float(y0);
    std::array<float, 4> out {};
    for (size_t c = 0; c < 4; ++c) {
        auto at = [&](size_t sx, size_t sy) { return float(px[(sy * size_t(w) + sx) * 4 + c]) / 255.f; };
        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
        const float bot = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
        out[c] = top + (bot - top) * ty;
    }
    return out;
}

// Colour samples (corners and centre where available) for every triangle of the volume's mesh. Empty when the
// volume carries no colour data.
std::vector<std::vector<RGB>> triangle_color_samples(const ModelVolume &volume, const indexed_triangle_set &its)
{
    const size_t n = its.indices.size();
    std::vector<std::vector<RGB>> samples;
    const RGB bg = background_rgb(volume);

    if (!volume.texture_mapping_color_facets.empty()) {
        std::vector<ColorFacetTriangle> facets;
        volume.texture_mapping_color_facets.get_facet_triangles(volume, facets);
        samples.assign(n, {});
        for (const ColorFacetTriangle &facet : facets)
            if (facet.source_triangle >= 0 && size_t(facet.source_triangle) < n)
                samples[size_t(facet.source_triangle)].push_back(composite(unpack_rgba(facet.rgba), bg));
        return samples;
    }

    const bool has_uv_texture = !volume.imported_texture_rgba.empty() && volume.imported_texture_width > 0 &&
                                volume.imported_texture_height > 0 && volume.imported_texture_uv_valid.size() == n &&
                                volume.imported_texture_uvs_per_face.size() >= n * 6 &&
                                volume.imported_texture_rgba.size() >=
                                    size_t(volume.imported_texture_width) * size_t(volume.imported_texture_height) * 4;
    if (has_uv_texture) {
        samples.assign(n, {});
        for (size_t t = 0; t < n; ++t) {
            if (volume.imported_texture_uv_valid[t] == 0)
                continue;
            const auto &uv = volume.imported_texture_uvs_per_face;
            const size_t o = t * 6;
            const float cu = (uv[o] + uv[o + 2] + uv[o + 4]) / 3.f;
            const float cv = (uv[o + 1] + uv[o + 3] + uv[o + 5]) / 3.f;
            std::vector<RGB> &s = samples[t];
            for (size_t k = 0; k < 3; ++k)
                // Corners pulled slightly toward the centre so seams with a neighbouring UV island do not bleed in.
                s.push_back(composite(sample_texture(volume, uv[o + 2 * k] * 0.8f + cu * 0.2f, uv[o + 2 * k + 1] * 0.8f + cv * 0.2f), bg));
            s.push_back(composite(sample_texture(volume, cu, cv), bg));
        }
        return samples;
    }

    if (volume.imported_vertex_colors_rgba.size() == its.vertices.size() && !its.vertices.empty()) {
        samples.assign(n, {});
        for (size_t t = 0; t < n; ++t) {
            const auto &tri = its.indices[t];
            for (size_t k = 0; k < 3; ++k)
                if (tri[k] >= 0 && size_t(tri[k]) < its.vertices.size())
                    samples[t].push_back(composite(unpack_rgba(volume.imported_vertex_colors_rgba[size_t(tri[k])]), bg));
        }
    }
    return samples;
}

} // namespace

TextureSnapToFilamentsResult snap_texture_colors_to_filaments(const ModelVolume                       &volume,
                                                              TriangleSelector                        &selector,
                                                              const Transform3d                       &trafo_no_translate,
                                                              const std::vector<std::array<float, 3>> &filament_rgb,
                                                              int                                      volume_extruder_1based,
                                                              const TextureSnapToFilamentsParams      &params)
{
    TextureSnapToFilamentsResult result;
    const indexed_triangle_set &its = volume.mesh().its;
    const size_t n = its.indices.size();
    const int num_physical = int(filament_rgb.size());
    if (n == 0 || num_physical == 0)
        return result;

    const std::vector<std::vector<RGB>> samples = triangle_color_samples(volume, its);
    if (samples.size() != n)
        return result;
    result.has_color_data = true;

    std::vector<Lab> filament_lab;
    filament_lab.reserve(filament_rgb.size());
    for (const RGB &rgb : filament_rgb)
        filament_lab.push_back(to_oklab(rgb));

    // -1: not a candidate. 0: stays texture. k > 0: physical filament k.
    std::vector<int> label(n, -1);
    // Filament each non-candidate triangle prints with, for choosing which solid colour a small texture patch joins.
    std::vector<int> fixed_filament(n, 0);
    for (size_t t = 0; t < n; ++t) {
        bool is_split = false;
        const EnforcerBlockerType state = selector.get_original_facet_state(int(t), is_split);
        if (is_split)
            continue;
        const int effective = state == EnforcerBlockerType::NONE ? volume_extruder_1based : int(state);
        if (effective >= 1 && effective <= num_physical) {
            fixed_filament[t] = effective;
            continue;
        }
        label[t] = 0;
        ++result.candidate_triangles;
        if (samples[t].empty())
            continue;
        // Snap only when every sample is within tolerance of the same filament. A looser majority rule also snapped
        // triangles on the edges of narrow features (trim, piping) and broke them into fragments that print as beads;
        // speckles inside solid areas are cleaned up by the minimum texture area instead.
        std::vector<int> votes(size_t(num_physical) + 1, 0);
        for (const RGB &rgb : samples[t]) {
            const Lab lab = to_oklab(rgb);
            int   nearest = 0;
            float nearest_d = std::numeric_limits<float>::max();
            for (int f = 0; f < num_physical; ++f) {
                const float d = lab_distance(lab, filament_lab[size_t(f)]);
                if (d < nearest_d) {
                    nearest_d = d;
                    nearest = f + 1;
                }
            }
            if (nearest_d <= params.tolerance_delta_e)
                ++votes[size_t(nearest)];
        }
        const auto best = std::max_element(votes.begin() + 1, votes.end());
        if (*best > 0 && *best == int(samples[t].size()))
            label[t] = int(best - votes.begin());
    }

    std::vector<float> area(n, 0.f);
    for (size_t t = 0; t < n; ++t) {
        const auto &tri = its.indices[t];
        const Vec3d p0 = trafo_no_translate * its.vertices[size_t(tri[0])].cast<double>();
        const Vec3d p1 = trafo_no_translate * its.vertices[size_t(tri[1])].cast<double>();
        const Vec3d p2 = trafo_no_translate * its.vertices[size_t(tri[2])].cast<double>();
        const double a = 0.5 * (p1 - p0).cross(p2 - p0).norm();
        area[t] = std::isfinite(a) ? float(a) : 0.f;
    }

    const std::vector<Vec3i32> neighbors = its_face_neighbors(its);

    // Connected patches of triangles sharing one label. Calls visit(patch, patch_area) for each patch.
    auto for_each_patch = [&](auto &&want, auto &&visit) {
        std::vector<uint8_t> seen(n, 0);
        std::vector<size_t>  patch;
        std::vector<size_t>  stack;
        for (size_t seed = 0; seed < n; ++seed) {
            if (seen[seed] || !want(label[seed]))
                continue;
            const int lbl = label[seed];
            patch.clear();
            stack.assign(1, seed);
            seen[seed] = 1;
            float patch_area = 0.f;
            while (!stack.empty()) {
                const size_t t = stack.back();
                stack.pop_back();
                patch.push_back(t);
                patch_area += area[t];
                for (int k = 0; k < 3; ++k) {
                    const int nb = neighbors[t][k];
                    if (nb < 0 || size_t(nb) >= n || seen[size_t(nb)] || label[size_t(nb)] != lbl)
                        continue;
                    seen[size_t(nb)] = 1;
                    stack.push_back(size_t(nb));
                }
            }
            visit(patch, patch_area);
        }
    };

    // 1) Small snapped patches go back to the texture.
    if (params.min_solid_area_mm2 > 0.f)
        for_each_patch([](int l) { return l > 0; }, [&](const std::vector<size_t> &patch, float patch_area) {
            if (patch_area >= params.min_solid_area_mm2)
                return;
            for (size_t t : patch)
                label[t] = 0;
            ++result.solid_patches_dropped;
        });

    // 2) Small texture patches next to solid colour join the colour they share the most edges with.
    if (params.min_texture_area_mm2 > 0.f) {
        std::vector<std::pair<std::vector<size_t>, int>> absorb;
        for_each_patch([](int l) { return l == 0; }, [&](const std::vector<size_t> &patch, float patch_area) {
            if (patch_area >= params.min_texture_area_mm2)
                return;
            // Only patches mostly enclosed by solid colour join it; one that mostly borders other texture paint (split
            // triangles, painted texture) is a real texture area.
            std::map<int, int> edges_per_filament;
            int solid_edges = 0;
            int other_edges = 0;
            for (size_t t : patch)
                for (int k = 0; k < 3; ++k) {
                    const int nb = neighbors[t][k];
                    if (nb < 0 || size_t(nb) >= n)
                        continue;
                    const int nl = label[size_t(nb)];
                    const int solid = nl > 0 ? nl : (nl < 0 ? fixed_filament[size_t(nb)] : 0);
                    if (solid > 0) {
                        ++edges_per_filament[solid];
                        ++solid_edges;
                    } else if (nl < 0) {
                        ++other_edges;
                    }
                }
            if (edges_per_filament.empty() || 4 * solid_edges < 3 * (solid_edges + other_edges))
                return;
            const auto best = std::max_element(edges_per_filament.begin(), edges_per_filament.end(),
                                               [](const auto &a, const auto &b) { return a.second < b.second; });
            absorb.emplace_back(patch, best->first);
        });
        for (const auto &[patch, filament] : absorb) {
            for (size_t t : patch)
                label[t] = filament;
            ++result.texture_patches_absorbed;
        }
    }

    for (size_t t = 0; t < n; ++t)
        if (label[t] > 0) {
            selector.set_facet(int(t), EnforcerBlockerType(label[t]));
            ++result.snapped_triangles;
        }
    return result;
}

} // namespace Slic3r
