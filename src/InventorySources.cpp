#include "NearbyCraftingInternal.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace NearbyCrafting
{
    using namespace RC;
    using namespace RC::Unreal;
    using namespace Detail;

    auto NearbyCraftingMod::seed_source_registry() -> void
    {
        if (!IsInGameThreadRaw())
        {
            return;
        }

#if defined(NEARBYCRAFTING_DEBUG)
        const auto old_source_count = m_source_registry.size();
#endif
        m_source_registry.clear();
        m_source_class_cache.clear();
        ++m_source_generation;

#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Source registry initialization started; cleared {} prior entries (generation={}).\n"),
            old_source_count,
            m_source_generation);
#endif

        std::vector<UObject*> existing_containers{};
        UObjectGlobals::FindAllOf(STR("BP_DeployableContainerBase_C"), existing_containers);

        std::vector<UObject*> existing_processing_components{};
        if (m_config.include_bench_inventories ||
            (m_config.deposit_enabled && m_config.deposit_include_bench_inventories))
        {
            UObjectGlobals::FindAllOf(STR("ProcessingComponent"), existing_processing_components);
        }

#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Initial source discovery returned {} container candidates and {} processing-component candidates.\n"),
            existing_containers.size(),
            existing_processing_components.size());
#endif

        for (auto* object : existing_containers)
        {
            if (is_live_uobject(object) && object->IsA(AActor::StaticClass()))
            {
                register_source_actor(static_cast<AActor*>(object));
            }
        }

        for (auto* object : existing_processing_components)
        {
            if (!is_live_uobject(object) || !m_processing_component_class ||
                !object->IsA(m_processing_component_class))
            {
                continue;
            }

            if (auto* owner = get_component_owner(object))
            {
                register_source_actor(owner);
            }
        }

#if defined(NEARBYCRAFTING_DEBUG)
        log_debug_state(L"source registry initialization complete");
#endif
    }

    auto NearbyCraftingMod::on_actor_begin_play(AActor* actor) -> void
    {
        register_source_actor(actor);
    }

    auto NearbyCraftingMod::on_actor_end_play(AActor* actor) -> void
    {
#if defined(NEARBYCRAFTING_DEBUG)
        const auto source_removed = unregister_source_actor(actor);
        std::size_t caches_removed{};
        remove_bench_caches_for_actor(actor, &caches_removed);

        if (source_removed || caches_removed > 0)
        {
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] EndPlay cleanup: source-removed={}, bench-caches-removed={}.\n"),
                source_removed,
                caches_removed);
        }
#else
        unregister_source_actor(actor);
        remove_bench_caches_for_actor(actor);
#endif
    }

    auto NearbyCraftingMod::register_source_actor(AActor* actor) -> bool
    {
        if (!IsInGameThreadRaw() || !is_live_uobject(actor) ||
            !actor->IsA(AActor::StaticClass()))
        {
            return false;
        }

        SourceEntry new_source{};
        if (!build_source_entry(actor, new_source))
        {
            return unregister_source_actor(actor);
        }

        if (const auto existing = m_source_registry.find(actor); existing != m_source_registry.end())
        {
            if (existing->second.actor.Get() == actor)
            {
                const auto& old_source = existing->second;
                auto unchanged = old_source.is_storage == new_source.is_storage &&
                    old_source.is_bench == new_source.is_bench &&
                    old_source.inventories.size() == new_source.inventories.size();
                for (std::size_t index = 0; unchanged && index < old_source.inventories.size(); ++index)
                {
                    unchanged = old_source.inventories[index].Get() == new_source.inventories[index].Get();
                }

                if (unchanged)
                {
                    return false;
                }

                existing->second = std::move(new_source);
                ++m_source_generation;
                return true;
            }
            m_source_registry.erase(existing);
        }

        const auto was_inserted = m_source_registry.emplace(actor, std::move(new_source)).second;
        if (!was_inserted)
        {
            return false;
        }
        ++m_source_generation;
        return true;
    }

    auto NearbyCraftingMod::unregister_source_actor(AActor* actor) -> bool
    {
        if (!actor)
        {
            return false;
        }

        const auto existing = m_source_registry.find(actor);
        if (existing == m_source_registry.end())
        {
            return false;
        }

        const auto* registered_actor = existing->second.actor.Get();
        if (registered_actor && registered_actor != actor)
        {
            return false;
        }

        m_source_registry.erase(existing);
        ++m_source_generation;
        return true;
    }

    auto NearbyCraftingMod::prune_source_registry() -> void
    {
        const auto old_size = m_source_registry.size();
        std::erase_if(m_source_registry, [](const auto& item) {
            return !item.second.actor.Get();
        });
        if (m_source_registry.size() != old_size)
        {
            ++m_source_generation;

#if defined(NEARBYCRAFTING_DEBUG)
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] Source prune removed {} invalid entries (sources={}, generation={}).\n"),
                old_size - m_source_registry.size(),
                m_source_registry.size(),
                m_source_generation);
