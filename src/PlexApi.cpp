#include <stdlib.h>
#include <stdio.h>
#include "PlexApi.h"
#include "Http.h"
#include "PlexTv.h"
#include "Xml.h"
#include "Log.h"

namespace
{
    std::string Attr( const Xml::Attributes& a, const char* name )
    {
        Xml::Attributes::const_iterator it = a.find( name );
        return it == a.end() ? std::string() : it->second;
    }

    std::wstring WAttr( const Xml::Attributes& a, const char* name )
    {
        return Xml::Utf8ToWide( Attr( a, name ) );
    }

    int IAttr( const Xml::Attributes& a, const char* name )
    {
        return atoi( Attr( a, name ).c_str() );
    }

    std::string ResolveKey( const std::string& base, const std::string& key )
    {
        if( key.empty() || key[0] == '/' )
            return key;
        std::string b = base.substr( 0, base.find( '?' ) );
        if( !b.empty() && b[b.size() - 1] == '/' )
            b.erase( b.size() - 1 );
        return b + "/" + key;
    }

    std::wstring Subtitle( const Plex::Item& it )
    {
        wchar_t buf[96];
        buf[0] = 0;
        if( it.type == "movie" )
        {
            if( it.year )
                swprintf_s( buf, L"%d", it.year );
        }
        else if( it.type == "show" )
        {
            if( it.childCount )
                swprintf_s( buf, it.childCount == 1 ? L"%d season" : L"%d seasons", it.childCount );
        }
        else if( it.type == "season" )
        {
            if( it.leafCount )
                swprintf_s( buf, L"%d episodes", it.leafCount );
        }
        else if( it.type == "episode" )
        {
            swprintf_s( buf, L"S%d E%d", it.parentIndex, it.index );
        }
        else if( it.type == "album" || it.type == "track" )
        {
            return it.parentTitle;
        }
        return buf;
    }

    Plex::Item ParseItem( const Xml::Attributes& a, bool browsable, const std::string& base )
    {
        Plex::Item it;
        it.type = Attr( a, "type" );
        it.title = WAttr( a, "title" );
        it.summary = WAttr( a, "summary" );
        it.parentTitle = WAttr( a, "parentTitle" );
        it.grandparentTitle = WAttr( a, "grandparentTitle" );
        it.key = ResolveKey( base, Attr( a, "key" ) );
        it.ratingKey = Attr( a, "ratingKey" );
        it.thumb = Attr( a, "thumb" );
        it.art = Attr( a, "art" );
        it.parentThumb = Attr( a, "parentThumb" );
        it.grandparentThumb = Attr( a, "grandparentThumb" );
        it.contentRating = Attr( a, "contentRating" );
        it.year = IAttr( a, "year" );
        it.durationMs = IAttr( a, "duration" );
        it.viewOffsetMs = IAttr( a, "viewOffset" );
        it.viewCount = IAttr( a, "viewCount" );
        it.leafCount = IAttr( a, "leafCount" );
        it.viewedLeafCount = IAttr( a, "viewedLeafCount" );
        it.childCount = IAttr( a, "childCount" );
        it.index = IAttr( a, "index" );
        it.parentIndex = IAttr( a, "parentIndex" );
        it.rating = (float)atof( Attr( a, "rating" ).c_str() );
        if( it.rating == 0 )
            it.rating = (float)atof( Attr( a, "audienceRating" ).c_str() );
        it.browsable = browsable;
        // Episodes in "Continue Watching" lists: show the show's name as the title.
        it.subtitle = Subtitle( it );
        return it;
    }

    void AddItems( const std::string& xml, const char* tag, bool browsable, const std::string& base,
                   std::vector<Plex::Item>& out )
    {
        std::vector<Xml::Attributes> elems = Xml::FindElements( xml, tag );
        for( size_t i = 0; i < elems.size(); ++i )
            out.push_back( ParseItem( elems[i], browsable, base ) );
    }

    void AddAllItems( const std::string& xml, const std::string& base, std::vector<Plex::Item>& out )
    {
        AddItems( xml, "Directory", true, base, out );
        AddItems( xml, "Video", false, base, out );
        AddItems( xml, "Track", false, base, out );
    }
}

