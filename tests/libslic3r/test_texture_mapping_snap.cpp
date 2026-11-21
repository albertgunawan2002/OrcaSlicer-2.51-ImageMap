#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TextureMappingSnap.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

namespace {

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | 0xFFu; }

// A 20 mm cube assigned to texture zone 3 (two physical filaments: red and blue), vertex colours set by `color_of`.
template<class ColorOf> ModelVolume *make_colored_cube(Model &model, ColorOf &&color_of)
{
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
    std::vector<uint32_t> colors;
    for (const stl_vertex &v : volume->mesh().its.vertices)
        colors.push_back(color_of(v));
    volume->imported_vertex_colors_rgba.assign(colors.begin(), colors.end());
    return volume;
}

const std::vector<std::array<float, 3>> red_blue = { { 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f } };

} // namespace

TEST_CASE("Near-solid texture colours snap to the closest filament", "[TextureMappingSnap]")
{
    Model        model;
    ModelVolume *volume = make_colored_cube(model, [](const stl_vertex &) { return pack_rgba(240, 12, 10); });
    TriangleSelector selector(volume->mesh());

    TextureSnapToFilamentsParams params;
    params.min_solid_area_mm2 = 0.f;
    params.min_texture_area_mm2 = 0.f;
    const TextureSnapToFilamentsResult result =
        snap_texture_colors_to_filaments(*volume, selector, Transform3d::Identity(), red_blue, 3, params);

    REQUIRE(result.has_color_data);
    REQUIRE(result.snapped_triangles == volume->mesh().its.indices.size());
    for (size_t t = 0; t < volume->mesh().its.indices.size(); ++t) {
        bool split = false;
        CHECK(selector.get_original_facet_state(int(t), split) == EnforcerBlockerType::Extruder1);
    }
}

TEST_CASE("Colours far from every filament stay in the texture", "[TextureMappingSnap]")
{
    Model        model;
    ModelVolume *volume = make_colored_cube(model, [](const stl_vertex &) { return pack_rgba(40, 200, 40); });
    TriangleSelector selector(volume->mesh());

    const TextureSnapToFilamentsResult result =
        snap_texture_colors_to_filaments(*volume, selector, Transform3d::Identity(), red_blue, 3, TextureSnapToFilamentsParams{});

    CHECK(result.snapped_triangles == 0);
}

TEST_CASE("Triangles already painted with a physical filament are left alone", "[TextureMappingSnap]")
{
    Model        model;
    ModelVolume *volume = make_colored_cube(model, [](const stl_vertex &) { return pack_rgba(250, 0, 0); });
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder2);

    TextureSnapToFilamentsParams params;
    params.min_solid_area_mm2 = 0.f;
    params.min_texture_area_mm2 = 0.f;
    snap_texture_colors_to_filaments(*volume, selector, Transform3d::Identity(), red_blue, 3, params);

    bool split = false;
    CHECK(selector.get_original_facet_state(0, split) == EnforcerBlockerType::Extruder2);
    CHECK(selector.get_original_facet_state(1, split) == EnforcerBlockerType::Extruder1);
}

TEST_CASE("A volume printed with a physical filament is not touched", "[TextureMappingSnap]")
{
    Model        model;
    ModelVolume *volume = make_colored_cube(model, [](const stl_vertex &) { return pack_rgba(250, 0, 0); });
    TriangleSelector selector(volume->mesh());

    const TextureSnapToFilamentsResult result =
        snap_texture_colors_to_filaments(*volume, selector, Transform3d::Identity(), red_blue, 2, TextureSnapToFilamentsParams{});

    CHECK(result.candidate_triangles == 0);
    CHECK(result.snapped_triangles == 0);
}

TEST_CASE("Small patches follow the minimum areas", "[TextureMappingSnap]")
{
    // Top face red, everything else green: the top is 400 mm^2 of red, the rest stays texture. add_volume() centres
    // the mesh, so the top face is at z = +10.
    Model        model;
    ModelVolume *volume = make_colored_cube(model, [](const stl_vertex &v) {
        return v.z() > 9.f ? pack_rgba(250, 0, 0) : pack_rgba(40, 200, 40);
    });
    REQUIRE(volume->mesh().bounding_box().max.z() == Catch::Approx(10.).margin(1e-3));
    const indexed_triangle_set &its = volume->mesh().its;

    SECTION("a solid patch below the minimum solid area goes back to the texture") {
        TriangleSelector selector(volume->mesh());
        TextureSnapToFilamentsParams params;
        params.min_solid_area_mm2 = 1000.f;
        params.min_texture_area_mm2 = 0.f;
        const auto result = snap_texture_colors_to_filaments(*volume, selector, Transform3d::Identity(), red_blue, 3, params);
        CHECK(result.snapped_triangles == 0);
    }

    SECTION("a texture patch below the minimum texture area joins the neighbouring solid colour") {
        TriangleSelector selector(volume->mesh());
        TextureSnapToFilamentsParams params;
        params.min_solid_area_mm2 = 0.f;
        params.min_texture_area_mm2 = 10000.f;
        const auto result = snap_texture_colors_to_filaments(*volume, selector, Transform3d::Identity(), red_blue, 3, params);
        CHECK(result.texture_patches_absorbed >= 1);
        CHECK(result.snapped_triangles == its.indices.size());
    }
}
