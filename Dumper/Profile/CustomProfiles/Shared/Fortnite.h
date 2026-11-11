#pragma once

#include "../../IProfile.h"

class FortniteProfile : public IProfile
{
public:
    FortniteProfile() = default;

    static constexpr uintptr_t GUOBJECTARRAY_OFFSET = 0x195C4000;
    static constexpr uintptr_t FNAMEPOOL_OFFSET     = 0x195C6000;

    std::vector<std::string> GetSupportedGames() const override
    {
        return {"com.epicgames.fortnite", "com.epicgames.fortnitemobile"};
    }

    std::string GetGameIdentifier() const override
    {
        return "com.epicgames.fortnite";
    }

    uintptr_t GetGObjects() const override
    {
        uintptr_t Base = MD7::Globals::GMemory->GetUnrealModule().GetBase();
        if (Base == 0) return 0;
        uintptr_t Addr = Base + GUOBJECTARRAY_OFFSET;
        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
            "FortniteProfile::GetGObjects: 0x{:x} = base 0x{:x} + 0x{:x}\n",
            Addr, Base, (uintptr_t)GUOBJECTARRAY_OFFSET);
        return Addr;
    }

    uintptr_t GetGNames() const override
    {
        uintptr_t Base = MD7::Globals::GMemory->GetUnrealModule().GetBase();
        if (Base == 0) return 0;
        uintptr_t Addr = Base + FNAMEPOOL_OFFSET;
        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
            "FortniteProfile::GetGNames: 0x{:x} = base 0x{:x} + 0x{:x}\n",
            Addr, Base, (uintptr_t)FNAMEPOOL_OFFSET);
        return Addr;
    }
