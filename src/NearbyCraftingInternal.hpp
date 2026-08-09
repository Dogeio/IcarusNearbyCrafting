#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <String/StringType.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/Core/HAL/UnrealMemory.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/UnrealCoreStructs.hpp>

#include <NearbyCrafting/NearbyCraftingMod.hpp>

namespace NearbyCrafting::Detail
{
    using namespace RC;
    using namespace RC::Unreal;

    inline constexpr const wchar_t* server_queue_hook_path =
        L"/Script/Icarus.ProcessingComponent:OnServer_AddProcessingRecipe";
    inline constexpr const wchar_t* repair_material_lookup_path =
        L"/Script/Icarus.ItemManipulationComponent:FindItemsRequiredForRepair";
    inline constexpr const wchar_t* item_manipulation_component_class_path =
        L"/Script/Icarus.ItemManipulationComponent";
    inline constexpr const wchar_t* processor_recipes_row_handle_struct_path =
        L"/Script/Icarus.ProcessorRecipesRowHandle";
    inline constexpr const wchar_t* icarus_player_character_class_path =
        L"/Script/Icarus.IcarusPlayerCharacter";
    inline constexpr const wchar_t* transfer_like_function_path =
        L"/Script/Icarus.IcarusController:OnServer_TransferLike";
    inline constexpr const wchar_t* transfer_all_of_type_function_path =
        L"/Script/Icarus.IcarusController:OnServer_TransferAllOfType";
    inline constexpr const wchar_t* get_all_items_function_path =
        L"/Script/Icarus.Inventory:GetAllItems";
    inline constexpr const wchar_t* get_itemable_data_function_path =
        L"/Script/Icarus.InventoryItemLibrary:GetItemableData";
    inline constexpr const wchar_t* item_data_struct_path =
        L"/Script/Icarus.ItemData";
    inline constexpr const wchar_t* queue_item_struct_path =
        L"/Script/Icarus.QueueItem";
    inline constexpr const wchar_t* items_static_row_handle_struct_path =
        L"/Script/Icarus.ItemsStaticRowHandle";
    inline constexpr const wchar_t* itemable_data_struct_path =
        L"/Script/Icarus.ItemableData";
    inline constexpr const wchar_t* data_valid_enum_path =
        L"/Script/Icarus.EDataValid";

    inline constexpr std::size_t generic_crafting_hook_count = 6;
    inline constexpr std::size_t maximum_reflected_parameter_bytes = 65535;
    inline constexpr std::int32_t maximum_reasonable_inventory_capacity = 4096;
    inline constexpr std::int32_t maximum_reasonable_item_count = 4096;

    static_assert(sizeof(void*) == 8, "NearbyCrafting supports only the verified Win64 ABI");
    static_assert(sizeof(TArray<UObject*>) == 16, "Unexpected Unreal TArray layout");
    static_assert(alignof(TArray<UObject*>) == alignof(void*), "Unexpected Unreal TArray alignment");
    static_assert(sizeof(FScriptArray) == sizeof(TArray<UObject*>), "Unexpected FScriptArray layout");

    auto pin_current_module_for_process_safety() -> bool;
    [[nodiscard]] auto is_accessible_memory(
        const void* address,
        std::size_t size,
        bool require_write = false,
        bool require_execute = false) -> bool;
    [[nodiscard]] auto is_live_uobject(UObject* object) -> bool;
    [[nodiscard]] auto has_sane_parameter_buffer(UFunction* function) -> bool;
    [[nodiscard]] auto property_fits_parameter_buffer(
        UFunction* function,
        FProperty* property,
        std::size_t expected_size = 0) -> bool;
    [[nodiscard]] auto property_fits_struct(UStruct* structure, FProperty* property) -> bool;
    [[nodiscard]] auto has_exact_parameters(
        UFunction* function,
        std::initializer_list<FProperty*> expected) -> bool;
    [[nodiscard]] auto validate_script_array(
        const FScriptArray* array,
        std::size_t element_size,
        std::int32_t maximum_count,
        std::size_t element_alignment = 1) -> bool;
    [[nodiscard]] auto validate_inventory_array(
        const void* inventories_value,
        bool require_write) -> bool;
    [[nodiscard]] auto validate_inventory_elements(
        const TArray<UObject*>& inventories,
        UClass* inventory_class) -> bool;
    auto wait_for_calls(std::atomic_uint32_t& active_calls) -> void;

    class CallbackLease
    {
    public:
        explicit CallbackLease(std::shared_ptr<CallbackLifetimeState> state)
            : m_state(std::move(state))
        {
            if (!m_state ||
                !m_state->accepting_calls.load(std::memory_order_acquire))
            {
                return;
            }

            m_state->active_calls.fetch_add(1, std::memory_order_acq_rel);
            m_active = true;
            if (!m_state->accepting_calls.load(std::memory_order_acquire))
            {
                release();
                return;
            }
            m_owner = m_state->owner.load(std::memory_order_acquire);
            if (!m_owner)
            {
                release();
            }
        }

        ~CallbackLease()
        {
            release();
        }

        CallbackLease(const CallbackLease&) = delete;
        CallbackLease& operator=(const CallbackLease&) = delete;

        [[nodiscard]] auto get() const -> NearbyCraftingMod*
        {
            return m_owner;
        }

    private:
        auto release() -> void
        {
            m_owner = nullptr;
            if (m_active)
            {
                m_state->active_calls.fetch_sub(1, std::memory_order_acq_rel);
                m_active = false;
            }
        }

