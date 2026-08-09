#include "NearbyCraftingInternal.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/ZydisDisassembler.hpp>

namespace NearbyCrafting
{
    using namespace RC;
    using namespace RC::Unreal;
    using namespace Detail;

    namespace
    {
        struct HookSpec
        {
            const wchar_t* function_path{};
            const wchar_t* inventories_property_name{};
        };

        constexpr HookSpec hook_specs[]{
            {L"/Script/Icarus.ProcessingComponent:CanSatisfyRecipeInput", L"AdditionalInventories"},
            {L"/Script/Icarus.ProcessingComponent:CanSatisfyRecipeQueryInput", L"AdditionalInventories"},
            {L"/Script/Icarus.ProcessingComponent:CanQueueItem", L"AdditionalInventories"},
            {L"/Script/Icarus.ProcessingComponent:GetMaxCraftableStack", L"AdditionalInventories"},
            {L"/Script/Icarus.ProcessingComponent:HasSufficientResource", L"AdditionalInventories"},
            {L"/Script/Icarus.ProcessingComponent:GetResourceRecipeValidity", L"AdditionalInventories"},
        };

        static_assert(std::size(hook_specs) == generic_crafting_hook_count);

        SRWLOCK server_queue_callback_lock = SRWLOCK_INIT;
        SRWLOCK repair_callback_lock = SRWLOCK_INIT;

        class SharedSrwLockGuard
        {
        public:
            explicit SharedSrwLockGuard(SRWLOCK& lock) noexcept
                : m_lock(lock)
            {
                AcquireSRWLockShared(&m_lock);
            }

            ~SharedSrwLockGuard()
            {
                ReleaseSRWLockShared(&m_lock);
            }

            SharedSrwLockGuard(const SharedSrwLockGuard&) = delete;
            SharedSrwLockGuard& operator=(const SharedSrwLockGuard&) = delete;

        private:
            SRWLOCK& m_lock;
        };

        class ExclusiveSrwLockGuard
        {
        public:
            explicit ExclusiveSrwLockGuard(SRWLOCK& lock) noexcept
                : m_lock(lock)
            {
                AcquireSRWLockExclusive(&m_lock);
            }

            ~ExclusiveSrwLockGuard()
            {
                ReleaseSRWLockExclusive(&m_lock);
            }

            ExclusiveSrwLockGuard(const ExclusiveSrwLockGuard&) = delete;
            ExclusiveSrwLockGuard& operator=(const ExclusiveSrwLockGuard&) = delete;

        private:
            SRWLOCK& m_lock;
        };

        class AuditedX64Detour final : public PLH::x64Detour
        {
        public:
            using PLH::x64Detour::x64Detour;

            [[nodiscard]] auto patched_address() const noexcept -> std::uint64_t
            {
                return m_fnAddress;
            }

            [[nodiscard]] auto patched_size() const noexcept -> std::size_t
            {
                return static_cast<std::size_t>(m_hookSize);
            }
        };

        struct PeImage
        {
            std::uintptr_t base{};
            std::uintptr_t end{};
            const IMAGE_NT_HEADERS64* headers{};
        };

        struct AddressRange
        {
            std::uintptr_t begin{};
            std::uintptr_t end{};
        };

        struct DecodedInstruction
        {
            std::uintptr_t address{};
            ZydisDecodedInstruction instruction{};
            std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
        };

        struct DecodedWrapper
        {
            PeImage image{};
            std::uintptr_t exec_address{};
            std::uintptr_t size{};
            std::vector<DecodedInstruction> instructions{};
        };


        class NativeCallLease
        {
        public:
            explicit NativeCallLease(std::atomic_uint32_t& active_calls)
                : m_active_calls(active_calls)
            {
                m_active_calls.fetch_add(1, std::memory_order_acq_rel);
            }

            ~NativeCallLease()
            {
                m_active_calls.fetch_sub(1, std::memory_order_acq_rel);
            }

            NativeCallLease(const NativeCallLease&) = delete;
            NativeCallLease& operator=(const NativeCallLease&) = delete;

        private:
            std::atomic_uint32_t& m_active_calls;
        };


        auto get_main_pe_image() -> std::optional<PeImage>
        {
            const auto module = GetModuleHandleW(nullptr);
            if (!module || !is_accessible_memory(
                    module, sizeof(IMAGE_DOS_HEADER), false, false))
            {
                return std::nullopt;
            }

            const auto module_base = reinterpret_cast<std::uintptr_t>(module);
            const auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
            if (dos_header->e_magic != IMAGE_DOS_SIGNATURE || dos_header->e_lfanew <= 0 ||
                dos_header->e_lfanew > 0x100000)
            {
                return std::nullopt;
            }

            const auto nt_address =
                module_base + static_cast<std::uintptr_t>(dos_header->e_lfanew);
            if (nt_address <= module_base || !is_accessible_memory(
                    reinterpret_cast<const void*>(nt_address),
                    sizeof(IMAGE_NT_HEADERS64),
                    false,
                    false))
            {
                return std::nullopt;
            }
            const auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt_address);
            if (nt_headers->Signature != IMAGE_NT_SIGNATURE ||
                nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                nt_headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
                nt_headers->FileHeader.SizeOfOptionalHeader !=
                    sizeof(IMAGE_OPTIONAL_HEADER64) ||
                nt_headers->FileHeader.NumberOfSections == 0 ||
                nt_headers->FileHeader.NumberOfSections > 96 ||
                nt_headers->OptionalHeader.SizeOfImage == 0 ||
                nt_headers->OptionalHeader.SizeOfHeaders < sizeof(IMAGE_DOS_HEADER))
            {
                return std::nullopt;
            }

            const auto module_end = module_base + nt_headers->OptionalHeader.SizeOfImage;
            const auto headers_end = module_base + nt_headers->OptionalHeader.SizeOfHeaders;
            if (module_end <= module_base || headers_end <= module_base ||
                headers_end > module_end)
            {
                return std::nullopt;
            }

            const auto* first_section = IMAGE_FIRST_SECTION(nt_headers);
            const auto section_table_size =
                static_cast<std::size_t>(nt_headers->FileHeader.NumberOfSections) *
                sizeof(IMAGE_SECTION_HEADER);
            const auto section_table_address =
                reinterpret_cast<std::uintptr_t>(first_section);
            const auto section_table_end = section_table_address + section_table_size;
            if (section_table_address < nt_address ||
                section_table_end <= section_table_address ||
                section_table_end > headers_end ||
                !is_accessible_memory(
                    first_section, section_table_size, false, false))
            {
                return std::nullopt;
            }
            return PeImage{module_base, module_end, nt_headers};
        }

