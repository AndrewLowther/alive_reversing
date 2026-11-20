#include "stdafx.h"
#include "Archipelago.hpp"

#if ARCHIPELAGO_CLIENT

#include "ArchipelagoJson.hpp"
#include "ArchipelagoLock.hpp"

#include "Abe.hpp"
#include "Alarm.hpp"
#include "BaseBomb.hpp"
#include "DDCheat.hpp"
#include "Events.hpp"
#include "Factory.hpp"
#include "Game.hpp"
#include "GameSpeak.hpp"
#include "Layer.hpp"
#include "PsxDisplay.hpp"
#include "Map.hpp"
#include "Particle.hpp"
#include "PathData.hpp"
#include "LvlArchive.hpp"
#include "Sfx.hpp"
#include "Throwable.hpp"
#include "ThrowableArray.hpp"
#include "stdlib.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace Archipelago {

static const char_type* kEventLogFile = "archipelago_events.jsonl";
static const char_type* kUnlocksFile = "archipelago_unlocks.txt";
static const char_type* kCommandFile = "archipelago_commands.jsonl";
static const char_type* kAckFile = "archipelago_ack.txt";

static s64 NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// One JSON object per line, flushed immediately so a crash loses nothing.
static void AppendLine(const char_type* line)
{
    if (FILE* f = std::fopen(kEventLogFile, "a+b"))
    {
        // If someone edited the file and left no final newline, start a new line rather than gluing onto the last one.
        bool needNewline = false;
        if (std::fseek(f, -1, SEEK_END) == 0)
        {
            const int last = std::fgetc(f);
            needNewline = last != '\n' && last != EOF;
        }
        std::fseek(f, 0, SEEK_END); // required between reading and writing on the same stream
        if (needNewline)
        {
            std::fputc('\n', f);
        }
        std::fputs(line, f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

void OnMudokonRescued(const RescueEvent& event)
{
    TlvItemInfoUnion tlv = {};
    tlv.all = event.mTlvInfo;

    char_type buffer[512] = {};
    std::snprintf(
        buffer, sizeof(buffer),
        "{\"t\":%lld,\"event\":\"rescue\",\"level\":%d,\"path\":%d,\"zulag\":%d,"
        "\"tlv\":{\"offset\":%u,\"level\":%u,\"path\":%u,\"x\":%d,\"y\":%d},\"total_rescued\":%d,\"zulag_saved_killed\":%d}",
        static_cast<long long>(NowMs()),
        static_cast<s32>(event.mLevel), event.mPath, event.mZulag,
        static_cast<u32>(tlv.parts.tlvOffset), static_cast<u32>(tlv.parts.levelId), static_cast<u32>(tlv.parts.pathId),
        event.mTlvX, event.mTlvY, event.mTotalRescued, event.mZulagSavedKilled);
    AppendLine(buffer);
}

void OnSecretCue(const SecretCueEvent& event)
{
    char_type buffer[320] = {};
    std::snprintf(
        buffer, sizeof(buffer),
        "{\"t\":%lld,\"event\":\"secret_cue\",\"kind\":\"%s\",\"level\":%d,\"path\":%d,\"camera\":%d,"
        "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
        static_cast<long long>(NowMs()),
        event.mFromDoor ? "door" : "touch",
        static_cast<s32>(event.mLevel), event.mPath, event.mCamera, event.mX, event.mY, event.mWidth, event.mHeight);
    AppendLine(buffer);
}

void OnLevelChanged(LevelIds from, s16 fromPath, LevelIds to, s16 toPath, s16 camera)
{
    char_type buffer[256] = {};
    std::snprintf(
        buffer, sizeof(buffer),
        "{\"t\":%lld,\"event\":\"level\",\"from\":{\"level\":%d,\"path\":%d},\"to\":{\"level\":%d,\"path\":%d,\"camera\":%d}}",
        static_cast<long long>(NowMs()),
        static_cast<s32>(from), fromPath, static_cast<s32>(to), toPath, camera);
    AppendLine(buffer);
}

// ---------------------------------------------------------------------------------------------------------
// Physical blocking

namespace {

// Lines of text shown in the top-left corner of the screen, drawn with the engine's debug font.
struct Message final
{
    std::string mText;
    u32 mExpiresAtFrame = 0;
};
std::vector<Message> sMessages;
constexpr u32 kMessageFrames = 150; // about five seconds
constexpr size_t kMaxMessages = 4;  // older lines are dropped
constexpr size_t kMaxMessageLength = 70;

void AddMessage(std::string text)
{
    if (text.size() > kMaxMessageLength)
    {
        text.resize(kMaxMessageLength);
    }
    for (Message& m : sMessages)
    {
        if (m.mText == text) // already showing: just keep it up a little longer
        {
            m.mExpiresAtFrame = sGnFrame_5C1B84 + kMessageFrames;
            return;
        }
    }
    sMessages.push_back({std::move(text), sGnFrame_5C1B84 + kMessageFrames});
    if (sMessages.size() > kMaxMessages)
    {
        sMessages.erase(sMessages.begin());
    }
}

void DrawMessages()
{
    sMessages.erase(std::remove_if(sMessages.begin(), sMessages.end(), [](const Message& m) { return sGnFrame_5C1B84 > m.mExpiresAtFrame; }), sMessages.end());
    for (const Message& m : sMessages)
    {
        // The debug font is flushed to the screen once per frame, so text added here appears this frame.
        DebugFont_Printf_4F8B60(0, "%s\n", m.mText.c_str()); // index 0 is the one the cheat menu uses
    }
}

using Lock::AreaEntry;
using Lock::UnlockState;
using Lock::BaseLevel;

// Generated by scripts/build_areas.py (AOAP repo): which areas (zulags) each path contains.
const AreaEntry kAreas[] = {
#include "ArchipelagoAreas.inc"
};

// Generated by scripts/build_fences.py (AOAP repo): the electric walls a fence group switches off.
const Lock::FenceEntry kFences[] = {
#include "ArchipelagoFences.inc"
};

UnlockState sUnlocks;
std::filesystem::file_time_type sUnlocksStamp = {};
bool sUnlocksFileSeen = false;
std::chrono::steady_clock::time_point sLastPoll = {};

void Reload()
{
    UnlockState fresh;
    std::ifstream in(kUnlocksFile);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string key;
        if (!(words >> key))
        {
            continue;
        }
        if (key == "enforce")
        {
            fresh.mEnforce = true;
        }
        else if (key == "area_access")
        {
            fresh.mAreaAccess = true;
        }
        else if (key == "lock_start_level")
        {
            fresh.mLockStartLevel = true; // testing only
        }
        else if (key == "level")
        {
            s32 id = 0;
            if (words >> id)
            {
                fresh.mLevels.insert(static_cast<s16>(id));
            }
        }
        else if (key == "open")
        {
            // "open <group>": a fence group the client has opened (an ElectricWall that the group lists is then switched off)
            std::string group;
            if (words >> group)
            {
                fresh.mOpenGroups.insert(group);
            }
        }
        else if (key == "need")
        {
            // "need <level> <text to the end of the line>": what the level still needs (shown when its door is tried)
            s32 level = 0;
            if (words >> level)
            {
                std::string rest;
                std::getline(words, rest);
                const auto first = rest.find_first_not_of(' ');
                fresh.mNeeds[static_cast<s16>(level)] = first == std::string::npos ? std::string() : rest.substr(first);
            }
        }
        else if (key == "zulag")
        {
            s32 level = 0;
            s32 zulag = 0;
            if (words >> level >> zulag)
            {
                fresh.mZulags.insert({static_cast<s16>(level), static_cast<s16>(zulag)});
            }
        }
    }
    sUnlocks = fresh;
}

// Cheap: looks at the file's timestamp at most once a second.
void Refresh()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - sLastPoll < std::chrono::seconds(1))
    {
        return;
    }
    sLastPoll = now;

    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(kUnlocksFile, ec);
    if (ec)
    {
        if (sUnlocksFileSeen)
        {
            sUnlocksFileSeen = false;
            sUnlocks = UnlockState();
        }
        return;
    }
    if (!sUnlocksFileSeen || stamp != sUnlocksStamp)
    {
        sUnlocksFileSeen = true;
        sUnlocksStamp = stamp;
        Reload();
    }
}

} // namespace

