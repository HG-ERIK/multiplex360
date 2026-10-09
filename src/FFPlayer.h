#pragma once
#include <xtl.h>
#include <string>
#include <vector>
#include <deque>
#include "ffglue.h"

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;
namespace Http { class Stream; }

struct SubtitleCue
{
    double      start, end;     // seconds, media time
    std::string text;           // UTF-8
};

// FFmpeg-based player. Audio (XAudio2) is the master clock. Public methods: render thread only.
class FFPlayer
{
public:
    FFPlayer();
    ~FFPlayer();

    bool Startup( IDirect3DDevice9* device );

    // urls are tried in order. timeOffset is added to displayed times (a transcode that
    // starts mid-movie); durationHint is for streams that don't know their length.
    void Open( const std::vector<std::string>& urls, const std::vector<std::wstring>& labels,
               const std::string& headers, double startSeconds = 0.0, double timeOffset = 0.0,
               double durationHint = 0.0 );
    void Stop();
    bool IsActive() const { return m_active; }

    void Update();
    void TogglePause();
    void SeekRelative( int seconds );
    void SeekAbsolute( double seconds );    // in displayed (movie) time
    bool IsPaused() const { return m_paused; }

    void SetSpeedTest( bool on ) { m_speedTest = on; }
    bool IsSeeking() const { return m_seekRequest != 0; }
    bool IsBuffering() const { return m_active && ( !m_opened || !m_haveFrame || m_seekRequest ); }

    void RenderFrame( const D3DRECT& screen );

    std::wstring StatusText() const;

    // Numbers for the stats-for-nerds overlay.
    struct Stats
    {
        long    decoded, shown, dropped, catchups;
        int     videoPackets, audioPackets;
        int     audioBufferedMs;
        __int64 bytesRead;
        int     width, height;
        bool    skippingB;
    };
    void         GetStats( Stats& s );
    std::wstring MediaInfo() const;     // "h264 1280x720 / aac  [direct play]"
    bool         Failed() const { return !m_error.empty() && !m_active; }
    const std::string& Error() const { return m_error; }
    double       Position() const;
    double       Duration() const { return m_duration; }
    bool         CanSeek() const  { return m_opened && m_streamSize >= 0; }   // live transcodes can't

    // Audio and subtitle tracks, once the file is open.
    std::vector<FgTrack> Tracks();
    // Embedded subtitle track to show (container index, -1 = none), or cues from a file.
    void         SetSubtitleTrack( int index );
    void         SetExternalSubtitles( const std::vector<SubtitleCue>& cues );
    std::string  SubtitleAt( double seconds );    // UTF-8, empty if nothing is shown

private:
    // --- worker threads ---
    static DWORD WINAPI DemuxThread( LPVOID p );
    static DWORD WINAPI VideoThread( LPVOID p );
    static DWORD WINAPI AudioThread( LPVOID p );
    void  DemuxLoop();
    void  VideoLoop();
    void  AudioLoop();
    bool  OpenMedia( std::string& error );
    bool  OpenStream( std::string& error );
    void  AddSubtitle( FgPacket* pkt );
    void  DoSeek( double seconds );

    // --- byte source for FFmpeg (demux thread) ---
    static int SourceRead( void* opaque, unsigned char* buf, int size );
    static int SourceSeek( void* opaque, __int64 pos );

    // --- packet queues ---
    struct PacketQueue
    {
        std::deque<FgPacket*> items;
        bool                  eof;
        PacketQueue() : eof( false ) {}
    };
    FgPacket*  Pop( PacketQueue& q, HANDLE dataEvent, bool& eof );   // NULL when stopping/seeking/empty+eof
    void       Push( PacketQueue& q, HANDLE dataEvent, FgPacket* p );
    void       ClearQueue( PacketQueue& q );

    // --- decoded video frames ---
    struct Frame
    {
        std::vector<BYTE> data;     // Y, then U, then V, tightly packed
        int               width, height;
        double            pts;
        int               colorspace, fullRange;
        volatile LONG     ready;
    };
    enum { FRAME_COUNT = 4 };

    // --- audio ---
    bool   SubmitAudio( const short* pcm, int bytes, double pts, int rate, int channels );
    double Clock() const;

    // --- rendering ---
    bool   CreateShaders();
    void   EnsureTextures( int w, int h );
    void   Upload( const Frame& f );

