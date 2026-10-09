/* See ffglue.h. Compiled as C against external/ffmpeg360 (FFmpeg 0.7 era API). */
#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "ffglue.h"

#define IO_BUFFER_SIZE  ( 64 * 1024 )
/* Video decode threads (FFmpeg frame threading). Needs the native xbthreads layer:
   pthreads-win32 froze the console. Workers run on hardware threads 3, 4, 5, 2. */
static int g_decodeThreads = 4;
static int g_stereo = 1;

void fg_set_decode_threads( int n ) { g_decodeThreads = n < 1 ? 1 : ( n > 6 ? 6 : n ); }
void fg_set_stereo( int on ) { g_stereo = on; }

/* Container stream index of the audio track to play (-1 = first audio track). */
static int g_preferredAudio = -1;

void fg_set_audio_track( int containerIndex ) { g_preferredAudio = containerIndex; }

static FgLogFn g_log;   /* set by fg_init */

struct FgPacket
{
    AVPacket pkt;
};

struct FgMedia
{
    FgSource          src;
    AVIOContext*      io;
    unsigned char*    ioBuffer;
    AVFormatContext*  fmt;
    int               videoIndex, audioIndex, subIndex;
    AVCodecContext*   vctx;
    AVCodecContext*   actx;
    AVFrame*          frame;
    struct SwsContext* sws;       /* only for non-4:2:0 sources */
    AVPicture         converted;
    int               convertedAlloc;
};

static int ReadCb( void* opaque, uint8_t* buf, int size )
{
    FgMedia* m = (FgMedia*)opaque;
    int n = m->src.read( m->src.opaque, buf, size );
    return n < 0 ? AVERROR( EIO ) : n;
}

static int64_t SeekCb( void* opaque, int64_t offset, int whence )
{
    FgMedia* m = (FgMedia*)opaque;
    if( whence == AVSEEK_SIZE )
        return m->src.size;
    whence &= ~AVSEEK_FORCE;
    if( whence != SEEK_SET )
        return -1;   /* avio handles SEEK_CUR itself; SEEK_END needs a size */
    return m->src.seek( m->src.opaque, offset ) == 0 ? offset : -1;
}

static void SetErr( char* err, int size, const char* msg )
{
    if( err && size > 0 )
        strncpy_s( err, size, msg, _TRUNCATE );
}

static AVCodecContext* OpenDecoder( AVStream* st, int isVideo )
{
    AVCodecContext* ctx = st->codec;
    AVCodec* codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
        return NULL;
    if( isVideo )
    {
        ctx->thread_count = g_decodeThreads;
        ctx->thread_type = FF_THREAD_FRAME;
    }
    else
    {
        if( g_stereo )
            ctx->request_channels = 2;    /* AC3/DTS downmix in the decoder */
    }
    if( avcodec_open( ctx, codec ) < 0 )
        return NULL;
    if( isVideo && g_log )
    {
        char line[200];
        sprintf_s( line, sizeof( line ), "fg_open: video decoder %s, %d threads, active threading %s, profile %d level %d, refs %d, "
                   "b-frames %d, %s\n", codec->name, ctx->thread_count,
                   ctx->active_thread_type == FF_THREAD_FRAME ? "frame" :
                   ctx->active_thread_type == FF_THREAD_SLICE ? "slice" : "none",
                   ctx->profile, ctx->level, ctx->refs, ctx->has_b_frames,
                   ( ctx->flags & CODEC_FLAG_LOW_DELAY ) ? "low delay" : "normal delay" );
        g_log( line );
    }
    return ctx;
}

static FgLogFn g_log = NULL;

static void LogCb( void* avcl, int level, const char* fmt, va_list vl )
{
    char line[512];
    if( !g_log || level > AV_LOG_INFO )
        return;
    vsnprintf_s( line, sizeof( line ), _TRUNCATE, fmt, vl );
    g_log( line );
}

void fg_init( FgLogFn log )
{
    g_log = log;
    av_log_set_callback( LogCb );
    avcodec_register_all();
    av_register_all();
}

#define STEP( msg ) do { if( g_log ) g_log( "fg_open: " msg "\n" ); } while( 0 )

