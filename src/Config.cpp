#include <NearbyCrafting/Config.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <stdexcept>

namespace NearbyCrafting
{
    namespace
    {
        auto trim(std::string value) -> std::string
        {
            const auto is_space = [](const unsigned char character) { return std::isspace(character) != 0; };
            value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
            value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
            return value;
        }

        auto lowercase(std::string value) -> std::string
        {
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        auto uppercase(std::string value) -> std::string
        {
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
            return value;
        }

        auto parse_bool(const std::string& value) -> bool
        {
            const auto normalized = lowercase(trim(value));
            if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on")
            {
                return true;
            }
            if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off")
            {
                return false;
            }
            throw std::invalid_argument("expected a boolean value");
        }

        auto parse_double(const std::string& value) -> double
        {
            std::size_t consumed{};
            const auto parsed = std::stod(value, &consumed);
            if (consumed != value.size() || !std::isfinite(parsed))
            {
                throw std::invalid_argument("expected a finite decimal number");
            }
            return parsed;
        }

        auto parse_integer(const std::string& value) -> long long
        {
            std::size_t consumed{};
            const auto parsed = std::stoll(value, &consumed);
            if (consumed != value.size())
            {
                throw std::invalid_argument("expected a whole number");
            }
            return parsed;
        }

        auto parse_key(const std::string& value) -> std::string
        {
            auto normalized = uppercase(trim(value));
            if (normalized.empty())
            {
                throw std::invalid_argument("expected a key name");
            }

            if (normalized.size() == 1 && normalized.front() >= 'A' && normalized.front() <= 'Z')
            {
                return normalized;
            }
            if (normalized.size() == 1 && normalized.front() >= '0' && normalized.front() <= '9')
            {
                constexpr std::array<const char*, 10> digit_names{
                    "ZERO", "ONE", "TWO", "THREE", "FOUR",
                    "FIVE", "SIX", "SEVEN", "EIGHT", "NINE"};
                return digit_names[static_cast<std::size_t>(normalized.front() - '0')];
            }
            if (normalized.front() == 'F' && normalized.size() <= 3)
            {
                try
                {
                    const auto number = parse_integer(normalized.substr(1));
                    if (number >= 1 && number <= 24)
                    {
                        return normalized;
                    }
                }
                catch (...)
                {
                }
            }

            if (normalized == "ENTER") normalized = "RETURN";
            if (normalized == "INSERT") normalized = "INS";
            if (normalized == "DELETE") normalized = "DEL";
            if (normalized == "PAGEUP") normalized = "PAGE_UP";
            if (normalized == "PAGEDOWN") normalized = "PAGE_DOWN";
            if (normalized == "LEFT") normalized = "LEFT_ARROW";
            if (normalized == "RIGHT") normalized = "RIGHT_ARROW";
            if (normalized == "UP") normalized = "UP_ARROW";
            if (normalized == "DOWN") normalized = "DOWN_ARROW";

            constexpr std::array supported_names{
                "LEFT_MOUSE_BUTTON", "RIGHT_MOUSE_BUTTON", "MIDDLE_MOUSE_BUTTON",
                "XBUTTON_ONE", "XBUTTON_TWO", "BACKSPACE", "TAB", "CLEAR", "RETURN",
                "PAUSE", "CAPS_LOCK", "ESCAPE", "SPACE", "PAGE_UP", "PAGE_DOWN", "END",
                "HOME", "LEFT_ARROW", "UP_ARROW", "RIGHT_ARROW", "DOWN_ARROW", "PRINT_SCREEN",
                "INS", "DEL", "LEFT_WIN", "RIGHT_WIN", "APPS", "SLEEP", "NUM_ZERO", "NUM_ONE",
                "NUM_TWO", "NUM_THREE", "NUM_FOUR", "NUM_FIVE", "NUM_SIX", "NUM_SEVEN",
                "NUM_EIGHT", "NUM_NINE", "MULTIPLY", "ADD", "SUBTRACT", "DECIMAL", "DIVIDE",
                "NUM_LOCK", "SCROLL_LOCK", "VOLUME_MUTE", "VOLUME_DOWN", "VOLUME_UP",
                "MEDIA_NEXT_TRACK", "MEDIA_PREV_TRACK", "MEDIA_STOP", "MEDIA_PLAY_PAUSE"};
            if (std::ranges::find(supported_names, normalized) != supported_names.end())
            {
                return normalized;
            }

            throw std::invalid_argument("unsupported key name");
        }

        auto parse_reload_config_key(const std::string& value) -> std::string
        {
            const auto normalized = uppercase(trim(value));
            if (normalized.empty() || normalized == "NONE")
            {
                return {};
            }
            return parse_key(normalized);
        }

        auto parse_deposit_modifiers(const std::string& value) -> std::uint8_t
        {
            auto normalized = uppercase(trim(value));
            std::replace(normalized.begin(), normalized.end(), ',', '+');
            std::replace(normalized.begin(), normalized.end(), '|', '+');
            if (normalized == "NONE")
            {
                return 0;
            }
            if (normalized.empty())
            {
                throw std::invalid_argument("expected NONE, SHIFT, CTRL, or ALT");
            }

            std::uint8_t modifiers{};
            std::size_t start{};
            while (start <= normalized.size())
            {
                const auto separator = normalized.find('+', start);
                const auto token = trim(normalized.substr(start, separator - start));
                if (token == "SHIFT")
                {
                    modifiers |= deposit_modifier_shift;
                }
                else if (token == "CTRL" || token == "CONTROL")
                {
                    modifiers |= deposit_modifier_control;
                }
                else if (token == "ALT")
                {
                    modifiers |= deposit_modifier_alt;
                }
                else
                {
                    throw std::invalid_argument("expected NONE, SHIFT, CTRL, or ALT");
                }

                if (separator == std::string::npos)
                {
                    break;
                }
                start = separator + 1;
            }
            return modifiers;
        }

        auto parse_deposit_excluded_items(const std::string& value) -> std::vector<std::string>
        {
            std::vector<std::string> items{};
            std::size_t start{};
            while (start <= value.size())
            {
                const auto separator = value.find(',', start);
                auto item = trim(value.substr(start, separator - start));
                if (item.empty())
                {
                    if (value.empty())
                    {
                        return items;
                    }
                    throw std::invalid_argument("expected comma-separated item names without empty entries");
                }

                const auto normalized = lowercase(item);
                const auto duplicate = std::ranges::any_of(items, [&normalized](const std::string& existing) {
                    return lowercase(existing) == normalized;
                });
                if (!duplicate)
                {
                    items.emplace_back(std::move(item));
                }

                if (separator == std::string::npos)
                {
                    break;
                }
                start = separator + 1;
            }
            return items;
        }

        auto parse_container_class_names(const std::string& value) -> std::vector<std::string>
        {
            std::vector<std::string> containers{};
            std::size_t start{};
            while (start <= value.size())
            {
                const auto separator = value.find(',', start);
                auto container = trim(value.substr(start, separator - start));
                if (container.empty())
                {
                    if (value.empty())
                    {
                        return containers;
                    }
                    throw std::invalid_argument(
                        "expected comma-separated container class names without empty entries");
                }

                const auto normalized = normalize_container_class_name(
                    std::wstring{container.begin(), container.end()});
                if (normalized.empty())
                {
                    throw std::invalid_argument("expected a container Blueprint class name");
                }
                if (!normalized.ends_with(L"_c"))
                {
                    throw std::invalid_argument(
                        "expected a container Blueprint class name ending in _C");
                }
                const auto duplicate = std::ranges::any_of(
                    containers, [&normalized](const std::string& existing) {
                        return normalize_container_class_name(
                            std::wstring{existing.begin(), existing.end()}) == normalized;
                    });
                if (!duplicate)
                {
                    containers.emplace_back(std::move(container));
                }

                if (separator == std::string::npos)
                {
                    break;
                }
                start = separator + 1;
            }
            return containers;
        }

        template <typename Number>
        auto in_range(const Number value, const Number minimum, const Number maximum) -> Number
        {
            if (value < minimum || value > maximum)
            {
                throw std::out_of_range("value is outside the supported range");
            }
            return value;
        }
    }

