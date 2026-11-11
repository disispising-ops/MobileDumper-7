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

    bool ResolveGObjectsLayout(uintptr_t ObjectsPtr,
                               std::unique_ptr<IObjectsLayout>& Out,
                               std::string* OutError = nullptr) const override
    {
        Out.reset();
        if (ObjectsPtr == 0) { if (OutError) *OutError = "null"; return false; }

        auto Layout = std::make_unique<FChunkedUObjectArrayLayout>();
        Layout->Objects          = 0x10;
        Layout->MaxElements      = 0x20;
        Layout->NumElements      = 0x24;
        Layout->MaxChunks        = 0x28;
        Layout->NumChunks        = 0x2C;
        Layout->ElementsPerChunk = 64 * 1024;
        Layout->FUObjectItem.Object = 0x00;
        Layout->FUObjectItem.Size   = 0x18;

        int32 Num = MD7::Globals::GMemory->Read<int32>(ObjectsPtr + Layout->NumElements);
        int32 Max = MD7::Globals::GMemory->Read<int32>(ObjectsPtr + Layout->MaxElements);
        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
            "FortniteProfile::ResolveGObjectsLayout: NumElements={} MaxElements={}\n", Num, Max);

        Out = std::move(Layout);
        return true;
    }

    bool ResolveGNamesLayout(uintptr_t NamesPtr,
                             std::unique_ptr<INamesLayout>& Out,
                             std::string* OutError = nullptr) const override
    {
        Out.reset();
        if (NamesPtr == 0) { if (OutError) *OutError = "null"; return false; }

        auto Layout = std::make_unique<FNamePoolLayout>();
        Layout->BlocksBit              = 16;
        Layout->MaxChunkIndex          = 0x30;
        Layout->ByteCursor             = 0x34;
        Layout->Blocks                 = 0x40;
        Layout->FNameEntry.Stride      = 2;
        Layout->FNameEntry.Header      = 0x00;
        Layout->FNameEntry.String      = 0x02;
        Layout->FNameEntry.LengthShift = 6;
        Layout->FNameEntry.WideMask    = 0x1;
        Layout->FNameEntry.GetLength = [](uint16 H) -> int32 { return int32(H >> 6); };
        Layout->FNameEntry.GetIsWide = [](uint16 H) -> bool  { return (H & 0x1) != 0; };

        Out = std::move(Layout);
        return true;
    }

    void OverrideSettings(FSettings& Settings) const override
    {
        Settings.General.MaxFNameLen = 1024;
    }
};