        std::shared_ptr<CallbackLifetimeState> m_state{};
        NearbyCraftingMod* m_owner{};
        bool m_active{};
    };

    class ReflectedParameters
    {
    public:
        explicit ReflectedParameters(UFunction* function)
            : m_function(function)
        {
            if (!has_sane_parameter_buffer(m_function))
            {
                throw std::runtime_error("reflected function has an invalid parameter buffer");
            }

            for (auto* property : TFieldRange<FProperty>(
                     m_function, EFieldIterationFlags::IncludeDeprecated))
            {
                if (property->HasAnyPropertyFlags(CPF_Parm))
                {
                    if (!property_fits_parameter_buffer(m_function, property))
                    {
                        throw std::runtime_error(
                            "reflected parameter lies outside its function buffer");
                    }
                    m_properties.emplace_back(property);
                }
            }

            const auto minimum_alignment = m_function->GetMinAlignment();
            const auto alignment = minimum_alignment > 0
                ? static_cast<std::uint32_t>(minimum_alignment)
                : static_cast<std::uint32_t>(alignof(std::max_align_t));
            if ((alignment & (alignment - 1U)) != 0 || alignment > 4096U)
            {
                throw std::runtime_error(
                    "reflected function has an invalid parameter alignment");
            }
            m_data = FMemory::MallocZeroed(
                static_cast<std::size_t>(m_function->GetParmsSize()), alignment);
            if (!m_data)
            {
                throw std::runtime_error(
                    "could not allocate reflected function parameters");
            }

            try
            {
                for (auto* property : m_properties)
                {
                    property->InitializeValue_InContainer(m_data);
                    ++m_initialized_properties;
                }
            }
            catch (...)
            {
                reset();
                throw;
            }
        }

        ~ReflectedParameters()
        {
            reset();
        }

        ReflectedParameters(const ReflectedParameters&) = delete;
        ReflectedParameters& operator=(const ReflectedParameters&) = delete;

        [[nodiscard]] auto data() const -> void*
        {
            return m_data;
        }

    private:
        auto reset() -> void
        {
            while (m_data && m_initialized_properties > 0)
            {
                --m_initialized_properties;
                m_properties[m_initialized_properties]->DestroyValue_InContainer(m_data);
            }
            if (m_data)
            {
                FMemory::Free(m_data);
                m_data = nullptr;
            }
        }

        UFunction* m_function{};
        void* m_data{};
        std::vector<FProperty*> m_properties{};
        std::size_t m_initialized_properties{};
    };

    class ReflectedValue
    {
    public:
        ReflectedValue(FProperty* property, const void* source)
            : m_property(property)
        {
            const auto size = m_property ? m_property->GetSize() : 0;
            const auto minimum_alignment = m_property ? m_property->GetMinAlignment() : 0;
            const auto alignment = minimum_alignment > 0
                ? static_cast<std::uint32_t>(minimum_alignment)
                : static_cast<std::uint32_t>(alignof(std::max_align_t));
            if (!m_property || !source || size <= 0 ||
                static_cast<std::size_t>(size) > maximum_reflected_parameter_bytes ||
                !is_accessible_memory(source, static_cast<std::size_t>(size)) ||
                (alignment & (alignment - 1U)) != 0 || alignment > 4096U)
            {
                throw std::runtime_error("could not copy a reflected inventory item");
            }

            m_data = FMemory::Malloc(static_cast<std::size_t>(size), alignment);
            if (!m_data)
            {
                throw std::runtime_error(
                    "could not allocate a reflected inventory item");
            }
            try
            {
                m_property->InitializeValue(m_data);
                m_initialized = true;
                m_property->CopyCompleteValue(m_data, source);
            }
            catch (...)
            {
                reset();
                throw;
            }
        }

        ~ReflectedValue()
        {
            reset();
        }

        ReflectedValue(const ReflectedValue&) = delete;
        ReflectedValue& operator=(const ReflectedValue&) = delete;

        ReflectedValue(ReflectedValue&& other) noexcept
            : m_property(std::exchange(other.m_property, nullptr)),
              m_data(std::exchange(other.m_data, nullptr)),
              m_initialized(std::exchange(other.m_initialized, false))
        {
        }

        ReflectedValue& operator=(ReflectedValue&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_property = std::exchange(other.m_property, nullptr);
                m_data = std::exchange(other.m_data, nullptr);
                m_initialized = std::exchange(other.m_initialized, false);
            }
            return *this;
        }

        [[nodiscard]] auto data() const -> void*
        {
            return m_data;
        }

    private:
        auto reset() -> void
        {
            if (m_data)
            {
                if (m_initialized)
                {
                    m_property->DestroyValue(m_data);
                }
                FMemory::Free(m_data);
            }
            m_property = nullptr;
            m_data = nullptr;
            m_initialized = false;
        }

        FProperty* m_property{};
        void* m_data{};
        bool m_initialized{};
    };

    [[nodiscard]] inline auto narrow_ascii(const std::string& value) -> StringType
    {
        return StringType{value.begin(), value.end()};
    }

    [[nodiscard]] inline auto squared_distance(
        const FVector& left,
        const double x,
        const double y,
        const double z) -> double
    {
        const auto delta_x = left.X() - x;
        const auto delta_y = left.Y() - y;
        const auto delta_z = left.Z() - z;
        return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;
    }

    [[nodiscard]] inline auto contains_inventory(
        const TArray<UObject*>& inventories,
        UObject* candidate) -> bool
    {
        for (const auto inventory : inventories)
        {
            if (inventory == candidate)
            {
                return true;
            }
        }
        return false;
    }
}