namespace {

const char_type* LevelName(s16 level)
{
    switch (BaseLevel(level))
    {
        case 1: return "Necrum Mines";
        case 2: return "Necrum";
        case 3: return "Mudomo Vault";
        case 4: return "Mudanchee Vault";
        case 5: return "FeeCo Depot";
        case 6: return "Slig Barracks";
        case 8: return "Bonewerkz";
        case 9: return "SoulStorm Brewery";
        default: return "this level";
    }
}

using LockReason = Lock::Reason;

// Why a door to this destination is locked, and for an area lock, the area (zulag) numbers the path holds.
LockReason WhyLocked(LevelIds level, s16 path, std::vector<s16>* areas)
{
    Refresh();
    return Lock::WhyLocked(
        sUnlocks, kAreas, sizeof(kAreas) / sizeof(kAreas[0]), static_cast<Lock::Level>(level), static_cast<Lock::Level>(path),
        static_cast<Lock::Level>(gMap_5C3030.field_0_current_level), areas);
}

} // namespace

void OnAbeEnteredPortal(LevelIds level, s16 path, s16 camera)
{
    // Whether the goal's count is met: Mudokon Saved items when they are the goal, otherwise the game's rescues (as at the ending).
    const bool enough = sGoalItems > 0 ? sSavedMudokons >= sGoalItems : (sGoodEnding > 0 ? sRescuedMudokons_5C1BC2 >= sGoodEnding : true);
    char_type buffer[192] = {};
    std::snprintf(
        buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"%s\",\"level\":%d,\"path\":%d,\"camera\":%d,\"enough\":%s}", static_cast<long long>(NowMs()),
        Lock::IsFinalPortalRoom(static_cast<Lock::Level>(level), static_cast<Lock::Level>(path), static_cast<Lock::Level>(camera)) ? "final_portal" : "abe_portal",
        static_cast<s32>(level), path, camera, enough ? "true" : "false");
    AppendLine(buffer);
}

