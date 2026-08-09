#include "NearbyCraftingInternal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/Handler.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>

namespace NearbyCrafting
{
    using namespace RC;
    using namespace RC::Unreal;
    using namespace Detail;

    auto NearbyCraftingMod::install_lifecycle_hooks() -> bool
    {
        Hook::FCallbackOptions begin_options{};
        begin_options.bReadonly = true;
        begin_options.OwnerModName = STR("NearbyCrafting");
        begin_options.HookName = STR("TrackStorageBeginPlay");
        const auto lifetime = m_callback_lifetime;
        m_begin_play_callback_id = Hook::RegisterBeginPlayPostCallback(
            [lifetime]([[maybe_unused]] auto& callback_data, AActor* actor) {
                CallbackLease lease{lifetime};
                auto* owner = lease.get();
                if (!owner || !IsInGameThreadRaw() ||
                    !owner->m_initialized.load(std::memory_order_acquire))
                {
                    return;
                }
                try
                {
                    owner->on_actor_begin_play(actor);
                }
                catch (const std::exception& exception)
                {
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] BeginPlay tracking failed safely: {}\n"),
                        narrow_ascii(exception.what()));
                }
                catch (...)
                {
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] BeginPlay tracking failed safely with an unknown error.\n"));
                }
            },
            std::move(begin_options));

        if (m_begin_play_callback_id == Hook::ERROR_ID)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Required BeginPlay post-hook registration failed.\n"));
            return false;
        }

#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] BeginPlay post-hook installed (callback-id={}).\n"),
            m_begin_play_callback_id);
#endif

        Hook::FCallbackOptions end_options{};
        end_options.bReadonly = true;
        end_options.OwnerModName = STR("NearbyCrafting");
        end_options.HookName = STR("TrackStorageEndPlay");
        m_end_play_callback_id = Hook::RegisterEndPlayPreCallback(
            [lifetime]([[maybe_unused]] auto& callback_data, AActor* actor, [[maybe_unused]] EEndPlayReason reason) {
                CallbackLease lease{lifetime};
                auto* owner = lease.get();
                if (!owner || !IsInGameThreadRaw() ||
                    !owner->m_initialized.load(std::memory_order_acquire))
                {
                    return;
                }
                try
                {
                    owner->on_actor_end_play(actor);
                }
                catch (const std::exception& exception)
                {
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] EndPlay tracking failed safely: {}\n"),
                        narrow_ascii(exception.what()));
                }
                catch (...)
                {
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] EndPlay tracking failed safely with an unknown error.\n"));
                }
            },
            std::move(end_options));

        if (m_end_play_callback_id == Hook::ERROR_ID)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Required EndPlay pre-hook registration failed.\n"));
            if (!Hook::UnregisterCallback(m_begin_play_callback_id))
            {
                static_cast<void>(pin_current_module_for_process_safety());
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Warning: BeginPlay rollback reported that its callback was already absent.\n"));
            }
            m_begin_play_callback_id = Hook::ERROR_ID;
            return false;
        }

#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] EndPlay pre-hook installed (callback-id={}).\n"),
            m_end_play_callback_id);
#endif

        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Installed incremental BeginPlay/EndPlay source tracking.\n"));
        return true;
    }

    auto NearbyCraftingMod::remove_lifecycle_hooks() -> void
    {
#if defined(NEARBYCRAFTING_DEBUG)
        const auto begin_play_id = m_begin_play_callback_id;
        const auto end_play_id = m_end_play_callback_id;
#endif
        if (m_begin_play_callback_id != Hook::ERROR_ID)
        {
            if (!Hook::UnregisterCallback(m_begin_play_callback_id))
            {
                static_cast<void>(pin_current_module_for_process_safety());
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Warning: BeginPlay callback was already absent during cleanup.\n"));
            }
            m_begin_play_callback_id = Hook::ERROR_ID;
        }
        if (m_end_play_callback_id != Hook::ERROR_ID)
        {
            if (!Hook::UnregisterCallback(m_end_play_callback_id))
            {
                static_cast<void>(pin_current_module_for_process_safety());
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Warning: EndPlay callback was already absent during cleanup.\n"));
            }
            m_end_play_callback_id = Hook::ERROR_ID;
        }

#if defined(NEARBYCRAFTING_DEBUG)
        const auto source_count = m_source_registry.size();
        const auto source_class_count = m_source_class_cache.size();
        const auto cache_count = m_bench_caches.size();
#endif
        m_source_registry.clear();
        m_source_class_cache.clear();
        m_bench_caches.clear();
        m_bench_cache_keys_by_actor.clear();

#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Lifecycle cleanup complete (begin-id={}, end-id={}, sources-cleared={}, source-classes-cleared={}, caches-cleared={}).\n"),
            begin_play_id,
            end_play_id,
            source_count,
            source_class_count,
            cache_count);