#endif
        }
    }

    auto NearbyCraftingMod::refresh_stale_source_entries() -> void
    {
        prune_source_registry();

        std::vector<AActor*> stale_actors{};
        for (const auto& item : m_source_registry)
        {
            const auto has_invalid_inventory = std::ranges::any_of(
                item.second.inventories,
                [](const FWeakObjectPtr& inventory) { return !inventory.Get(); });
            if (!item.second.inventories.empty() && !has_invalid_inventory)
            {
                continue;
            }

            auto* actor_object = item.second.actor.Get();
            if (actor_object && actor_object->IsA(AActor::StaticClass()))
            {
                stale_actors.emplace_back(static_cast<AActor*>(actor_object));
            }
        }

        for (auto* actor : stale_actors)
        {
            register_source_actor(actor);
        }
    }

    auto NearbyCraftingMod::build_source_entry(AActor* actor, SourceEntry& source) -> bool
    {
        if (!IsInGameThreadRaw() || !is_live_uobject(actor) ||
            !actor->IsA(AActor::StaticClass()) || !m_inventory_class)
        {
            return false;
        }

        const auto classification = classify_source_actor(actor);
        if (!classification.is_storage && !classification.is_bench)
        {
            return false;
        }
        if (classification.is_bench && !m_config.include_bench_inventories &&
            !(m_config.deposit_enabled && m_config.deposit_include_bench_inventories))
        {
            return false;
        }

        source = {};
        source.actor = actor;
        // A crafting station can inherit from the container base class. Treat any
        // actor with a processing component as a bench.
        source.is_storage = classification.is_storage && !classification.is_bench;
        source.is_bench = classification.is_bench;

        std::unordered_set<UObject*> seen{};
        const auto append_inventory = [this, &source, &seen](UObject* inventory) {
            if (inventory && seen.emplace(inventory).second && should_use_inventory(inventory))
            {
                source.inventories.emplace_back(inventory);
            }
        };

        if (classification.is_bench && m_processing_component_class)
        {
            auto processing_components = actor->GetComponentsByClass(m_processing_component_class);
            if (!validate_inventory_array(&processing_components, false))
            {
                std::memset(&processing_components, 0, sizeof(processing_components));
                return true;
            }
            for (auto* processing_component : processing_components)
            {
                append_inventory(get_processing_inventory(processing_component));
            }
        }
        if (source.inventories.empty() && classification.is_storage)
        {
            auto inventories = actor->GetComponentsByClass(m_inventory_class);
            if (!validate_inventory_array(&inventories, false))
            {
                std::memset(&inventories, 0, sizeof(inventories));
                return true;
            }
            for (auto* inventory : inventories)
            {
                append_inventory(inventory);
            }
        }

        // Keep eligible actors registered even if their inventory is initialized
        // later. A cache rebuild retries only empty or invalid source entries,
        // without requiring the actor to restart BeginPlay.
        return true;
    }

    auto NearbyCraftingMod::remove_bench_cache(UObject* processing_component) -> bool
    {
        const auto existing = m_bench_caches.find(processing_component);
        if (existing == m_bench_caches.end())
        {
            return false;
        }

        auto* bench_key = existing->second.bench_key;
        if (bench_key)
        {
            const auto [first, last] = m_bench_cache_keys_by_actor.equal_range(bench_key);
            for (auto reverse = first; reverse != last; ++reverse)
            {
                if (reverse->second == processing_component)
                {
                    m_bench_cache_keys_by_actor.erase(reverse);
                    break;
                }
            }
        }

        m_bench_caches.erase(existing);
        return true;
    }

    auto NearbyCraftingMod::remove_bench_caches_for_actor(
        AActor* actor
#if defined(NEARBYCRAFTING_DEBUG)
        , std::size_t* removed_count
#endif
        ) -> void
    {
        if (!actor)
        {
            return;
        }

        const auto [first, last] = m_bench_cache_keys_by_actor.equal_range(actor);
        for (auto reverse = first; reverse != last; ++reverse)
        {
#if defined(NEARBYCRAFTING_DEBUG)
            const auto removed = m_bench_caches.erase(reverse->second);
            if (removed_count)
            {
                *removed_count += removed;
            }
#else
            m_bench_caches.erase(reverse->second);
#endif
        }
        m_bench_cache_keys_by_actor.erase(first, last);
    }