bool IsFenceOpen(LevelIds level, s16 path, s32 x, s32 y)
{
    Refresh();
    return Lock::IsFenceOpen(sUnlocks, kFences, sizeof(kFences) / sizeof(kFences[0]), static_cast<Lock::Level>(level), static_cast<Lock::Level>(path), x, y);
}

bool IsDestinationLocked(LevelIds level, s16 path)
{
    return WhyLocked(level, path, nullptr) != Lock::Reason::eNone;
}

void OnLockedExitTried(LevelIds level, s16 path, const char* kind)
{
    static u32 sLastFrame = 0;
    if (sGnFrame_5C1B84 - sLastFrame < 45)
    {
        return;
    }
    sLastFrame = sGnFrame_5C1B84;

    SFX_Play_46FBA0(SoundEffect::SecurityDoorDeny_38, 90, 0);

    std::vector<s16> areas;
    const LockReason reason = WhyLocked(level, path, &areas);
    std::string text = "Locked";
    const auto need = sUnlocks.mNeeds.find(Lock::BaseLevel(static_cast<s16>(level)));
    if (reason == Lock::Reason::eLevelPass && need != sUnlocks.mNeeds.end() && !need->second.empty())
    {
        text = std::string("Locked: needs ") + need->second; // written by the client: the pass and/or the Mudokon count
    }
    else if (reason == Lock::Reason::eLevelPass)
    {
        text = std::string("Locked: needs the ") + LevelName(static_cast<s16>(level)) + " Pass";
    }
    else if (reason == Lock::Reason::eAreaAccess && !areas.empty())
    {
        text = std::string("Locked: needs ") + LevelName(static_cast<s16>(level)) + " Zulag " + std::to_string(areas[0]) + " Access";
        if (areas.size() > 1)
        {
            text += " (or one of " + std::to_string(areas.size() - 1) + " more)";
        }
    }
    AddMessage(text);

    char_type buffer[192] = {};
    std::snprintf(
        buffer, sizeof(buffer),
        "{\"t\":%lld,\"event\":\"locked_%s\",\"level\":%d,\"path\":%d,\"to\":{\"level\":%d,\"path\":%d}}",
        static_cast<long long>(NowMs()), kind,
        static_cast<s32>(gMap_5C3030.field_0_current_level), gMap_5C3030.field_2_current_path,
        static_cast<s32>(level), path);
    AppendLine(buffer);
}

