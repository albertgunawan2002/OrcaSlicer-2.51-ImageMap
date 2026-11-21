#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;

namespace {

// Paints the top face of `volume` with filament 2 and its bottom face with filament 3.
void paint_top_and_bottom(ModelVolume &volume)
{
    const TriangleMesh  &mesh = volume.mesh();
    const BoundingBoxf3  bbox = mesh.bounding_box();
    TriangleSelector     selector(mesh);
    for (int i = 0; i < int(mesh.its.indices.size()); ++i) {
        bool top = true, bottom = true;
        for (int j = 0; j < 3; ++j) {
            const double z = mesh.its.vertices[mesh.its.indices[i][j]].z();
            top    = top && std::abs(z - bbox.max.z()) < EPSILON;
            bottom = bottom && std::abs(z - bbox.min.z()) < EPSILON;
        }
        if (top)
            selector.set_facet(i, EnforcerBlockerType::Extruder2);
        else if (bottom)
            selector.set_facet(i, EnforcerBlockerType::Extruder3);
    }
    REQUIRE(volume.mmu_segmentation_facets.set(selector));
}

size_t layers_with_filament(const PrintObject &object, int filament_id)
{
    size_t count = 0;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *region : layer->regions())
            if (region->region().config().outer_wall_filament_id.value == filament_id && !region->slices.empty()) {
                ++count;
                break;
            }
    return count;
}

} // namespace

TEST_CASE("Painted top and bottom surfaces color their paint penetration layers", "[MultiMaterialSegmentation]")
{
    // 0 follows the shell layers; other values apply as set, below or above the shell.
    const auto [top_penetration, bottom_penetration, expected_top, expected_bottom] =
        GENERATE(table<int, int, size_t, size_t>({{0, 0, 5, 4}, {2, 3, 2, 3}, {7, 6, 7, 6}}));
    CAPTURE(top_penetration, bottom_penetration);

    // A 4 mm cube at 0.2 mm layers has 20 layers, enough to keep the top and bottom bands apart.
    DynamicPrintConfig config = Test::multifilament_config(3, {
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"top_shell_layers", "5"},
        {"bottom_shell_layers", "4"},
        {"top_color_penetration_layers", std::to_string(top_penetration)},
        {"bottom_color_penetration_layers", std::to_string(bottom_penetration)},
        {"gcode_comments", "1"}, // Match init_print so the second apply changes only the model.
    });

    Print print;
    Model model;
    Test::init_print({TriangleMesh(its_make_cube(20, 20, 4))}, print, model, config);
    paint_top_and_bottom(*model.objects.front()->volumes.front());
    print.apply(model, config);

    REQUIRE(print.objects().size() == 1);
    PrintObject &object = *print.get_object(0);
    object.slice();
    REQUIRE(object.layers().size() == 20);

    CHECK(layers_with_filament(object, 2) == expected_top);
    CHECK(layers_with_filament(object, 3) == expected_bottom);
}

namespace {

// Paints the +X side face of `volume` with filament 2.
void paint_positive_x_side(ModelVolume &volume)
{
    const TriangleMesh &mesh = volume.mesh();
    const BoundingBoxf3 bbox = mesh.bounding_box();
    TriangleSelector    selector(mesh);
    for (int i = 0; i < int(mesh.its.indices.size()); ++i) {
        bool side = true;
        for (int j = 0; j < 3; ++j)
            side = side && std::abs(mesh.its.vertices[mesh.its.indices[i][j]].x() - bbox.max.x()) < EPSILON;
        if (side)
            selector.set_facet(i, EnforcerBlockerType::Extruder2);
    }
    REQUIRE(volume.mmu_segmentation_facets.set(selector));
}

void collect_paths(const ExtrusionEntityCollection &collection, std::vector<const ExtrusionPath *> &out)
{
    for (const ExtrusionEntity *entity : collection.entities) {
        if (const auto *child = dynamic_cast<const ExtrusionEntityCollection *>(entity))
            collect_paths(*child, out);
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
            for (const ExtrusionPath &path : loop->paths)
                out.push_back(&path);
        else if (const auto *multi_path = dynamic_cast<const ExtrusionMultiPath *>(entity))
            for (const ExtrusionPath &path : multi_path->paths)
                out.push_back(&path);
        else if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
            out.push_back(path);
    }
}

} // namespace

TEST_CASE("Thin painted shell keeps a painted side to its outer and backing wall", "[MultiMaterialSegmentation]")
{
    const bool walls_only = GENERATE(false, true);
    CAPTURE(walls_only);

    DynamicPrintConfig config = Test::multifilament_config(2, {
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"wall_loops", "2"},
        {"mmu_segmented_region_walls_only", walls_only ? "1" : "0"},
        {"gcode_comments", "1"},
    });

    Print print;
    Model model;
    Test::init_print({TriangleMesh(its_make_cube(20, 20, 4))}, print, model, config);
    paint_positive_x_side(*model.objects.front()->volumes.front());
    print.apply(model, config);
    print.process();

    const PrintObject &object = *print.objects().front();
    const float shell_width = object.thin_painted_shell_width_mm();
    CHECK((shell_width > 0.f) == walls_only);

    // A layer in the middle, away from the top and bottom shells.
    const Layer &layer = *object.layers()[object.layers().size() / 2];
    ExPolygons painted;
    const LayerRegion *core = nullptr;
    for (const LayerRegion *region : layer.regions()) {
        if (region->region().config().outer_wall_filament_id.value == 2)
            append(painted, to_expolygons(region->slices.surfaces));
        else if (!region->slices.empty())
            core = region;
    }
    REQUIRE(!painted.empty());
    REQUIRE(core != nullptr);

    const bool painted_is_strip = offset_ex(painted, -float(scale_(0.5 * (walls_only ? shell_width : 0.9) + 0.1))).empty();
    if (!walls_only) {
        // Without the option the painted side reaches deep into the cube.
        CHECK_FALSE(painted_is_strip);
        return;
    }
    CHECK(painted_is_strip);

    // The core's walls do not run along the inner edge of the strip: nothing of them lies inside it.
    std::vector<const ExtrusionPath *> core_paths;
    for (const LayerRegion *region : layer.regions())
        if (region->region().config().outer_wall_filament_id.value != 2)
            collect_paths(region->perimeters, core_paths);
    const Polygons strip_inside = offset(painted, -float(scale_(0.1)));
    for (const ExtrusionPath *path : core_paths)
        CHECK(intersection_pl(path->polyline.to_polyline(), strip_inside).empty());

    // The strip's own wall loop is outer wall along the surface and a plain backing wall along its inner edge.
    std::vector<const ExtrusionPath *> strip_paths;
    for (const LayerRegion *region : layer.regions())
        if (region->region().config().outer_wall_filament_id.value == 2)
            collect_paths(region->perimeters, strip_paths);
    bool has_outer = false, has_backing = false;
    for (const ExtrusionPath *path : strip_paths) {
        has_outer   = has_outer || is_external_perimeter(path->role());
        has_backing = has_backing || path->role() == erPerimeter;
    }
    CHECK(has_outer);
    CHECK(has_backing);
}
