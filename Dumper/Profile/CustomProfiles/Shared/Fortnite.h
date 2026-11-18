#pragma once

#include "../../IProfile.h"
#include "../../../Engine/OffsetFinder/Layouts.h"

#include <cstring>
#include <vector>

class FortniteProfile : public IProfile
{
public:
    FortniteProfile() = default;

    // Legacy hardcoded offsets kept as documentation of a known-good older
    // build. 43.10.0 moves them, so GetGObjects()/GetGNames() do a
    // structural scan rather than relying on these.
    static constexpr uintptr_t GUOBJECTARRAY_OFFSET_LEGACY = 0x195C4000;
    static constexpr uintptr_t FNAMEPOOL_OFFSET_LEGACY     = 0x195C6000;

    // Layout constants for Fortnite's FUObjectArray wrapper.
    // Fortnite embeds FChunkedFixedUObjectArray inside FUObjectArray; the
    // outer struct begins with GC bookkeeping (~0x10) so the chunked fields
    // start at +0x10, not +0x00.
    //   FUObjectArray {
    //     int32 ObjFirstGCIndex;              // 0x00
    //     int32 ObjLastNonGCIndex;            // 0x04
    //     int32 MaxObjectsNotConsideredByGC;  // 0x08
    //     bool  OpenForDisregardForGC;        // 0x0C (+padding)
    //     FChunkedFixedUObjectArray ObjObjects {
    //         FUObjectItem** Objects;         // +0x10
    //         FUObjectItem*  PreAllocated;    // +0x18
    //         int32          MaxElements;     // +0x20
    //         int32          NumElements;     // +0x24
    //         int32          MaxChunks;       // +0x28
    //         int32          NumChunks;       // +0x2C
    //     };
    //   };
    static constexpr int32 kChunkedObjectsOffset     = 0x10;
    static constexpr int32 kChunkedMaxElementsOffset = 0x20;
    static constexpr int32 kChunkedNumElementsOffset = 0x24;
    static constexpr int32 kChunkedMaxChunksOffset   = 0x28;
    static constexpr int32 kChunkedNumChunksOffset   = 0x2C;
    static constexpr int32 kChunkedItemSize          = 0x18;
    static constexpr int32 kElementsPerChunk         = 64 * 1024;

    std::vector<std::string> GetSupportedGames() const override
    {
        return {"com.epicgames.fortnite", "com.epicgames.fortnitemobile"};
    }

    std::string GetGameIdentifier() const override
    {
        return "com.epicgames.fortnite";
    }

    static std::unique_ptr<FChunkedUObjectArrayLayout> MakeChunkedLayout()
    {
        auto Layout = std::make_unique<FChunkedUObjectArrayLayout>();
        Layout->Objects             = kChunkedObjectsOffset;
        Layout->MaxElements         = kChunkedMaxElementsOffset;
        Layout->NumElements         = kChunkedNumElementsOffset;
        Layout->MaxChunks           = kChunkedMaxChunksOffset;
        Layout->NumChunks           = kChunkedNumChunksOffset;
        Layout->ElementsPerChunk    = kElementsPerChunk;
        Layout->FUObjectItem.Object = 0x00;
        Layout->FUObjectItem.Size   = kChunkedItemSize;
        return Layout;
    }

