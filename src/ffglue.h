/* Thin C wrapper around FFmpeg 0.6 (FFPlay360's Xbox 360 port) so the C++ code never
   includes FFmpeg headers. Not thread-safe per media object: demux on one thread, and
   decode each stream on one thread. */
#ifndef FFGLUE_H
#define FFGLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Byte source FFmpeg reads from (our Http::Stream, implemented in C++). */
typedef struct FgSource
{
    void*   opaque;
    int     ( *read )( void* opaque, unsigned char* buf, int size );   /* <0 error, 0 end */
    int     ( *seek )( void* opaque, __int64 pos );                    /* 0 ok */
    __int64 size;                                                     /* -1 if unknown */
} FgSource;

typedef struct FgMedia FgMedia;

typedef struct FgInfo
{
    int     hasVideo, hasAudio;
    int     width, height;
    double  fps;
    double  duration;           /* seconds, 0 if unknown */
    int     sampleRate, channels;
    char    videoCodec[32], audioCodec[32], container[32];
    int     threads;            /* video decode threads in use */
    int     frameThreading;     /* 1 frame, 2 slice, 0 none */
    int     profile, level, refs;
    double  pixelAspect;        /* width:height of one pixel as the file wants it shown; 0 if unknown */
} FgInfo;

/* Decoded picture, always YUV 4:2:0 planar; pointers valid until the next decode call. */
typedef struct FgPicture
{
    const unsigned char* plane[3];
    int                  pitch[3];
    int                  width, height;
    double               pts;           /* seconds */
    int                  colorspace;    /* FG_CS_* as tagged in the stream */
    int                  fullRange;     /* 1 = 0-255 (JPEG) levels, 0 = 16-235 TV levels */
} FgPicture;

enum { FG_CS_UNKNOWN = 0, FG_CS_BT601 = 1, FG_CS_BT709 = 2 };

typedef struct FgPacket FgPacket;
enum { FG_PACKET_VIDEO = 1, FG_PACKET_AUDIO = 2, FG_PACKET_OTHER = 3, FG_PACKET_SUBTITLE = 4 };

/* log receives FFmpeg's messages (info and above) and open-progress lines. */
typedef void ( *FgLogFn )( const char* line );
void     fg_init( FgLogFn log );

/* 1-4 frame-threaded video decode threads for files opened afterwards (default 4). */
void     fg_set_decode_threads( int n );

/* 1 (default): AC3/DTS are mixed down to stereo in the decoder. 0: keep all channels. */
void     fg_set_stereo( int on );

/* Audio track (container stream index, as Plex reports it) for files opened afterwards;
   -1 = the first one. */
void     fg_set_audio_track( int containerIndex );

/* name: shown in logs/probing only. Returns NULL and fills err on failure. */
FgMedia* fg_open( FgSource* src, const char* name, char* err, int errSize );
void     fg_close( FgMedia* m );
void     fg_get_info( FgMedia* m, FgInfo* info );

/* Reads the next packet. Returns its type, 0 at end of stream, <0 on error. */
int      fg_read_packet( FgMedia* m, FgPacket** pkt );
void     fg_free_packet( FgPacket* pkt );
double   fg_packet_time( FgMedia* m, FgPacket* pkt );     /* seconds, <0 if unknown */

/* 1 = picture produced, 0 = needs more data, <0 = error. */
int      fg_decode_video( FgMedia* m, FgPacket* pkt, FgPicture* pic );

/* Decodes into pcm (16-bit interleaved). Returns bytes written (0 = none), <0 on error. */
int      fg_decode_audio( FgMedia* m, FgPacket* pkt, short* pcm, int pcmBytes, double* pts );

/* Audio and subtitle tracks of an open file. */
typedef struct FgTrack
{
    int  index;             /* container stream index */
    int  type;              /* 2 audio, 3 subtitles */
    int  channels;
    int  text;              /* subtitles we can draw (text, not pictures) */
    char codec[32], language[16], title[64];
} FgTrack;

int      fg_track_count( FgMedia* m );
void     fg_get_track( FgMedia* m, int n, FgTrack* track );

/* Subtitle stream to read (-1 = none). Call from the thread that reads packets. */
void     fg_select_subtitle( FgMedia* m, int containerIndex );

/* Text of a subtitle packet (UTF-8, ASS fields stripped), its start and length in seconds.
   1 if it had text. */
int      fg_subtitle( FgMedia* m, FgPacket* pkt, char* text, int size, double* start, double* duration );

/* Seek to seconds (keyframe at or before). Flushes the decoders. 0 ok. */
int      fg_seek( FgMedia* m, double seconds );

/* Catch-up level when decoding falls behind: 0 normal, 1 skip deblocking of B-frames,
   2 also skip decoding B-frames. */
void     fg_set_fast( FgMedia* m, int level );

/* H.264 decode time since the last reset, summed over decode threads (milliseconds):
   entropy decoding, reconstruction (motion compensation etc.; includes waiting for other threads'
   reference frames, also reported alone as waitMs), loop filter. */
/* VMX chroma MC: -2 not tested yet, 0 in use, >0 self-test mismatches (C code kept). */
int      fg_vmx_status( void );
/* VMX luma quarter-pel MC, same codes. */
int      fg_vmx_qpel_status( void );

void     fg_profile( double* cabacMs, double* reconMs, double* filterMs, double* waitMs, int reset );

#ifdef __cplusplus
}
#endif

#endif