        auto find_executable_section(const PeImage& image, std::uintptr_t address)
            -> std::optional<AddressRange>
        {
            const auto* section = IMAGE_FIRST_SECTION(image.headers);
            for (std::uint16_t index = 0; index < image.headers->FileHeader.NumberOfSections;
                 ++index, ++section)
            {
                if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                {
                    continue;
                }

                const auto section_size = std::max<std::uint32_t>(
                    section->Misc.VirtualSize, section->SizeOfRawData);
                const auto section_begin = image.base + section->VirtualAddress;
                const auto section_end = section_begin + section_size;
                if (section_end <= section_begin || section_begin < image.base ||
                    section_end > image.end)
                {
                    continue;
                }
                if (address >= section_begin && address < section_end)
                {
                    return AddressRange{section_begin, section_end};
                }
            }
            return std::nullopt;
        }

        auto find_runtime_function(const PeImage& image, std::uintptr_t address)
            -> std::optional<AddressRange>
        {
            DWORD64 runtime_image_base{};
            const auto* runtime_function = RtlLookupFunctionEntry(
                static_cast<DWORD64>(address), &runtime_image_base, nullptr);
            if (!runtime_function || runtime_image_base != image.base)
            {
                return std::nullopt;
            }

            const auto begin = image.base + runtime_function->BeginAddress;
            const auto end = image.base + runtime_function->EndAddress;
            if (begin < image.base || end <= begin || end > image.end ||
                address < begin || address >= end)
            {
                return std::nullopt;
            }
            return AddressRange{begin, end};
        }


