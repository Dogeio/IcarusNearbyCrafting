#include <NearbyCrafting/NearbyCraftingMod.hpp>

#define NEARBY_CRAFTING_API __declspec(dllexport)

extern "C"
{
    NEARBY_CRAFTING_API RC::CppUserModBase* start_mod()
    {
        return new NearbyCrafting::NearbyCraftingMod();
    }

    NEARBY_CRAFTING_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
