#include "NearbyCraftingInternal.hpp"

#include <exception>
#include <string>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <polyhook2/Detour/x64Detour.hpp>

namespace NearbyCrafting
{
    using namespace RC;
    using namespace RC::Unreal;
    using namespace Detail;

    NearbyCraftingMod::NearbyCraftingMod()
        : CppUserModBase()
    {
        m_callback_lifetime->owner.store(this, std::memory_order_release);
        ModName = STR("NearbyCrafting");
#if defined(NEARBYCRAFTING_DEBUG)
        ModVersion = narrow_ascii(std::string{NEARBYCRAFTING_VERSION} + "-debug");
#else
        ModVersion = narrow_ascii(NEARBYCRAFTING_VERSION);
#endif
        ModDescription = STR("Craft, repair, and deposit items using nearby storage inventories.");
        ModAuthors = STR("Dogeio");
        const auto loaded = load_config(config_path());
        m_config = loaded.config;

        if (!loaded.file_found)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Config not found; using safe defaults (20 m radius, 96 inventories).\n"));
        }
        for (const auto& warning : loaded.warnings)
        {
            Output::send<LogLevel::Warning>(STR("[NearbyCrafting] Config warning: {}\n"), narrow_ascii(warning));
        }

        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Native mod loaded (enabled={}, reload-key={}, radius={} cm, bench-cache={} ms, player-cache={} ms, bench-sources={}, repairs={}, deposit={}, deposit-key={}, deposit-modifiers={}, deposit-benches={}, deposit-exclusions={}).\n"),
            m_config.enabled,
            narrow_ascii(m_config.reload_config_key),
            m_config.scan_radius_centimeters,
            m_config.bench_cache_refresh_milliseconds,
            m_config.player_cache_refresh_milliseconds,
            m_config.include_bench_inventories,
            m_config.repairs_enabled,
            m_config.deposit_enabled,
            narrow_ascii(m_config.deposit_key),
            m_config.deposit_modifier_mask,
            m_config.deposit_include_bench_inventories,
            m_config.deposit_excluded_items.size());

#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Diagnostic build loaded; high-volume console logging is active.\n"));
        log_debug_state(L"constructor complete");
#endif
    }

    NearbyCraftingMod::~NearbyCraftingMod()
    {
        m_deposit_request_state->active.store(false, std::memory_order_release);
        m_reload_request_state->active.store(false, std::memory_order_release);
        disable_callback_entry();
        wait_for_callback_quiescence();
#if defined(NEARBYCRAFTING_DEBUG)
        log_debug_state(L"shutdown started");
#endif
        remove_initial_source_seed();
        remove_server_queue_hook();
        remove_repair_material_lookup_hook();
        m_initialized.store(false, std::memory_order_release);
        remove_hooks();
        remove_deposit_feature();
        remove_config_reload_feature();
        remove_lifecycle_hooks();
#if defined(NEARBYCRAFTING_DEBUG)
        log_debug_state(L"shutdown complete");
#endif
    }

    auto NearbyCraftingMod::on_unreal_init() -> void
    {
#if defined(NEARBYCRAFTING_DEBUG)
        log_debug_state(L"initialization started");
#endif
        if (!m_config.enabled)
        {
            Output::send<LogLevel::Warning>(STR("[NearbyCrafting] Disabled by configuration; no hooks installed.\n"));
            return;
        }

        try
        {
            m_inventory_class = UObjectGlobals::StaticFindObject<UClass*>(
                nullptr, nullptr, STR("/Script/Icarus.Inventory"));
            if (!m_inventory_class)
            {
                fail_initialization(L"/Script/Icarus.Inventory is unavailable");
                return;
            }

            m_character_class = UObjectGlobals::StaticFindObject<UClass*>(
                nullptr, nullptr, STR("/Script/Engine.Character"));
            m_player_controller_class = UObjectGlobals::StaticFindObject<UClass*>(
                nullptr, nullptr, STR("/Script/Engine.PlayerController"));
            m_icarus_player_character_class = UObjectGlobals::StaticFindObject<UClass*>(
                nullptr, nullptr, icarus_player_character_class_path);
            if (!m_character_class && !m_player_controller_class)
            {
                fail_initialization(L"player crafting owner classes are unavailable");
                return;
            }

            m_processing_component_class = UObjectGlobals::StaticFindObject<UClass*>(
                nullptr, nullptr, STR("/Script/Icarus.ProcessingComponent"));
            if (!m_processing_component_class)
            {
                fail_initialization(L"/Script/Icarus.ProcessingComponent is unavailable");
                return;
            }

#if defined(NEARBYCRAFTING_DEBUG)
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] Resolved inventory, player-owner, and processing-component class requirements (bench-sources={}).\n"),
                m_config.include_bench_inventories);