std::string Plex::Client::Headers() const
{
    return PlexTv::ClientHeaders( m_clientId ) + "X-Plex-Token: " + m_token + "\r\n";
}

Plex::Result Plex::Client::Fetch( const std::string& path, std::string& xml, int start, int count )
{
    Result r;
    std::string headers = Headers();
    if( count > 0 )
    {
        char page[96];
        sprintf_s( page, "X-Plex-Container-Start: %d\r\nX-Plex-Container-Size: %d\r\n", start, count );
        headers += page;
    }
    Http::Response resp = Http::Get( m_base + path, headers );
    if( resp.status == 0 )
    {
        r.error = resp.error;
        return r;
    }
    if( resp.status == 401 )
    {
        r.error = "The server rejected this account (401)";
        return r;
    }
    if( resp.status != 200 )
    {
        char buf[64];
        sprintf_s( buf, "Server replied HTTP %d", resp.status );
        r.error = buf;
        return r;
    }
    xml.swap( resp.body );
    r.ok = true;
    return r;
}

bool Plex::Client::TransientToken( std::string& token )
{
    if( m_base.compare( 0, 8, "https://" ) != 0 )
        return false;   // only fetch it over an encrypted connection
    Http::Response resp = Http::Get( m_base + "/security/token?type=delegation&scope=all", Headers(), 8000 );
    if( resp.status != 200 )
        return false;
    token = Attr( Xml::FindFirst( resp.body, "MediaContainer" ), "token" );
    return token.compare( 0, 10, "transient-" ) == 0;
}

bool Plex::Client::Ping( unsigned long timeoutMs, std::wstring& version, std::string& error )
{
    Http::Response resp = Http::Get( m_base + "/identity", PlexTv::ClientHeaders( m_clientId ), timeoutMs );
    if( resp.status != 200 )
    {
        if( resp.status == 0 )
            error = resp.error;
        else
        {
            char buf[64];
            sprintf_s( buf, "HTTP %d", resp.status );
            error = buf;
        }
        return false;
    }
    std::wstring v = WAttr( Xml::FindFirst( resp.body, "MediaContainer" ), "version" );
    version = v.substr( 0, v.find( L'-' ) );
    return true;
}

Plex::Result Plex::Client::Sections()
{
    std::string xml;
    Result r = Fetch( "/library/sections", xml );
    if( !r.ok )
        return r;

    std::vector<Xml::Attributes> dirs = Xml::FindElements( xml, "Directory" );
    for( size_t i = 0; i < dirs.size(); ++i )
    {
        Item item;
        item.type = Attr( dirs[i], "type" );
        item.title = WAttr( dirs[i], "title" );
        item.subtitle = Xml::Utf8ToWide( item.type );
        item.sectionId = Attr( dirs[i], "key" );
        item.key = "/library/sections/" + item.sectionId + "/all";
        item.thumb = Attr( dirs[i], "thumb" );
        item.art = Attr( dirs[i], "art" );
        item.browsable = true;
        r.items.push_back( item );
    }
    r.title = L"Libraries";
    r.total = (int)r.items.size();
    Log::Write( "Sections: %u", (unsigned)r.items.size() );
    return r;
}

Plex::Result Plex::Client::Browse( const std::string& path, int start, int count )
{
    std::string xml;
    Result r = Fetch( path, xml, start, count );
    if( !r.ok )
        return r;

    Xml::Attributes mc = Xml::FindFirst( xml, "MediaContainer" );
    r.title = WAttr( mc, "title2" );
    if( r.title.empty() )
        r.title = WAttr( mc, "title1" );

    AddAllItems( xml, path, r.items );
    r.total = IAttr( mc, "totalSize" );
    if( r.total < start + (int)r.items.size() )
        r.total = start + (int)r.items.size();
    Log::Write( "Browse %s [%d+%u of %d]", path.c_str(), start, (unsigned)r.items.size(), r.total );
    return r;
}