#endif
    }

    auto NearbyCraftingMod::install_initial_source_seed() -> bool
    {
        static constexpr std::uint32_t retry_delay_ticks = 60;
        const auto lifetime = m_callback_lifetime;
        const auto state = m_initial_source_seed_state;
        state->completed.store(false, std::memory_order_release);
        state->failure_logged.store(false, std::memory_order_release);
        state->retry_delay_ticks.store(0, std::memory_order_release);

        Hook::FCallbackOptions options{};
        options.bReadonly = true;
        options.OwnerModName = STR("NearbyCrafting");
        options.HookName = STR("SeedInitialInventorySources");
        m_initial_source_seed_tick_callback_id = Hook::RegisterEngineTickPostCallback(
            [lifetime, state]([[maybe_unused]] auto& callback_data,
                              [[maybe_unused]] auto* engine,
                              [[maybe_unused]] float delta_seconds,
                              [[maybe_unused]] bool idle_mode) {
                CallbackLease lease{lifetime};
                auto* owner = lease.get();
                if (!owner || !IsInGameThreadRaw() ||
                    !owner->m_initialized.load(std::memory_order_acquire) ||
                    state->completed.load(std::memory_order_acquire))
                {
                    return;
                }

                const auto remaining_delay =
                    state->retry_delay_ticks.load(std::memory_order_relaxed);
                if (remaining_delay > 0)
                {
                    state->retry_delay_ticks.store(
                        remaining_delay - 1, std::memory_order_relaxed);
                    return;
                }

                try
                {
                    owner->seed_source_registry();
                    state->completed.store(true, std::memory_order_release);
                    if (state->failure_logged.load(std::memory_order_acquire))
                    {
                        Output::send<LogLevel::Normal>(
                            STR("[NearbyCrafting] Initial game-thread source discovery recovered after a retry.\n"));
                    }
                }
                catch (const std::exception& exception)
                {
                    state->retry_delay_ticks.store(
                        retry_delay_ticks, std::memory_order_release);
                    if (!state->failure_logged.exchange(true, std::memory_order_acq_rel))
                    {
                        Output::send<LogLevel::Error>(
                            STR("[NearbyCrafting] Initial game-thread source discovery failed safely and will retry: {}\n"),
                            narrow_ascii(exception.what()));
                    }
                }
                catch (...)
                {
                    state->retry_delay_ticks.store(
                        retry_delay_ticks, std::memory_order_release);
                    if (!state->failure_logged.exchange(true, std::memory_order_acq_rel))
                    {
                        Output::send<LogLevel::Error>(
                            STR("[NearbyCrafting] Initial game-thread source discovery failed safely with an unknown error and will retry.\n"));
                    }
                }
            },
            std::move(options));

        if (m_initial_source_seed_tick_callback_id == Hook::ERROR_ID)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Required initial game-thread source scan callback registration failed.\n"));
            return false;
        }

        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Initial inventory source discovery scheduled for the first engine tick.\n"));
        return true;
    }

    auto NearbyCraftingMod::remove_initial_source_seed() -> void
    {
        const auto callback_id = std::exchange(
            m_initial_source_seed_tick_callback_id, Hook::ERROR_ID);
        if (callback_id == Hook::ERROR_ID)
        {
            return;
        }

        if (!Hook::UnregisterCallback(callback_id))
        {
            static_cast<void>(pin_current_module_for_process_safety());
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Warning: initial source scan callback was already absent during cleanup.\n"));
        }
    }

    auto NearbyCraftingMod::disable_callback_entry() -> void
    {
        if (!m_callback_lifetime)
        {
            return;
        }
        m_callback_lifetime->accepting_calls.store(false, std::memory_order_release);
        m_callback_lifetime->owner.store(nullptr, std::memory_order_release);
    }

    auto NearbyCraftingMod::wait_for_callback_quiescence() const -> void
    {
        if (m_callback_lifetime)
        {
            wait_for_calls(m_callback_lifetime->active_calls);
        }
    }

    auto NearbyCraftingMod::install_deposit_feature() -> bool
    {
        m_icarus_controller_class = UObjectGlobals::StaticFindObject<UClass*>(
            nullptr, nullptr, STR("/Script/Icarus.IcarusController"));
        m_transfer_like_function = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, transfer_like_function_path);
        auto* transfer_owner = m_transfer_like_function
            ? m_transfer_like_function->GetOuterPrivate()
            : nullptr;
        if (!m_icarus_controller_class || !m_transfer_like_function ||
            !m_player_controller_class ||
            !m_icarus_controller_class->IsChildOf(m_player_controller_class) ||
            !m_transfer_like_function->HasAllFunctionFlags(
                FUNC_Native | FUNC_Net | FUNC_NetServer) ||
            m_transfer_like_function->HasAnyFunctionFlags(FUNC_Static) ||
            !has_sane_parameter_buffer(m_transfer_like_function) ||
            !transfer_owner || !transfer_owner->IsA(UClass::StaticClass()) ||
            static_cast<UClass*>(transfer_owner) != m_icarus_controller_class ||
            m_transfer_like_function->GetReturnProperty())
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Nearby deposit function is unavailable or has an unexpected return value: {}.\n"),
                transfer_like_function_path);
            remove_deposit_feature();
            return false;
        }

        const auto resolve_inventory_parameter = [this](const wchar_t* name) -> FProperty* {
            auto* property = m_transfer_like_function->FindProperty(FName(name));
            if (!property || !property->IsA<FObjectProperty>() ||
                !property->HasAnyPropertyFlags(CPF_Parm) ||
                property->HasAnyPropertyFlags(
                    CPF_ConstParm | CPF_OutParm | CPF_ReferenceParm | CPF_ReturnParm) ||
                !property_fits_parameter_buffer(
                    m_transfer_like_function, property, sizeof(UObject*)))
            {
                return nullptr;
            }

            auto* object_property = static_cast<FObjectPropertyBase*>(property);
            auto* property_class = object_property->GetPropertyClass().Get();
            if (!property_class || !m_inventory_class ||
                property_class != m_inventory_class)
            {
                return nullptr;
            }
            return property;
        };

        m_transfer_from_inventory_property = resolve_inventory_parameter(L"FromInventory");
        m_transfer_to_inventory_property = resolve_inventory_parameter(L"ToInventory");
        if (!m_transfer_from_inventory_property || !m_transfer_to_inventory_property ||
            !has_exact_parameters(
                m_transfer_like_function,
                {m_transfer_from_inventory_property, m_transfer_to_inventory_property}) ||
            m_transfer_from_inventory_property->GetOffset_Internal() >=
                m_transfer_to_inventory_property->GetOffset_Internal())
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Nearby deposit function parameters are incompatible; the shortcut was not installed.\n"));
            remove_deposit_feature();
            return false;
        }

        if (!update_deposit_exclusions(m_config))
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Deposit exclusions could not be prepared safely and were disabled; the shortcut will continue without exclusion filtering.\n"));
        }

        Hook::FCallbackOptions tick_options{};
        tick_options.OwnerModName = STR("NearbyCrafting");
        tick_options.HookName = STR("ProcessNearbyDepositRequest");
        const auto request_state = m_deposit_request_state;
        const auto lifetime = m_callback_lifetime;
        m_deposit_tick_callback_id = Hook::RegisterEngineTickPostCallback(
            [lifetime, request_state]([[maybe_unused]] auto& callback_data,
                   [[maybe_unused]] auto* engine,
                   [[maybe_unused]] float delta_seconds,
                   [[maybe_unused]] bool idle_mode) {
                if (!request_state->requested.exchange(false, std::memory_order_acq_rel))
                {
                    return;
                }
                CallbackLease lease{lifetime};
                auto* owner = lease.get();
                if (!owner || !IsInGameThreadRaw() ||
                    !owner->m_initialized.load(std::memory_order_acquire))
                {
                    return;
                }

                try
                {
                    owner->deposit_inventory_to_nearby();
                }
                catch (const std::exception& exception)
                {
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Nearby deposit failed safely: {}\n"),
                        narrow_ascii(exception.what()));
                }
                catch (...)
                {
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Nearby deposit failed safely with an unknown error.\n"));
                }
            },
            std::move(tick_options));
        if (m_deposit_tick_callback_id == Hook::ERROR_ID)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Engine-tick callback registration failed; the deposit shortcut was not installed.\n"));
            remove_deposit_feature();
            return false;
        }

        const auto key = Input::string_to_key(narrow_ascii(m_config.deposit_key));
        const auto on_key_down = [request_state]() {
            if (request_state->active.load(std::memory_order_acquire))
            {
                request_state->requested.store(true, std::memory_order_release);
            }
        };

        if (m_config.deposit_modifier_mask == 0)
        {
            register_keydown_event(key, on_key_down);
        }
        else
        {
            Input::Handler::ModifierKeyArray modifiers{};
            std::size_t modifier_index{};
            if ((m_config.deposit_modifier_mask & deposit_modifier_shift) != 0)
            {
                modifiers[modifier_index++] = Input::ModifierKey::SHIFT;
            }
            if ((m_config.deposit_modifier_mask & deposit_modifier_control) != 0)
            {
                modifiers[modifier_index++] = Input::ModifierKey::CONTROL;
            }
            if ((m_config.deposit_modifier_mask & deposit_modifier_alt) != 0)
            {
                modifiers[modifier_index++] = Input::ModifierKey::ALT;
            }
            register_keydown_event(key, modifiers, on_key_down);
        }

        request_state->active.store(true, std::memory_order_release);
        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Nearby deposit shortcut installed (key={}, modifier-mask={}, benches={}, exclusions={}).\n"),
            narrow_ascii(m_config.deposit_key),
            m_config.deposit_modifier_mask,
            m_config.deposit_include_bench_inventories,
            m_normalized_deposit_exclusions.size());
        return true;
    }

    auto NearbyCraftingMod::update_deposit_exclusions(const Config& config) -> bool
    {
        std::vector<std::wstring> normalized_exclusions{};
        normalized_exclusions.reserve(config.deposit_excluded_items.size());
        for (const auto& exclusion : config.deposit_excluded_items)
        {
            auto normalized = normalize_deposit_item_name(narrow_ascii(exclusion));
            if (normalized.empty())
            {
                m_normalized_deposit_exclusions.clear();
                return false;
            }
            normalized_exclusions.emplace_back(std::move(normalized));
        }

        if (!normalized_exclusions.empty() && m_transfer_like_function &&
            !m_deposit_filter_bindings.get_all_items_function &&
            !install_deposit_filter_bindings())
        {
            m_normalized_deposit_exclusions.clear();
            return false;
        }

        m_normalized_deposit_exclusions = std::move(normalized_exclusions);
        return true;
    }

    auto NearbyCraftingMod::install_deposit_filter_bindings() -> bool
    {
        const auto reject_binding = [](const wchar_t* reason) -> bool {
#if defined(NEARBYCRAFTING_DEBUG)
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting][DEBUG] Deposit exclusion binding rejected: {}.\n"),
                reason);
