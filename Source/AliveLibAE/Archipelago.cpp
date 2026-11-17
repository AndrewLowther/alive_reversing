#include "stdafx.h"
#include "Archipelago.hpp"

#if ARCHIPELAGO_CLIENT

#include "Factory.hpp"
#include "PathData.hpp"

#include <chrono>
#include <cstdio>

namespace Archipelago {

static const char_type* kEventLogFile = "archipelago_events.jsonl";

static s64 NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// One JSON object per line, flushed immediately so a crash loses nothing.
static void AppendLine(const char_type* line)
{
    if (FILE* f = std::fopen(kEventLogFile, "a"))
    {
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
        "\"tlv\":{\"offset\":%u,\"level\":%u,\"path\":%u},\"total_rescued\":%d,\"zulag_saved_killed\":%d}",
        static_cast<long long>(NowMs()),
        static_cast<s32>(event.mLevel), event.mPath, event.mZulag,
        static_cast<u32>(tlv.parts.tlvOffset), static_cast<u32>(tlv.parts.levelId), static_cast<u32>(tlv.parts.pathId),
        event.mTotalRescued, event.mZulagSavedKilled);
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

} // namespace Archipelago

#endif