#if defined(NEARBYCRAFTING_DEBUG)
    auto NearbyCraftingMod::log_debug_state(const wchar_t* event) const -> void
    {
        std::size_t storage_sources{};
        std::size_t bench_sources{};
        std::size_t source_inventories{};
        for (const auto& item : m_source_registry)
        {
            storage_sources += item.second.is_storage ? 1U : 0U;
            bench_sources += item.second.is_bench ? 1U : 0U;
            source_inventories += item.second.inventories.size();
        }

        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] State after {}: initialized={}, reflected-crafting-hooks={}, server-queue-hook={}, begin-id={}, end-id={}, sources={} (storage={}, bench={}, inventories={}), source-classes={}, generation={}, bench-caches={}, reverse-keys={}, hook-calls={}, cache-hits={}, cache-rebuilds={}.\n"),
            event ? event : L"unknown event",
            m_initialized.load(std::memory_order_acquire),
            m_hooks.size(),
            m_server_queue_trampoline != 0,
            m_begin_play_callback_id,
            m_end_play_callback_id,
            m_source_registry.size(),
            storage_sources,
            bench_sources,
            source_inventories,
            m_source_class_cache.size(),
            m_source_generation,
            m_bench_caches.size(),
            m_bench_cache_keys_by_actor.size(),
            m_hook_invocation_count,
            m_cache_hit_count,
            m_cache_rebuild_count);
    }
#endif

    auto NearbyCraftingMod::inject_nearby_inventories(
        UnrealScriptFunctionCallableContext& context,
        UFunction* function,
        FProperty* inventories_property,
        const wchar_t* function_path) -> void
    {
        try
        {
            if (!IsInGameThreadRaw() || !m_config.enabled || !function ||
                !inventories_property || !m_processing_component_class ||
                !is_live_uobject(context.Context) ||
                !context.Context->IsA(m_processing_component_class) ||
                !property_fits_parameter_buffer(
                    function, inventories_property, sizeof(TArray<UObject*>)))
            {
#if defined(NEARBYCRAFTING_DEBUG)
                Output::send<LogLevel::Normal>(
                    STR("[NearbyCrafting][DEBUG] Hook {} skipped because its thread, context, or reflected property contract was incompatible.\n"),
                    function_path ? function_path : L"<unknown>");
#endif
                return;
            }

            auto* locals = context.TheStack.Locals();
            if (!locals || !is_accessible_memory(
                    locals,
                    static_cast<std::size_t>(function->GetParmsSize()),
                    true))
            {
                if (!m_hook_error_logged)
                {
                    m_hook_error_logged = true;
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Hook {} received an invalid parameter frame; nearby inventory injection was skipped.\n"),
                        function_path ? function_path : L"<unknown>");
                }
                return;
            }

            auto* inventories = inventories_property
                                    ->ContainerPtrToValuePtr<TArray<UObject*>>(locals);
            if (!inventories || !validate_inventory_array(inventories, true) ||
                !validate_inventory_elements(*inventories, m_inventory_class))
            {
                if (!m_hook_error_logged)
                {
                    m_hook_error_logged = true;
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Hook {} received an invalid inventory array; nearby inventory injection was skipped.\n"),
                        function_path ? function_path : L"<unknown>");
                }
                return;
            }

            append_nearby_inventories(context.Context, *inventories, function_path);
        }
        catch (const std::exception& exception)
        {
            if (!m_hook_error_logged)
            {
                m_hook_error_logged = true;
                Output::send<LogLevel::Error>(
                    STR("[NearbyCrafting] Inventory hook failed safely and was skipped: {}\n"),
                    narrow_ascii(exception.what()));
            }
        }
        catch (...)
        {
            if (!m_hook_error_logged)
            {
                m_hook_error_logged = true;
                Output::send<LogLevel::Error>(
                    STR("[NearbyCrafting] Inventory hook failed safely with an unknown error and was skipped.\n"));
            }
        }
    }

    auto NearbyCraftingMod::append_nearby_inventories(
        UObject* cache_context,
        TArray<UObject*>& inventories,
        const wchar_t* function_path) -> void
    {
#if defined(NEARBYCRAFTING_DEBUG)
        ++m_hook_invocation_count;
#endif
        if (!IsInGameThreadRaw() || !m_config.enabled ||
            !is_live_uobject(cache_context) ||
            !validate_inventory_array(&inventories, true) ||
            !validate_inventory_elements(inventories, m_inventory_class))
        {
            return;
        }

        auto* cache = find_or_create_bench_cache(cache_context);
        if (!cache)
        {
#if defined(NEARBYCRAFTING_DEBUG)
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] Hook {} skipped because no proximity anchor could be resolved.\n"),
                function_path ? function_path : L"<unknown>");