FgMedia* fg_open( FgSource* src, const char* name, char* err, int errSize )
{
    FgMedia* m = (FgMedia*)av_mallocz( sizeof( FgMedia ) );
    AVInputFormat* ifmt = NULL;
    unsigned i;

    m->src = *src;
    m->videoIndex = m->audioIndex = m->subIndex = -1;
    m->ioBuffer = (unsigned char*)av_malloc( IO_BUFFER_SIZE );
    m->io = avio_alloc_context( m->ioBuffer, IO_BUFFER_SIZE, 0, m, ReadCb, NULL, SeekCb );
    if( src->size < 0 )
        m->io->seekable = 0;

    STEP( "probe" );
    if( av_probe_input_buffer( m->io, &ifmt, name, NULL, 0, 0 ) < 0 || !ifmt )
    {
        SetErr( err, errSize, "Unknown container format" );
        fg_close( m );
        return NULL;
    }
    STEP( "open input" );
    if( av_open_input_stream( &m->fmt, m->io, name, ifmt, NULL ) < 0 )
    {
        SetErr( err, errSize, "Could not open the stream" );
        fg_close( m );
        return NULL;
    }
    STEP( "find stream info" );
    /* Matroska/MP4 headers already describe the streams; don't read seconds of a live
       transcode to confirm it (this made every transcode seek take ~7 s). */
    m->fmt->max_analyze_duration = AV_TIME_BASE / 2;
    m->fmt->probesize = 512 * 1024;
    if( av_find_stream_info( m->fmt ) < 0 )
    {
        SetErr( err, errSize, "Could not read stream info" );
        fg_close( m );
        return NULL;
    }

    for( i = 0; i < m->fmt->nb_streams; ++i )
    {
        enum AVMediaType t = m->fmt->streams[i]->codec->codec_type;
        if( t == AVMEDIA_TYPE_VIDEO && m->videoIndex < 0 )
            m->videoIndex = i;
        else if( t == AVMEDIA_TYPE_AUDIO && ( m->audioIndex < 0 || (int)i == g_preferredAudio ) )
            m->audioIndex = i;
    }
    for( i = 0; i < m->fmt->nb_streams; ++i )
        if( (int)i != m->videoIndex && (int)i != m->audioIndex )
            m->fmt->streams[i]->discard = AVDISCARD_ALL;

    STEP( "open decoders" );
    if( m->videoIndex >= 0 )
        m->vctx = OpenDecoder( m->fmt->streams[m->videoIndex], 1 );
    if( m->audioIndex >= 0 )
        m->actx = OpenDecoder( m->fmt->streams[m->audioIndex], 0 );
    if( !m->vctx )
    {
        SetErr( err, errSize, m->videoIndex < 0 ? "No video in this file" : "Video codec not supported" );
        fg_close( m );
        return NULL;
    }
    m->frame = avcodec_alloc_frame();
    STEP( "done" );
    return m;
}

void fg_close( FgMedia* m )
{
    if( !m )
        return;
    if( m->vctx )
        avcodec_close( m->vctx );
    if( m->actx )
        avcodec_close( m->actx );
    if( m->fmt )
        av_close_input_stream( m->fmt );
    if( m->io )
    {
        /* Probing may have swapped in a bigger buffer; free the one avio holds now, not
           m->ioBuffer (freeing that twice corrupted the heap). */
        av_free( m->io->buffer );
        av_free( m->io );
    }
    else if( m->ioBuffer )
        av_free( m->ioBuffer );
    if( m->frame )
        av_free( m->frame );
    if( m->sws )
        sws_freeContext( m->sws );
    if( m->convertedAlloc )
        avpicture_free( &m->converted );
    av_free( m );
}

static void CodecName( AVCodecContext* ctx, char* out, int size )
{
    AVCodec* c = ctx ? avcodec_find_decoder( ctx->codec_id ) : NULL;
    strncpy_s( out, size, c ? c->name : "", _TRUNCATE );
}

