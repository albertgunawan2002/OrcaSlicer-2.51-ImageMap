#ifndef slic3r_TextureMappingSnap_hpp_
#define slic3r_TextureMappingSnap_hpp_

#include <array>
#include <cstddef>
#include <vector>

#include "Point.hpp"

namespace Slic3r {

class ModelVolume;
class TriangleSelector;

// Turns the parts of a texture-mapped surface whose colour is already (almost) one of the physical filaments into
// plain painted regions of that filament, so they print solid instead of going through the per-layer texture cycle.
struct TextureSnapToFilamentsParams
{
    // Largest Oklab colour distance (0..1 scale, ~0.02 is barely visible) at which a triangle snaps to a filament.
    float tolerance_delta_e { 0.08f };
    // Snapped patches smaller than this (mm^2) go back to the texture, so the result is not confetti.
    float min_solid_area_mm2 { 4.f };
    // Texture patches smaller than this (mm^2) that touch solid regions join the neighbouring solid colour, so a
    // slightly off-colour spot does not need its own texture cycle and tool changes.
    float min_texture_area_mm2 { 4.f };
};

struct TextureSnapToFilamentsResult
{
    bool   has_color_data { false };
    size_t candidate_triangles { 0 };
    size_t snapped_triangles { 0 };
    size_t solid_patches_dropped { 0 };
    size_t texture_patches_absorbed { 0 };
};

// filament_rgb[i] is the sRGB colour (0..1) of physical filament i + 1. A triangle is a candidate when it is not split
// by painting and its effective filament (its paint, or volume_extruder_1based when unpainted) is not a physical
// filament, i.e. it belongs to a texture mapping zone. Only candidates are changed.
TextureSnapToFilamentsResult snap_texture_colors_to_filaments(const ModelVolume                         &volume,
                                                              TriangleSelector                          &selector,
                                                              const Transform3d                         &trafo_no_translate,
                                                              const std::vector<std::array<float, 3>>   &filament_rgb,
                                                              int                                        volume_extruder_1based,
                                                              const TextureSnapToFilamentsParams        &params);

} // namespace Slic3r

#endif // slic3r_TextureMappingSnap_hpp_