#endif
            return;
        }

        auto* bench_object = cache->bench.Get();
        if (!bench_object || !bench_object->IsA(AActor::StaticClass()))
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto cache_uninitialized = !cache->initialized;
        const auto generation_changed = cache->source_generation != m_source_generation;
        const auto refresh_milliseconds = cache->is_player_context
            ? m_config.player_cache_refresh_milliseconds
            : m_config.bench_cache_refresh_milliseconds;
        const auto cache_expired = now - cache->last_refresh >=
            std::chrono::milliseconds{refresh_milliseconds};
        const auto authoritative_player_refresh =
            cache->is_player_context && function_path == server_queue_hook_path;
        if (cache_uninitialized || generation_changed || cache_expired || authoritative_player_refresh)
        {
#if defined(NEARBYCRAFTING_DEBUG)
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] Hook {} requested cache rebuild (player-context={}, refresh-ms={}, uninitialized={}, generation-changed={}, expired={}, authoritative-player-refresh={}).\n"),
                function_path ? function_path : L"<unknown>",
                cache->is_player_context,
                refresh_milliseconds,
                cache_uninitialized,
                generation_changed,
                cache_expired,
                authoritative_player_refresh);
#endif
            rebuild_bench_cache(*cache, static_cast<AActor*>(bench_object));
        }
#if defined(NEARBYCRAFTING_DEBUG)
        else
        {
            ++m_cache_hit_count;
        }
#endif

        auto* processor_inventory = cache->processor_inventory.Get();
        for (const auto& weak_inventory : cache->nearby_inventories)
        {
            if (inventories.Num() >= maximum_reasonable_inventory_capacity)
            {
                break;
            }
            auto* inventory = weak_inventory.Get();
            if (!is_live_uobject(inventory) || inventory == processor_inventory ||
                contains_inventory(inventories, inventory))
            {
                continue;
            }
            inventories.Add(inventory);
        }

    }

    auto NearbyCraftingMod::rebuild_bench_cache(BenchCacheEntry& cache, AActor* bench) -> void
    {
        if (!IsInGameThreadRaw() || !m_inventory_class ||
            !is_live_uobject(bench) || !bench->IsA(AActor::StaticClass()))
        {
            return;
        }

#if defined(NEARBYCRAFTING_DEBUG)
        const auto rebuild_started = std::chrono::steady_clock::now();
        ++m_cache_rebuild_count;
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Bench cache rebuild started (sources={}, old-cached-inventories={}, generation={}).\n"),
            m_source_registry.size(),
            cache.nearby_inventories.size(),
            m_source_generation);
