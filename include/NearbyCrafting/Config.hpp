#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace NearbyCrafting
{
    inline constexpr std::uint8_t deposit_modifier_shift = 1U << 0U;
    inline constexpr std::uint8_t deposit_modifier_control = 1U << 1U;
    inline constexpr std::uint8_t deposit_modifier_alt = 1U << 2U;

    struct Config
    {
        bool enabled{true};
        std::string reload_config_key{"F5"};
        double scan_radius_centimeters{2000.0};
        std::size_t max_nearby_inventories{96};
        int bench_cache_refresh_milliseconds{30000};
        int player_cache_refresh_milliseconds{1000};
        bool include_bench_inventories{true};
        std::vector<std::string> crafting_excluded_containers{};
        bool repairs_enabled{true};
        bool deposit_enabled{true};
        std::string deposit_key{"E"};
        std::uint8_t deposit_modifier_mask{deposit_modifier_shift};
        bool deposit_include_bench_inventories{true};
        std::vector<std::string> deposit_excluded_items{};
        std::vector<std::string> deposit_excluded_containers{};
        bool exclude_client_only_inventories{true};
        bool exclude_remove_only_inventories{true};
    };

    struct ConfigLoadResult
    {
        Config config{};
        bool file_found{false};
        std::vector<std::string> warnings{};
    };

    [[nodiscard]] auto normalize_deposit_item_name(std::wstring_view value) -> std::wstring;
    [[nodiscard]] auto is_deposit_item_name_excluded(
        std::wstring_view display_name,
        const std::vector<std::wstring>& normalized_exclusions) -> bool;
    [[nodiscard]] auto normalize_container_class_name(std::wstring_view value) -> std::wstring;
    [[nodiscard]] auto is_container_class_excluded(
        std::wstring_view class_name,
        const std::vector<std::wstring>& normalized_exclusions) -> bool;
    [[nodiscard]] auto merge_reloadable_config(
        const Config& current,
        const Config& loaded) -> Config;
    [[nodiscard]] auto load_config(const std::filesystem::path& path) -> ConfigLoadResult;
}
