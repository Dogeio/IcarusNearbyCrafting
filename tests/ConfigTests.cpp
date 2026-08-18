#include <NearbyCrafting/Config.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    int failures{};

    auto expect(const bool condition, const std::string& message) -> void
    {
        if (!condition)
        {
            ++failures;
            std::cerr << "FAILED: " << message << '\n';
        }
    }

    auto load_text_config(const std::string& name, const std::string& contents)
        -> NearbyCrafting::ConfigLoadResult
    {
        const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path() /
            ("NearbyCraftingConfigTests-" + name + "-" + std::to_string(unique_suffix) + ".ini");

        {
            std::ofstream output{path};
            if (!output)
            {
                throw std::runtime_error("could not create temporary config file");
            }
            output << contents;
        }

        auto result = NearbyCrafting::load_config(path);
        std::error_code remove_error{};
        std::filesystem::remove(path, remove_error);
        return result;
    }
}

auto main() -> int
{
    try
    {
        const auto valid = load_text_config(
            "valid",
            "[General]\n"
            "Enabled=false\n"
            "ReloadConfigKey=HOME\n"
            "ScanRadiusMeters=12.5\n"
            "MaxNearbyInventories=32\n"
            "BenchCacheRefreshMilliseconds=5000\n"
            "PlayerCacheRefreshMilliseconds=250\n"
            "IncludeBenchInventories=false\n"
            "CraftingExcludedContainers=BP_MortarAndPestle_C, bp_mortarandpestle_c, BP_Metal_Cupboard_C\n"
            "RepairsEnabled=false\n"
            "DepositEnabled=false\n"
            "DepositKey=F8\n"
            "DepositModifier=CTRL+SHIFT\n"
            "DepositIncludeBenchInventories=false\n"
            "DepositExcludedItems=Composite Arrow, 12-Gauge Slug, composite arrow\n"
            "DepositExcludedContainers=BP_Deep_Freeze_C, bp_deep_freeze_c, BP_IceBox_C\n"
            "ExcludeClientOnlyInventories=false\n"
            "ExcludeRemoveOnlyInventories=false\n");
        expect(valid.file_found, "valid config should be found");
        expect(valid.warnings.empty(), "valid config should not produce warnings");
        expect(!valid.config.enabled, "Enabled=false should be applied");
        expect(valid.config.reload_config_key == "HOME", "reload key should be applied");
        expect(valid.config.scan_radius_centimeters == 1250.0, "radius should convert to centimeters");
        expect(valid.config.max_nearby_inventories == 32, "inventory limit should be applied");
        expect(valid.config.bench_cache_refresh_milliseconds == 5000, "bench refresh should be applied");
        expect(valid.config.player_cache_refresh_milliseconds == 250, "player refresh should be applied");
        expect(!valid.config.include_bench_inventories, "bench inventory setting should be applied");
        expect(valid.config.crafting_excluded_containers.size() == 2, "crafting container exclusions should be parsed and deduplicated");
        expect(valid.config.crafting_excluded_containers[0] == "BP_MortarAndPestle_C", "crafting exclusion spelling should be preserved");
        expect(valid.config.crafting_excluded_containers[1] == "BP_Metal_Cupboard_C", "bench and storage exclusions should both be accepted");
        expect(!valid.config.repairs_enabled, "repairs enabled setting should be applied");
        expect(!valid.config.deposit_enabled, "deposit enabled setting should be applied");
        expect(valid.config.deposit_key == "F8", "deposit key should be applied");
        expect(
            valid.config.deposit_modifier_mask ==
                (NearbyCrafting::deposit_modifier_control | NearbyCrafting::deposit_modifier_shift),
            "deposit modifier combination should be applied");
        expect(!valid.config.deposit_include_bench_inventories, "deposit bench setting should be applied");
        expect(valid.config.deposit_excluded_items.size() == 2, "deposit exclusions should be parsed and deduplicated");
        expect(valid.config.deposit_excluded_items[0] == "Composite Arrow", "deposit exclusion spelling should be preserved");
        expect(valid.config.deposit_excluded_items[1] == "12-Gauge Slug", "multiple deposit exclusions should be applied");
        expect(valid.config.deposit_excluded_containers.size() == 2, "container exclusions should be normalized and deduplicated");
        expect(!valid.config.exclude_client_only_inventories, "client-only setting should be applied");
        expect(!valid.config.exclude_remove_only_inventories, "remove-only setting should be applied");

        const auto empty_reload_key = load_text_config(
            "empty-reload-key",
            "ReloadConfigKey=\n");
        expect(empty_reload_key.warnings.empty(), "an empty reload key should be accepted");
        expect(empty_reload_key.config.reload_config_key.empty(), "an empty reload key should disable configuration reload");

        const auto none_reload_key = load_text_config(
            "none-reload-key",
            "ReloadConfigKey=NONE\n");
        expect(none_reload_key.warnings.empty(), "NONE should be accepted as a disabled reload key");
        expect(none_reload_key.config.reload_config_key.empty(), "NONE should disable configuration reload");

        const auto invalid_reload_key = load_text_config(
            "invalid-reload-key",
            "ReloadConfigKey=NotARealKey\n");
        expect(invalid_reload_key.warnings.size() == 1, "invalid reload keys should still be rejected");
        expect(invalid_reload_key.config.reload_config_key == "F5", "an invalid reload key should retain its default");

        const auto non_finite = load_text_config(
            "non-finite",
            "ScanRadiusMeters=nan\n");
        expect(non_finite.warnings.size() == 1, "NaN radius should produce one warning");
        expect(non_finite.config.scan_radius_centimeters == 2000.0, "NaN radius should retain the safe default");

        const auto trailing_characters = load_text_config(
            "trailing",
            "ScanRadiusMeters=15meters\n"
            "MaxNearbyInventories=64items\n"
            "BenchCacheRefreshMilliseconds=30000ms\n"
            "PlayerCacheRefreshMilliseconds=1000ms\n");
        expect(trailing_characters.warnings.size() == 4, "partial numeric values should all be rejected");
        expect(trailing_characters.config.scan_radius_centimeters == 2000.0, "invalid radius should retain its default");
        expect(trailing_characters.config.max_nearby_inventories == 96, "invalid inventory limit should retain its default");
        expect(trailing_characters.config.bench_cache_refresh_milliseconds == 30000, "invalid bench refresh should retain its default");
        expect(trailing_characters.config.player_cache_refresh_milliseconds == 1000, "invalid player refresh should retain its default");

        const auto key_aliases = load_text_config(
            "key-aliases",
            "DepositKey=Delete\n"
            "DepositModifier=alt, shift\n");
        expect(key_aliases.warnings.empty(), "supported key aliases and modifier separators should be accepted");
        expect(key_aliases.config.deposit_key == "DEL", "key aliases should be canonicalized");
        expect(
            key_aliases.config.deposit_modifier_mask ==
                (NearbyCrafting::deposit_modifier_alt | NearbyCrafting::deposit_modifier_shift),
            "comma-separated modifiers should be combined");

        const auto invalid_deposit_bind = load_text_config(
            "invalid-deposit-bind",
            "DepositKey=NotARealKey\n"
            "DepositModifier=NONE+ALT\n");
        expect(invalid_deposit_bind.warnings.size() == 2, "invalid deposit binding values should be rejected");
        expect(invalid_deposit_bind.config.deposit_key == "E", "invalid deposit key should retain its default");
        expect(
            invalid_deposit_bind.config.deposit_modifier_mask == NearbyCrafting::deposit_modifier_shift,
            "invalid deposit modifier should retain its default");

        const auto empty_deposit_exclusions = load_text_config(
            "empty-deposit-exclusions",
            "DepositExcludedItems=\n");
        expect(empty_deposit_exclusions.warnings.empty(), "an empty deposit exclusion list should be accepted");
        expect(empty_deposit_exclusions.config.deposit_excluded_items.empty(), "an empty exclusion list should keep the fast path enabled");

        const auto empty_crafting_container_exclusions = load_text_config(
            "empty-crafting-container-exclusions",
            "CraftingExcludedContainers=\n");
        expect(empty_crafting_container_exclusions.warnings.empty(), "an empty crafting container exclusion list should be accepted");
        expect(empty_crafting_container_exclusions.config.crafting_excluded_containers.empty(), "crafting container exclusions should be empty by default");

        const auto invalid_deposit_exclusions = load_text_config(
            "invalid-deposit-exclusions",
            "DepositExcludedItems=Composite Arrow,,12-Gauge Slug\n");
        expect(invalid_deposit_exclusions.warnings.size() == 1, "empty exclusion entries should be rejected");
        expect(invalid_deposit_exclusions.config.deposit_excluded_items.empty(), "an invalid exclusion list should retain its default");

        const auto invalid_container_exclusions = load_text_config(
            "invalid-container-exclusions",
            "DepositExcludedContainers=BP_Deep_Freeze_C,,BP_IceBox_C\n");
        expect(invalid_container_exclusions.warnings.size() == 1, "empty container exclusion entries should be rejected");
        expect(invalid_container_exclusions.config.deposit_excluded_containers.empty(), "an invalid container exclusion list should retain its default");

        const auto missing_container_suffix = load_text_config(
            "missing-container-suffix",
            "DepositExcludedContainers=BP_Deep_Freeze\n");
        expect(missing_container_suffix.warnings.size() == 1, "container exclusions without _C should be rejected");
        expect(missing_container_suffix.config.deposit_excluded_containers.empty(), "a missing container suffix should retain the default exclusion list");

        const std::vector<std::wstring> normalized_exclusions{
            NearbyCrafting::normalize_deposit_item_name(L"Composite Arrow"),
            NearbyCrafting::normalize_deposit_item_name(L"12-Gauge Slug"),
        };
        expect(
            NearbyCrafting::is_deposit_item_name_excluded(L"12-gauge slug", normalized_exclusions),
            "display-name exclusion matching should be case-insensitive");
        expect(
            NearbyCrafting::is_deposit_item_name_excluded(L"  Composite   Arrow  ", normalized_exclusions),
            "display-name exclusion matching should tolerate surrounding and repeated whitespace");
        expect(
            !NearbyCrafting::is_deposit_item_name_excluded(L"Slug", normalized_exclusions),
            "partial display names should not match an exclusion");
        expect(
            !NearbyCrafting::is_deposit_item_name_excluded(L"12 Gauge Slug", normalized_exclusions),
            "display-name punctuation should be matched exactly");
        expect(
            !NearbyCrafting::is_deposit_item_name_excluded(L"Shell_Slug", normalized_exclusions),
            "internal item identifiers should not match an exclusion");

        const std::vector<std::wstring> normalized_container_exclusions{
            NearbyCrafting::normalize_container_class_name(L"BP_Deep_Freeze_C"),
        };
        expect(
            NearbyCrafting::is_container_class_excluded(
                L"bp_deep_freeze_c", normalized_container_exclusions),
            "container class matching should be case-insensitive");
        expect(
            NearbyCrafting::is_container_class_excluded(
                L"/Game/Deployables/Food/BP_Deep_Freeze.BP_Deep_Freeze_C",
                normalized_container_exclusions),
            "container class matching should accept a pasted Unreal object path");
        expect(
            !NearbyCrafting::is_container_class_excluded(
                L"BP_Deep_Freeze", normalized_container_exclusions),
            "container class matching should require the generated suffix");
        expect(
            !NearbyCrafting::is_container_class_excluded(
                L"BP_IceBox_C", normalized_container_exclusions),
            "other container class names should not match an exclusion");

        NearbyCrafting::Config current{};
        current.enabled = false;
        current.reload_config_key = "F6";
        current.repairs_enabled = false;
        current.deposit_enabled = false;
        current.deposit_key = "F7";
        current.deposit_modifier_mask = NearbyCrafting::deposit_modifier_alt;

        NearbyCrafting::Config changed{};
        changed.scan_radius_centimeters = 3500.0;
        changed.max_nearby_inventories = 48;
        changed.bench_cache_refresh_milliseconds = 45000;
        changed.player_cache_refresh_milliseconds = 750;
        changed.include_bench_inventories = false;
        changed.crafting_excluded_containers = {"BP_MortarAndPestle_C", "BP_Metal_Cupboard_C"};
        changed.deposit_include_bench_inventories = false;
        changed.deposit_excluded_items = {"Composite Arrow"};
        changed.deposit_excluded_containers = {"BP_Deep_Freeze_C", "BP_IceBox_C"};
        changed.exclude_client_only_inventories = false;
        changed.exclude_remove_only_inventories = false;

        const auto reloaded = NearbyCrafting::merge_reloadable_config(current, changed);
        expect(reloaded.scan_radius_centimeters == 3500.0, "reload should apply the scan radius");
        expect(reloaded.max_nearby_inventories == 48, "reload should apply the inventory limit");
        expect(reloaded.bench_cache_refresh_milliseconds == 45000, "reload should apply the bench cache interval");
        expect(reloaded.player_cache_refresh_milliseconds == 750, "reload should apply the player cache interval");
        expect(!reloaded.include_bench_inventories, "reload should apply crafting bench inclusion");
        expect(reloaded.crafting_excluded_containers == changed.crafting_excluded_containers, "reload should apply crafting container exclusions");
        expect(!reloaded.deposit_include_bench_inventories, "reload should apply deposit bench inclusion");
        expect(reloaded.deposit_excluded_items == changed.deposit_excluded_items, "reload should apply deposit exclusions");
        expect(reloaded.deposit_excluded_containers == changed.deposit_excluded_containers, "reload should apply container exclusions");
        expect(!reloaded.exclude_client_only_inventories, "reload should apply the client-only inventory filter");
        expect(!reloaded.exclude_remove_only_inventories, "reload should apply the remove-only inventory filter");
        expect(!reloaded.enabled, "reload should preserve Enabled");
        expect(reloaded.reload_config_key == "F6", "reload should preserve its registered key");
        expect(!reloaded.repairs_enabled, "reload should preserve RepairsEnabled");
        expect(!reloaded.deposit_enabled, "reload should preserve DepositEnabled");
        expect(reloaded.deposit_key == "F7", "reload should preserve the registered deposit key");
        expect(
            reloaded.deposit_modifier_mask == NearbyCrafting::deposit_modifier_alt,
            "reload should preserve the registered deposit modifiers");
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: unexpected exception: " << exception.what() << '\n';
        return 1;
    }

    if (failures != 0)
    {
        std::cerr << failures << " config test(s) failed.\n";
        return 1;
    }

    std::cout << "All NearbyCrafting config tests passed.\n";
    return 0;
}