#else
            static_cast<void>(reason);
#endif
            return false;
        };

#if defined(NEARBYCRAFTING_DEBUG)
        const auto log_property = [](const wchar_t* context, FProperty* property) {
            if (!property)
            {
                Output::send<LogLevel::Normal>(
                    STR("[NearbyCrafting][DEBUG] {} property=<null>.\n"),
                    context);
                return;
            }

            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] {} property={} class={} flags={} element-size={}.\n"),
                context,
                property->GetName(),
                property->GetClass().GetName(),
                static_cast<std::uint64_t>(property->GetPropertyFlags()),
                property->GetElementSize());

            if (property->IsA<FArrayProperty>())
            {
                auto* inner = static_cast<FArrayProperty*>(property)->GetInner();
                Output::send<LogLevel::Normal>(
                    STR("[NearbyCrafting][DEBUG] {} array-inner={} array-inner-class={}.\n"),
                    context,
                    inner ? inner->GetName() : L"<null>",
                    inner ? inner->GetClass().GetName() : L"<null>");
            }
            else if (property->IsA<FStructProperty>())
            {
                auto* structure = static_cast<FStructProperty*>(property)->GetStruct().Get();
                Output::send<LogLevel::Normal>(
                    STR("[NearbyCrafting][DEBUG] {} struct={}.\n"),
                    context,
                    structure ? structure->GetNamePrivate().ToString() : L"<null>");
            }
            else if (property->IsA<FObjectProperty>())
            {
                auto* property_class =
                    static_cast<FObjectPropertyBase*>(property)->GetPropertyClass().Get();
                Output::send<LogLevel::Normal>(
                    STR("[NearbyCrafting][DEBUG] {} object-class={}.\n"),
                    context,
                    property_class ? property_class->GetNamePrivate().ToString() : L"<null>");
            }
        };

        const auto log_function = [&log_property](const wchar_t* context, UFunction* function) {
            if (!function)
            {
                Output::send<LogLevel::Normal>(
                    STR("[NearbyCrafting][DEBUG] {} function=<null>.\n"),
                    context);
                return;
            }

            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] {} function={} parms-size={}.\n"),
                context,
                function->GetFullName(),
                function->GetParmsSize());
            for (auto* property : TFieldRange<FProperty>(
                     function, EFieldIterationFlags::IncludeDeprecated))
            {
                if (property->HasAnyPropertyFlags(CPF_Parm))
                {
                    log_property(context, property);
                }
            }
        };

        const auto log_struct = [&log_property](const wchar_t* context, UStruct* structure) {
            if (!structure)
            {
                Output::send<LogLevel::Normal>(
                    STR("[NearbyCrafting][DEBUG] {} struct=<null>.\n"),
                    context);
                return;
            }

            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] {} struct={} properties follow.\n"),
                context,
                structure->GetNamePrivate().ToString());
            for (auto* property : TFieldRange<FProperty>(
                     structure, EFieldIterationFlags::IncludeDeprecated))
            {
                log_property(context, property);
            }
        };

        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Resolving deposit exclusion bindings (items={}, transfer={}).\n"),
            get_all_items_function_path,
            transfer_all_of_type_function_path);
#endif

        auto* item_struct = UObjectGlobals::StaticFindObject<UScriptStruct*>(
            nullptr, nullptr, item_data_struct_path);
        auto* item_static_data_struct = UObjectGlobals::StaticFindObject<UScriptStruct*>(
            nullptr, nullptr, items_static_row_handle_struct_path);
        auto* itemable_data_struct = UObjectGlobals::StaticFindObject<UScriptStruct*>(
            nullptr, nullptr, itemable_data_struct_path);
        auto* data_valid_enum = UObjectGlobals::StaticFindObject<UEnum*>(
            nullptr, nullptr, data_valid_enum_path);
        if (!is_live_uobject(item_struct) ||
            !is_live_uobject(item_static_data_struct) ||
            !is_live_uobject(itemable_data_struct) ||
            !is_live_uobject(data_valid_enum))
        {
#if defined(NEARBYCRAFTING_DEBUG)
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting][DEBUG] Exact exclusion types: ItemData={}, ItemsStaticRowHandle={}, ItemableData={}, EDataValid={}.\n"),
                item_struct != nullptr,
                item_static_data_struct != nullptr,
                itemable_data_struct != nullptr,
                data_valid_enum != nullptr);