void OnLockedDoorTried(LevelIds level, s16 path)
{
    OnLockedExitTried(level, path, "door");
}


// ---------------------------------------------------------------------------------------------------------
// Commands from the client, and the per-frame update

namespace {

struct Command final
{
    s32 mSeq = 0;          // > 0: carried out once, remembered in the ack file. 0: ephemeral.
    s64 mTime = 0;         // ephemeral commands only: when it was sent, in ms
    std::string mName;
    s32 mCount = 0;
    s32 mValue = 0;
    s32 mLevel = 0;
    s32 mPath = 0;
    s32 mCamera = 0;
    std::string mText;
};

std::vector<Command> sPending;
std::streamoff sCommandOffset = 0;
s32 sLastAck = 0;
bool sCommandsStarted = false;
s32 sGoodEnding = 0;
s32 sGoalItems = 0;       // with Mudokon Saved items: how many the good ending needs (0 = count rescues as usual)
s32 sSavedMudokons = 0;   // Mudokon Saved items received so far
bool sAbeWasDead = false;
u32 sFramesSincePoll = 0;

constexpr s64 kEphemeralLifetimeMs = 15000;
constexpr u32 kPollEveryFrames = 15;

void StartCommands()
{
    sCommandsStarted = true;
    std::ifstream in(kAckFile);
    s32 ack = 0;
    if (in >> ack)
    {
        sLastAck = ack;
    }
    // Numbered commands at or below this are skipped as already done, which is easy to forget when testing by hand.
    char_type buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"commands_start\",\"ack\":%d}", static_cast<long long>(NowMs()), sLastAck);
    AppendLine(buffer);
}

void SaveAck()
{
    std::ofstream out(kAckFile, std::ios::trunc);
    out << sLastAck << "\n";
}

void ReadNewCommands()
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(kCommandFile, ec);
    if (ec)
    {
        return;
    }
    if (static_cast<std::streamoff>(size) < sCommandOffset)
    {
        sCommandOffset = 0; // the client rewrote the file
    }
    if (static_cast<std::streamoff>(size) == sCommandOffset)
    {
        return;
    }

    std::ifstream in(kCommandFile, std::ios::binary);
    in.seekg(sCommandOffset);
    const std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Read the file as a series of JSON objects, not lines: a file saved without a final newline, or a command
    // appended straight onto the end of the last line, is still read correctly. A half-written object waits.
    std::vector<std::string> objects;
    sCommandOffset += static_cast<std::streamoff>(Json::ExtractObjects(chunk, objects));

    for (const std::string& object : objects)
    {
        Command cmd;
        cmd.mName = Json::String(object, "cmd");
        if (cmd.mName.empty())
        {
            continue;
        }
        cmd.mSeq = static_cast<s32>(Json::Int(object, "seq", 0));
        cmd.mTime = Json::Int(object, "t", 0);
        cmd.mCount = static_cast<s32>(Json::Int(object, "count", 0));
        cmd.mValue = static_cast<s32>(Json::Int(object, "value", 0));
        cmd.mText = Json::String(object, "text");
        cmd.mLevel = static_cast<s32>(Json::Int(object, "level", 0));
        cmd.mPath = static_cast<s32>(Json::Int(object, "path", 0));
        cmd.mCamera = static_cast<s32>(Json::Int(object, "camera", 0));
        // A "cfg" line is a standing setting: always applied, and the last one in the file wins.
        const bool isConfig = Json::Int(object, "cfg", 0) != 0;
        if (!isConfig && cmd.mSeq > 0 && cmd.mSeq <= sLastAck)
        {
            char_type buffer[128] = {};
            std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"command_skipped\",\"seq\":%d,\"ack\":%d,\"cmd\":\"%s\"}", static_cast<long long>(NowMs()), cmd.mSeq, sLastAck, cmd.mName.c_str());
            AppendLine(buffer);
            continue; // already done
        }
        if (!isConfig && cmd.mSeq == 0 && NowMs() - cmd.mTime > kEphemeralLifetimeMs)
        {
            continue; // too old to matter
        }
        if (cmd.mName == "message")
        {
            AddMessage(cmd.mText); // shown at once; it does not wait behind commands that need Abe in a level
            continue;
        }
        sPending.push_back(cmd);
    }
}