void fg_get_info( FgMedia* m, FgInfo* info )
{
    memset( info, 0, sizeof( *info ) );
    info->hasVideo = m->vctx != NULL;
    info->hasAudio = m->actx != NULL;
    if( m->vctx )
    {
        AVStream* st = m->fmt->streams[m->videoIndex];
        info->width = m->vctx->width;
        info->height = m->vctx->height;
        info->threads = m->vctx->thread_count;
        info->frameThreading = m->vctx->active_thread_type == FF_THREAD_FRAME ? 1 :
                               m->vctx->active_thread_type == FF_THREAD_SLICE ? 2 : 0;
        info->profile = m->vctx->profile;
        info->level = m->vctx->level;
        info->refs = m->vctx->refs;
        if( st->r_frame_rate.den )
            info->fps = av_q2d( st->r_frame_rate );
        /* The container's value wins (MKV display size), as in ffplay. */
        if( st->sample_aspect_ratio.num > 0 && st->sample_aspect_ratio.den > 0 )
            info->pixelAspect = av_q2d( st->sample_aspect_ratio );
        else if( m->vctx->sample_aspect_ratio.num > 0 && m->vctx->sample_aspect_ratio.den > 0 )
            info->pixelAspect = av_q2d( m->vctx->sample_aspect_ratio );
    }
    if( m->actx )
    {
        info->sampleRate = m->actx->sample_rate;
        info->channels = m->actx->channels;
    }
    if( m->fmt->duration != AV_NOPTS_VALUE )
        info->duration = m->fmt->duration / (double)AV_TIME_BASE;
    CodecName( m->vctx, info->videoCodec, sizeof( info->videoCodec ) );
    CodecName( m->actx, info->audioCodec, sizeof( info->audioCodec ) );
    strncpy_s( info->container, sizeof( info->container ), m->fmt->iformat->name, _TRUNCATE );
}

int fg_read_packet( FgMedia* m, FgPacket** out )
{
    FgPacket* p = (FgPacket*)av_mallocz( sizeof( FgPacket ) );
    int ret = av_read_frame( m->fmt, &p->pkt );
    if( ret < 0 )
    {
        av_free( p );
        *out = NULL;
        return ret == AVERROR_EOF || url_feof( m->fmt->pb ) ? 0 : ret;
    }
    av_dup_packet( &p->pkt );
    *out = p;
    if( p->pkt.stream_index == m->videoIndex )
        return FG_PACKET_VIDEO;
    if( p->pkt.stream_index == m->audioIndex && m->actx )
        return FG_PACKET_AUDIO;
    if( p->pkt.stream_index == m->subIndex )
        return FG_PACKET_SUBTITLE;
    return FG_PACKET_OTHER;
}

void fg_free_packet( FgPacket* p )
{
    if( p )
    {
        av_free_packet( &p->pkt );
        av_free( p );
    }
}

static double TsToSeconds( FgMedia* m, int streamIndex, int64_t ts )
{
    if( ts == AV_NOPTS_VALUE )
        return -1.0;
    return ts * av_q2d( m->fmt->streams[streamIndex]->time_base );
}

double fg_packet_time( FgMedia* m, FgPacket* p )
{
    int64_t ts = p->pkt.pts != AV_NOPTS_VALUE ? p->pkt.pts : p->pkt.dts;
    return TsToSeconds( m, p->pkt.stream_index, ts );
}