#endif
            return reject_binding(L"one or more exact Icarus exclusion types were not found");
        }

        auto* get_all_items_function = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, get_all_items_function_path);
        auto* get_all_items_owner = get_all_items_function
            ? get_all_items_function->GetOuterPrivate()
            : nullptr;
        if (!get_all_items_function ||
            !get_all_items_function->HasAnyFunctionFlags(FUNC_Native) ||
            get_all_items_function->HasAnyFunctionFlags(FUNC_Static) ||
            !has_sane_parameter_buffer(get_all_items_function) ||
            !get_all_items_owner ||
            !get_all_items_owner->IsA(UClass::StaticClass()) ||
            static_cast<UClass*>(get_all_items_owner) != m_inventory_class)
        {
            return reject_binding(L"Inventory:GetAllItems has an incompatible function contract");
        }

        auto* return_property = get_all_items_function->GetReturnProperty();
        if (!return_property || !return_property->IsA<FArrayProperty>() ||
            !return_property->HasAllPropertyFlags(
                CPF_Parm | CPF_OutParm | CPF_ReturnParm) ||
            return_property->HasAnyPropertyFlags(CPF_ConstParm | CPF_ReferenceParm) ||
            !property_fits_parameter_buffer(
                get_all_items_function, return_property, sizeof(FScriptArray)) ||
            !has_exact_parameters(get_all_items_function, {return_property}))
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_function(L"GetAllItems incompatible return", get_all_items_function);
#endif
            return reject_binding(L"Inventory:GetAllItems does not return an array");
        }
        auto* return_array_property = static_cast<FArrayProperty*>(return_property);
        auto* item_property = return_array_property->GetInner();
        if (!item_property || !item_property->IsA<FStructProperty>() ||
            item_property->GetSize() != item_struct->GetPropertiesSize())
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_property(L"GetAllItems return", return_property);
#endif
            return reject_binding(L"Inventory:GetAllItems array elements are not structs");
        }
        auto* returned_item_struct = static_cast<FStructProperty*>(item_property)->GetStruct().Get();
        if (returned_item_struct != item_struct)
        {
            return reject_binding(L"Inventory:GetAllItems does not return exact ItemData elements");
        }

        auto* item_static_data_property = item_struct->FindProperty(FName(STR("ItemStaticData")));
        if (!item_static_data_property || !item_static_data_property->IsA<FStructProperty>() ||
            static_cast<FStructProperty*>(item_static_data_property)->GetStruct().Get() !=
                item_static_data_struct ||
            !property_fits_struct(item_struct, item_static_data_property))
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_struct(L"GetAllItems item struct missing ItemStaticData", item_struct);
#endif
            return reject_binding(L"ItemData has no exact ItemsStaticRowHandle ItemStaticData property");
        }

        auto* get_itemable_data_function = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, get_itemable_data_function_path);
        auto* get_itemable_data_owner = get_itemable_data_function
            ? get_itemable_data_function->GetOuterPrivate()
            : nullptr;
        if (!get_itemable_data_function ||
            !get_itemable_data_function->HasAllFunctionFlags(FUNC_Native | FUNC_Static) ||
            !has_sane_parameter_buffer(get_itemable_data_function) ||
            !get_itemable_data_owner ||
            !get_itemable_data_owner->IsA(UClass::StaticClass()))
        {
            return reject_binding(
                L"InventoryItemLibrary:GetItemableData has an incompatible function contract");
        }
        if (get_itemable_data_function->GetReturnProperty())
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_function(L"GetItemableData unexpected return", get_itemable_data_function);
#endif
            return reject_binding(L"InventoryItemLibrary:GetItemableData has an unexpected return value");
        }

        // Reflected signature:
        //   GetItemableData(const ItemData& Item, out ItemableData ItemableData,
        //                   out EDataValid Paths)
        auto* get_itemable_data_input_property =
            get_itemable_data_function->FindProperty(FName(STR("Item")));
        if (!get_itemable_data_input_property ||
            !get_itemable_data_input_property->IsA<FStructProperty>() ||
            !get_itemable_data_input_property->HasAllPropertyFlags(
                CPF_Parm | CPF_OutParm | CPF_ConstParm | CPF_ReferenceParm) ||
            get_itemable_data_input_property->HasAnyPropertyFlags(CPF_ReturnParm) ||
            static_cast<FStructProperty*>(get_itemable_data_input_property)->GetStruct().Get() !=
                item_struct ||
            !property_fits_parameter_buffer(
                get_itemable_data_function, get_itemable_data_input_property))
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_function(L"GetItemableData incompatible Item input", get_itemable_data_function);
#endif
            return reject_binding(L"InventoryItemLibrary:GetItemableData Item parameter is incompatible");
        }

        auto* get_itemable_data_output_property =
            get_itemable_data_function->FindProperty(FName(STR("ItemableData")));
        if (!get_itemable_data_output_property ||
            !get_itemable_data_output_property->IsA<FStructProperty>() ||
            !get_itemable_data_output_property->HasAllPropertyFlags(CPF_Parm | CPF_OutParm) ||
            get_itemable_data_output_property->HasAnyPropertyFlags(
                CPF_ConstParm | CPF_ReferenceParm | CPF_ReturnParm) ||
            !property_fits_parameter_buffer(
                get_itemable_data_function, get_itemable_data_output_property))
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_function(L"GetItemableData incompatible ItemableData output", get_itemable_data_function);
#endif
            return reject_binding(L"InventoryItemLibrary:GetItemableData ItemableData output is incompatible");
        }
        auto* reflected_itemable_data_struct =
            static_cast<FStructProperty*>(get_itemable_data_output_property)->GetStruct().Get();
        if (reflected_itemable_data_struct != itemable_data_struct)
        {
            return reject_binding(L"GetItemableData does not output exact ItemableData");
        }
        auto* itemable_display_name_property =
            itemable_data_struct->FindProperty(FName(STR("DisplayName")));
        if (!itemable_display_name_property ||
            !itemable_display_name_property->IsA<FTextProperty>() ||
            !property_fits_struct(itemable_data_struct, itemable_display_name_property))
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_struct(L"GetItemableData ItemableData output missing FText DisplayName", itemable_data_struct);
#endif
            return reject_binding(L"ItemableData has no FText DisplayName property");
        }

        auto* get_itemable_data_paths_property =
            get_itemable_data_function->FindProperty(FName(STR("Paths")));
        auto* reflected_paths_enum =
            get_itemable_data_paths_property &&
                get_itemable_data_paths_property->IsA<FEnumProperty>()
            ? static_cast<FEnumProperty*>(get_itemable_data_paths_property)->GetEnum().Get()
            : nullptr;
        if (!get_itemable_data_paths_property ||
            !get_itemable_data_paths_property->IsA<FEnumProperty>() ||
            !get_itemable_data_paths_property->HasAllPropertyFlags(CPF_Parm | CPF_OutParm) ||
            get_itemable_data_paths_property->HasAnyPropertyFlags(
                CPF_ConstParm | CPF_ReferenceParm | CPF_ReturnParm) ||
            reflected_paths_enum != data_valid_enum ||
            !property_fits_parameter_buffer(
                get_itemable_data_function, get_itemable_data_paths_property) ||
            !has_exact_parameters(
                get_itemable_data_function,
                {get_itemable_data_input_property,
                 get_itemable_data_output_property,
                 get_itemable_data_paths_property}))
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_function(L"GetItemableData incompatible Paths output", get_itemable_data_function);
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] GetItemableData Paths enum identity: actual={} (address={}), expected={} (address={}), exact-match={}.\n"),
                reflected_paths_enum ? reflected_paths_enum->GetFullName() : L"<null>",
                reinterpret_cast<std::uintptr_t>(reflected_paths_enum),
                data_valid_enum ? data_valid_enum->GetFullName() : L"<null>",
                reinterpret_cast<std::uintptr_t>(data_valid_enum),
                reflected_paths_enum == data_valid_enum);