bool Plex::Client::Hubs( std::vector<Hub>& hubs, std::string& error )
{
    std::string xml;
    Result r = Fetch( "/hubs?count=20&excludeFields=summary", xml );
    if( !r.ok )
    {
        error = r.error;
        return false;
    }

    size_t pos = 0;
    while( ( pos = xml.find( "<Hub ", pos ) ) != std::string::npos )
    {
        size_t end = xml.find( "</Hub>", pos );
        std::string chunk = xml.substr( pos, end == std::string::npos ? std::string::npos : end - pos );
        pos += 5;
        Xml::Attributes h = Xml::FindFirst( chunk, "Hub" );
        Hub hub;
        hub.title = WAttr( h, "title" );
        hub.key = Attr( h, "key" );
        std::string hubType = Attr( h, "type" );
        if( hubType != "movie" && hubType != "show" && hubType != "episode" && hubType != "season" &&
            hubType != "mixed" )
            continue;   // skip music, photos, playlists
        AddAllItems( chunk, "/", hub.items );
        if( !hub.items.empty() )
            hubs.push_back( hub );
    }
    Log::Write( "Hubs: %u", (unsigned)hubs.size() );
    return true;
}

Plex::Result Plex::Client::Details( const std::string& ratingKey )
{
    return Browse( "/library/metadata/" + ratingKey );
}

Plex::Media Plex::Client::GetMedia( const std::string& key, int maxHeight )
{
    Media m;
    std::string xml;
    Result r = Fetch( key, xml );
    if( !r.ok )
    {
        m.error = r.error;
        return m;
    }

    Xml::Attributes video = Xml::FindFirst( xml, "Video" );

    // Pick the best <Media> version: H.264 within maxHeight if any, else the best source to
    // transcode. Only its own <Stream>s are parsed.
    std::string block = xml;
    int bestScore = -1, versions = 0;
    m.mediaIndex = 0;
    for( size_t pos = xml.find( "<Media " ); pos != std::string::npos; pos = xml.find( "<Media ", pos + 1 ), ++versions )
    {
        size_t end = xml.find( "</Media>", pos );
        std::string b = xml.substr( pos, end == std::string::npos ? std::string::npos : end - pos );
        Xml::Attributes a = Xml::FindFirst( b, "Media" );
        int h = IAttr( a, "height" ), kbps = IAttr( a, "bitrate" );
        bool fits = Attr( a, "videoCodec" ) == "h264" && h > 0 && h <= maxHeight;
        int score = ( fits ? 1 << 30 : 0 ) + ( ( h < 4096 ? h : 4095 ) << 15 ) + ( kbps < 0x7FFF ? kbps : 0x7FFF );
        if( score > bestScore )
        {
            bestScore = score;
            block = b;
            m.mediaIndex = versions;
        }
    }
    if( versions > 1 )
        Log::Write( "Media %s: %d versions, using #%d", key.c_str(), versions, m.mediaIndex );
    Xml::Attributes media = Xml::FindFirst( block, "Media" );
    Xml::Attributes part = Xml::FindFirst( block, "Part" );
    m.ratingKey = Attr( video, "ratingKey" );
    m.title = WAttr( video, "title" );
    if( Attr( video, "type" ) == "episode" )
    {
        wchar_t buf[48];
        swprintf_s( buf, L"S%d E%d  ", IAttr( video, "parentIndex" ), IAttr( video, "index" ) );
        m.title = WAttr( video, "grandparentTitle" ) + L"  -  " + buf + m.title;
    }
    m.viewOffset = IAttr( video, "viewOffset" ) / 1000.0;
    m.partKey = Attr( part, "key" );
    m.partId = Attr( part, "id" );
    m.hasPreviews = Attr( part, "indexes" ).find( "sd" ) != std::string::npos;
    m.container = Attr( media, "container" );
    m.videoCodec = Attr( media, "videoCodec" );
    m.audioCodec = Attr( media, "audioCodec" );
    m.width = IAttr( media, "width" );
    m.height = IAttr( media, "height" );
    m.bitrate = IAttr( media, "bitrate" );
    m.duration = atof( Attr( media, "duration" ).c_str() ) / 1000.0;

    std::vector<Xml::Attributes> streams = Xml::FindElements( block, "Stream" );
    for( size_t i = 0; i < streams.size(); ++i )
    {
        Stream s;
        s.streamType = IAttr( streams[i], "streamType" );
        if( s.streamType != 2 && s.streamType != 3 )
            continue;
        s.id = IAttr( streams[i], "id" );
        s.index = Attr( streams[i], "index" ).empty() ? -1 : IAttr( streams[i], "index" );
        s.channels = IAttr( streams[i], "channels" );
        s.codec = Attr( streams[i], "codec" );
        s.language = Attr( streams[i], "languageCode" );
        s.shortTitle = WAttr( streams[i], "displayTitle" );
        s.title = WAttr( streams[i], "extendedDisplayTitle" );
        if( s.title.empty() )
            s.title = WAttr( streams[i], "displayTitle" );
        s.selected = Attr( streams[i], "selected" ) == "1";
        m.streams.push_back( s );
    }

    if( m.partKey.empty() )
    {
        m.error = "This item has no playable file";
        return m;
    }
    Log::Write( "Media %s: %s %s/%s %dx%d, %u tracks", key.c_str(), m.container.c_str(), m.videoCodec.c_str(),
                m.audioCodec.c_str(), m.width, m.height, (unsigned)m.streams.size() );
    m.ok = true;
    return m;
}