int fg_decode_video( FgMedia* m, FgPacket* p, FgPicture* pic )
{
    int got = 0;
    AVPacket empty;
    AVPacket* pkt = p ? &p->pkt : NULL;
    AVCodecContext* ctx = m->vctx;
    int64_t ts;

    if( !p )
    {
        /* Drain frames still inside the threaded decoder. */
        av_init_packet( &empty );
        empty.data = NULL;
        empty.size = 0;
        pkt = &empty;
    }
    if( avcodec_decode_video2( ctx, m->frame, &got, pkt ) < 0 )
        return -1;
    if( !got )
        return 0;

    ts = m->frame->best_effort_timestamp != AV_NOPTS_VALUE ? m->frame->best_effort_timestamp
                                                           : m->frame->pkt_pts;
    pic->pts = TsToSeconds( m, m->videoIndex, ts );
    pic->width = ctx->width;
    pic->height = ctx->height;
    pic->colorspace = ctx->colorspace == AVCOL_SPC_BT709 ? FG_CS_BT709
                    : ( ctx->colorspace == AVCOL_SPC_BT470BG || ctx->colorspace == AVCOL_SPC_SMPTE170M ) ? FG_CS_BT601
                    : FG_CS_UNKNOWN;
    pic->fullRange = ctx->color_range == AVCOL_RANGE_JPEG || ctx->pix_fmt == PIX_FMT_YUVJ420P;

    if( ctx->pix_fmt == PIX_FMT_YUV420P || ctx->pix_fmt == PIX_FMT_YUVJ420P )
    {
        int i;
        for( i = 0; i < 3; ++i )
        {
            pic->plane[i] = m->frame->data[i];
            pic->pitch[i] = m->frame->linesize[i];
        }
        return 1;
    }

    /* Anything else (4:2:2, 10-bit, RGB...) is converted to 4:2:0 first. */
    if( !m->convertedAlloc )
    {
        avpicture_alloc( &m->converted, PIX_FMT_YUV420P, ctx->width, ctx->height );
        m->convertedAlloc = 1;
    }
    m->sws = sws_getCachedContext( m->sws, ctx->width, ctx->height, ctx->pix_fmt, ctx->width,
                                   ctx->height, PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL );
    if( !m->sws )
        return -1;
    sws_scale( m->sws, (const uint8_t* const*)m->frame->data, m->frame->linesize, 0, ctx->height,
               m->converted.data, m->converted.linesize );
    {
        int i;
        for( i = 0; i < 3; ++i )
        {
            pic->plane[i] = m->converted.data[i];
            pic->pitch[i] = m->converted.linesize[i];
        }
    }
    return 1;
}

int fg_decode_audio( FgMedia* m, FgPacket* p, short* pcm, int pcmBytes, double* pts )
{
    AVPacket pkt = p->pkt;     /* shallow copy: data/size advance as frames are consumed */
    int written = 0;

    if( !m->actx )
        return 0;
    *pts = fg_packet_time( m, p );
    while( pkt.size > 0 )
    {
        int outSize = pcmBytes - written;
        int used;
        if( outSize < AVCODEC_MAX_AUDIO_FRAME_SIZE )
            break;
        used = avcodec_decode_audio3( m->actx, (int16_t*)( (char*)pcm + written ), &outSize, &pkt );
        if( used < 0 )
            return written > 0 ? written : -1;
        written += outSize;
        pkt.data += used;
        pkt.size -= used;
    }
    return written;
}

int fg_seek( FgMedia* m, double seconds )
{
    int64_t target = (int64_t)( seconds * AV_TIME_BASE );
    if( target < 0 )
        target = 0;
    if( av_seek_frame( m->fmt, -1, target, AVSEEK_FLAG_BACKWARD ) < 0 )
        return -1;
    if( m->vctx )
        avcodec_flush_buffers( m->vctx );
    if( m->actx )
        avcodec_flush_buffers( m->actx );
    return 0;
}