#endif
            return reject_binding(L"InventoryItemLibrary:GetItemableData Paths output is incompatible");
        }

        auto* get_itemable_data_context =
            static_cast<UClass*>(get_itemable_data_owner)->GetClassDefaultObject().Get();
        if (!is_live_uobject(get_itemable_data_context))
        {
            return reject_binding(L"InventoryItemLibrary has no class default object");
        }

        auto* transfer_function = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, transfer_all_of_type_function_path);
        auto* transfer_owner = transfer_function
            ? transfer_function->GetOuterPrivate()
            : nullptr;
        if (!transfer_function ||
            !transfer_function->HasAllFunctionFlags(
                FUNC_Native | FUNC_Net | FUNC_NetServer) ||
            transfer_function->HasAnyFunctionFlags(FUNC_Static) ||
            !has_sane_parameter_buffer(transfer_function) ||
            !transfer_owner || !transfer_owner->IsA(UClass::StaticClass()) ||
            static_cast<UClass*>(transfer_owner) != m_icarus_controller_class)
        {
            return reject_binding(
                L"IcarusController:OnServer_TransferAllOfType has an incompatible function contract");
        }
        if (transfer_function->GetReturnProperty())
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_function(L"TransferAllOfType unexpected return", transfer_function);
#endif
            return reject_binding(L"IcarusController:OnServer_TransferAllOfType has a return value");
        }

        const auto resolve_inventory_parameter = [this, transfer_function](const wchar_t* name) -> FProperty* {
            auto* property = transfer_function->FindProperty(FName(name));
            if (!property || !property->IsA<FObjectProperty>() ||
                !property->HasAnyPropertyFlags(CPF_Parm) ||
                property->HasAnyPropertyFlags(
                    CPF_ConstParm | CPF_OutParm | CPF_ReferenceParm | CPF_ReturnParm) ||
                !property_fits_parameter_buffer(
                    transfer_function, property, sizeof(UObject*)))
            {
                return nullptr;
            }
            auto* object_property = static_cast<FObjectPropertyBase*>(property);
            auto* property_class = object_property->GetPropertyClass().Get();
            if (!property_class || !m_inventory_class ||
                property_class != m_inventory_class)
            {
                return nullptr;
            }
            return property;
        };

        auto* from_inventory_property = resolve_inventory_parameter(L"FromInventory");
        auto* to_inventory_property = resolve_inventory_parameter(L"ToInventory");
        if (!from_inventory_property || !to_inventory_property)
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_function(L"TransferAllOfType incompatible inventories", transfer_function);
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting][DEBUG] TransferAllOfType inventory parameters: from-compatible={}, to-compatible={}.\n"),
                from_inventory_property != nullptr,
                to_inventory_property != nullptr);
#endif
            return reject_binding(L"TransferAllOfType inventory parameters are incompatible");
        }

        // Reflected signature has exactly FromInventory, ToInventory, and a
        // 24-byte ItemsStaticRowHandle parameter named Type.
        auto* type_property = transfer_function->FindProperty(FName(STR("Type")));
        if (!type_property || !type_property->IsA<FStructProperty>() ||
            !type_property->HasAnyPropertyFlags(CPF_Parm) ||
            type_property->HasAnyPropertyFlags(
                CPF_ConstParm | CPF_OutParm | CPF_ReferenceParm | CPF_ReturnParm) ||
            static_cast<FStructProperty*>(type_property)->GetStruct().Get() !=
                item_static_data_struct ||
            type_property->GetSize() != item_static_data_property->GetSize() ||
            !property_fits_parameter_buffer(transfer_function, type_property) ||
            !has_exact_parameters(
                transfer_function,
                {from_inventory_property, to_inventory_property, type_property}) ||
            !(from_inventory_property->GetOffset_Internal() <
                  to_inventory_property->GetOffset_Internal() &&
              to_inventory_property->GetOffset_Internal() <
                  type_property->GetOffset_Internal()))
        {
#if defined(NEARBYCRAFTING_DEBUG)
            log_function(L"TransferAllOfType incompatible Type parameter", transfer_function);
            log_property(L"Expected Type parameter shape", item_static_data_property);
            log_property(L"Reflected Type parameter", type_property);
#endif
            return reject_binding(L"TransferAllOfType Type parameter is incompatible");
        }

        m_deposit_filter_bindings = {
            get_all_items_function,
            return_property,
            item_struct,
            item_static_data_property,
            get_itemable_data_function,
            get_itemable_data_context,
            get_itemable_data_input_property,
            get_itemable_data_output_property,
            itemable_display_name_property,
            transfer_function,
            from_inventory_property,
            to_inventory_property,
            type_property,
        };
#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Deposit exclusion bindings resolved: items-function={}, item-struct={}, item-static-data={}, itemable-function={}, transfer-function={}, selector={}.\n"),
            get_all_items_function->GetFullName(),
            item_struct->GetNamePrivate().ToString(),
            item_static_data_property->GetName(),
            get_itemable_data_function->GetFullName(),
            transfer_function->GetFullName(),
            type_property->GetName());
