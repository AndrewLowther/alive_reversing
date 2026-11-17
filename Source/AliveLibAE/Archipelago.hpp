#pragma once

#include "../AliveLibCommon/FunctionFwd.hpp"
#include "../AliveLibCommon/relive_config.h"

// Archipelago multiworld client hooks (Exoddus only).
//
// Phase 1 is observe-only: the engine reports rescues and level changes, and they are appended to
// archipelago_events.jsonl in the working directory. Nothing here changes game behaviour.
// Everything is compiled out unless the ARCHIPELAGO_CLIENT CMake option is ON.

#if ARCHIPELAGO_CLIENT

enum class LevelIds : s16;

namespace Archipelago {

struct RescueEvent final
{
    LevelIds mLevel;          // level the Mudokon was rescued in
    s16 mPath;                // path within that level
    s16 mZulag;               // zulag the status board currently reports (global, set by the last board seen)
    u32 mTlvInfo;             // the Mudokon's TlvItemInfoUnion (tlv offset, level id, path id): a stable identity
    s16 mTotalRescued;        // the game's running rescued count
    s16 mZulagSavedKilled;    // the game's per-zulag saved+killed counter (kills share it); -1 if out of range
};

void OnMudokonRescued(const RescueEvent& event);

void OnLevelChanged(LevelIds from, s16 fromPath, LevelIds to, s16 toPath, s16 camera);

} // namespace Archipelago

#endif
