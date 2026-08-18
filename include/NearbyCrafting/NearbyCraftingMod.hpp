#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Mod/CppUserModBase.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/FWeakObjectPtr.hpp>

#include <NearbyCrafting/Config.hpp>

namespace RC::Unreal
{
    class AActor;
    class FProperty;
    class UClass;
    class UFunction;
    class UObject;
    class UScriptStruct;
    class UnrealScriptFunctionCallableContext;
}

namespace PLH
{
    class x64Detour;
}

namespace NearbyCrafting
{
    class NearbyCraftingMod;

    struct CallbackLifetimeState
    {
        std::atomic<NearbyCraftingMod*> owner{};
        std::atomic_uint32_t active_calls{};
        std::atomic_bool accepting_calls{true};
    };

    class NearbyCraftingMod final : public RC::CppUserModBase
    {
    public:
        NearbyCraftingMod();
        ~NearbyCraftingMod() override;

        auto on_unreal_init() -> void override;

    private:
        struct HookBinding
        {
            std::shared_ptr<CallbackLifetimeState> lifetime{};
            RC::Unreal::UFunction* function{};
            RC::Unreal::FProperty* inventories_property{};
            const wchar_t* function_path{};
            std::int32_t callback_id{-1};
        };

        struct SourceEntry
        {
            RC::Unreal::FWeakObjectPtr actor{};
            std::vector<RC::Unreal::FWeakObjectPtr> inventories{};
            bool is_storage{false};
            bool is_bench{false};
            bool is_crafting_excluded{false};
        };

        struct SourceClassInfo
        {
            RC::Unreal::FWeakObjectPtr actor_class{};
            bool is_storage{false};
            bool is_bench{false};
            bool is_crafting_excluded{false};
        };

        struct BenchCacheEntry
        {
            RC::Unreal::FWeakObjectPtr processing_component{};
            RC::Unreal::FWeakObjectPtr bench{};
            RC::Unreal::FWeakObjectPtr processor_inventory{};
            RC::Unreal::AActor* bench_key{};
            std::uint64_t source_generation{};
            std::chrono::steady_clock::time_point last_refresh{};
            std::vector<RC::Unreal::FWeakObjectPtr> nearby_inventories{};
            bool is_player_context{false};
            bool initialized{false};
        };

        struct DepositRequestState
        {
            std::atomic_bool active{false};
            std::atomic_bool requested{false};
        };

        struct ReloadRequestState
        {
            std::atomic_bool active{false};
            std::atomic_bool requested{false};
        };

        struct InitialSourceSeedState
        {
            std::atomic_bool completed{false};
            std::atomic_bool failure_logged{false};
            std::atomic_uint32_t retry_delay_ticks{};
        };

        struct DepositFilterBindings
        {
            RC::Unreal::UFunction* get_all_items_function{};
            RC::Unreal::FProperty* get_all_items_return_property{};
            RC::Unreal::UScriptStruct* item_struct{};
            RC::Unreal::FProperty* item_static_data_property{};
            RC::Unreal::UFunction* get_itemable_data_function{};
            RC::Unreal::UObject* get_itemable_data_context{};
            RC::Unreal::FProperty* get_itemable_data_input_property{};
            RC::Unreal::FProperty* get_itemable_data_output_property{};
            RC::Unreal::FProperty* itemable_display_name_property{};
            RC::Unreal::UFunction* transfer_all_of_type_function{};
            RC::Unreal::FProperty* transfer_from_inventory_property{};
            RC::Unreal::FProperty* transfer_to_inventory_property{};
            RC::Unreal::FProperty* transfer_type_property{};
        };

        static auto pre_hook(RC::Unreal::UnrealScriptFunctionCallableContext& context, void* custom_data) -> void;
        static auto server_queue_hook(
            RC::Unreal::UObject* processing_component,
            const void* recipe,
            std::int32_t count,
            const RC::Unreal::TArray<RC::Unreal::UObject*>* additional_inventories,
            RC::Unreal::UObject* player) -> void;
        static auto repair_material_lookup_hook(
            void* return_items,
            const void* item_data,
            void* inventories) -> void*;

