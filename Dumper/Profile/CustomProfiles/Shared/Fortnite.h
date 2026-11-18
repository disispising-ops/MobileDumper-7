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

    // Default FChunkedFixedUObjectArray layout wrapped inside FUObjectArray.
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

    static std::unique_ptr<FChunkedUObjectArrayLayout> MakeChunkedLayout(
        int32 ObjectsOff, int32 MaxOff, int32 NumOff, int32 MaxChunksOff, int32 NumChunksOff)
    {
        auto Layout = std::make_unique<FChunkedUObjectArrayLayout>();
        Layout->Objects             = ObjectsOff;
        Layout->MaxElements         = MaxOff;
        Layout->NumElements         = NumOff;
        Layout->MaxChunks           = MaxChunksOff;
        Layout->NumChunks           = NumChunksOff;
        Layout->ElementsPerChunk    = kElementsPerChunk;
        Layout->FUObjectItem.Object = 0x00;
        Layout->FUObjectItem.Size   = kChunkedItemSize;
        return Layout;
    }

    // Walk every writable, non-executable segment of libUnreal.so and look
    // for a byte pattern that matches an FUObjectArray / FChunkedFixedUObjectArray
    // under one of several plausible wrapper layouts (chunked block at +0x00,
    // +0x10, +0x18, or +0x20). The shape check is strict and cheap — the
    // killer constraint is MaxChunks == ceil(Max/65536), which coincidental
    // hits almost never satisfy — so we only hand a small number of
    // candidates to the full LayoutDetection validator.
    //
    // Previous version called DetectObjectsLayout on every prefiltered
    // position and ran for minutes before the OS killed the app; this one
    // caps work hard and returns within ~1s on failure.
    static uintptr_t FindGObjectsStructuralScan()
    {
        struct LayoutVariant { int32 Objects, Max, Num, MaxChunks, NumChunks; };
        static constexpr LayoutVariant kVariants[] = {
            // {Objects, MaxElements, NumElements, MaxChunks, NumChunks}
            {0x10, 0x20, 0x24, 0x28, 0x2C},  // wrapper + 0x10 GC header (common)
            {0x00, 0x10, 0x14, 0x18, 0x1C},  // bare FChunkedFixedUObjectArray
            {0x18, 0x28, 0x2C, 0x30, 0x34},  // wrapper + crit section
            {0x20, 0x30, 0x34, 0x38, 0x3C},  // wrapper + more padding
        };

        constexpr size_t   kBlockSize     = 1024 * 1024;
        constexpr size_t   kHeaderBytes   = 0x48;
        constexpr uint64_t kMinUserVA     = 0x1000;
        constexpr uint64_t kMaxUserVA     = 1ull << 48;
        constexpr int      kMaxValidatorCalls = 256;  // hard cap, never spin

        std::vector<uint8_t> Block;
        int ValidatorCalls = 0, ShapeHits = 0;
        uintptr_t BestHit = 0;
        double    BestConfidence = 0.0;

        auto PeekPtrAt = [](const uint8_t* P, size_t Off) -> uintptr_t {
            uintptr_t V = 0; std::memcpy(&V, P + Off, sizeof(V)); return V;
        };
        auto PeekI32At = [](const uint8_t* P, size_t Off) -> int32 {
            int32 V = 0; std::memcpy(&V, P + Off, sizeof(V)); return V;
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
            if (ValidatorCalls >= kMaxValidatorCalls) break;

            const size_t Size = Segment.GetSize();
            for (size_t Offset = 0; Offset + kHeaderBytes <= Size; Offset += kBlockSize)
            {
                if (ValidatorCalls >= kMaxValidatorCalls) break;
                const size_t BlockSize = std::min(kBlockSize + kHeaderBytes, Size - Offset);
                if (BlockSize != Block.size()) Block.resize(BlockSize);

                if (MD7::Globals::GMemory->ReadBytes(Segment.GetStart() + Offset, Block.data(), BlockSize) != BlockSize)
                    continue;

                for (size_t InBlock = 0; InBlock + kHeaderBytes <= BlockSize; InBlock += sizeof(uintptr_t))
                {
                    if (ValidatorCalls >= kMaxValidatorCalls) break;
                    const uint8_t* P = Block.data() + InBlock;

                    // Try each wrapper layout — shape filter is cheap.
                    for (const LayoutVariant& V : kVariants)
                    {
                        uintptr_t ItemsPtr = PeekPtrAt(P, (size_t)V.Objects);
                        if (!LooksLikePtr(ItemsPtr)) continue;

                        int32 Max       = PeekI32At(P, (size_t)V.Max);
                        int32 Num       = PeekI32At(P, (size_t)V.Num);
                        int32 MaxChunks = PeekI32At(P, (size_t)V.MaxChunks);
                        int32 NumChunks = PeekI32At(P, (size_t)V.NumChunks);

                        if (Num < LayoutDetection::kMinObjectCount || Num > LayoutDetection::kMaxObjectCount) continue;
                        if (Max < Num || Max > LayoutDetection::kMaxObjectCount)                              continue;
                        if (MaxChunks <= 0 || MaxChunks > 2048)                                               continue;
                        if (NumChunks <= 0 || NumChunks > MaxChunks)                                          continue;
                        const int32 Expected = (Max + kElementsPerChunk - 1) / kElementsPerChunk;
                        if (MaxChunks != Expected)                                                            continue;

                        ++ShapeHits;
                        const uintptr_t Root = Segment.GetStart() + Offset + InBlock;

                        // Shape matched — hand off to DetectObjectsLayout which
                        // auto-discovers the exact field layout and item decryption
                        // instead of trusting our variant guess.
                        ++ValidatorCalls;
                        auto Det = LayoutDetection::DetectObjectsLayout(Root);

                        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
                            "FortniteProfile::Scan: shape@0x{:X} variant(Obj=+0x{:X}) Num={} Max={} detect={} conf={:.2f} — {}\n",
                            Root, V.Objects, Num, Max,
                            Det.bSuccess ? 1 : 0, Det.Confidence, Det.Summary);

                        if (Det.bSuccess)
                        {
                            MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
                                "FortniteProfile::Scan: HIT at 0x{:X} (base+0x{:X}) — {}\n",
                                Root,
                                MD7::Globals::GMemory->GetUnrealModule().AddressToOffset(Root),
                                Det.Summary);
                            return Root;
                        }
                        if (Det.Confidence > BestConfidence) { BestHit = Root; BestConfidence = Det.Confidence; }
                        break;  // one variant per Root is enough — don't re-probe the same address
                    }
                }
            }
        }

        MD7::Globals::GLogger.FmtWrite(ELogLevel::Info,
            "FortniteProfile::Scan: done — shape hits={}, validator calls={}, best conf={:.2f}\n",
            ShapeHits, ValidatorCalls, BestConfidence);

        return BestHit; // may be 0
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

        auto Layout = MakeChunkedLayout(kChunkedObjectsOffset, kChunkedMaxElementsOffset,
                                        kChunkedNumElementsOffset, kChunkedMaxChunksOffset,
                                        kChunkedNumChunksOffset);

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

    // Fortnite packs flag bits into unused upper bits of each FUObjectItem's
    // Object pointer. Published Fortnite offset dumps list:
    //   objects_bad_flags  = 0x1020000000000000  (bits 52, 56)
    //   objects_upper_mask = 0x3FFF00000000      (bits 32..45, the ARM64 VA band)
    // ARM64 userspace pointers live at 0x0000007XXXXXXXXX, so bits 52+ are
    // never part of a real address — they're the packed flags, and masking
    // them off recovers the clean pointer.
    void OverrideDecryptCallbacks(FDecryptCallbacks& Callbacks) const override
    {
        constexpr uint64_t kBadFlags = 0x1020000000000000ULL;

        auto Decrypt = [](const FDecryptContext<uintptr_t>& Ctx) -> uintptr_t
        {
            uintptr_t Cleaned = Ctx.RawValue & ~kBadFlags;
            // Sanity check — a real UObject must be in userspace and
            // 8-byte aligned. If after masking it's still garbage,
            // report 0 so the walker treats the slot as empty rather
            // than following a wild pointer and crashing.
            if (Cleaned < 0x1000 || Cleaned >= (1ULL << 48) || (Cleaned & 0x7))
                return 0;
            return Cleaned;
        };

        // Same mask may be applied to the chunks-array pointer too.
        Callbacks.ChunkedObjects.FUObjectItem.Object = Decrypt;
        Callbacks.ChunkedObjects.Objects             = Decrypt;
        Callbacks.FixedObjects.FUObjectItem.Object   = Decrypt;
        Callbacks.FixedObjects.Objects               = Decrypt;
    }
};
