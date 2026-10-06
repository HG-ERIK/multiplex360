#pragma once
#include <stdlib.h>

// Transcode presets: resolution and bitrate always come as a pair.
struct QualityPreset
{
    int            height;
    int            kbps;
    const wchar_t* bitrateLabel;    // shown in the bitrate list of its resolution
};

const QualityPreset QUALITY_PRESETS[] =
{
    { 720, 6000, L"6 Mbps  (best)" },
    { 720, 4000, L"4 Mbps" },
    { 720, 3000, L"3 Mbps  (slower internet)" },
    { 576, 3000, L"3 Mbps" },
    { 576, 2000, L"2 Mbps" },
    { 480, 2000, L"2 Mbps" },
    { 480, 1500, L"1.5 Mbps" },
    { 360, 1000, L"1 Mbps" },
};
const int QUALITY_PRESET_COUNT = sizeof( QUALITY_PRESETS ) / sizeof( QUALITY_PRESETS[0] );

const int QUALITY_HEIGHTS[] = { 720, 576, 480, 360 };
const int QUALITY_HEIGHT_COUNT = sizeof( QUALITY_HEIGHTS ) / sizeof( QUALITY_HEIGHTS[0] );

// The preset closest to a height/bitrate pair (e.g. from an older settings.ini).
inline int NearestQualityPreset( int height, int kbps )
{
    int best = 1, bestScore = 0x7FFFFFFF;
    for( int i = 0; i < QUALITY_PRESET_COUNT; ++i )
    {
        int score = abs( QUALITY_PRESETS[i].height - height ) * 100 + abs( QUALITY_PRESETS[i].kbps - kbps );
        if( score < bestScore )
        {
            bestScore = score;
            best = i;
        }
    }
    return best;
}
