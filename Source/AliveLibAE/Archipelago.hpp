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

// ---------------------------------------------------------------------------------------------------------
// Commands from the client, and game events the client needs.
//
// The client appends one JSON object per line to archipelago_commands.jsonl:
//   {"seq":12,"cmd":"give_throwables","count":5}   add throwables of the level's own kind
//   (give_throwables waits, without holding up the other commands, until Abe is on a path whose level has throwables)
//   {"seq":13,"cmd":"alarm"}                       sound the alarm (a trap)
//   {"seq":14,"cmd":"fart"}                        make Abe fart (a trap): sound, noise Sligs hear, smoke
//   {"seq":0,"t":<ms>,"cmd":"teleport","level":2,"path":5,"camera":9}  TESTING ONLY: jump to a camera, like the debug
//                                                  teleport menu does (game LevelId, path, camera)
//   {"seq":0,"t":<ms>,"cmd":"message","text":"Got 5 Throwables"}  show a line of text in the corner of the screen for a few
//                                                  seconds (ASCII only, no quotes; ephemeral like kill)
//   {"cfg":1,"cmd":"set_good_ending","value":120}  a standing setting: the good ending needs this many rescues.
//                                                  Applied every time the game starts; the last one in the file wins.
//   {"cfg":1,"cmd":"set_goal_items","value":100}   a standing setting: the good ending needs this many Mudokon Saved items
//                                                  (not rescues). Without it rescues count, as before.
//   {"cfg":1,"cmd":"set_saved_mudokons","value":12}  a standing setting: Mudokon Saved items received so far
//   {"seq":0,"t":1731850000000,"cmd":"kill"}       kill Abe (Death Link); ephemeral, ignored if older than 15 s
// A command with seq > 0 is carried out once: the highest seq done is stored in archipelago_ack.txt, so the client
// can safely resend everything after a restart. Commands that cannot run yet (no Abe, menu, credits) wait.

enum class EndingKind : s16
{
    eAngelic = 0, // every Mudokon rescued
    eGood = 1,    // enough rescued
    eBad = 2,
    eVeryBad = 3, // too many killed
};

// The game chose an ending (Abe entered the final bird portal).
void OnEnding(EndingKind kind);

// Called once per frame from the main loop: reads commands, carries them out, and reports Abe dying.
void Tick();

// The number of rescues the good ending needs, or 0 to use the game's own value.
s32 GoodEndingOverride();

// The good ending's own check, `enoughRescues`, may be replaced: with Mudokon Saved items the ending needs that many items
// instead, and when it does not have them a message in the corner says so.
bool GoodEndingAllowed(bool enoughRescues);

// True if the current path's throwables can be loaded (their sprites are in the level's archive). Necrum has no rock sprites, so
// throwables there crash the game when a save is loaded, or when one is thrown.
bool ThrowablesUsableHere(bool duringLoad = false);

// The "Saved Mudokons" number on the in-game status boards: with Mudokon Saved items it is the number of items received (what
// the goal counts), otherwise `rescued`, the game's own count.
s32 SavedMudokonsShown(s32 rescued);

// Physical blocking. A door whose destination is locked behaves as a closed door, and Abe will not enter a well that leads
// somewhere locked, until the client unlocks it.
//
// Lock state comes from archipelago_unlocks.txt in the working directory (re-read about once a second, so it
// can be edited while playing). One directive per line:
//   enforce          turn blocking on (without it nothing is ever blocked)
//   area_access      also block individual areas (zulags), not just levels
//   level <id>       the level with this game LevelId is unlocked
//   zulag <l> <z>    area <z> of level <l> is unlocked
//   need <l> <text>  what level <l> still needs (its pass, and/or Mudokons for the final level), shown when its door is tried
// Levels 1 (Necrum Mines) and 2 (Necrum) are always open. If the file is missing nothing is blocked.
//
// Doors and express wells are blocked. Path transitions, level loaders, teleporters and portals are not. Checks are held back
// by the client regardless, so logic stays sound.
bool IsDestinationLocked(LevelIds level, s16 path);

// Abe tried a locked way out (a door or a well): logs it, plays the game's "access denied" buzzer and shows what is needed
// in the corner (rate limited). `kind` is "door" or "well" and names the log event (locked_door, locked_well).
void OnLockedExitTried(LevelIds level, s16 path, const char* kind);

// The same, for doors.
void OnLockedDoorTried(LevelIds level, s16 path);

} // namespace Archipelago

#endif