    // Walk every writable, non-executable segment of libUnreal.so and look
    // for a byte pattern that matches FUObjectArray's shape — a readable
    // chunked-array pointer at +0x10 followed by plausible Max/Num counts
    // at +0x20/+0x24. Modelled on PUBGMHD's FindGObjectsStructuralScan.
    static uintptr_t FindGObjectsStructuralScan()
    {
        constexpr size_t   kBlockSize   = 1024 * 1024;              // 1 MiB window
        constexpr size_t   kHeaderBytes = kChunkedNumChunksOffset + sizeof(int32);
        constexpr uint64_t kMaxUserVA   = 1ull << 48;

        std::vector<uint8_t> Block;

        for (const auto& Segment : MD7::Globals::GMemory->GetUnrealModule().GetSegments())
        {
            if (!Segment.IsValid() || !Segment.IsReadable() || !Segment.IsWriteable() || Segment.IsExecutable())
                continue;

            const size_t Size = Segment.GetSize();
            for (size_t Offset = 0; Offset + kHeaderBytes <= Size; Offset += kBlockSize)
            {
                const size_t BlockSize = std::min(kBlockSize + kHeaderBytes, Size - Offset);
                if (BlockSize != Block.size()) Block.resize(BlockSize);

                if (MD7::Globals::GMemory->ReadBytes(Segment.GetStart() + Offset, Block.data(), BlockSize) != BlockSize)
                    continue;

                for (size_t InBlock = 0; InBlock + kHeaderBytes <= BlockSize; InBlock += sizeof(uintptr_t))
                {
                    uintptr_t ItemsPtr = 0;
                    std::memcpy(&ItemsPtr, Block.data() + InBlock + kChunkedObjectsOffset, sizeof(ItemsPtr));
                    if (static_cast<uint64_t>(ItemsPtr) < 0x1000 || static_cast<uint64_t>(ItemsPtr) >= kMaxUserVA)
                        continue;

                    int32 Max = 0, Num = 0, MaxChunks = 0, NumChunks = 0;
                    std::memcpy(&Max,       Block.data() + InBlock + kChunkedMaxElementsOffset, sizeof(Max));
                    std::memcpy(&Num,       Block.data() + InBlock + kChunkedNumElementsOffset, sizeof(Num));
                    std::memcpy(&MaxChunks, Block.data() + InBlock + kChunkedMaxChunksOffset,   sizeof(MaxChunks));
                    std::memcpy(&NumChunks, Block.data() + InBlock + kChunkedNumChunksOffset,   sizeof(NumChunks));

                    // Shape filter — reject obviously wrong candidates
                    // cheaply before we call the full validator.
                    if (Num < LayoutDetection::kMinObjectCount || Num > LayoutDetection::kMaxObjectCount) continue;
                    if (Max < Num || Max > LayoutDetection::kMaxObjectCount)                              continue;
                    if (MaxChunks <= 0 || MaxChunks > 2048)                                               continue;
                    if (NumChunks <= 0 || NumChunks > MaxChunks)                                          continue;
                    const int32 ExpectedMaxChunks = (Max + kElementsPerChunk - 1) / kElementsPerChunk;
                    if (MaxChunks != ExpectedMaxChunks)                                                   continue;

                    const uintptr_t Global = Segment.GetStart() + Offset + InBlock;
                    const std::unique_ptr<FChunkedUObjectArrayLayout> Layout = MakeChunkedLayout();

                    if (!LayoutDetection::TestObjectsLayout(Global, Layout.get()).bValid)
                        continue;

                    MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
                        "FortniteProfile::FindGObjectsStructuralScan: HIT at 0x{:X} (base+0x{:X}) "
                        "Num={} Max={} NumChunks={} MaxChunks={}\n",
                        Global, MD7::Globals::GMemory->GetUnrealModule().AddressToOffset(Global),
                        Num, Max, NumChunks, MaxChunks);
                    return Global;
                }
            }
        }

        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
            "FortniteProfile::FindGObjectsStructuralScan: no FUObjectArray-shaped region found\n");
        return 0;
    }

    uintptr_t GetGObjects() const override
    {
        // Structural scan over writable segments. Hardcoded 0x195C4000 is stale
        // for 43.10.0, and UEAnalyzerKitty's generic pattern scan rejects
        // every candidate for Fortnite — so we run our own shape-matcher.
        uintptr_t Found = FindGObjectsStructuralScan();
        if (Found) return Found;

        // Last-ditch fallback to the legacy hardcoded offset in case the
        // scan missed; the caller (InitObjects) will validate it and
        // defer to UEAnalyzerKitty if this is also wrong.
        uintptr_t Base = MD7::Globals::GMemory->GetUnrealModule().GetBase();
        if (Base == 0) return 0;
        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
            "FortniteProfile::GetGObjects: scan missed, falling back to legacy base+0x{:x}\n",
            (uintptr_t)GUOBJECTARRAY_OFFSET_LEGACY);
        return Base + GUOBJECTARRAY_OFFSET_LEGACY;
    }

    uintptr_t GetGNames() const override
    {
        // Return 0 so MD7 defers to UEAnalyzerKitty. Once GObjects is
        // locked in, FNamePool is usually within a few KiB of it and the
        // generic name scanner has better luck than it did for the
        // object array.
        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
            "FortniteProfile::GetGNames: 0 (defer to pattern scanner)\n");
        return 0;
    }

    bool ResolveGObjectsLayout(uintptr_t ObjectsPtr,
                               std::unique_ptr<IObjectsLayout>& Out,
                               std::string* OutError = nullptr) const override
    {
        Out.reset();
        if (ObjectsPtr == 0) { if (OutError) *OutError = "null"; return false; }

        auto Layout = MakeChunkedLayout();

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