#endif
        return true;
    }

    auto NearbyCraftingMod::remove_deposit_feature() -> void
    {
        m_deposit_request_state->active.store(false, std::memory_order_release);
        m_deposit_request_state->requested.store(false, std::memory_order_release);
        if (m_deposit_tick_callback_id != Hook::ERROR_ID)
        {
            if (!Hook::UnregisterCallback(m_deposit_tick_callback_id))
            {
                static_cast<void>(pin_current_module_for_process_safety());
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Warning: deposit tick callback was already absent during cleanup.\n"));
            }
            m_deposit_tick_callback_id = Hook::ERROR_ID;
        }
        m_transfer_like_function = nullptr;
        m_transfer_from_inventory_property = nullptr;
        m_transfer_to_inventory_property = nullptr;
        m_icarus_controller_class = nullptr;
        m_deposit_filter_bindings = {};
        m_normalized_deposit_exclusions.clear();
    }

    auto NearbyCraftingMod::install_config_reload_feature() -> bool
    {
        if (m_config.reload_config_key.empty())
        {
            Output::send<LogLevel::Verbose>(
                STR("[NearbyCrafting] Configuration reload shortcut is disabled by configuration.\n"));
            return false;
        }

        Hook::FCallbackOptions tick_options{};
        tick_options.OwnerModName = STR("NearbyCrafting");
        tick_options.HookName = STR("ProcessConfigReloadRequest");
        const auto request_state = m_reload_request_state;
        const auto lifetime = m_callback_lifetime;
        m_reload_tick_callback_id = Hook::RegisterEngineTickPostCallback(
            [lifetime, request_state]([[maybe_unused]] auto& callback_data,
                   [[maybe_unused]] auto* engine,
                   [[maybe_unused]] float delta_seconds,
                   [[maybe_unused]] bool idle_mode) {
                if (!request_state->requested.exchange(false, std::memory_order_acq_rel))
                {
                    return;
                }
                CallbackLease lease{lifetime};
                auto* owner = lease.get();
                if (!owner || !IsInGameThreadRaw() ||
                    !owner->m_initialized.load(std::memory_order_acquire))
                {
                    return;
                }

                try
                {
                    owner->reload_config();
                }
                catch (const std::exception& exception)
                {
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Configuration reload failed safely: {}\n"),
                        narrow_ascii(exception.what()));
                }
                catch (...)
                {
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Configuration reload failed safely with an unknown error.\n"));
                }
            },
            std::move(tick_options));
        if (m_reload_tick_callback_id == Hook::ERROR_ID)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Engine-tick callback registration failed; configuration reload was not installed.\n"));
            return false;
        }

        const auto key = Input::string_to_key(narrow_ascii(m_config.reload_config_key));
        register_keydown_event(key, [request_state]() {
            if (request_state->active.load(std::memory_order_acquire))
            {
                request_state->requested.store(true, std::memory_order_release);
            }
        });

        request_state->active.store(true, std::memory_order_release);
        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Configuration reload shortcut installed (key={}).\n"),
            narrow_ascii(m_config.reload_config_key));
        return true;
    }

    auto NearbyCraftingMod::remove_config_reload_feature() -> void
    {
        m_reload_request_state->active.store(false, std::memory_order_release);
        m_reload_request_state->requested.store(false, std::memory_order_release);
        if (m_reload_tick_callback_id != Hook::ERROR_ID)
        {
            if (!Hook::UnregisterCallback(m_reload_tick_callback_id))
            {
                static_cast<void>(pin_current_module_for_process_safety());
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Warning: configuration reload tick callback was already absent during cleanup.\n"));
            }
            m_reload_tick_callback_id = Hook::ERROR_ID;
        }
    }

    auto NearbyCraftingMod::reload_config() -> void
    {
        if (!IsInGameThreadRaw())
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Configuration reload skipped outside the game thread; live settings are unchanged.\n"));
            return;
        }

        const auto loaded = load_config(config_path());
        if (!loaded.file_found)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Configuration reload skipped because NearbyCrafting.ini was not found; live settings are unchanged.\n"));
            return;
        }

        for (const auto& warning : loaded.warnings)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Config reload warning: {}\n"),
                narrow_ascii(warning));
        }
        if (!loaded.warnings.empty())
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Configuration reload aborted because the file contains warnings; live settings are unchanged.\n"));
            return;
        }

        auto reloaded = merge_reloadable_config(m_config, loaded.config);
        if (!update_deposit_exclusions(reloaded))
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Deposit exclusions from the reloaded configuration could not be prepared safely and were disabled; Quick Deposit will continue without exclusion filtering.\n"));
        }

        m_config = std::move(reloaded);
        m_bench_caches.clear();
        m_bench_cache_keys_by_actor.clear();
        seed_source_registry();
        m_hook_error_logged = false;

        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting] Configuration reloaded (radius={} cm, inventory-limit={}, bench-cache={} ms, player-cache={} ms, craft-benches={}, deposit-benches={}, deposit-exclusions={}, exclude-client-only={}, exclude-remove-only={}).\n"),
            m_config.scan_radius_centimeters,
            m_config.max_nearby_inventories,
            m_config.bench_cache_refresh_milliseconds,
            m_config.player_cache_refresh_milliseconds,
            m_config.include_bench_inventories,
            m_config.deposit_include_bench_inventories,
            m_normalized_deposit_exclusions.size(),
            m_config.exclude_client_only_inventories,
            m_config.exclude_remove_only_inventories);
    }

    auto NearbyCraftingMod::deposit_inventory_to_nearby() -> void
    {
        if (!IsInGameThreadRaw() || !m_config.deposit_enabled ||
            !m_transfer_like_function)
        {
            return;
        }

        auto* controller = find_local_player_controller();
        auto* player = get_controller_pawn(controller);
        auto* backpack_inventory = get_player_backpack_inventory(player);
        if (!controller || !player || !backpack_inventory)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Deposit skipped: the local player's backpack inventory is unavailable.\n"));
            return;
        }

        refresh_stale_source_entries();
        const auto player_location = player->K2_GetActorLocation();
        const auto* player_world = player->GetWorld();
        const auto radius_squared = m_config.scan_radius_centimeters * m_config.scan_radius_centimeters;

        struct DepositCandidate
        {
            bool is_storage{};
            double distance_squared{};
            UObject* inventory{};
        };
        std::vector<DepositCandidate> candidates{};
        std::unordered_set<UObject*> seen{};
        candidates.reserve(m_config.max_nearby_inventories);
        seen.reserve(m_config.max_nearby_inventories);

        for (const auto& item : m_source_registry)
        {
            const auto& source = item.second;
            if (!source.is_storage && !(source.is_bench && m_config.deposit_include_bench_inventories))
            {
                continue;
            }

            auto* source_object = source.actor.Get();
            if (!is_live_uobject(source_object) || source_object == player ||
                !source_object->IsA(AActor::StaticClass()))
            {
                continue;
            }

            auto* source_actor = static_cast<AActor*>(source_object);
            if (source_actor->GetWorld() != player_world)
            {
                continue;
            }

            const auto source_location = source_actor->K2_GetActorLocation();
            const auto distance_squared = squared_distance(
                player_location, source_location.X(), source_location.Y(), source_location.Z());
            if (distance_squared > radius_squared)
            {
                continue;
            }

            for (const auto& weak_inventory : source.inventories)
            {
                auto* inventory = weak_inventory.Get();
                if (inventory && inventory != backpack_inventory &&
                    seen.emplace(inventory).second && should_use_inventory(inventory))
                {
                    candidates.emplace_back(source.is_storage, distance_squared, inventory);
                }
            }
        }

        std::ranges::sort(candidates, [](const DepositCandidate& left, const DepositCandidate& right) {
            if (left.is_storage != right.is_storage)
            {
                return left.is_storage;
            }
            return left.distance_squared < right.distance_squared;
        });

        const auto inventory_count = std::min(candidates.size(), m_config.max_nearby_inventories);
        if (m_normalized_deposit_exclusions.empty())
        {
            for (std::size_t index = 0; index < inventory_count; ++index)
            {
                // Icarus's TransferLike RPC only moves item types that already exist in
                // the destination.
                transfer_matching_items(controller, backpack_inventory, candidates[index].inventory);
            }

            Output::send<LogLevel::Verbose>(
                STR("[NearbyCrafting] Deposit requested for {} nearby inventories ({} eligible before limit, all TransferLike fast path).\n"),
                inventory_count,
                candidates.size());
            return;
        }

        const auto get_inventory_items = [this](UObject* inventory)
            -> std::optional<std::vector<ReflectedValue>> {
            const auto& bindings = m_deposit_filter_bindings;
            if (!IsInGameThreadRaw() || !is_live_uobject(inventory) ||
                !m_inventory_class || !inventory->IsA(m_inventory_class) ||
                !bindings.get_all_items_function ||
                !bindings.get_all_items_return_property ||
                !bindings.get_all_items_return_property->IsA<FArrayProperty>())
            {
                return std::nullopt;
            }

            ReflectedParameters parameters{bindings.get_all_items_function};
            inventory->ProcessEvent(bindings.get_all_items_function, parameters.data());
            auto* array_property = static_cast<FArrayProperty*>(bindings.get_all_items_return_property);
            auto* item_property = array_property->GetInner();
            auto* array = array_property->ContainerPtrToValuePtr<FScriptArray>(parameters.data());
            const auto element_size = item_property ? item_property->GetSize() : 0;
            const auto element_alignment = item_property
                ? item_property->GetMinAlignment()
                : 0;
            if (!item_property || element_size <= 0 || element_alignment <= 0 || !array ||
                !validate_script_array(
                    array,
                    static_cast<std::size_t>(element_size),
                    maximum_reasonable_item_count,
                    static_cast<std::size_t>(element_alignment)))
            {
                // This temporary owns the reflected return value. If the game wrote
                // an invalid array descriptor, make its destructor inert rather than
                // dereferencing or freeing untrusted storage while unwinding.
                if (array && is_accessible_memory(array, sizeof(FScriptArray), true))
                {
                    std::memset(array, 0, sizeof(FScriptArray));
                }
                return std::nullopt;
            }

            FScriptArrayHelper items{array_property, array};
            const auto item_count = items.Num();
            std::vector<ReflectedValue> result{};
            result.reserve(static_cast<std::size_t>(item_count));
            for (std::int32_t index = 0; index < item_count; ++index)
            {
                auto* item = items.GetRawPtr(index);
                if (!item || !is_accessible_memory(
                        item, static_cast<std::size_t>(element_size)))
                {
                    return std::nullopt;
                }
                result.emplace_back(item_property, item);
            }
            return result;
        };

        const auto source_items_result = get_inventory_items(backpack_inventory);
        if (!source_items_result)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Deposit skipped: backpack items could not be inspected safely for configured exclusions.\n"));
            return;
        }
        const auto& source_items = *source_items_result;
        auto* item_static_data_property = m_deposit_filter_bindings.item_static_data_property;
        const auto same_item_type = [item_static_data_property](const ReflectedValue& left, const ReflectedValue& right) {
            if (!item_static_data_property)
            {
                return false;
            }
            const auto* left_type = item_static_data_property->ContainerPtrToValuePtr<void>(left.data());
            const auto* right_type = item_static_data_property->ContainerPtrToValuePtr<void>(right.data());
            return item_static_data_property->Identical(left_type, right_type);
        };

        struct SourceItemType
        {
            std::size_t item_index{};
            bool excluded{};
        };
        std::vector<SourceItemType> source_item_types{};
        source_item_types.reserve(source_items.size());
        for (std::size_t item_index = 0; item_index < source_items.size(); ++item_index)
        {
            const auto duplicate = std::ranges::any_of(source_item_types, [&](const SourceItemType& existing) {
                return same_item_type(source_items[item_index], source_items[existing.item_index]);
            });
            if (duplicate)
            {
                continue;
            }

            const auto& bindings = m_deposit_filter_bindings;
            if (!is_live_uobject(bindings.get_itemable_data_context))
            {
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Deposit skipped: the item metadata library context is no longer valid.\n"));
                return;
            }
            ReflectedParameters itemable_parameters{bindings.get_itemable_data_function};
            auto* itemable_input =
                bindings.get_itemable_data_input_property->ContainerPtrToValuePtr<void>(
                    itemable_parameters.data());
            const auto input_size = bindings.get_itemable_data_input_property->GetSize();
            if (!itemable_input || input_size <= 0 || !is_accessible_memory(
                    itemable_input, static_cast<std::size_t>(input_size), true))
            {
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Deposit skipped: an item metadata input could not be addressed safely.\n"));
                return;
            }
            bindings.get_itemable_data_input_property->CopyCompleteValue(
                itemable_input, source_items[item_index].data());
            bindings.get_itemable_data_context->ProcessEvent(
                bindings.get_itemable_data_function, itemable_parameters.data());
            auto* itemable_data =
                bindings.get_itemable_data_output_property->ContainerPtrToValuePtr<void>(
                    itemable_parameters.data());
            auto* itemable_struct = static_cast<FStructProperty*>(
                bindings.get_itemable_data_output_property)->GetStruct().Get();
            const auto itemable_size = itemable_struct
                ? itemable_struct->GetPropertiesSize()
                : 0;
            if (!itemable_data || itemable_size <= 0 || !is_accessible_memory(
                    itemable_data, static_cast<std::size_t>(itemable_size)))
            {
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Deposit skipped: an item metadata output could not be inspected safely.\n"));
                return;
            }
            const auto* display_name =
                bindings.itemable_display_name_property->ContainerPtrToValuePtr<FText>(itemable_data);
            const auto display_name_value = display_name &&
                    is_accessible_memory(display_name, sizeof(FText))
                ? display_name->ToString()
                : StringType{};
            if (normalize_deposit_item_name(display_name_value).empty())
            {
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Deposit skipped: one backpack item type has no resolvable display name, so configured exclusions could not be applied safely.\n"));
                return;
            }
            const auto excluded = is_deposit_item_name_excluded(
                display_name_value,
                m_normalized_deposit_exclusions);
            source_item_types.emplace_back(item_index, excluded);
        }

        const auto has_excluded_source_item = std::ranges::any_of(
            source_item_types, [](const SourceItemType& item) { return item.excluded; });
        if (!has_excluded_source_item)
        {
            for (std::size_t index = 0; index < inventory_count; ++index)
            {
                transfer_matching_items(controller, backpack_inventory, candidates[index].inventory);
            }
            Output::send<LogLevel::Verbose>(
                STR("[NearbyCrafting] Deposit requested for {} nearby inventories ({} eligible before limit, backpack contains no excluded item types; all TransferLike fast path).\n"),
                inventory_count,
                candidates.size());
            return;
        }

        std::size_t transfer_like_count{};
        std::size_t filtered_inventory_count{};
        std::size_t transfer_type_request_count{};
        for (std::size_t index = 0; index < inventory_count; ++index)
        {
            auto* destination_inventory = candidates[index].inventory;
            const auto destination_items_result = get_inventory_items(destination_inventory);
            if (!destination_items_result)
            {
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] One deposit destination was skipped because its items could not be inspected safely.\n"));
                continue;
            }
            const auto& destination_items = *destination_items_result;
            const auto destination_contains_type = [&](const SourceItemType& source_type) {
                return std::ranges::any_of(destination_items, [&](const ReflectedValue& destination_item) {
                    return same_item_type(source_items[source_type.item_index], destination_item);
                });
            };

            const auto transfer_like_would_move_excluded_item = std::ranges::any_of(
                source_item_types, [&](const SourceItemType& source_type) {
                    return source_type.excluded && destination_contains_type(source_type);
                });
            if (!transfer_like_would_move_excluded_item)
            {
                transfer_matching_items(controller, backpack_inventory, destination_inventory);
                ++transfer_like_count;
                continue;
            }

            ++filtered_inventory_count;
            for (const auto& source_type : source_item_types)
            {
                if (!source_type.excluded && destination_contains_type(source_type))
                {
                    transfer_items_of_type(
                        controller,
                        backpack_inventory,
                        destination_inventory,
                        source_items[source_type.item_index].data());
                    ++transfer_type_request_count;
                }
            }
        }

        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Deposit requested for {} nearby inventories ({} eligible before limit, TransferLike fast path={}, filtered inventories={}, per-type requests={}).\n"),
            inventory_count,
            candidates.size(),
            transfer_like_count,
            filtered_inventory_count,
            transfer_type_request_count);
    }

    auto NearbyCraftingMod::transfer_matching_items(
        UObject* controller,
        UObject* from_inventory,
        UObject* to_inventory) const -> void
    {
        if (!IsInGameThreadRaw() || !is_live_uobject(controller) ||
            !is_live_uobject(from_inventory) || !is_live_uobject(to_inventory) ||
            from_inventory == to_inventory || !m_icarus_controller_class ||
            !controller->IsA(m_icarus_controller_class) || !m_inventory_class ||
            !from_inventory->IsA(m_inventory_class) ||
            !to_inventory->IsA(m_inventory_class) || !m_transfer_like_function ||
            !m_transfer_from_inventory_property || !m_transfer_to_inventory_property)
        {
            return;
        }

        ReflectedParameters parameters{m_transfer_like_function};
        *m_transfer_from_inventory_property->ContainerPtrToValuePtr<UObject*>(parameters.data()) = from_inventory;
        *m_transfer_to_inventory_property->ContainerPtrToValuePtr<UObject*>(parameters.data()) = to_inventory;
        controller->ProcessEvent(m_transfer_like_function, parameters.data());
    }

    auto NearbyCraftingMod::transfer_items_of_type(
        UObject* controller,
        UObject* from_inventory,
        UObject* to_inventory,
        const void* item_data) const -> void
    {
        const auto& bindings = m_deposit_filter_bindings;
        if (!IsInGameThreadRaw() || !is_live_uobject(controller) ||
            !is_live_uobject(from_inventory) || !is_live_uobject(to_inventory) ||
            from_inventory == to_inventory || !item_data || !m_icarus_controller_class ||
            !controller->IsA(m_icarus_controller_class) || !m_inventory_class ||
            !from_inventory->IsA(m_inventory_class) ||
            !to_inventory->IsA(m_inventory_class) ||
            !bindings.transfer_all_of_type_function ||
            !bindings.transfer_from_inventory_property ||
            !bindings.transfer_to_inventory_property ||
            !bindings.transfer_type_property ||
            !bindings.item_static_data_property)
        {
            return;
        }

        const auto* type_source =
            bindings.item_static_data_property->ContainerPtrToValuePtr<void>(item_data);
        const auto type_size = bindings.transfer_type_property->GetSize();
        if (!type_source || type_size <= 0 ||
            !is_accessible_memory(
                type_source, static_cast<std::size_t>(type_size)))
        {
            return;
        }

        ReflectedParameters parameters{bindings.transfer_all_of_type_function};
        *bindings.transfer_from_inventory_property->ContainerPtrToValuePtr<UObject*>(parameters.data()) = from_inventory;
        *bindings.transfer_to_inventory_property->ContainerPtrToValuePtr<UObject*>(parameters.data()) = to_inventory;
        auto* type_destination =
            bindings.transfer_type_property->ContainerPtrToValuePtr<void>(parameters.data());
        if (!type_destination || !is_accessible_memory(
                type_destination, static_cast<std::size_t>(type_size), true))
        {
            return;
        }
        bindings.transfer_type_property->CopyCompleteValue(type_destination, type_source);
        controller->ProcessEvent(bindings.transfer_all_of_type_function, parameters.data());
    }
}