bool AbeIsDead()
{
    return sActiveHero_5C1B68 && sActiveHero_5C1B68->field_106_current_motion == eAbeMotions::Motion_57_Dead_4589A0;
}

bool InGameplay()
{
    const LevelIds level = gMap_5C3030.field_0_current_level;
    return sActiveHero_5C1B68 && level != LevelIds::eMenu_0 && level != LevelIds::eCredits_16 && level != LevelIds::eNone;
}

} // namespace

// Can the current path's throwables be used? The kind comes from the path (rocks in the Mines, bones, meat, grenades...), and each
// kind's sprites are in the level's own archive: Necrum has no rock sprites because nothing there is ever thrown. Loading a
// throwable whose file is missing crashes the game, so a Throwables item must not be given in such a level, and a save made with
// throwables in one cannot be loaded (Couldn't find PUIROCK.BAN in LVL).
bool ThrowablesUsableHere(bool duringLoad)
{
    // Looking in the archive while the map is changing cameras could catch it half set up, so then say yes without remembering it
    // (a save being loaded is the exception: the game itself is about to read the same archive).
    if (!duringLoad && gMap_5C3030.field_6_state != Map::CamChangeStates::eInactive_0)
    {
        return true;
    }
    static LevelIds sCheckedLevel = LevelIds::eNone;
    static s16 sCheckedPath = -1;
    static bool sUsable = true;
    const LevelIds level = gMap_5C3030.field_0_current_level;
    const s16 path = gMap_5C3030.field_2_current_path;
    if (level == sCheckedLevel && path == sCheckedPath)
    {
        return sUsable; // the archive does not change while we stay on this path
    }
    sCheckedLevel = level;
    sCheckedPath = path;
    sUsable = true;
    if (level == LevelIds::eMenu_0 || level == LevelIds::eCredits_16 || level == LevelIds::eNone)
    {
        return sUsable;
    }
    const PathBlyRec* pRec = Path_Get_Bly_Record_460F30(level, static_cast<u16>(path));
    if (!pRec)
    {
        return sUsable;
    }
    const char_type* file = nullptr;
    switch (throwable_types_55FAFC[LOBYTE(pRec->field_C_overlay_id)])
    {
        case AETypes::eBone_11: file = "BONE.BAN"; break;
        case AETypes::eMetal_24: file = "METAL.BAN"; break;
        case AETypes::eGrenade_65: file = "GRENADE.BAN"; break;
        case AETypes::eMeat_84: file = "MEAT.BAN"; break;
        case AETypes::eRock_105: file = "PUIROCK.BAN"; break;
        default: break; // not a kind we know: leave it to the game
    }
    if (file && !sLvlArchive_5BC520.Find_File_Record_433160(file))
    {
        sUsable = false; // the lookup has logged "Couldn't find ... in LVL"; that is expected here
        char_type buffer[160] = {};
        std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"throwables_unusable\",\"level\":%d,\"path\":%d,\"missing\":\"%s\"}", static_cast<long long>(NowMs()), static_cast<s32>(level), path, file);
        AppendLine(buffer);
    }
    return sUsable;
}

namespace {

// Abe is carrying throwables that this level cannot use (a save made before the check existed, or carried over somehow): take
// them away rather than crash the first time one is thrown.
void DropUnusableThrowables()
{
    if (!sActiveHero_5C1B68 || sActiveHero_5C1B68->field_1A2_throwable_count <= 0 || ThrowablesUsableHere())
    {
        return;
    }
    const s32 count = sActiveHero_5C1B68->field_1A2_throwable_count;
    if (gpThrowableArray_5D1E2C)
    {
        gpThrowableArray_5D1E2C->Remove_49AA00(static_cast<s16>(count));
    }
    sActiveHero_5C1B68->field_1A2_throwable_count = 0;
    char_type buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"throwables_dropped\",\"count\":%d}", static_cast<long long>(NowMs()), count);
    AppendLine(buffer);
    AddMessage("Nothing can be thrown here: throwables removed");
}

