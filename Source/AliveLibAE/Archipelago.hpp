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
    s16 mTlvX;                // the Mudokon's original position in the level data (matches xpos/ypos in the path dumps)
    s16 mTlvY;                // -1 if the level data entry could not be found
    s16 mTotalRescued;        // the game's running rescued count
    s16 mZulagSavedKilled;    // the game's per-zulag saved+killed counter (kills share it); -1 if out of range
};

// The game plays its "secret area" cue: either Abe touched a secret-area music trigger, or a door that
// plays the secret music opened. Used to tell secret-area Mudokons from main-path ones.
struct SecretCueEvent final
{
    LevelIds mLevel;
    s16 mPath;
    s16 mCamera;
    bool mFromDoor;           // false: touch trigger, true: door
    s16 mX;                   // the trigger's top-left (touch trigger) or the door's position
    s16 mY;
    s16 mWidth;               // 0 for doors
    s16 mHeight;
};

void OnMudokonRescued(const RescueEvent& event);

void OnSecretCue(const SecretCueEvent& event);

void OnLevelChanged(LevelIds from, s16 fromPath, LevelIds to, s16 toPath, s16 camera);

} // namespace Archipelago

#endif