    IDirect3DDevice9*          m_device;
    IDirect3DVertexShader9*    m_vs;
    IDirect3DPixelShader9*     m_ps;
    IDirect3DVertexDeclaration9* m_decl;
    IDirect3DTexture9*         m_tex[3];
    int                        m_texW, m_texH;
    bool                       m_haveFrame;     // textures hold a picture
    int                        m_frameW, m_frameH;
    int                        m_frameColorspace, m_frameFullRange;
    float                      m_brightness;    // added to luma, for dark transfers (-0.1..0.2)
    int                        m_aspect;        // ASPECT_*
    double                     m_pixelAspect;   // the file's pixel shape (anamorphic DVDs), 1 = square
    float                      m_tvAspect;      // 16:9 or 4:3, from the console's display setting
    bool                       m_screenSaverOff;

    void   KeepScreenAwake( bool awake );

public:
    // Auto keeps the picture's shape on the TV (4:3 sets included); Zoom fills the screen
    // and crops; Stretch fills it and distorts; 4:3 / 16:9 force the picture's shape.
    enum { ASPECT_AUTO, ASPECT_ZOOM, ASPECT_STRETCH, ASPECT_4_3, ASPECT_16_9, ASPECT_COUNT };
    static const wchar_t* AspectName( int mode );
    void   SetAspect( int mode )    { m_aspect = mode >= 0 && mode < ASPECT_COUNT ? mode : ASPECT_AUTO; }
    int    Aspect() const           { return m_aspect; }

    void   SetBrightness( float b ) { m_brightness = b; }
    float  Brightness() const       { return m_brightness; }
    std::wstring ColorInfo() const;
private:

    IXAudio2*                  m_xaudio;
    IXAudio2MasteringVoice*    m_master;
    IXAudio2SourceVoice*       m_voice;
    int                        m_voiceRate, m_voiceChannels;
    enum { AUDIO_BUFFERS = 12, AUDIO_BUFFER_BYTES = 192000 * 2 };
    std::vector<BYTE>          m_audioBuf[AUDIO_BUFFERS];
    std::vector<BYTE>          m_silence;   // all zero, looped while sound starts after the picture
    int                        m_audioNext;
    UINT64                     m_samplesAtBase;  // voice SamplesPlayed when m_audioBasePts was queued
    double                     m_audioBasePts;
    volatile LONG              m_audioStarted;

    // --- state shared with workers ---
    CRITICAL_SECTION           m_lock;
    // Auto-reset events wake one waiter, so each kind of waiting has its own event.
    HANDLE                     m_videoData;     // a video packet was queued (video thread waits)
    HANDLE                     m_audioData;     // an audio packet was queued (audio thread waits)
    HANDLE                     m_room;          // a packet was taken (demux waits for room)
    HANDLE                     m_frameFree;     // a frame slot was freed (video thread waits)
    void                       WakeAll();       // quit / seek: wake everyone
    HANDLE                     m_threads[3];
    volatile LONG              m_quit;
    volatile LONG              m_seekRequest;   // 1 when m_seekTarget is pending
    double                     m_seekTarget;
    volatile LONG              m_flushing;      // decoders must drop what they hold
    volatile LONG              m_decodersParked;
    PacketQueue                m_videoQ, m_audioQ;
    Frame                      m_frames[FRAME_COUNT];

    std::vector<std::string>   m_urls;
    std::vector<std::wstring>  m_labels;
    std::string                m_headers;
    size_t                     m_current;
    Http::Stream*              m_stream;
    std::vector<FgTrack>       m_tracks;
    std::vector<SubtitleCue>   m_cues;          // from the selected embedded track
    std::vector<SubtitleCue>   m_external;      // from a subtitle file
    volatile LONG              m_subWanted;
    volatile LONG              m_subChanged;
    HANDLE                     m_file;          // local file instead of a stream (paths without "://")
    __int64                    m_streamPos;
    __int64                    m_streamSize;
    FgMedia*                   m_media;
    bool                       m_hasAudio;
    bool                       m_hasVideo;
    volatile DWORD             m_lastShowTick;   // when the render thread last showed a new frame
    double                     m_duration;
    double                     m_startSeconds;
    double                     m_timeOffset;

    bool                       m_active;
    volatile LONG              m_opened;
    volatile LONG              m_ended;
    bool                       m_paused;
    DWORD                      m_wallStart;     // clock for files without audio
    double                     m_wallBase;
    double                     m_lastPts;
    std::string                m_error;
    std::wstring               m_info;          // "h264 1280x720 / ac3"

    // stats
    volatile LONG              m_decoded, m_dropped, m_shown, m_catchups;
    volatile __int64           m_bytesRead;
    bool                       m_skippingB;     // video thread only
    int                        m_catchLevel;    // fg_set_fast level in use (video thread)
    volatile double            m_seekFloor;     // after a seek: frames and sound before this are skipped
    volatile LONG              m_reachingFloor; // video is still decoding its way up to m_seekFloor
    volatile LONG              m_connectionLost; // reconnecting gave up: the end is an error, not the film's end
    bool                       m_speedTest;
    DWORD                      m_statsTick;
};