void GiveThrowables(s32 count)
{
    // The game decides the kind of throwable by level, so this is "more of what this level uses".
    // Same sequence the pickups use: make the array if it does not exist, add to it, and count them on Abe.
    if (!gpThrowableArray_5D1E2C)
    {
        gpThrowableArray_5D1E2C = ae_new<ThrowableArray>();
        gpThrowableArray_5D1E2C->ctor_49A630();
    }
    const s32 room = 99 - static_cast<s32>(sActiveHero_5C1B68->field_1A2_throwable_count);
    const s32 amount = std::min(count, std::max(room, 0));
    if (amount > 0)
    {
        gpThrowableArray_5D1E2C->Add_49A7A0(static_cast<s16>(amount));
        sActiveHero_5C1B68->field_1A2_throwable_count = static_cast<s8>(sActiveHero_5C1B68->field_1A2_throwable_count + amount);
    }
}

// The Fart Trap: Abe lets one go. The same sound, game speak event, noise for Sligs to hear and puff of smoke as the
// player's own fart (Abe::Motion_0_Idle), without needing a button press. Only a standing Abe also does the fart animation,
// because forcing that motion on a running or jumping Abe would break his state.
void Fart()
{
    Abe* pAbe = sActiveHero_5C1B68;
    Mudokon_SFX_457EC0(MudSounds::eFart_7, 0, 0, pAbe);
    pEventSystem_5BC11C->PushEvent_4218D0(GameSpeakEvents::eFart_3);
    Event_Broadcast_422BC0(kEventNoise, pAbe);
    Event_Broadcast_422BC0(kEventSuspiciousNoise, pAbe);

    const FP scale = FP_FromDouble(0.5) * pAbe->field_CC_sprite_scale;
    const FP y = pAbe->field_BC_ypos - (FP_FromInteger(24) * pAbe->field_CC_sprite_scale);
    const FP offset = FP_FromInteger(12) * pAbe->field_CC_sprite_scale;
    const FP x = pAbe->field_20_animation.field_4_flags.Get(AnimFlags::eBit5_FlipX) ? pAbe->field_B8_xpos + offset : pAbe->field_B8_xpos - offset;
    New_Smoke_Particles_426C70(x, y, scale, 3, 32u, 128u, 32u);

    const bool idle = pAbe->field_106_current_motion == eAbeMotions::Motion_0_Idle_44EEB0;
    if (idle)
    {
        pAbe->field_106_current_motion = eAbeMotions::Motion_10_Fart_45B1A0;
    }
    char_type buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"fart\",\"idle\":%s}", static_cast<long long>(NowMs()), idle ? "true" : "false");
    AppendLine(buffer);
}

// TESTING ONLY. Copies what the game's debug teleport menu does: point the map at a camera, then on a later frame, once it
// has loaded, put Abe in it. Lets a locked well or door far into the game be tried in seconds.
struct Teleport final
{
    bool mActive = false;
    LevelIds mLevel = LevelIds::eNone;
    s16 mPath = 0;
    s16 mCamera = 0;
    u32 mStartedAtFrame = 0;
};
Teleport sTeleport;