void fg_set_fast( FgMedia* m, int level )
{
    /* 0 normal; 1 skip deblocking on non-reference (B) frames only - nothing is predicted
       from them, so no error builds up; 2 also skip decoding those frames. Skipping the
       deblock of reference frames would make errors smear across frames, so never do that. */
    if( !m->vctx )
        return;
    m->vctx->skip_loop_filter = level >= 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    m->vctx->skip_frame = level >= 2 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

/* Decode time split from libavcodec/h264.c (Xbox profiling hook), in milliseconds. */
void ff_xb_h264_profile( int64_t* cabac, int64_t* recon, int64_t* filter, int reset );

extern volatile __int64 ff_xb_await_ticks;   /* xb_thread.c */
extern int ff_xb_chroma_verdict;             /* xbox360/h264_chroma_xb.c */

extern int ff_xb_qpel_verdict;               /* xbox360/h264_qpel_xb.c */

int fg_vmx_status( void ) { return ff_xb_chroma_verdict; }
int fg_vmx_qpel_status( void ) { return ff_xb_qpel_verdict; }

void fg_profile( double* cabacMs, double* reconMs, double* filterMs, double* waitMs, int reset )
{
    int64_t a, b, c;
    const double ticksPerMs = 50000.0;   /* Xenon time base: 50 MHz */
    ff_xb_h264_profile( &a, &b, &c, reset );
    *cabacMs = a / ticksPerMs;
    *reconMs = b / ticksPerMs;
    *filterMs = c / ticksPerMs;
    *waitMs = ff_xb_await_ticks / ticksPerMs;
    if( reset )
        ff_xb_await_ticks = 0;
}
static void Tag( AVStream* st, const char* key, char* out, int size )
{
    AVMetadataTag* tag = av_metadata_get( st->metadata, key, NULL, 0 );
    strncpy_s( out, size, tag && tag->value ? tag->value : "", _TRUNCATE );
}

static int IsTextSubtitle( enum CodecID id )
{
    return id == CODEC_ID_TEXT || id == CODEC_ID_SRT || id == CODEC_ID_SSA || id == CODEC_ID_MOV_TEXT;
}

static AVStream* TrackStream( FgMedia* m, int n )
{
    unsigned i;
    for( i = 0; i < m->fmt->nb_streams; ++i )
    {
        enum AVMediaType t = m->fmt->streams[i]->codec->codec_type;
        if( t == AVMEDIA_TYPE_AUDIO || t == AVMEDIA_TYPE_SUBTITLE )
            if( n-- == 0 )
                return m->fmt->streams[i];
    }
    return NULL;
}

int fg_track_count( FgMedia* m )
{
    int n = 0;
    while( TrackStream( m, n ) )
        ++n;
    return n;
}

void fg_get_track( FgMedia* m, int n, FgTrack* track )
{
    AVStream* st = TrackStream( m, n );
    memset( track, 0, sizeof( *track ) );
    if( !st )
        return;
    track->index = st->index;
    track->type = st->codec->codec_type == AVMEDIA_TYPE_AUDIO ? 2 : 3;
    track->channels = st->codec->channels;
    track->text = track->type == 3 && IsTextSubtitle( st->codec->codec_id );
    CodecName( st->codec, track->codec, sizeof( track->codec ) );
    if( !track->codec[0] && track->text )
        strcpy_s( track->codec, sizeof( track->codec ), st->codec->codec_id == CODEC_ID_SSA ? "ass" : "srt" );
    Tag( st, "language", track->language, sizeof( track->language ) );
    Tag( st, "title", track->title, sizeof( track->title ) );
}

void fg_select_subtitle( FgMedia* m, int containerIndex )
{
    if( m->subIndex >= 0 )
        m->fmt->streams[m->subIndex]->discard = AVDISCARD_ALL;
    m->subIndex = -1;
    if( containerIndex >= 0 && containerIndex < (int)m->fmt->nb_streams &&
        m->fmt->streams[containerIndex]->codec->codec_type == AVMEDIA_TYPE_SUBTITLE )
    {
        m->subIndex = containerIndex;
        m->fmt->streams[containerIndex]->discard = AVDISCARD_DEFAULT;
    }
}

int fg_subtitle( FgMedia* m, FgPacket* p, char* text, int size, double* start, double* duration )
{
    AVStream* st = m->fmt->streams[p->pkt.stream_index];
    const char* data = (const char*)p->pkt.data;
    int len = p->pkt.size, i, commas = 0;
    int64_t dur = p->pkt.convergence_duration > 0 ? p->pkt.convergence_duration : p->pkt.duration;
    if( !data || len <= 0 || size <= 1 )
        return 0;
    if( st->codec->codec_id == CODEC_ID_MOV_TEXT )
    {
        /* 16-bit big-endian length, then the text */
        int n = len >= 2 ? ( ( (unsigned char)data[0] << 8 ) | (unsigned char)data[1] ) : 0;
        data += 2;
        len = n < len - 2 ? n : len - 2;
    }
    else if( st->codec->codec_id == CODEC_ID_SSA )
    {
        /* Matroska ASS events: ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text */
        for( i = 0; i < len && commas < 8; ++i )
            if( data[i] == ',' )
                ++commas;
        if( commas == 8 )
        {
            data += i;
            len -= i;
        }
    }
    if( len <= 0 )
        return 0;
    if( len > size - 1 )
        len = size - 1;
    memcpy( text, data, len );
    text[len] = 0;
    *start = TsToSeconds( m, p->pkt.stream_index, p->pkt.pts != AV_NOPTS_VALUE ? p->pkt.pts : p->pkt.dts );
    *duration = dur > 0 ? dur * av_q2d( st->time_base ) : 3.0;
    return *start >= 0;
}