        auto install_hooks() -> bool;
        auto install_hook(
            const wchar_t* function_path,
            const wchar_t* inventories_property_name) -> bool;
        auto remove_hooks() -> void;
        auto install_server_queue_hook() -> bool;
        auto remove_server_queue_hook() -> void;
        auto install_repair_material_lookup_hook() -> bool;
        auto remove_repair_material_lookup_hook() -> void;
        auto install_lifecycle_hooks() -> bool;
        auto remove_lifecycle_hooks() -> void;
        auto install_initial_source_seed() -> bool;
        auto remove_initial_source_seed() -> void;
        auto disable_callback_entry() -> void;
        auto wait_for_callback_quiescence() const -> void;
        auto install_deposit_feature() -> bool;
        auto install_deposit_filter_bindings() -> bool;
        auto update_deposit_exclusions(const Config& config) -> bool;
        auto update_crafting_exclusions(const Config& config) -> void;
        auto remove_deposit_feature() -> void;
        auto install_config_reload_feature() -> bool;
        auto remove_config_reload_feature() -> void;
        auto reload_config() -> void;
        auto deposit_inventory_to_nearby() -> void;
        auto transfer_matching_items(
            RC::Unreal::UObject* controller,
            RC::Unreal::UObject* from_inventory,
            RC::Unreal::UObject* to_inventory) const -> void;
        auto transfer_items_of_type(
            RC::Unreal::UObject* controller,
            RC::Unreal::UObject* from_inventory,
            RC::Unreal::UObject* to_inventory,
            const void* item_data) const -> void;
        auto fail_initialization(const wchar_t* reason) -> void;
        auto seed_source_registry() -> void;
        auto on_actor_begin_play(RC::Unreal::AActor* actor) -> void;
        auto on_actor_end_play(RC::Unreal::AActor* actor) -> void;
        auto register_source_actor(RC::Unreal::AActor* actor) -> bool;
        auto unregister_source_actor(RC::Unreal::AActor* actor) -> bool;
        auto prune_source_registry() -> void;
        auto refresh_stale_source_entries() -> void;
        auto build_source_entry(RC::Unreal::AActor* actor, SourceEntry& source) -> bool;
        auto remove_bench_cache(RC::Unreal::UObject* processing_component) -> bool;
        auto remove_bench_caches_for_actor(
            RC::Unreal::AActor* actor
#if defined(NEARBYCRAFTING_DEBUG)
            , std::size_t* removed_count
#endif
            ) -> void;
#if defined(NEARBYCRAFTING_DEBUG)
        auto log_debug_state(const wchar_t* event) const -> void;
#endif
        auto inject_nearby_inventories(
            RC::Unreal::UnrealScriptFunctionCallableContext& context,
            RC::Unreal::UFunction* function,
            RC::Unreal::FProperty* inventories_property,
            const wchar_t* function_path) -> void;
        auto append_nearby_inventories(
            RC::Unreal::UObject* cache_context,
            RC::Unreal::TArray<RC::Unreal::UObject*>& inventories,
            const wchar_t* function_path) -> void;
        auto rebuild_bench_cache(BenchCacheEntry& cache, RC::Unreal::AActor* bench) -> void;

        [[nodiscard]] auto is_storage_actor(RC::Unreal::AActor* actor) -> bool;
        [[nodiscard]] auto is_player_crafting_actor(RC::Unreal::AActor* actor) const -> bool;
        [[nodiscard]] auto classify_source_actor(RC::Unreal::AActor* actor) -> SourceClassInfo;
        [[nodiscard]] auto get_processing_inventory(RC::Unreal::UObject* processing_component) const -> RC::Unreal::UObject*;
        [[nodiscard]] auto find_or_create_bench_cache(RC::Unreal::UObject* processing_component) -> BenchCacheEntry*;
        [[nodiscard]] auto get_component_owner(RC::Unreal::UObject* component) const -> RC::Unreal::AActor*;
        [[nodiscard]] auto find_local_player_controller() const -> RC::Unreal::UObject*;
        [[nodiscard]] auto get_controller_pawn(RC::Unreal::UObject* controller) const -> RC::Unreal::AActor*;
        [[nodiscard]] auto get_player_backpack_inventory(RC::Unreal::AActor* player) const -> RC::Unreal::UObject*;
        [[nodiscard]] auto read_object_property(
            RC::Unreal::UObject* object,
            const wchar_t* property_name,
            RC::Unreal::UClass* expected_base_class) const -> RC::Unreal::UObject*;
        [[nodiscard]] auto call_object_no_args(
            RC::Unreal::UObject* object,
            const wchar_t* function_name,
            RC::Unreal::UClass* expected_base_class) const -> RC::Unreal::UObject*;
        [[nodiscard]] auto should_use_inventory(RC::Unreal::UObject* inventory) const -> bool;
        [[nodiscard]] auto call_bool_no_args(RC::Unreal::UObject* object, const wchar_t* function_name, bool fallback) const -> bool;
        [[nodiscard]] auto config_path() const -> std::filesystem::path;