void StepTeleport()
{
    if (!sTeleport.mActive)
    {
        return;
    }
    const bool arrived = gMap_5C3030.field_6_state == Map::CamChangeStates::eInactive_0
        && gMap_5C3030.field_0_current_level == sTeleport.mLevel && gMap_5C3030.field_2_current_path == sTeleport.mPath
        && gMap_5C3030.field_4_current_camera == sTeleport.mCamera;
    if (arrived && sActiveHero_5C1B68)
    {
        PSX_Point pos;
        gMap_5C3030.GetCurrentCamCoords_480680(&pos);
        sActiveHero_5C1B68->field_B8_xpos = FP_FromInteger(pos.field_0_x + 184);
        sActiveHero_5C1B68->field_BC_ypos = FP_FromInteger(pos.field_2_y + 60);
        sActiveHero_5C1B68->field_106_current_motion = 3;
        sActiveHero_5C1B68->field_1AC_flags.Set(Abe::e1AC_Bit7_land_softly);
        sActiveHero_5C1B68->field_C2_lvl_number = sTeleport.mLevel;
        sActiveHero_5C1B68->field_C0_path_number = sTeleport.mPath;
        sDDCheat_FlyingEnabled_5C2C08 = false;
        sControlledCharacter_5C1B8C->field_100_pCollisionLine = nullptr;
        sControlledCharacter_5C1B8C->field_F8_LastLineYPos = sControlledCharacter_5C1B8C->field_BC_ypos;
        sTeleport.mActive = false;

        char_type buffer[128] = {};
        std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"teleported\",\"level\":%d,\"path\":%d,\"camera\":%d}", static_cast<long long>(NowMs()), static_cast<s32>(sTeleport.mLevel), sTeleport.mPath, sTeleport.mCamera);
        AppendLine(buffer);
    }
    else if (sGnFrame_5C1B84 - sTeleport.mStartedAtFrame > 300)
    {
        sDDCheat_FlyingEnabled_5C2C08 = false; // the camera never arrived: give up rather than leave Abe flying
        sTeleport.mActive = false;
        AddMessage("Teleport failed");
    }
}

// Returns true when the command is finished (done, or not worth keeping).
bool Execute(const Command& cmd)
{
    if (cmd.mName == "set_good_ending")
    {
        sGoodEnding = cmd.mValue; // needs no Abe
        return true;
    }
    if (cmd.mName == "set_goal_items")
    {
        sGoalItems = cmd.mValue;
        return true;
    }
    if (cmd.mName == "set_saved_mudokons")
    {
        sSavedMudokons = cmd.mValue;
        return true;
    }
    if (!InGameplay())
    {
        return false; // try again once Abe is in a level
    }
    if (cmd.mName == "give_throwables")
    {
        if (!ThrowablesUsableHere())
        {
            return false; // waits (the other commands go on) until Abe is on a path whose level has throwables
        }
        GiveThrowables(cmd.mCount);
    }
    else if (cmd.mName == "alarm")
    {
        // The game's Alarm destroys itself if another alarm is already counted as active.
        const s16 instancesBefore = alarmInstanceCount_5C1BB4;
        bool diedAtOnce = false;
        if (auto* pAlarm = ae_new<Alarm>())
        {
            // Starts on the next frame and runs for 120 frames (about four seconds): a few pulses and buzzes.
            pAlarm->ctor_4091F0(120, 0, 0, Layer::eLayer_Above_FG1_39);
            diedAtOnce = pAlarm->field_6_flags.Get(BaseGameObject::eDead_Bit3);
        }

        char_type buffer[160] = {};
        std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"alarm\",\"instances_before\":%d,\"died_at_once\":%s}", static_cast<long long>(NowMs()), instancesBefore, diedAtOnce ? "true" : "false");
        AppendLine(buffer);
    }
    else if (cmd.mName == "fart")
    {
        if (!AbeIsDead())
        {
            Fart();
        }
    }
    else if (cmd.mName == "teleport")
    {
        if (!sTeleport.mActive)
        {
            sTeleport = {true, static_cast<LevelIds>(cmd.mLevel), static_cast<s16>(cmd.mPath), static_cast<s16>(cmd.mCamera), sGnFrame_5C1B84};
            sDDCheat_FlyingEnabled_5C2C08 = true;
            gMap_5C3030.SetActiveCam_480D30(sTeleport.mLevel, sTeleport.mPath, sTeleport.mCamera, CameraSwapEffects::eInstantChange_0, 0, 0);
        }
    }
    else if (cmd.mName == "kill")
    {
        if (!AbeIsDead())
        {
            if (auto* pBomb = ae_new<BaseBomb>())
            {
                pBomb->ctor_423E70(sActiveHero_5C1B68->field_B8_xpos, sActiveHero_5C1B68->field_BC_ypos, 0, sActiveHero_5C1B68->field_CC_sprite_scale);
            }
        }
    }
    else
    {
        char_type buffer[160] = {};
        std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"unknown_command\",\"cmd\":\"%s\"}", static_cast<long long>(NowMs()), cmd.mName.c_str());
        AppendLine(buffer);
    }
    return true;
}

} // namespace

