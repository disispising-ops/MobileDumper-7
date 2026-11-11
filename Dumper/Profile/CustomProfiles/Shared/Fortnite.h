#pragma once

#include "../../IProfile.h"
#include <KittyMemoryEx/KittyMemoryEx.hpp>

/**
 * @brief Fortnite Mobile (Android) profile for MobileDumper-7.
 *
 * Reverse-engineered from libUnreal.so of FN v43.00.0-59596594 (aarch64).
 * Globals located via static ADRP+ADD xref density analysis on 100MB .text:
 *   - GUObjectArray  (FUObjectArray wrapper)
 *       +0x10 = ObjObjects.Objects**     (8,911 xrefs)
 *       +0x20 = ObjObjects.MaxElements   (16,791 xrefs)  <-- top xref count in page
 *       Base relative to libUnreal.so: 0x195C4000
 *   - FNamePool
 *       +0x40 = Blocks[]                 (13,796 xrefs)
 *       Base relative to libUnreal.so: 0x195C6000
 *
 * These offsets will drift on every FN patch. Update after each patch by
 * re-running the static xref analysis.
 */
class FortniteProfile : public IProfile
{
public:
    FortniteProfile() = default;

    static constexpr uintptr_t GUOBJECTARRAY_OFFSET = 0x195C4000;
    static constexpr uintptr_t FNAMEPOOL_OFFSET     = 0x195C6000;

    std::vector<std::string> GetSupportedGames() const override
    {
        return {
            "com.epicgames.fortnite",
            "com.epicgames.fortnitemobile",
        };
    }

    std::string GetGameIdentifier() const override
    {
        return "com.epicgames.fortnite";
    }

    uintptr_t GetModuleBase() const