    auto normalize_deposit_item_name(const std::wstring_view value) -> std::wstring
    {
        std::wstring normalized{};
        normalized.reserve(value.size());
        auto pending_space = false;
        for (const auto character : value)
        {
            if (std::iswspace(character) == 0)
            {
                if (pending_space && !normalized.empty())
                {
                    normalized.push_back(L' ');
                }
                normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
                pending_space = false;
            }
            else
            {
                pending_space = true;
            }
        }
        return normalized;
    }

    auto is_deposit_item_name_excluded(
        const std::wstring_view display_name,
        const std::vector<std::wstring>& normalized_exclusions) -> bool
    {
        const auto normalized_display_name = normalize_deposit_item_name(display_name);
        return std::ranges::find(normalized_exclusions, normalized_display_name) !=
            normalized_exclusions.end();
    }

    auto normalize_container_class_name(const std::wstring_view value) -> std::wstring
    {
        const auto first = std::find_if_not(value.begin(), value.end(), [](const wchar_t character) {
            return std::iswspace(character) != 0;
        });
        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const wchar_t character) {
            return std::iswspace(character) != 0;
        }).base();
        if (first >= last)
        {
            return {};
        }

        std::wstring normalized{first, last};
        if (const auto delimiter = normalized.find_last_of(L"/.: "); delimiter != std::wstring::npos)
        {
            normalized.erase(0, delimiter + 1);
        }
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return normalized;
    }

    auto is_container_class_excluded(
        const std::wstring_view class_name,
        const std::vector<std::wstring>& normalized_exclusions) -> bool
    {
        const auto normalized_class_name = normalize_container_class_name(class_name);
        return std::ranges::find(normalized_exclusions, normalized_class_name) !=
            normalized_exclusions.end();
    }

    auto merge_reloadable_config(const Config& current, const Config& loaded) -> Config
    {
        auto merged = current;
        merged.scan_radius_centimeters = loaded.scan_radius_centimeters;
        merged.max_nearby_inventories = loaded.max_nearby_inventories;
        merged.bench_cache_refresh_milliseconds = loaded.bench_cache_refresh_milliseconds;
        merged.player_cache_refresh_milliseconds = loaded.player_cache_refresh_milliseconds;
        merged.include_bench_inventories = loaded.include_bench_inventories;
        merged.crafting_excluded_containers = loaded.crafting_excluded_containers;
        merged.deposit_include_bench_inventories = loaded.deposit_include_bench_inventories;
        merged.deposit_excluded_items = loaded.deposit_excluded_items;
        merged.deposit_excluded_containers = loaded.deposit_excluded_containers;
        merged.exclude_client_only_inventories = loaded.exclude_client_only_inventories;
        merged.exclude_remove_only_inventories = loaded.exclude_remove_only_inventories;
        return merged;
    }

    auto load_config(const std::filesystem::path& path) -> ConfigLoadResult
    {
        ConfigLoadResult result{};
        std::ifstream input{path};
        if (!input)
        {
            return result;
        }

        result.file_found = true;
        std::string line{};
        std::size_t line_number{};
        while (std::getline(input, line))
        {
            ++line_number;
            const auto comment = line.find_first_of("#;");
            if (comment != std::string::npos)
            {
                line.erase(comment);
            }

            line = trim(std::move(line));
            if (line.empty() || (line.front() == '[' && line.back() == ']'))
            {
                continue;
            }

            const auto separator = line.find('=');
            if (separator == std::string::npos)
            {
                result.warnings.emplace_back("line " + std::to_string(line_number) + ": expected key=value");
                continue;
            }

            const auto key = lowercase(trim(line.substr(0, separator)));
            const auto value = trim(line.substr(separator + 1));

            try
            {
                if (key == "enabled")
                {
                    result.config.enabled = parse_bool(value);
                }
                else if (key == "reloadconfigkey")
                {
                    result.config.reload_config_key = parse_reload_config_key(value);
                }
                else if (key == "scanradiusmeters")
                {
                    result.config.scan_radius_centimeters = in_range(parse_double(value), 1.0, 100.0) * 100.0;
                }
                else if (key == "maxnearbyinventories")
                {
                    const auto parsed = in_range(parse_integer(value), 1LL, 256LL);
                    result.config.max_nearby_inventories = static_cast<std::size_t>(parsed);
                }
                else if (key == "benchcacherefreshmilliseconds" || key == "cacherefreshmilliseconds")
                {
                    const auto parsed = in_range(parse_integer(value), 1000LL, 600000LL);
                    result.config.bench_cache_refresh_milliseconds = static_cast<int>(parsed);
                }
                else if (key == "playercacherefreshmilliseconds")
                {
                    const auto parsed = in_range(parse_integer(value), 100LL, 30000LL);
                    result.config.player_cache_refresh_milliseconds = static_cast<int>(parsed);
                }
                else if (key == "includebenchinventories")
                {
                    result.config.include_bench_inventories = parse_bool(value);
                }
                else if (key == "craftingexcludedcontainers")
                {
                    result.config.crafting_excluded_containers =
                        parse_container_class_names(value);
                }
                else if (key == "repairsenabled")
                {
                    result.config.repairs_enabled = parse_bool(value);
                }
                else if (key == "depositenabled")
                {
                    result.config.deposit_enabled = parse_bool(value);
                }
                else if (key == "depositkey")
                {
                    result.config.deposit_key = parse_key(value);
                }
                else if (key == "depositmodifier" || key == "depositmodifiers")
                {
                    result.config.deposit_modifier_mask = parse_deposit_modifiers(value);
                }
                else if (key == "depositincludebenchinventories")
                {
                    result.config.deposit_include_bench_inventories = parse_bool(value);
                }
                else if (key == "depositexcludeditems")
                {
                    result.config.deposit_excluded_items = parse_deposit_excluded_items(value);
                }
                else if (key == "depositexcludedcontainers")
                {
                    result.config.deposit_excluded_containers =
                        parse_container_class_names(value);
                }
                else if (key == "excludeclientonlyinventories")
                {
                    result.config.exclude_client_only_inventories = parse_bool(value);
                }
                else if (key == "excluderemoveonlyinventories")
                {
                    result.config.exclude_remove_only_inventories = parse_bool(value);
                }
                else
                {
                    result.warnings.emplace_back("line " + std::to_string(line_number) + ": unknown setting '" + key + "'");
                }
            }
            catch (const std::exception& exception)
            {
                result.warnings.emplace_back(
                    "line " + std::to_string(line_number) + ": invalid value for '" + key + "' (" + exception.what() + ")");
            }
        }

        return result;
    }
}