#endif

        cache.initialized = false;
        cache.nearby_inventories.clear();
        refresh_stale_source_entries();

        if (auto* processing_component = cache.processing_component.Get())
        {
            cache.processor_inventory = get_processing_inventory(processing_component);
        }

        const auto bench_location = bench->K2_GetActorLocation();
        const auto* bench_world = bench->GetWorld();
        const auto radius_squared = m_config.scan_radius_centimeters * m_config.scan_radius_centimeters;

        struct Candidate
        {
            bool is_storage{};
            double distance_squared{};
            UObject* inventory{};
        };
        std::vector<Candidate> candidates{};
        std::unordered_set<UObject*> seen{};
        candidates.reserve(m_config.max_nearby_inventories);
        seen.reserve(m_config.max_nearby_inventories);

        for (const auto& item : m_source_registry)
        {
            if (item.second.is_bench && !m_config.include_bench_inventories)
            {
                continue;
            }

            auto* source_object = item.second.actor.Get();
            if (!source_object || source_object == bench || !source_object->IsA(AActor::StaticClass()))
            {
                continue;
            }

            auto* source_actor = static_cast<AActor*>(source_object);
            if (source_actor->GetWorld() != bench_world)
            {
                continue;
            }

            const auto source_location = source_actor->K2_GetActorLocation();
            const auto distance_squared = squared_distance(
                bench_location, source_location.X(), source_location.Y(), source_location.Z());
            if (distance_squared > radius_squared)
            {
                continue;
            }

            for (const auto& weak_inventory : item.second.inventories)
            {
                auto* inventory = weak_inventory.Get();
                if (inventory && inventory != cache.processor_inventory.Get() &&
                    seen.emplace(inventory).second && should_use_inventory(inventory))
                {
                    candidates.emplace_back(item.second.is_storage, distance_squared, inventory);
                }
            }
        }

        std::ranges::sort(candidates, [](const Candidate& left, const Candidate& right) {
            if (left.is_storage != right.is_storage)
            {
                return left.is_storage;
            }
            return left.distance_squared < right.distance_squared;
        });
        const auto inventory_count = std::min(candidates.size(), m_config.max_nearby_inventories);
        cache.nearby_inventories.reserve(inventory_count);
#if defined(NEARBYCRAFTING_DEBUG)
        std::size_t selected_storage_count{};
#endif
        for (std::size_t index = 0; index < inventory_count; ++index)
        {
            cache.nearby_inventories.emplace_back(candidates[index].inventory);
#if defined(NEARBYCRAFTING_DEBUG)
            selected_storage_count += candidates[index].is_storage ? 1U : 0U;
#endif
        }

        cache.source_generation = m_source_generation;
        cache.last_refresh = std::chrono::steady_clock::now();
        cache.initialized = true;

#if defined(NEARBYCRAFTING_DEBUG)
        const auto elapsed_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
            cache.last_refresh - rebuild_started).count();
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Bench cache rebuild complete: candidates={}, selected={} (storage={}, bench={}), sources={}, generation={}, elapsed-us={}.\n"),
            candidates.size(),
            cache.nearby_inventories.size(),
            selected_storage_count,
            cache.nearby_inventories.size() - selected_storage_count,
            m_source_registry.size(),
            cache.source_generation,
            elapsed_microseconds);