        auto decode_generated_wrapper(UFunction* reflected_function)
            -> std::optional<DecodedWrapper>
        {
            if (!reflected_function)
            {
                return std::nullopt;
            }

            const auto image = get_main_pe_image();
            if (!image)
            {
                return std::nullopt;
            }

            const auto exec_address = std::bit_cast<std::uintptr_t>(
                reflected_function->GetFuncPtr());
            const auto wrapper_section = find_executable_section(*image, exec_address);
            const auto wrapper_function = find_runtime_function(*image, exec_address);
            if (!wrapper_section || !wrapper_function ||
                wrapper_function->begin != exec_address ||
                wrapper_function->begin < wrapper_section->begin ||
                wrapper_function->end > wrapper_section->end)
            {
                return std::nullopt;
            }

            // Decode the complete PE unwind-function range linearly. PolyHook's
            // detour-oriented disassembler stops at unconditional jumps, while
            // generated exec wrappers can place their native call in a later block.
            const auto wrapper_size = wrapper_function->end - wrapper_function->begin;
            if (wrapper_size == 0 || wrapper_size > 0x10000 ||
                !is_accessible_memory(
                    reinterpret_cast<const void*>(wrapper_function->begin),
                    static_cast<std::size_t>(wrapper_size),
                    false,
                    true))
            {
                return std::nullopt;
            }

            ZydisDecoder decoder{};
            if (ZYAN_FAILED(ZydisDecoderInit(
                    &decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
            {
                return std::nullopt;
            }

            DecodedWrapper wrapper{};
            wrapper.image = *image;
            wrapper.exec_address = exec_address;
            wrapper.size = wrapper_size;
            wrapper.instructions.reserve(static_cast<std::size_t>(wrapper_size / 4));

            const auto* wrapper_bytes = reinterpret_cast<const std::uint8_t*>(
                wrapper_function->begin);
            std::size_t offset{};
            while (offset < wrapper_size)
            {
                DecodedInstruction decoded{};
                decoded.address = wrapper_function->begin + offset;
                if (ZYAN_FAILED(ZydisDecoderDecodeFull(
                        &decoder,
                        wrapper_bytes + offset,
                        static_cast<ZyanUSize>(wrapper_size - offset),
                        &decoded.instruction,
                        decoded.operands.data())) ||
                    decoded.instruction.length == 0 ||
                    decoded.instruction.length > wrapper_size - offset)
                {
                    return std::nullopt;
                }
                offset += decoded.instruction.length;
                wrapper.instructions.emplace_back(std::move(decoded));
            }
            return wrapper;
        }

        auto stack_lea_displacement(
            const DecodedInstruction& decoded,
            ZydisRegister destination,
            ZydisRegister base) -> std::optional<std::int64_t>
        {
            if (decoded.instruction.mnemonic != ZYDIS_MNEMONIC_LEA ||
                decoded.instruction.operand_count_visible != 2 ||
                decoded.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                decoded.operands[0].reg.value != destination ||
                decoded.operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY ||
                decoded.operands[1].mem.base != base ||
                decoded.operands[1].mem.index != ZYDIS_REGISTER_NONE ||
                !decoded.operands[1].mem.disp.has_displacement)
            {
                return std::nullopt;
            }
            return decoded.operands[1].mem.disp.value;
        }

        auto is_register_move(
            const DecodedInstruction& decoded,
            ZydisRegister destination,
            ZydisRegister source) -> bool
        {
            return decoded.instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
                decoded.instruction.operand_count_visible == 2 &&
                decoded.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                decoded.operands[0].reg.value == destination &&
                decoded.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                decoded.operands[1].reg.value == source;
        }

        auto is_stack_register_store(
            const DecodedInstruction& decoded,
            std::int64_t displacement,
            ZydisRegister source) -> bool
        {
            return decoded.instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
                decoded.instruction.operand_count_visible == 2 &&
                decoded.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                decoded.operands[0].mem.base == ZYDIS_REGISTER_RSP &&
                decoded.operands[0].mem.index == ZYDIS_REGISTER_NONE &&
                decoded.operands[0].mem.disp.has_displacement &&
                decoded.operands[0].mem.disp.value == displacement &&
                decoded.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                decoded.operands[1].reg.value == source;
        }

        auto resolve_direct_call_target(
            const DecodedWrapper& wrapper,
            const DecodedInstruction& call_instruction) -> std::uintptr_t
        {
            const auto* call_bytes = reinterpret_cast<const std::uint8_t*>(
                call_instruction.address);
            if (call_instruction.instruction.mnemonic != ZYDIS_MNEMONIC_CALL ||
                call_instruction.instruction.operand_count_visible != 1 ||
                call_instruction.operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                call_instruction.instruction.length != 5 || call_bytes[0] != 0xE8)
            {
                return 0;
            }

            ZyanU64 absolute_target{};
            if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(
                    &call_instruction.instruction,
                    &call_instruction.operands[0],
                    call_instruction.address,
                    &absolute_target)))
            {
                return 0;
            }

            const auto target = static_cast<std::uintptr_t>(absolute_target);
            const auto target_function = find_runtime_function(wrapper.image, target);
            if (!find_executable_section(wrapper.image, target) || !target_function ||
                target_function->begin != target ||
                !is_accessible_memory(reinterpret_cast<const void*>(target), 1, false, true))
            {
                return 0;
            }
            return target;
        }

        auto is_detour_safe_native_target(
            const PeImage& image,
            std::uintptr_t target) -> bool
        {
            const auto target_section = find_executable_section(image, target);
            if (!target_section)
            {
                return false;
            }

            // Refuse functions whose entry point already has the common shape of a
            // detour or compiler thunk. PolyHook must see an untouched, directly
            // decodable function entry.
            PLH::MemAccessor memory_accessor;
            PLH::ZydisDisassembler disassembler(PLH::Mode::x64);
            const auto target_scan_end = std::min(
                target_section->end,
                target + static_cast<std::uintptr_t>(32));
            const auto target_instructions = disassembler.disassemble(
                target, target, target_scan_end, memory_accessor);
            auto first_semantic_instruction = std::size_t{};
            while (first_semantic_instruction < target_instructions.size() &&
                   (target_instructions[first_semantic_instruction].getMnemonic() == "nop" ||
                    target_instructions[first_semantic_instruction].getMnemonic() == "endbr64"))
            {
                ++first_semantic_instruction;
            }
            if (first_semantic_instruction >= target_instructions.size())
            {
                return false;
            }

            const auto& target_entry = target_instructions[first_semantic_instruction];
            if (target_entry.isBranching() || target_entry.isCalling() ||
                target_entry.getBytes().empty() || target_entry.getBytes().front() == 0xCC)
            {
                return false;
            }
            if (first_semantic_instruction + 1 < target_instructions.size())
            {
                const auto& next = target_instructions[first_semantic_instruction + 1];
                const auto& entry_mnemonic = target_entry.getMnemonic();
                const auto& next_mnemonic = next.getMnemonic();
                if ((next_mnemonic == "jmp" &&
                     (entry_mnemonic == "mov" || entry_mnemonic == "lea")) ||
                    (next_mnemonic == "ret" && entry_mnemonic == "push"))
                {
                    return false;
                }
            }
            return true;
        }


        auto resolve_native_repair_material_lookup(UFunction* reflected_function) -> std::uintptr_t
        {
            auto* inventories_property = reflected_function
                ? reflected_function->FindProperty(FName(L"Inventories"))
                : nullptr;
            auto* return_property = reflected_function
                ? reflected_function->GetReturnProperty()
                : nullptr;
            if (!inventories_property || !return_property ||
                inventories_property->GetOffset_Internal() < 0 ||
                return_property->GetOffset_Internal() <=
                    inventories_property->GetOffset_Internal())
            {
                return 0;
            }
            const auto expected_array_displacement_delta =
                static_cast<std::int64_t>(return_property->GetOffset_Internal()) -
                static_cast<std::int64_t>(inventories_property->GetOffset_Internal());

            const auto wrapper = decode_generated_wrapper(reflected_function);
            if (!wrapper)
            {
                return 0;
            }

            // The generated exec wrapper copies the by-value inventory array, then
            // loads it into R8 immediately before calling the native implementation.
            std::uintptr_t resolved_target{};
            std::size_t matching_call_sites{};
            for (std::size_t index = 0; index + 3 < wrapper->instructions.size(); ++index)
            {
                const auto& inventories_instruction = wrapper->instructions[index];
                const auto& item_instruction = wrapper->instructions[index + 1];
                const auto& return_instruction = wrapper->instructions[index + 2];
                const auto& call_instruction = wrapper->instructions[index + 3];
                const auto inventories_displacement = stack_lea_displacement(
                    inventories_instruction, ZYDIS_REGISTER_R8, ZYDIS_REGISTER_RSP);
                const auto return_displacement = stack_lea_displacement(
                    return_instruction, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RSP);
                if (!inventories_displacement || !return_displacement ||
                    *return_displacement - *inventories_displacement !=
                        expected_array_displacement_delta ||
                    !is_register_move(
                        item_instruction, ZYDIS_REGISTER_RDX, ZYDIS_REGISTER_RDI))
                {
                    continue;
                }

                const auto target = resolve_direct_call_target(*wrapper, call_instruction);
                if (!target)
                {
                    continue;
                }

                // A wrapper can contain the same native call in multiple control-flow
                // branches. Multiple sites are safe only when every site resolves to
                // the same exact function entry.
                if (resolved_target && resolved_target != target)
                {
                    return 0;
                }
                resolved_target = target;
                ++matching_call_sites;
            }

            if (!resolved_target || matching_call_sites != 1 ||
                !is_detour_safe_native_target(wrapper->image, resolved_target))
            {
                return 0;
            }

#if defined(NEARBYCRAFTING_DEBUG)
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] Resolved repair wrapper RVA=0x{:X} (size=0x{:X}, instructions={}, matching-call-sites={}) to native target RVA=0x{:X}.\n"),
                wrapper->exec_address - wrapper->image.base,
                wrapper->size,
                wrapper->instructions.size(),
                matching_call_sites,
                resolved_target - wrapper->image.base);
#endif
            return resolved_target;
        }

        auto resolve_native_server_queue(UFunction* reflected_function) -> std::uintptr_t
        {
            auto* recipe_property = reflected_function
                ? reflected_function->FindProperty(FName(L"Recipe"))
                : nullptr;
            auto* inventories_property = reflected_function
                ? reflected_function->FindProperty(FName(L"AdditionalInventories"))
                : nullptr;
            if (!recipe_property || !inventories_property ||
                recipe_property->GetSize() <= 0 || inventories_property->GetSize() <= 0)
            {
                return 0;
            }
            const auto expected_local_displacement_delta =
                static_cast<std::int64_t>(recipe_property->GetSize()) +
                static_cast<std::int64_t>(inventories_property->GetSize());

            const auto wrapper = decode_generated_wrapper(reflected_function);
            if (!wrapper)
            {
                return 0;
            }

            // Verified in both current Icarus client and dedicated-server builds.
            // The generated wrapper stages the native implementation call as:
            //   mov [rsp+20h], Player
            //   lea r9,  [rbp + AdditionalInventories]
            //   mov r8d, Count
            //   lea rdx, [rbp + Recipe]
            //   mov rcx, ProcessingComponent
            //   call OnServer_AddProcessingRecipe_Implementation
            // Match the complete register contract and require one unique target.
            std::uintptr_t resolved_target{};
            std::size_t matching_call_sites{};
            for (std::size_t index = 0; index + 5 < wrapper->instructions.size(); ++index)
            {
                const auto inventories_displacement = stack_lea_displacement(
                    wrapper->instructions[index + 1], ZYDIS_REGISTER_R9, ZYDIS_REGISTER_RBP);
                const auto recipe_displacement = stack_lea_displacement(
                    wrapper->instructions[index + 3], ZYDIS_REGISTER_RDX, ZYDIS_REGISTER_RBP);
                if (!inventories_displacement || !recipe_displacement ||
                    *recipe_displacement - *inventories_displacement !=
                        expected_local_displacement_delta ||
                    !is_stack_register_store(
                        wrapper->instructions[index], 0x20, ZYDIS_REGISTER_RBX) ||
                    !is_register_move(
                        wrapper->instructions[index + 2],
                        ZYDIS_REGISTER_R8D,
                        ZYDIS_REGISTER_EDI) ||
                    !is_register_move(
                        wrapper->instructions[index + 4],
                        ZYDIS_REGISTER_RCX,
                        ZYDIS_REGISTER_RSI))
                {
                    continue;
                }

                const auto target = resolve_direct_call_target(
                    *wrapper, wrapper->instructions[index + 5]);
                if (!target)
                {
                    continue;
                }
                if (resolved_target && resolved_target != target)
                {
                    return 0;
                }
                resolved_target = target;
                ++matching_call_sites;
            }

            if (!resolved_target || matching_call_sites != 1 ||
                !is_detour_safe_native_target(wrapper->image, resolved_target))
            {
                return 0;
            }

#if defined(NEARBYCRAFTING_DEBUG)
            Output::send<LogLevel::Normal>(
                STR("[NearbyCrafting][DEBUG] Resolved server queue wrapper RVA=0x{:X} (size=0x{:X}, instructions={}, matching-call-sites={}) to native target RVA=0x{:X}.\n"),
                wrapper->exec_address - wrapper->image.base,
                wrapper->size,
                wrapper->instructions.size(),
                matching_call_sites,
                resolved_target - wrapper->image.base);
#endif
            return resolved_target;
        }

    }

    std::atomic<NearbyCraftingMod*> NearbyCraftingMod::s_server_queue_hook_owner{};
    std::atomic_uint64_t NearbyCraftingMod::s_server_queue_trampoline{};
    std::atomic_uint32_t NearbyCraftingMod::s_server_queue_active_calls{};
    std::atomic<NearbyCraftingMod*> NearbyCraftingMod::s_repair_hook_owner{};
    std::atomic_uint64_t NearbyCraftingMod::s_repair_trampoline{};
    std::atomic_uint32_t NearbyCraftingMod::s_repair_active_calls{};

    auto NearbyCraftingMod::pre_hook(UnrealScriptFunctionCallableContext& context, void* custom_data) -> void
    {
        const auto binding = static_cast<HookBinding*>(custom_data);
        if (!binding)
        {
            return;
        }

        CallbackLease lease{binding->lifetime};
        auto* owner = lease.get();
        if (!owner || !IsInGameThreadRaw() ||
            !owner->m_initialized.load(std::memory_order_acquire) ||
            !binding->inventories_property)
        {
            return;
        }
        owner->inject_nearby_inventories(
            context,
            binding->function,
            binding->inventories_property,
            binding->function_path);
    }

    auto NearbyCraftingMod::server_queue_hook(
        UObject* processing_component,
        const void* recipe,
        std::int32_t count,
        const TArray<UObject*>* additional_inventories,
        UObject* player) -> void
    {
        NativeCallLease active_call{s_server_queue_active_calls};
        SharedSrwLockGuard callback_guard{server_queue_callback_lock};
        auto* owner = s_server_queue_hook_owner.load(std::memory_order_acquire);
        using NativeServerQueue = void (*)(
            UObject*,
            const void*,
            std::int32_t,
            const TArray<UObject*>*,
            UObject*);
        const auto original = std::bit_cast<NativeServerQueue>(
            s_server_queue_trampoline.load(std::memory_order_acquire));
        if (!original ||
            !is_accessible_memory(reinterpret_cast<const void*>(original), 1, false, true))
        {
            if (IsInGameThreadRaw() && owner && !owner->m_hook_error_logged)
            {
                owner->m_hook_error_logged = true;
                Output::send<LogLevel::Error>(
                    STR("[NearbyCrafting] Native server queue trampoline is unavailable; the queue request could not be forwarded safely.\n"));
            }
            return;
        }

        std::optional<TArray<UObject*>> owned_inventories{};
        if (IsInGameThreadRaw() && owner &&
            owner->m_initialized.load(std::memory_order_acquire) &&
            recipe && additional_inventories &&
            is_live_uobject(processing_component) && is_live_uobject(player) &&
            owner->m_processing_component_class &&
            processing_component->IsA(owner->m_processing_component_class) &&
            owner->m_icarus_player_character_class &&
            player->IsA(owner->m_icarus_player_character_class))
        {
            try
            {
                if (!validate_inventory_array(additional_inventories, false) ||
                    !validate_inventory_elements(
                        *additional_inventories, owner->m_inventory_class))
                {
                    if (!owner->m_hook_error_logged)
                    {
                        owner->m_hook_error_logged = true;
                        Output::send<LogLevel::Error>(
                            STR("[NearbyCrafting] Native server queue hook received an invalid inventory array; nearby inventory injection was skipped.\n"));
                    }
                }
                else
                {
                    // The reflected RPC parameter is const-by-reference and may alias
                    // storage owned by UE4SS's temporary frame. Never resize it. The
                    // native implementation receives this independently owned copy and
                    // makes its own queue copy before this local array is destroyed.
                    owned_inventories.emplace(*additional_inventories);
                    owner->append_nearby_inventories(
                        processing_component,
                        *owned_inventories,
                        server_queue_hook_path);
#if defined(NEARBYCRAFTING_DEBUG)
                    Output::send<LogLevel::Normal>(
                        STR("[NearbyCrafting][DEBUG] Native server queue prepared an owned inventory array (incoming={}, augmented={}).\n"),
                        additional_inventories->Num(),
                        owned_inventories->Num());
#endif
                }
            }
            catch (const std::exception& exception)
            {
                owned_inventories.reset();
                if (!owner->m_hook_error_logged)
                {
                    owner->m_hook_error_logged = true;
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Native server queue preparation failed safely and was skipped: {}\n"),
                        narrow_ascii(exception.what()));
                }
            }
            catch (...)
            {
                owned_inventories.reset();
                if (!owner->m_hook_error_logged)
                {
                    owner->m_hook_error_logged = true;
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Native server queue preparation failed safely with an unknown error and was skipped.\n"));
                }
            }
        }

        const auto* inventories_for_call = owned_inventories
            ? &*owned_inventories
            : additional_inventories;
        original(processing_component, recipe, count, inventories_for_call, player);
    }

    auto NearbyCraftingMod::repair_material_lookup_hook(
        void* return_items,
        const void* item_data,
        void* inventories_value) -> void*
    {
        NativeCallLease active_call{s_repair_active_calls};
        SharedSrwLockGuard callback_guard{repair_callback_lock};
        auto* owner = s_repair_hook_owner.load(std::memory_order_acquire);
        using NativeRepairMaterialLookup = void* (*)(void*, const void*, void*);
        const auto original = std::bit_cast<NativeRepairMaterialLookup>(
            s_repair_trampoline.load(std::memory_order_acquire));
        const auto original_is_valid = original &&
            is_accessible_memory(reinterpret_cast<const void*>(original), 1, false, true);
        if (!original_is_valid)
        {
            if (return_items && is_accessible_memory(
                    return_items, sizeof(FScriptArray), true))
            {
                std::memset(return_items, 0, sizeof(FScriptArray));
            }
            if (IsInGameThreadRaw() && owner && !owner->m_hook_error_logged)
            {
                owner->m_hook_error_logged = true;
                Output::send<LogLevel::Error>(
                    STR("[NearbyCrafting] Native repair trampoline is unavailable; an empty repair-material result was returned safely.\n"));
            }
            return return_items;
        }

        if (IsInGameThreadRaw() && owner &&
            owner->m_initialized.load(std::memory_order_acquire) &&
            return_items && item_data && inventories_value)
        {
            try
            {
                if (!validate_inventory_array(inventories_value, true) ||
                    !validate_inventory_elements(
                        *static_cast<TArray<UObject*>*>(inventories_value),
                        owner->m_inventory_class))
                {
                    if (!owner->m_hook_error_logged)
                    {
                        owner->m_hook_error_logged = true;
                        Output::send<LogLevel::Error>(
                            STR("[NearbyCrafting] Native repair hook received an invalid inventory array; nearby inventory injection was skipped.\n"));
                    }
                }
                else
                {
                    auto& inventories = *static_cast<TArray<UObject*>*>(inventories_value);
                    UObject* player_inventory{};
                    for (auto* inventory : inventories)
                    {
                        if (!is_live_uobject(inventory) || !owner->m_inventory_class ||
                            !inventory->IsA(owner->m_inventory_class))
                        {
                            continue;
                        }
                        auto* actor = owner->get_component_owner(inventory);
                        if (actor && owner->is_player_crafting_actor(actor))
                        {
                            player_inventory = inventory;
                            break;
                        }
                    }

                    if (player_inventory)
                    {
                        owner->append_nearby_inventories(
                            player_inventory,
                            inventories,
                            repair_material_lookup_path);
                    }
                }
            }
            catch (const std::exception& exception)
            {
                if (!owner->m_hook_error_logged)
                {
                    owner->m_hook_error_logged = true;
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Native repair hook failed safely and was skipped: {}\n"),
                        narrow_ascii(exception.what()));
                }
            }
            catch (...)
            {
                if (!owner->m_hook_error_logged)
                {
                    owner->m_hook_error_logged = true;
                    Output::send<LogLevel::Error>(
                        STR("[NearbyCrafting] Native repair hook failed safely with an unknown error and was skipped.\n"));
                }
            }
        }

        return original(return_items, item_data, inventories_value);
    }

    auto NearbyCraftingMod::install_hooks() -> bool
    {
        for (const auto& spec : hook_specs)
        {
            if (!install_hook(
                    spec.function_path,
                    spec.inventories_property_name))
            {
                return false;
            }
        }

        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Installed all {} guarded crafting hooks.\n"),
            std::size(hook_specs));
        return true;
    }

    auto NearbyCraftingMod::install_hook(
        const wchar_t* function_path,
        const wchar_t* inventories_property_name) -> bool
    {
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, function_path);
        auto* function_owner = function ? function->GetOuterPrivate() : nullptr;
        if (!function || !function->HasAnyFunctionFlags(FUNC_Native) ||
            function->HasAnyFunctionFlags(FUNC_Static) ||
            !has_sane_parameter_buffer(function) || !function_owner ||
            !function_owner->IsA(UClass::StaticClass()) ||
            static_cast<UClass*>(function_owner) != m_processing_component_class)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Function unavailable or has an incompatible owner/parameter buffer: {}\n"),
                function_path);
            return false;
        }

        auto* property = function->FindProperty(FName(inventories_property_name));
        if (!property || !property->IsA<FArrayProperty>() ||
            !property->HasAnyPropertyFlags(CPF_Parm) ||
            property->HasAnyPropertyFlags(
                CPF_ConstParm | CPF_OutParm | CPF_ReferenceParm | CPF_ReturnParm) ||
            !property_fits_parameter_buffer(function, property, sizeof(TArray<UObject*>)))
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] {} has no owned mutable {} input array; reflected hook skipped.\n"),
                function_path,
                inventories_property_name);
            return false;
        }

        auto* array_property = static_cast<FArrayProperty*>(property);
        auto* inner_property = array_property->GetInner();
        if (!inner_property || !inner_property->IsA<FObjectProperty>() ||
            inner_property->GetElementSize() != sizeof(UObject*))
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] {} has an {} array with an incompatible element type; hook skipped.\n"),
                function_path,
                inventories_property_name);
            return false;
        }

        auto* object_property = static_cast<FObjectPropertyBase*>(inner_property);
        auto* property_class = object_property->GetPropertyClass().Get();
        if (!property_class || property_class != m_inventory_class)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] {} has an {} array that does not contain Inventory objects; hook skipped.\n"),
                function_path,
                inventories_property_name);
            return false;
        }

        auto binding = std::make_unique<HookBinding>();
        binding->lifetime = m_callback_lifetime;
        binding->function = function;
        binding->inventories_property = property;
        binding->function_path = function_path;
        binding->callback_id = function->RegisterPreHook(&NearbyCraftingMod::pre_hook, binding.get());

        if (binding->callback_id <= 0)
        {
            Output::send<LogLevel::Warning>(STR("[NearbyCrafting] Hook registration failed: {}\n"), function_path);
            return false;
        }

        const auto callback_id = binding->callback_id;
        try
        {
            m_hooks.emplace_back(std::move(binding));
        }
        catch (...)
        {
            if (!function->UnregisterHook(callback_id))
            {
                pin_current_module_for_process_safety();
                static_cast<void>(binding.release());
                Output::send<LogLevel::Error>(
                    STR("[NearbyCrafting] Hook container allocation failed and callback rollback also failed; an inert binding was retained for process safety: {}\n"),
                    function_path);
            }
            throw;
        }
