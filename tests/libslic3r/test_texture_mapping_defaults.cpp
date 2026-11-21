#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "libslic3r/TextureMapping.hpp"

using namespace Slic3r;

TEST_CASE("New texture mapping zones start from the saved defaults", "[TextureMappingDefaults]")
{
    const std::vector<std::string> colours = { "#FEE5A5", "#F4C032", "#080A0D", "#E2DEDB" };
    TextureMappingManager::set_new_zone_defaults({});

    TextureMappingManager manager;
    manager.refresh(colours);
    TextureMappingZone *first = manager.add_zone(colours.size(), colours);
    REQUIRE(first != nullptr);
    const float factory_gamma = first->tone_gamma;

    TextureMappingZone saved = *first;
    saved.tone_gamma = 1.3f;
    saved.filament_overhang_contrast_pct = 150.f;
    saved.filament_transmission_distances_mm = { 2.5f, 2.2f, 0.1f, 3.0f };
    TextureMappingManager::set_new_zone_defaults(TextureMappingManager::serialize_zone_as_defaults(saved, colours));

    const unsigned int first_zone_id = first->zone_id;
    TextureMappingZone *second = manager.add_zone(colours.size(), colours);
    REQUIRE(second != nullptr);
    CHECK(second->tone_gamma == Catch::Approx(1.3f));
    CHECK(second->filament_overhang_contrast_pct == Catch::Approx(150.f));
    REQUIRE(second->filament_transmission_distances_mm.size() >= 3);
    CHECK(second->filament_transmission_distances_mm[2] == Catch::Approx(0.1f));
    CHECK(second->zone_id != first_zone_id);
    CHECK(second->stable_id != manager.zones().front().stable_id);

    TextureMappingManager::set_new_zone_defaults({});
    TextureMappingZone *third = manager.add_zone(colours.size(), colours);
    REQUIRE(third != nullptr);
    CHECK(third->tone_gamma == Catch::Approx(factory_gamma));
}