#endif
    }

    auto NearbyCraftingMod::is_storage_actor(AActor* actor) -> bool
    {
        if (!IsInGameThreadRaw() || !is_live_uobject(actor) ||
            !actor->IsA(AActor::StaticClass()))
        {
            return false;
        }

        if (auto* cached_class = m_container_class.Get())
        {
            return actor->IsA(static_cast<UClass*>(cached_class));
        }

        static const FName container_class_name{STR("BP_DeployableContainerBase_C")};
        for (UStruct* current_class = actor->GetClassPrivate();
             current_class;
             current_class = current_class->GetSuperStruct())
        {
            if (current_class->GetNamePrivate().Equals(container_class_name))
            {
                m_container_class = static_cast<UObject*>(current_class);
                return true;
            }
        }
        return false;
    }

    auto NearbyCraftingMod::is_player_crafting_actor(AActor* actor) const -> bool
    {
        return IsInGameThreadRaw() && is_live_uobject(actor) &&
            ((m_character_class && actor->IsA(m_character_class)) ||
             (m_player_controller_class && actor->IsA(m_player_controller_class)));
    }

    auto NearbyCraftingMod::classify_source_actor(AActor* actor) -> SourceClassInfo
    {
        SourceClassInfo classification{};
        if (!IsInGameThreadRaw() || !is_live_uobject(actor) ||
            !actor->IsA(AActor::StaticClass()))
        {
            return classification;
        }

        auto* actor_class = actor->GetClassPrivate();
        if (!actor_class)
        {
            return classification;
        }

        if (const auto existing = m_source_class_cache.find(actor_class);
            existing != m_source_class_cache.end())
        {
            if (existing->second.actor_class.Get() == actor_class)
            {
                return existing->second;
            }
            m_source_class_cache.erase(existing);
        }

        classification.actor_class = actor_class;
        classification.is_storage = is_storage_actor(actor);
        if (m_processing_component_class && !is_player_crafting_actor(actor))
        {
            auto processing_components = actor->GetComponentsByClass(m_processing_component_class);
            if (validate_inventory_array(&processing_components, false))
            {
                classification.is_bench = processing_components.Num() > 0;
            }
            else
            {
                std::memset(&processing_components, 0, sizeof(processing_components));
            }
        }

        m_source_class_cache.emplace(actor_class, classification);

        if (classification.is_storage || classification.is_bench)
        {
            Output::send<LogLevel::Verbose>(
                STR("[NearbyCrafting] Deposit container identifier: {} (storage={}, bench={}).\n"),
                actor_class->GetNamePrivate().ToString(),
                classification.is_storage,
                classification.is_bench);
        }

#if defined(NEARBYCRAFTING_DEBUG)
        if (classification.is_storage || classification.is_bench)
        {
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] Classified source actor class {} (storage={}, bench={}, cached-classes={}).\n"),
                actor_class->GetNamePrivate().ToString(),
                classification.is_storage,
                classification.is_bench,
                m_source_class_cache.size());
        }
#endif
        return classification;
    }

    auto NearbyCraftingMod::get_processing_inventory(UObject* processing_component) const -> UObject*
    {
        return read_object_property(
            processing_component, STR("Inventory"), m_inventory_class);
    }

    auto NearbyCraftingMod::find_or_create_bench_cache(UObject* context_object) -> BenchCacheEntry*
    {
        if (!IsInGameThreadRaw() || !is_live_uobject(context_object))
        {
            return nullptr;
        }

        auto* bench = context_object->IsA(AActor::StaticClass())
            ? static_cast<AActor*>(context_object)
            : get_component_owner(context_object);
        if (!bench)
        {
            return nullptr;
        }

        const auto player_context = is_player_crafting_actor(bench);
        auto* cache_key = player_context ? static_cast<UObject*>(bench) : context_object;
        auto* processing_component = m_processing_component_class &&
                context_object->IsA(m_processing_component_class)
            ? context_object
            : nullptr;

        if (auto existing = m_bench_caches.find(cache_key); existing != m_bench_caches.end())
        {
            auto& cache = existing->second;
            if (cache.bench.Get() == bench)
            {
                if (processing_component)
                {
                    cache.processing_component = processing_component;
                    cache.processor_inventory = get_processing_inventory(processing_component);
                }
                return &cache;
            }

            remove_bench_cache(cache_key);
#if defined(NEARBYCRAFTING_DEBUG)
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] Removed a stale proximity cache before recreating it.\n"));
#endif
        }

        if (!processing_component && player_context && m_processing_component_class)
        {
            auto processing_components = bench->GetComponentsByClass(m_processing_component_class);
            if (!validate_inventory_array(&processing_components, false))
            {
                std::memset(&processing_components, 0, sizeof(processing_components));
                return nullptr;
            }
            for (auto* candidate : processing_components)
            {
                processing_component = candidate;
                break;
            }
        }

        BenchCacheEntry new_cache{};
        new_cache.processing_component = processing_component;
        new_cache.bench = bench;
        new_cache.bench_key = bench;
        new_cache.is_player_context = player_context;
        new_cache.processor_inventory = get_processing_inventory(processing_component);
        auto [inserted, was_inserted] = m_bench_caches.emplace(cache_key, std::move(new_cache));
        if (!was_inserted)
        {
            return nullptr;
        }

        try
        {
            m_bench_cache_keys_by_actor.emplace(bench, cache_key);
        }
        catch (...)
        {
            m_bench_caches.erase(inserted);
            throw;
        }