s32 GoodEndingOverride()
{
    return sGoodEnding;
}

s32 SavedMudokonsShown(s32 rescued)
{
    return sGoalItems > 0 ? sSavedMudokons : rescued;
}

bool GoodEndingAllowed(bool enoughRescues)
{
    if (sGoalItems <= 0)
    {
        return enoughRescues; // rescues are what count
    }
    if (sSavedMudokons >= sGoalItems)
    {
        return true;
    }
    char_type text[80] = {};
    std::snprintf(text, sizeof(text), "Need %d Mudokon Saved items, have %d", sGoalItems, sSavedMudokons);
    AddMessage(text);
    return false;
}

void OnEnding(EndingKind kind)
{
    char_type buffer[160] = {};
    std::snprintf(
        buffer, sizeof(buffer),
        "{\"t\":%lld,\"event\":\"ending\",\"kind\":\"%s\",\"rescued\":%d,\"killed\":%d}",
        static_cast<long long>(NowMs()),
        kind == EndingKind::eAngelic ? "angelic" : kind == EndingKind::eGood ? "good" : kind == EndingKind::eBad ? "bad" : "very_bad",
        sRescuedMudokons_5C1BC2, sKilledMudokons_5C1BC0);
    AppendLine(buffer);
}

void Tick()
{
    if (!sCommandsStarted)
    {
        StartCommands();
    }

    // Abe dying is reported once per death (Death Link).
    const bool dead = AbeIsDead();
    if (dead && !sAbeWasDead && InGameplay())
    {
        char_type buffer[96] = {};
        std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"abe_died\"}", static_cast<long long>(NowMs()));
        AppendLine(buffer);
    }
    sAbeWasDead = dead;

    // The game puts its rescued count back when a save is loaded or Abe restarts, which no other event shows.
    static s16 sPreviousRescued = 0;
    if (sRescuedMudokons_5C1BC2 < sPreviousRescued)
    {
        char_type buffer[128] = {};
        std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"counters_restored\",\"from\":%d,\"to\":%d}", static_cast<long long>(NowMs()), sPreviousRescued, sRescuedMudokons_5C1BC2);
        AppendLine(buffer);
    }
    sPreviousRescued = sRescuedMudokons_5C1BC2;

    StepTeleport();
    DrawMessages();

    if (++sFramesSincePoll >= kPollEveryFrames)
    {
        sFramesSincePoll = 0;
        ReadNewCommands();
    }

    DropUnusableThrowables();

    // Carry out waiting commands in order. One that cannot run yet (no Abe in a level, or no throwables on this path) waits while
    // the others go on. The ack never passes a waiting command, so after a restart it is still carried out.
    static s32 sHighestDone = 0;
    for (size_t i = 0; i < sPending.size();)
    {
        const Command cmd = sPending[i];
        if (!Execute(cmd))
        {
            ++i;
            continue;
        }
        sPending.erase(sPending.begin() + static_cast<std::ptrdiff_t>(i));
        if (cmd.mSeq > 0)
        {
            char_type buffer[128] = {};
            std::snprintf(buffer, sizeof(buffer), "{\"t\":%lld,\"event\":\"command_done\",\"seq\":%d,\"cmd\":\"%s\"}", static_cast<long long>(NowMs()), cmd.mSeq, cmd.mName.c_str());
            AppendLine(buffer);
            sHighestDone = std::max(sHighestDone, cmd.mSeq);
        }
    }
    s32 ack = sHighestDone;
    for (const Command& waiting : sPending)
    {
        if (waiting.mSeq > 0)
        {
            ack = std::min(ack, waiting.mSeq - 1);
        }
    }
    if (ack > sLastAck)
    {
        sLastAck = ack;
        SaveAck();
    }
}

} // namespace Archipelago

#endif