#endif

            // UE4SS defers destruction of unregistered callback targets and does
            // not export a synchronous collection API. Native detour entry stubs
            // also cannot be reclaimed safely while another thread may be paused
            // inside one, so an enabled instance keeps this DLL resident until the
            // process exits.
            if (!pin_current_module_for_process_safety())
            {
                fail_initialization(L"the mod DLL could not be retained for callback safety");
                return;
            }

            if (!install_lifecycle_hooks())
            {
                fail_initialization(L"a required BeginPlay or EndPlay hook is unavailable");
                return;
            }

            if (!install_hooks())
            {
                fail_initialization(L"one or more required crafting hooks are unavailable");
                return;
            }
            if (!m_icarus_player_character_class ||
                (m_character_class &&
                 !m_icarus_player_character_class->IsChildOf(m_character_class)))
            {
                fail_initialization(L"the Icarus player character class is unavailable or incompatible");
                return;
            }
            if (!install_server_queue_hook())
            {
                fail_initialization(L"the required native server queue hook is unavailable");
                return;
            }

            auto repair_hook_active = false;
            if (m_config.repairs_enabled)
            {
                try
                {
                    repair_hook_active = install_repair_material_lookup_hook();
                }
                catch (const std::exception& exception)
                {
                    Output::send<LogLevel::Warning>(
                        STR("[NearbyCrafting] Optional nearby-repair hook raised an exception during setup: {}\n"),
                        narrow_ascii(exception.what()));
                    try
                    {
                        remove_repair_material_lookup_hook();
                    }
                    catch (...)
                    {
                        Output::send<LogLevel::Warning>(
                            STR("[NearbyCrafting] Warning: optional nearby-repair hook cleanup also failed.\n"));
                    }
                }
                catch (...)
                {
                    Output::send<LogLevel::Warning>(
                        STR("[NearbyCrafting] Optional nearby-repair hook raised an unknown exception during setup.\n"));
                    try
                    {
                        remove_repair_material_lookup_hook();
                    }
                    catch (...)
                    {
                        Output::send<LogLevel::Warning>(
                            STR("[NearbyCrafting] Warning: optional nearby-repair hook cleanup also failed.\n"));
                    }
                }
            }

            if (m_config.repairs_enabled && !repair_hook_active)
            {
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Nearby inventory repairs are unavailable; nearby crafting will continue normally.\n"));
            }
            else if (!m_config.repairs_enabled)
            {
                Output::send<LogLevel::Verbose>(
                    STR("[NearbyCrafting] Nearby inventory repairs are disabled by configuration; no native repair hook was installed.\n"));
            }

            auto deposit_active = false;
            if (m_config.deposit_enabled)
            {
                try
                {
                    deposit_active = install_deposit_feature();
                }
                catch (const std::exception& exception)
                {
                    Output::send<LogLevel::Warning>(
                        STR("[NearbyCrafting] Nearby deposit setup failed safely: {}\n"),
                        narrow_ascii(exception.what()));
                    remove_deposit_feature();
                }
                catch (...)
                {
                    Output::send<LogLevel::Warning>(
                        STR("[NearbyCrafting] Nearby deposit setup failed safely with an unknown error.\n"));
                    remove_deposit_feature();
                }
            }

            auto config_reload_active = false;
            try
            {
                config_reload_active = install_config_reload_feature();
            }
            catch (const std::exception& exception)
            {
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Configuration reload setup failed safely: {}\n"),
                    narrow_ascii(exception.what()));
                remove_config_reload_feature();
            }
            catch (...)
            {
                Output::send<LogLevel::Warning>(
                    STR("[NearbyCrafting] Configuration reload setup failed safely with an unknown error.\n"));
                remove_config_reload_feature();
            }

            m_initialized.store(true, std::memory_order_release);
            if (!install_initial_source_seed())
            {
                m_initialized.store(false, std::memory_order_release);
                fail_initialization(L"the required game-thread source scan callback is unavailable");
                return;
            }

            Output::send<LogLevel::Verbose>(
                STR("[NearbyCrafting] Initialization complete; all {} required crafting/lifecycle hooks are active; nearby repairs={}; nearby deposit={}; config reload={}.\n"),
                generic_crafting_hook_count + 3,
                repair_hook_active ? L"active" : (m_config.repairs_enabled ? L"unavailable" : L"disabled"),
                deposit_active ? L"active" : (m_config.deposit_enabled ? L"unavailable" : L"disabled"),
                config_reload_active ? L"active" : (m_config.reload_config_key.empty() ? L"disabled" : L"unavailable"));
        }
        catch (const std::exception& exception)
        {
            Output::send<LogLevel::Error>(
                STR("[NearbyCrafting] Exception during initialization: {}\n"),
                narrow_ascii(exception.what()));
            fail_initialization(L"an exception interrupted initialization");
        }
        catch (...)
        {
            Output::send<LogLevel::Error>(
                STR("[NearbyCrafting] Unknown exception during initialization.\n"));
            fail_initialization(L"an unknown exception interrupted initialization");
        }
    }


    auto NearbyCraftingMod::fail_initialization(const wchar_t* reason) -> void
    {
        m_deposit_request_state->active.store(false, std::memory_order_release);
        m_reload_request_state->active.store(false, std::memory_order_release);
        disable_callback_entry();
        wait_for_callback_quiescence();
#if defined(NEARBYCRAFTING_DEBUG)
        log_debug_state(L"initialization rollback started");
#endif
        remove_initial_source_seed();
        remove_server_queue_hook();
        remove_repair_material_lookup_hook();
        m_initialized.store(false, std::memory_order_release);
        remove_hooks();
        remove_deposit_feature();
        remove_config_reload_feature();
        remove_lifecycle_hooks();
        m_inventory_class = nullptr;
        m_processing_component_class = nullptr;
        m_character_class = nullptr;
        m_player_controller_class = nullptr;
        m_icarus_player_character_class = nullptr;
        m_container_class.Reset();

        Output::send<LogLevel::Warning>(
            STR("[NearbyCrafting] Initialization failed: {}. All NearbyCrafting hooks were removed and runtime caches were cleared; the mod is inert.\n"),
            reason ? reason : L"an unspecified requirement was unavailable");
#if defined(NEARBYCRAFTING_DEBUG)
        log_debug_state(L"initialization rollback complete");
#endif
    }

}