#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Installed inventory hook {} (count={}).\n"),
            function_path,
            m_hooks.size());
#endif
        return true;
    }

    auto NearbyCraftingMod::remove_hooks() -> void
    {
#if defined(NEARBYCRAFTING_DEBUG)
        const auto hook_count = m_hooks.size();
#endif
        for (auto& binding : m_hooks)
        {
            if (binding && binding->function && binding->callback_id > 0)
            {
                if (!is_live_uobject(binding->function))
                {
                    pin_current_module_for_process_safety();
                    Output::send<LogLevel::Warning>(
                        STR("[NearbyCrafting] Warning: an inventory-hook UFunction was no longer live during cleanup; its inert binding and this DLL were retained.\n"));
                    static_cast<void>(binding.release());
                    continue;
                }
                if (!binding->function->UnregisterHook(binding->callback_id))
                {
                    pin_current_module_for_process_safety();
                    Output::send<LogLevel::Warning>(
                        STR("[NearbyCrafting] Warning: inventory hook was already absent during cleanup: {}\n"),
                        binding->function_path ? binding->function_path : L"<unknown>");
                    static_cast<void>(binding.release());
                }
            }
        }
        m_hooks.clear();
        m_bench_caches.clear();
        m_bench_cache_keys_by_actor.clear();

#if defined(NEARBYCRAFTING_DEBUG)
        Output::send<LogLevel::Normal>(
            STR("[NearbyCrafting][DEBUG] Inventory hook cleanup removed {} bindings and cleared all proximity caches.\n"),
            hook_count);
#endif
    }

    auto NearbyCraftingMod::install_server_queue_hook() -> bool
    {
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, server_queue_hook_path);
        auto* function_owner = function ? function->GetOuterPrivate() : nullptr;
        if (!function ||
            !function->HasAllFunctionFlags(FUNC_Native | FUNC_Net | FUNC_NetServer) ||
            function->HasAnyFunctionFlags(FUNC_Static) ||
            !has_sane_parameter_buffer(function) || !function_owner ||
            !function_owner->IsA(UClass::StaticClass()) ||
            static_cast<UClass*>(function_owner) != m_processing_component_class)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Native server queue function is unavailable or has an incompatible RPC contract.\n"));
            return false;
        }

        auto* recipe_property = function->FindProperty(FName(L"Recipe"));
        auto* count_property = function->FindProperty(FName(L"Count"));
        auto* inventories_property = function->FindProperty(FName(L"AdditionalInventories"));
        auto* player_property = function->FindProperty(FName(L"Player"));
        auto* expected_recipe_struct = UObjectGlobals::StaticFindObject<UScriptStruct*>(
            nullptr, nullptr, processor_recipes_row_handle_struct_path);
        const auto is_input_parameter = [](FProperty* property) {
            return property && property->HasAnyPropertyFlags(CPF_Parm) &&
                !property->HasAnyPropertyFlags(
                    CPF_ConstParm | CPF_OutParm | CPF_ReferenceParm | CPF_ReturnParm);
        };
        if (!is_input_parameter(recipe_property) ||
            !recipe_property->IsA<FStructProperty>() ||
            !expected_recipe_struct ||
            static_cast<FStructProperty*>(recipe_property)->GetStruct().Get() !=
                expected_recipe_struct ||
            !is_input_parameter(count_property) ||
            !count_property->IsA<FIntProperty>() ||
            count_property->GetElementSize() != sizeof(std::int32_t) ||
            !is_input_parameter(player_property) ||
            !player_property->IsA<FObjectProperty>() ||
            player_property->GetElementSize() != sizeof(UObject*) ||
            !m_icarus_player_character_class ||
            static_cast<FObjectPropertyBase*>(player_property)->GetPropertyClass().Get() !=
                m_icarus_player_character_class ||
            !inventories_property || !inventories_property->IsA<FArrayProperty>() ||
            !inventories_property->HasAllPropertyFlags(
                CPF_Parm | CPF_ConstParm | CPF_ReferenceParm) ||
            inventories_property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm) ||
            !property_fits_parameter_buffer(function, recipe_property) ||
            !property_fits_parameter_buffer(function, count_property, sizeof(std::int32_t)) ||
            !property_fits_parameter_buffer(
                function, inventories_property, sizeof(TArray<UObject*>)) ||
            !property_fits_parameter_buffer(function, player_property, sizeof(UObject*)) ||
            !has_exact_parameters(
                function,
                {recipe_property, count_property, inventories_property, player_property}) ||
            !(recipe_property->GetOffset_Internal() < count_property->GetOffset_Internal() &&
              count_property->GetOffset_Internal() < inventories_property->GetOffset_Internal() &&
              inventories_property->GetOffset_Internal() < player_property->GetOffset_Internal()))
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Native server queue parameters do not match the verified current Icarus ABI.\n"));
            return false;
        }

        auto* array_property = static_cast<FArrayProperty*>(inventories_property);
        auto* inner_property = array_property->GetInner();
        if (!inner_property || !inner_property->IsA<FObjectProperty>() ||
            inner_property->GetElementSize() != sizeof(UObject*))
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Native server queue inventory array has an incompatible element type.\n"));
            return false;
        }
        auto* object_property = static_cast<FObjectPropertyBase*>(inner_property);
        auto* property_class = object_property->GetPropertyClass().Get();
        if (!property_class || !m_inventory_class ||
            property_class != m_inventory_class)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Native server queue array does not contain exact Inventory objects.\n"));
            return false;
        }

        const auto native_target = resolve_native_server_queue(function);
        if (!native_target)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Native server queue wrapper did not match the current client/server ABI.\n"));
            return false;
        }

        ExclusiveSrwLockGuard callback_guard{server_queue_callback_lock};
        if (s_server_queue_hook_owner.load(std::memory_order_acquire) ||
            s_server_queue_trampoline.load(std::memory_order_acquire) != 0 ||
            m_server_queue_detour)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] A native server queue hook is already active or retained for safe forwarding.\n"));
            return false;
        }

        std::array<std::uint8_t, 16> original_entry_bytes{};
        if (!is_accessible_memory(
                reinterpret_cast<const void*>(native_target),
                original_entry_bytes.size(),
                false,
                true))
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Native server queue entry is not fully readable/executable.\n"));
            return false;
        }
        std::memcpy(
            original_entry_bytes.data(),
            reinterpret_cast<const void*>(native_target),
            original_entry_bytes.size());
        m_server_queue_native_target = native_target;
        m_server_queue_hooked_entry.fill(0);
        m_server_queue_hooked_entry_size = 0;
        m_server_queue_trampoline = 0;
        auto audited_detour = std::make_unique<AuditedX64Detour>(
            native_target,
            std::bit_cast<std::uint64_t>(&NearbyCraftingMod::server_queue_hook),
            &m_server_queue_trampoline);
        auto* const detour_audit = audited_detour.get();
        m_server_queue_detour = std::move(audited_detour);
        // Keep the required queue hook on PolyHook's conservative near-allocation
        // scheme; never fall back to in-place or code-cave patching.
        m_server_queue_detour->setDetourScheme(PLH::x64Detour::VALLOC2);
        const auto hook_succeeded = m_server_queue_detour->hook();
        if (!hook_succeeded)
        {
            m_server_queue_detour.reset();
            m_server_queue_trampoline = 0;
            m_server_queue_native_target = 0;
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Native server queue hook registration failed.\n"));
            return false;
        }

        const auto patched_size = detour_audit->patched_size();
        if (!m_server_queue_detour->isHooked() ||
            detour_audit->patched_address() != native_target ||
            patched_size == 0 || patched_size > m_server_queue_hooked_entry.size() ||
            !is_accessible_memory(
                reinterpret_cast<const void*>(m_server_queue_trampoline),
                1,
                false,
                true) ||
            !is_accessible_memory(
                reinterpret_cast<const void*>(native_target),
                patched_size,
                false,
                true) ||
            std::memcmp(
                original_entry_bytes.data(),
                reinterpret_cast<const void*>(native_target),
                original_entry_bytes.size()) == 0)
        {
            // hook() has published executable code. A caller may already be paused
            // in its entry stub before our callback counter is visible, so never
            // reclaim the detour in-process even when post-install auditing fails.
            s_server_queue_trampoline.store(
                m_server_queue_trampoline, std::memory_order_release);
            static_cast<void>(m_server_queue_detour.release());
            m_server_queue_trampoline = 0;
            m_server_queue_native_target = 0;
            Output::send<LogLevel::Error>(
                STR("[NearbyCrafting] Native server queue verification failed after patching; an inert forwarding detour was retained and a process restart is required.\n"));
            return false;
        }

        std::memcpy(
            m_server_queue_hooked_entry.data(),
            reinterpret_cast<const void*>(native_target),
            patched_size);
        m_server_queue_hooked_entry_size = patched_size;
        s_server_queue_trampoline.store(
            m_server_queue_trampoline, std::memory_order_release);
        s_server_queue_hook_owner.store(this, std::memory_order_release);

        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Installed native server queue hook with owned inventory marshalling (patched-span={} bytes).\n"),
            patched_size);
        return true;
    }

    auto NearbyCraftingMod::remove_server_queue_hook() -> void
    {
        auto* expected_owner = this;
        s_server_queue_hook_owner.compare_exchange_strong(
            expected_owner, nullptr, std::memory_order_acq_rel);
        if (!m_server_queue_detour)
        {
            // A non-zero static trampoline without member ownership belongs to an
            // inert process-lifetime detour retained by an earlier cleanup path.
            return;
        }
        {
            ExclusiveSrwLockGuard callback_guard{server_queue_callback_lock};
            if (m_server_queue_detour->isHooked() &&
                m_server_queue_hooked_entry_size != 0 &&
                (!m_server_queue_native_target ||
                 !is_accessible_memory(
                     reinterpret_cast<const void*>(m_server_queue_native_target),
                     m_server_queue_hooked_entry_size,
                     false,
                     true) ||
                 std::memcmp(
                     m_server_queue_hooked_entry.data(),
                     reinterpret_cast<const void*>(m_server_queue_native_target),
                     m_server_queue_hooked_entry_size) != 0))
            {
                pin_current_module_for_process_safety();
                s_server_queue_trampoline.store(
                    m_server_queue_trampoline, std::memory_order_release);
                static_cast<void>(m_server_queue_detour.release());
                m_server_queue_trampoline = 0;
                m_server_queue_native_target = 0;
                m_server_queue_hooked_entry_size = 0;
                Output::send<LogLevel::Error>(
                    STR("[NearbyCrafting] Native server queue entry was changed by another patch; the inert forwarding detour and DLL were retained instead of overwriting it during cleanup.\n"));
                return;
            }
            // Keep both PolyHook allocations alive. The exclusive gate drains any
            // callback that may still own this instance; later calls observe the
            // null owner and forward through the retained trampoline.
            s_server_queue_trampoline.store(
                m_server_queue_trampoline, std::memory_order_release);
            static_cast<void>(m_server_queue_detour.release());
        }
        m_server_queue_trampoline = 0;
        m_server_queue_native_target = 0;
        m_server_queue_hooked_entry.fill(0);
        m_server_queue_hooked_entry_size = 0;
        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Native server queue hook is inert; its forwarding detour is retained until process exit.\n"));
    }

    auto NearbyCraftingMod::install_repair_material_lookup_hook() -> bool
    {
        auto* reflected_function = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, repair_material_lookup_path);
        auto* expected_owner_class = UObjectGlobals::StaticFindObject<UClass*>(
            nullptr, nullptr, item_manipulation_component_class_path);
        auto* function_owner = reflected_function ? reflected_function->GetOuterPrivate() : nullptr;
        if (!reflected_function || !expected_owner_class ||
            !reflected_function->HasAllFunctionFlags(FUNC_Native | FUNC_Static) ||
            !has_sane_parameter_buffer(reflected_function) ||
            !function_owner || !function_owner->IsA(UClass::StaticClass()) ||
            static_cast<UClass*>(function_owner) != expected_owner_class)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Repair material lookup function is unavailable or has an incompatible owner/parameter buffer.\n"));
            return false;
        }

        auto* item_data_property = reflected_function->FindProperty(FName(L"ItemData"));
        auto* inventories_property = reflected_function->FindProperty(FName(L"Inventories"));
        auto* return_property = reflected_function->GetReturnProperty();
        auto* expected_item_struct = UObjectGlobals::StaticFindObject<UScriptStruct*>(
            nullptr, nullptr, item_data_struct_path);
        auto* expected_queue_item_struct = UObjectGlobals::StaticFindObject<UScriptStruct*>(
            nullptr, nullptr, queue_item_struct_path);
        const auto is_owned_input_parameter = [](FProperty* property) {
            return property && property->HasAnyPropertyFlags(CPF_Parm) &&
                !property->HasAnyPropertyFlags(
                    CPF_ConstParm | CPF_OutParm | CPF_ReferenceParm | CPF_ReturnParm);
        };
        if (!item_data_property || !item_data_property->IsA<FStructProperty>() ||
            !item_data_property->HasAllPropertyFlags(
                CPF_Parm | CPF_OutParm | CPF_ConstParm | CPF_ReferenceParm) ||
            item_data_property->HasAnyPropertyFlags(CPF_ReturnParm) ||
            !expected_item_struct ||
            static_cast<FStructProperty*>(item_data_property)->GetStruct().Get() !=
                expected_item_struct ||
            !is_owned_input_parameter(inventories_property) ||
            !inventories_property->IsA<FArrayProperty>() ||
            !return_property || !return_property->IsA<FArrayProperty>() ||
            !return_property->HasAllPropertyFlags(
                CPF_Parm | CPF_OutParm | CPF_ReturnParm) ||
            return_property->HasAnyPropertyFlags(CPF_ConstParm | CPF_ReferenceParm) ||
            !property_fits_parameter_buffer(reflected_function, item_data_property) ||
            !property_fits_parameter_buffer(
                reflected_function, inventories_property, sizeof(TArray<UObject*>)) ||
            !property_fits_parameter_buffer(
                reflected_function, return_property, sizeof(FScriptArray)) ||
            !has_exact_parameters(
                reflected_function,
                {item_data_property, inventories_property, return_property}) ||
            !(item_data_property->GetOffset_Internal() <
                  inventories_property->GetOffset_Internal() &&
              inventories_property->GetOffset_Internal() <
                  return_property->GetOffset_Internal()))
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Repair material lookup parameters do not match the verified current Icarus ABI.\n"));
            return false;
        }

        auto* inventory_inner = static_cast<FArrayProperty*>(inventories_property)->GetInner();
        auto* return_inner = static_cast<FArrayProperty*>(return_property)->GetInner();
        if (!inventory_inner || !inventory_inner->IsA<FObjectProperty>() ||
            inventory_inner->GetElementSize() != sizeof(UObject*) ||
            !m_inventory_class ||
            static_cast<FObjectPropertyBase*>(inventory_inner)->GetPropertyClass().Get() !=
                m_inventory_class ||
            !return_inner || !return_inner->IsA<FStructProperty>() ||
            !expected_queue_item_struct ||
            static_cast<FStructProperty*>(return_inner)->GetStruct().Get() !=
                expected_queue_item_struct)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Repair material lookup array element types are incompatible.\n"));
            return false;
        }

        const auto native_target = resolve_native_repair_material_lookup(reflected_function);
        if (!native_target)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Repair material lookup call site did not match the current game build.\n"));
            return false;
        }
        ExclusiveSrwLockGuard callback_guard{repair_callback_lock};
        if (s_repair_hook_owner.load(std::memory_order_acquire) ||
            s_repair_trampoline.load(std::memory_order_acquire) != 0 ||
            m_repair_material_lookup_detour)
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] A native repair hook is already active or retained for safe forwarding.\n"));
            return false;
        }

        std::array<std::uint8_t, 16> original_entry_bytes{};
        if (!is_accessible_memory(
                reinterpret_cast<const void*>(native_target),
                original_entry_bytes.size(),
                false,
                true))
        {
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Native repair entry is not fully readable/executable.\n"));
            return false;
        }
        std::memcpy(
            original_entry_bytes.data(),
            reinterpret_cast<const void*>(native_target),
            original_entry_bytes.size());
        m_repair_native_target = native_target;
        m_repair_hooked_entry.fill(0);
        m_repair_hooked_entry_size = 0;
        m_repair_material_lookup_trampoline = 0;
        auto audited_detour = std::make_unique<AuditedX64Detour>(
            native_target,
            std::bit_cast<std::uint64_t>(&NearbyCraftingMod::repair_material_lookup_hook),
            &m_repair_material_lookup_trampoline);
        auto* const detour_audit = audited_detour.get();
        m_repair_material_lookup_detour = std::move(audited_detour);
        // Restrict PolyHook to its near-allocation scheme. The in-place and code-cave
        // fallbacks modify more game code and are not appropriate for an optional
        // compatibility-sensitive feature.
        m_repair_material_lookup_detour->setDetourScheme(PLH::x64Detour::VALLOC2);
        const auto hook_succeeded = m_repair_material_lookup_detour->hook();
        if (!hook_succeeded)
        {
            m_repair_material_lookup_detour.reset();
            m_repair_material_lookup_trampoline = 0;
            m_repair_native_target = 0;
            Output::send<LogLevel::Warning>(
                STR("[NearbyCrafting] Native repair material lookup hook registration failed.\n"));
            return false;
        }

        const auto patched_size = detour_audit->patched_size();
        if (!m_repair_material_lookup_detour->isHooked() ||
            detour_audit->patched_address() != native_target ||
            patched_size == 0 || patched_size > m_repair_hooked_entry.size() ||
            !is_accessible_memory(
                reinterpret_cast<const void*>(m_repair_material_lookup_trampoline),
                1,
                false,
                true) ||
            !is_accessible_memory(
                reinterpret_cast<const void*>(native_target),
                patched_size,
                false,
                true) ||
            std::memcmp(
                original_entry_bytes.data(),
                reinterpret_cast<const void*>(native_target),
                original_entry_bytes.size()) == 0)
        {
            // See the queue hook above: once hook() succeeds, its executable entry
            // and trampoline must remain valid for any caller already in flight.
            s_repair_trampoline.store(
                m_repair_material_lookup_trampoline, std::memory_order_release);
            static_cast<void>(m_repair_material_lookup_detour.release());
            m_repair_material_lookup_trampoline = 0;
            m_repair_native_target = 0;
            Output::send<LogLevel::Error>(
                STR("[NearbyCrafting] Native repair verification failed after patching; an inert forwarding detour was retained and a process restart is required.\n"));
            return false;
        }

        std::memcpy(
            m_repair_hooked_entry.data(),
            reinterpret_cast<const void*>(native_target),
            patched_size);
        m_repair_hooked_entry_size = patched_size;
        s_repair_trampoline.store(
            m_repair_material_lookup_trampoline, std::memory_order_release);
        s_repair_hook_owner.store(this, std::memory_order_release);
        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Installed native repair material lookup hook (patched-span={} bytes).\n"),
            patched_size);
        return true;
    }

    auto NearbyCraftingMod::remove_repair_material_lookup_hook() -> void
    {
        auto* expected_owner = this;
        s_repair_hook_owner.compare_exchange_strong(
            expected_owner, nullptr, std::memory_order_acq_rel);
        if (!m_repair_material_lookup_detour)
        {
            // Preserve a trampoline published by an inert process-lifetime detour.
            return;
        }
        {
            ExclusiveSrwLockGuard callback_guard{repair_callback_lock};
            if (m_repair_material_lookup_detour->isHooked() &&
                m_repair_hooked_entry_size != 0 &&
                (!m_repair_native_target ||
                 !is_accessible_memory(
                     reinterpret_cast<const void*>(m_repair_native_target),
                     m_repair_hooked_entry_size,
                     false,
                     true) ||
                 std::memcmp(
                     m_repair_hooked_entry.data(),
                     reinterpret_cast<const void*>(m_repair_native_target),
                     m_repair_hooked_entry_size) != 0))
            {
                pin_current_module_for_process_safety();
                s_repair_trampoline.store(
                    m_repair_material_lookup_trampoline, std::memory_order_release);
                static_cast<void>(m_repair_material_lookup_detour.release());
                m_repair_material_lookup_trampoline = 0;
                m_repair_native_target = 0;
                m_repair_hooked_entry_size = 0;
                Output::send<LogLevel::Error>(
                    STR("[NearbyCrafting] Native repair entry was changed by another patch; the inert forwarding detour and DLL were retained instead of overwriting it during cleanup.\n"));
                return;
            }
            s_repair_trampoline.store(
                m_repair_material_lookup_trampoline, std::memory_order_release);
            static_cast<void>(m_repair_material_lookup_detour.release());
        }
        m_repair_material_lookup_trampoline = 0;
        m_repair_native_target = 0;
        m_repair_hooked_entry.fill(0);
        m_repair_hooked_entry_size = 0;
        Output::send<LogLevel::Verbose>(
            STR("[NearbyCrafting] Native repair hook is inert; its forwarding detour is retained until process exit.\n"));
    }

}