#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Proximity cache created for owner class {} (player-context={}, caches={}, reverse-keys={}).\n"),
            bench->GetClassPrivate()->GetNamePrivate().ToString(),
            inserted->second.is_player_context,
            m_bench_caches.size(),
            m_bench_cache_keys_by_actor.size());
#endif
        return &inserted->second;
    }

    auto NearbyCraftingMod::get_component_owner(UObject* component) const -> AActor*
    {
        return static_cast<AActor*>(call_object_no_args(
            component, STR("GetOwner"), AActor::StaticClass()));
    }

    auto NearbyCraftingMod::find_local_player_controller() const -> UObject*
    {
        if (!m_player_controller_class)
        {
            return nullptr;
        }

        std::vector<UObject*> controllers{};
        UObjectGlobals::FindAllOf(STR("PlayerController"), controllers);
        for (auto* controller : controllers)
        {
            if (!is_live_uobject(controller) ||
                !controller->IsA(m_player_controller_class) ||
                !m_icarus_controller_class || !controller->IsA(m_icarus_controller_class) ||
                controller->HasAnyFlags(RF_ClassDefaultObject))
            {
                continue;
            }
            if (call_bool_no_args(controller, STR("IsLocalPlayerController"), false) ||
                call_bool_no_args(controller, STR("IsLocalController"), false))
            {
                return controller;
            }
        }
        return nullptr;
    }

    auto NearbyCraftingMod::get_controller_pawn(UObject* controller) const -> AActor*
    {
        if (auto* pawn = read_object_property(
                controller, STR("Pawn"), AActor::StaticClass()))
        {
            return static_cast<AActor*>(pawn);
        }
        if (auto* pawn = read_object_property(
                controller, STR("AcknowledgedPawn"), AActor::StaticClass()))
        {
            return static_cast<AActor*>(pawn);
        }

        return static_cast<AActor*>(call_object_no_args(
            controller, STR("GetPawn"), AActor::StaticClass()));
    }

    auto NearbyCraftingMod::get_player_backpack_inventory(AActor* player) const -> UObject*
    {
        return read_object_property(
            player, STR("BackpackInventory"), m_inventory_class);
    }

    auto NearbyCraftingMod::read_object_property(
        UObject* object,
        const wchar_t* property_name,
        UClass* expected_base_class) const -> UObject*
    {
        if (!IsInGameThreadRaw() || !property_name || !expected_base_class ||
            !is_live_uobject(object))
        {
            return nullptr;
        }

        auto* object_class = object->GetClassPrivate();
        auto* property = object_class
            ? object_class->FindProperty(FName(property_name))
            : nullptr;
        if (!property || !property->IsA<FObjectProperty>() ||
            property->GetElementSize() != sizeof(UObject*) ||
            property->GetSize() != sizeof(UObject*) ||
            !property_fits_struct(object_class, property))
        {
            return nullptr;
        }

        auto* object_property = static_cast<FObjectPropertyBase*>(property);
        auto* declared_class = object_property->GetPropertyClass().Get();
        if (!declared_class || !declared_class->IsChildOf(expected_base_class))
        {
            return nullptr;
        }

        auto** value = property->ContainerPtrToValuePtr<UObject*>(object);
        if (!value || !is_accessible_memory(value, sizeof(*value)))
        {
            return nullptr;
        }
        auto* result = *value;
        return is_live_uobject(result) && result->IsA(declared_class) &&
                result->IsA(expected_base_class)
            ? result
            : nullptr;
    }

    auto NearbyCraftingMod::call_object_no_args(
        UObject* object,
        const wchar_t* function_name,
        UClass* expected_base_class) const -> UObject*
    {
        if (!IsInGameThreadRaw() || !function_name || !expected_base_class ||
            !is_live_uobject(object))
        {
            return nullptr;
        }

        auto* function = object->GetFunctionByNameInChain(function_name);
        auto* return_property = function ? function->GetReturnProperty() : nullptr;
        if (!function || function->HasAnyFunctionFlags(FUNC_Static) ||
            !has_sane_parameter_buffer(function) ||
            !return_property || !return_property->IsA<FObjectProperty>() ||
            !return_property->HasAllPropertyFlags(
                CPF_Parm | CPF_OutParm | CPF_ReturnParm) ||
            return_property->HasAnyPropertyFlags(CPF_ConstParm | CPF_ReferenceParm) ||
            !property_fits_parameter_buffer(function, return_property, sizeof(UObject*)) ||
            !has_exact_parameters(function, {return_property}))
        {
            return nullptr;
        }

        auto* object_property = static_cast<FObjectPropertyBase*>(return_property);
        auto* declared_class = object_property->GetPropertyClass().Get();
        if (!declared_class || !declared_class->IsChildOf(expected_base_class))
        {
            return nullptr;
        }

        ReflectedParameters parameters{function};
        object->ProcessEvent(function, parameters.data());
        auto** value = return_property->ContainerPtrToValuePtr<UObject*>(parameters.data());
        if (!value || !is_accessible_memory(value, sizeof(*value)))
        {
            return nullptr;
        }
        auto* result = *value;
        return is_live_uobject(result) && result->IsA(declared_class) &&
                result->IsA(expected_base_class)
            ? result
            : nullptr;
    }

    auto NearbyCraftingMod::should_use_inventory(UObject* inventory) const -> bool
    {
        if (!IsInGameThreadRaw() || !m_inventory_class ||
            !is_live_uobject(inventory) || !inventory->IsA(m_inventory_class))
        {
            return false;
        }
        if (m_config.exclude_client_only_inventories &&
            call_bool_no_args(inventory, STR("IsClientSideOnlyInventory"), true))
        {
            return false;
        }
        if (m_config.exclude_remove_only_inventories &&
            call_bool_no_args(inventory, STR("IsRemoveOnly"), true))
        {
            return false;
        }
        return true;
    }

    auto NearbyCraftingMod::call_bool_no_args(
        UObject* object,
        const wchar_t* function_name,
        const bool fallback) const -> bool
    {
        if (!IsInGameThreadRaw() || !function_name || !is_live_uobject(object))
        {
            return fallback;
        }

        auto* function = object->GetFunctionByNameInChain(function_name);
        auto* return_property = function ? function->GetReturnProperty() : nullptr;
        if (!function || function->HasAnyFunctionFlags(FUNC_Static) ||
            !has_sane_parameter_buffer(function) ||
            !return_property || !return_property->IsA<FBoolProperty>() ||
            !return_property->HasAllPropertyFlags(
                CPF_Parm | CPF_OutParm | CPF_ReturnParm) ||
            return_property->HasAnyPropertyFlags(CPF_ConstParm | CPF_ReferenceParm) ||
            !property_fits_parameter_buffer(function, return_property) ||
            !has_exact_parameters(function, {return_property}))
        {
            return fallback;
        }

        ReflectedParameters parameters{function};
        object->ProcessEvent(function, parameters.data());
        return static_cast<FBoolProperty*>(return_property)
            ->GetPropertyValueInContainer(parameters.data());
    }

    auto NearbyCraftingMod::config_path() const -> std::filesystem::path
    {
        std::wstring buffer(32768, L'\0');
        const auto length = GetModuleFileNameW(
            reinterpret_cast<HMODULE>(&__ImageBase), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
        {
            return std::filesystem::path{L"config"} / L"NearbyCrafting.ini";
        }

        buffer.resize(length);
        const auto dll_directory = std::filesystem::path{buffer}.parent_path();
        const auto mod_directory = dll_directory.parent_path();
        return mod_directory / L"config" / L"NearbyCrafting.ini";
    }
}
