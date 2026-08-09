#include "NearbyCraftingInternal.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace NearbyCrafting::Detail
{
    using namespace RC;
    using namespace RC::Unreal;

    namespace
    {
        std::atomic_bool module_pinned_for_process_safety{};
        std::atomic_bool module_pin_failure_logged{};
    }

        auto pin_current_module_for_process_safety() -> bool
        {
            if (module_pinned_for_process_safety.load(std::memory_order_acquire))
            {
                return true;
            }

            HMODULE module{};
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&__ImageBase),
                    &module))
            {
                module_pinned_for_process_safety.store(true, std::memory_order_release);
                return true;
            }

            if (!module_pin_failure_logged.exchange(true, std::memory_order_acq_rel))
            {
                Output::send<LogLevel::Error>(
                    STR("[NearbyCrafting] CRITICAL: the mod DLL could not be retained for callback and detour safety. NearbyCrafting will remain inert; restart the process.\n"));
            }
            return false;
        }

        auto is_accessible_memory(
            const void* address,
            std::size_t size,
            bool require_write,
            bool require_execute) -> bool
        {
            if (!address || size == 0)
            {
                return false;
            }

            auto cursor = reinterpret_cast<std::uintptr_t>(address);
            const auto end = cursor + size;
            if (end <= cursor)
            {
                return false;
            }

            while (cursor < end)
            {
                MEMORY_BASIC_INFORMATION region{};
                if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) !=
                    sizeof(region))
                {
                    return false;
                }
                if (region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
                {
                    return false;
                }

                const auto protection = region.Protect & 0xFFU;
                const auto writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                    protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
                const auto executable = protection == PAGE_EXECUTE ||
                    protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
                    protection == PAGE_EXECUTE_WRITECOPY;
                if ((require_write && !writable) || (require_execute && !executable))
                {
                    return false;
                }

                const auto region_begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
                const auto region_end = region_begin + region.RegionSize;
                if (region_end <= cursor)
                {
                    return false;
                }
                cursor = std::min(end, region_end);
            }
            return true;
        }


        auto is_live_uobject(UObject* object) -> bool
        {
            if (!object || !FUObjectArray::GetGUObjectArrayAddress())
            {
                return false;
            }

            const auto object_header_size = UObjectBase::UEP_TotalSize();
            if (object_header_size <= 0 || object_header_size > 1024 ||
                !is_accessible_memory(
                    object, static_cast<std::size_t>(object_header_size)))
            {
                return false;
            }

            const auto object_index = object->GetInternalIndex();
            auto* object_item = FUObjectArray::IndexToObject(object_index);
            return object_item && object_item->GetUObject() == object &&
                object_item->IsValid(false);
        }

        auto has_sane_parameter_buffer(UFunction* function) -> bool
        {
            if (!function)
            {
                return false;
            }
            const auto size = static_cast<std::size_t>(function->GetParmsSize());
            return size > 0 && size <= maximum_reflected_parameter_bytes;
        }

        auto property_fits_parameter_buffer(
            UFunction* function,
            FProperty* property,
            std::size_t expected_size) -> bool
        {
            if (!has_sane_parameter_buffer(function) || !property ||
                property->GetArrayDim() != 1)
            {
                return false;
            }

            const auto offset = property->GetOffset_Internal();
            const auto size = property->GetSize();
            const auto parameter_size = static_cast<std::size_t>(function->GetParmsSize());
            if (offset < 0 || size <= 0 ||
                (expected_size != 0 && static_cast<std::size_t>(size) != expected_size))
            {
                return false;
            }
            const auto unsigned_offset = static_cast<std::size_t>(offset);
            const auto unsigned_size = static_cast<std::size_t>(size);
            return unsigned_offset <= parameter_size &&
                unsigned_size <= parameter_size - unsigned_offset;
        }

        auto property_fits_struct(UStruct* structure, FProperty* property) -> bool
        {
            if (!structure || !property || property->GetArrayDim() != 1)
            {
                return false;
            }
            const auto structure_size = structure->GetPropertiesSize();
            const auto offset = property->GetOffset_Internal();
            const auto size = property->GetSize();
            return structure_size > 0 && offset >= 0 && size > 0 &&
                offset <= structure_size && size <= structure_size - offset;
        }

        auto has_exact_parameters(
            UFunction* function,
            std::initializer_list<FProperty*> expected) -> bool
        {
            if (!function)
            {
                return false;
            }
            for (auto* property : expected)
            {
                if (!property || !property->HasAnyPropertyFlags(CPF_Parm))
                {
                    return false;
                }
            }

            std::size_t actual_count{};
            for (auto* property : TFieldRange<FProperty>(
                     function, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!property->HasAnyPropertyFlags(CPF_Parm))
                {
                    continue;
                }
                ++actual_count;
                if (std::ranges::find(expected, property) == expected.end())
                {
                    return false;
                }
            }
            return actual_count == expected.size();
        }

        auto validate_script_array(
            const FScriptArray* array,
            std::size_t element_size,
            std::int32_t maximum_count,
            std::size_t element_alignment) -> bool
        {
            if (!array || element_size == 0 || maximum_count < 0 ||
                element_alignment == 0 ||
                (element_alignment & (element_alignment - 1)) != 0 ||
                element_alignment > 4096 ||
                (reinterpret_cast<std::uintptr_t>(array) % alignof(FScriptArray)) != 0 ||
                !is_accessible_memory(array, sizeof(FScriptArray)))
            {
                return false;
            }

            const auto count = array->NumUnchecked();
            const auto capacity = array->Max();
            if (count < 0 || capacity < count || capacity > maximum_count)
            {
                return false;
            }
            if (capacity == 0)
            {
                return count == 0 && array->GetData() == nullptr;
            }
            if (element_size > static_cast<std::size_t>(-1) /
                    static_cast<std::size_t>(capacity))
            {
                return false;
            }
            const auto* data = array->GetData();
            return data &&
                (reinterpret_cast<std::uintptr_t>(data) % element_alignment) == 0 &&
                is_accessible_memory(
                data,
                element_size * static_cast<std::size_t>(capacity));
        }


        auto wait_for_calls(std::atomic_uint32_t& active_calls) -> void
        {
            while (active_calls.load(std::memory_order_acquire) != 0)
            {
                SwitchToThread();
            }
        }


        auto validate_inventory_array(
            const void* inventories_value,
            bool require_write) -> bool
        {
            if ((reinterpret_cast<std::uintptr_t>(inventories_value) % alignof(TArray<UObject*>)) != 0 ||
                !is_accessible_memory(
                    inventories_value,
                    sizeof(TArray<UObject*>),
                    require_write))
            {
                return false;
            }

            const auto& inventories = *static_cast<const TArray<UObject*>*>(inventories_value);
            const auto count = inventories.Num();
            const auto capacity = inventories.Max();
            if (count < 0 || capacity < count || capacity > maximum_reasonable_inventory_capacity)
            {
                return false;
            }
            if (capacity == 0)
            {
                return count == 0 && inventories.GetData() == nullptr;
            }

            const auto* data = inventories.GetData();
            return data &&
                (reinterpret_cast<std::uintptr_t>(data) % alignof(UObject*)) == 0 &&
                is_accessible_memory(
                    data,
                    static_cast<std::size_t>(capacity) * sizeof(UObject*),
                    require_write);
        }

        auto validate_inventory_elements(
            const TArray<UObject*>& inventories,
            UClass* inventory_class) -> bool
        {
            if (!inventory_class)
            {
                return false;
            }
            return std::ranges::all_of(inventories, [inventory_class](UObject* inventory) {
                return !inventory ||
                    (is_live_uobject(inventory) && inventory->IsA(inventory_class));
            });
        }

}