bool Plex::Client::SelectStreams( const std::string& partId, int audioStreamId, int subtitleStreamId )
{
    char q[128];
    sprintf_s( q, "/library/parts/%s?allParts=1&audioStreamID=%d&subtitleStreamID=%d", partId.c_str(),
               audioStreamId, subtitleStreamId );
    Http::Response r = Http::Request( "PUT", m_base + q, Headers(), std::string(), 8000 );
    Log::Write( "Select streams audio %d subs %d -> %d", audioStreamId, subtitleStreamId, r.status );
    return r.status == 200;
}

void Plex::Client::Timeline( const std::string& ratingKey, const char* state, double seconds, double duration )
{
    char q[256];
    sprintf_s( q, "/:/timeline?ratingKey=%s&key=%%2Flibrary%%2Fmetadata%%2F%s&state=%s&time=%d&duration=%d",
               ratingKey.c_str(), ratingKey.c_str(), state, (int)( seconds * 1000 ), (int)( duration * 1000 ) );
    Http::Get( m_base + q, Headers(), 5000 );
}

void Plex::Client::SetWatched( const std::string& ratingKey, bool watched )
{
    std::string q = std::string( watched ? "/:/scrobble" : "/:/unscrobble" ) +
                    "?identifier=com.plexapp.plugins.library&key=" + ratingKey;
    Http::Get( m_base + q, Headers(), 5000 );
}

std::string Plex::Client::TranscodeUrl( const std::string& key, double offsetSeconds, int maxHeight, int maxKbps,
                                        const std::string& session, bool burnSubtitles, int mediaIndex ) const
{
    int width = maxHeight * 16 / 9;
    char buf[600];
    sprintf_s( buf, "/video/:/transcode/universal/start?path=%s&mediaIndex=%d&partIndex=0&offset=%d"
               "&fastSeek=1&directPlay=0&directStream=1&directStreamAudio=1&protocol=http"
               "&videoResolution=%dx%d&maxVideoBitrate=%d&videoQuality=100&session=%s&subtitles=%s",
               Http::Escape( key ).c_str(), mediaIndex, (int)offsetSeconds, width, maxHeight, maxKbps, session.c_str(),
               burnSubtitles ? "burn" : "none" );
    return m_base + buf;
}

std::string Plex::Client::TranscodeHeaders() const
{
    // What this client can decode: H.264 video, AAC/AC3 in stereo, in Matroska.
    return Headers() +
           "X-Plex-Client-Profile-Extra: add-transcode-target(type=videoProfile&context=streaming"
           "&protocol=http&container=mkv&videoCodec=h264&audioCodec=aac,ac3)"
           "+add-limitation(scope=videoAudioCodec&scopeName=*&type=upperBound&name=audio.channels&value=2)\r\n";
}

void Plex::Client::StopTranscode( const std::string& session )
{
    Http::Get( m_base + "/video/:/transcode/universal/stop?session=" + session, Headers(), 3000 );
}
