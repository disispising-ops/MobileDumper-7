#pragma once

#include "../../IProfile.h"
#include "../../../Engine/OffsetFinder/Layouts.h"

#include <cstring>
#include <vector>

class FortniteProfile : public IProfile
{
public:
    FortniteProfile() = default;

    // Legacy hardcoded offsets kept as last-ditch fallback.
    static constexpr uintptr_t GUOBJECTARRAY_OFFSET_LEGACY = 0x195C4000;
    static constexpr uintptr_t FNAMEPOOL_OFFSET_LEGACY     = 0x195C6000;

    // Default FChunkedFixedUObjectArray layout wrapped inside FUObjectArray
    // (~0x10 of GC bookkeeping before the embedded chunked struct).
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

    // Walk every writable, non-executable segment of libUnreal.so and ask
    // MD7's own LayoutDetection::DetectObjectsLayout to validate each
    // candidate root. DetectObjectsLayout knows every FUObjectArray /
    // FChunkedFixedUObjectArray / FFixedUObjectArray variant and copes with
    // any wrapper padding, so we don't have to guess the field offsets
    // ourselves (my previous fixed-shape scanner missed everything).
    //
    // Prefilter: only probe positions whose first ~0x30 bytes contain at
    // least one aligned, in-range userspace pointer. That skips the
    // overwhelming majority of .bss (zeros, small ints, strings) without
    // missing a real FUObjectArray — every variant starts with or soon has
    // a chunk-array / items pointer.
    static uintptr_t FindGObjectsStructuralScan()
    {
        constexpr size_t   kBlockSize   = 1024 * 1024;   // 1 MiB window
        constexpr size_t   kHeaderBytes = 0x40;
        constexpr uint64_t kMinUserVA   = 0x1000;
        constexpr uint64_t kMaxUserVA   = 1ull << 48;

        std::vector<uint8_t> Block;
        uintptr_t BestHit = 0;
        double    BestConfidence = 0.0;
        int       Attempts = 0, Hits = 0;

        auto PeekPtrAt = [](const uint8_t* P, size_t Off) -> uintptr_t {
            uintptr_t V = 0;
            std::memcpy(&V, P + Off, sizeof(V));
            return V;
        };
        auto LooksLikePtr = [](uintptr_t V) {
            return static_cast<uint64_t>(V) >= kMinUserVA
                && static_cast<uint64_t>(V) < kMaxUserVA
                && (V & 0x7) == 0;
        };

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
                    // Prefilter — need an aligned userspace pointer within +0..+0x28.
                    bool bHasPtr = false;
                    for (size_t O = 0; O <= 0x28; O += sizeof(uintptr_t))
                    {
                        if (LooksLikePtr(PeekPtrAt(Block.data() + InBlock, O))) { bHasPtr = true; break; }
                    }
                    if (!bHasPtr) continue;

                    const uintptr_t Root = Segment.GetStart() + Offset + InBlock;
                    ++Attempts;

                    LayoutDetection::FObjectsDetectionResult Det = LayoutDetection::DetectObjectsLayout(Root);
                    if (!Det.bSuccess) continue;

                    ++Hits;
                    MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
                        "FortniteProfile::FindGObjectsStructuralScan: HIT at 0x{:X} (base+0x{:X}) "
                        "confidence={:.2f} — {}\n",
                        Root,
                        MD7::Globals::GMemory->GetUnrealModule().AddressToOffset(Root),
                        Det.Confidence, Det.Summary);

                    if (Det.Confidence >= 0.8) return Root;
                    if (Det.Confidence > BestConfidence) { BestHit = Root; BestConfidence = Det.Confidence; }
                }
            }
        }

        if (BestHit)
        {
            MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
                "FortniteProfile::FindGObjectsStructuralScan: using best hit 0x{:X} ({:.2f}), "
                "{} hits / {} positions\n", BestHit, BestConfidence, Hits, Attempts);
            return BestHit;
        }

        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
            "FortniteProfile::FindGObjectsStructuralScan: no valid FUObjectArray — "
            "{} positions probed by DetectObjectsLayout, 0 hits\n", Attempts);
        return 0;
    }

    uintptr_t GetGObjects() const override
    {
        uintptr_t Found = FindGObjectsStructuralScan();
        if (Found) return Found;

        uintptr_t Base = MD7::Globals::GMemory->GetUnrealModule().GetBase();
        if (Base == 0) return 0;
        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
            "FortniteProfile::GetGObjects: scan missed, falling back to legacy base+0x{:x}\n",
            (uintptr_t)GUOBJECTARRAY_OFFSET_LEGACY);
        return Base + GUOBJECTARRAY_OFFSET_LEGACY;
    }

    uintptr_t GetGNames() const override
    {
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