        Config m_config{};
        RC::Unreal::UClass* m_inventory_class{};
        RC::Unreal::UClass* m_processing_component_class{};
        RC::Unreal::UClass* m_character_class{};
        RC::Unreal::UClass* m_player_controller_class{};
        RC::Unreal::UClass* m_icarus_player_character_class{};
        RC::Unreal::UClass* m_icarus_controller_class{};
        RC::Unreal::FWeakObjectPtr m_container_class{};
        RC::Unreal::UFunction* m_transfer_like_function{};
        RC::Unreal::FProperty* m_transfer_from_inventory_property{};
        RC::Unreal::FProperty* m_transfer_to_inventory_property{};
        DepositFilterBindings m_deposit_filter_bindings{};
        std::vector<std::wstring> m_normalized_deposit_exclusions{};
        std::vector<std::wstring> m_normalized_deposit_container_exclusions{};
        std::unordered_set<std::wstring> m_normalized_crafting_container_exclusions{};
        std::vector<std::unique_ptr<HookBinding>> m_hooks{};
        std::unique_ptr<PLH::x64Detour> m_server_queue_detour{};
        std::uint64_t m_server_queue_trampoline{};
        std::uint64_t m_server_queue_native_target{};
        std::array<std::uint8_t, 128> m_server_queue_hooked_entry{};
        std::size_t m_server_queue_hooked_entry_size{};
        std::unique_ptr<PLH::x64Detour> m_repair_material_lookup_detour{};
        std::uint64_t m_repair_material_lookup_trampoline{};
        std::uint64_t m_repair_native_target{};
        std::array<std::uint8_t, 128> m_repair_hooked_entry{};
        std::size_t m_repair_hooked_entry_size{};
        std::unordered_map<RC::Unreal::AActor*, SourceEntry> m_source_registry{};
        std::unordered_map<RC::Unreal::UClass*, SourceClassInfo> m_source_class_cache{};
        std::unordered_map<RC::Unreal::UObject*, BenchCacheEntry> m_bench_caches{};
        std::unordered_multimap<RC::Unreal::AActor*, RC::Unreal::UObject*> m_bench_cache_keys_by_actor{};
        std::uint64_t m_source_generation{1};
#if defined(NEARBYCRAFTING_DEBUG)
        std::uint64_t m_hook_invocation_count{};
        std::uint64_t m_cache_hit_count{};
        std::uint64_t m_cache_rebuild_count{};
#endif
        std::uint64_t m_begin_play_callback_id{};
        std::uint64_t m_end_play_callback_id{};
        std::uint64_t m_initial_source_seed_tick_callback_id{};
        std::uint64_t m_deposit_tick_callback_id{};
        std::uint64_t m_reload_tick_callback_id{};
        std::shared_ptr<InitialSourceSeedState> m_initial_source_seed_state{
            std::make_shared<InitialSourceSeedState>()};
        std::shared_ptr<DepositRequestState> m_deposit_request_state{std::make_shared<DepositRequestState>()};
        std::shared_ptr<ReloadRequestState> m_reload_request_state{std::make_shared<ReloadRequestState>()};
        std::shared_ptr<CallbackLifetimeState> m_callback_lifetime{std::make_shared<CallbackLifetimeState>()};
        std::atomic_bool m_initialized{false};
        bool m_hook_error_logged{false};
        static std::atomic<NearbyCraftingMod*> s_server_queue_hook_owner;
        static std::atomic_uint64_t s_server_queue_trampoline;
        static std::atomic_uint32_t s_server_queue_active_calls;
        static std::atomic<NearbyCraftingMod*> s_repair_hook_owner;
        static std::atomic_uint64_t s_repair_trampoline;
        static std::atomic_uint32_t s_repair_active_calls;
    };
}
