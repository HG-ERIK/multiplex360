#pragma once
#include <string>
#include <vector>

namespace Plex
{
    // One thing in a list or grid: a library, movie, show, season, episode, artist...
    struct Item
    {
        std::wstring title;
        std::wstring subtitle;        // year, "S1 E2", item count, ...
        std::wstring summary;
        std::wstring parentTitle;     // season / album
        std::wstring grandparentTitle;// show / artist
        std::string  key;             // path to browse into or fetch details from
        std::string  ratingKey;       // metadata id
        std::string  type;            // movie, show, season, episode, artist, album, track, ...
        std::string  thumb;           // poster (or episode still)
        std::string  art;             // background artwork
        std::string  parentThumb;     // season poster, for episodes
        std::string  grandparentThumb;// show poster, for episodes / seasons
        std::string  contentRating;
        std::string  sectionId;       // library section, for libraries themselves
        int          year;
        int          durationMs;
        int          viewOffsetMs;    // resume point, 0 if not started
        int          viewCount;
        int          leafCount;       // episodes in a show/season
        int          viewedLeafCount;
        int          childCount;      // seasons in a show
        int          index;           // episode / season number
        int          parentIndex;     // season number of an episode
        float        rating;          // 0-10, 0 if none
        bool         browsable;       // key leads to another list

        Item() : year( 0 ), durationMs( 0 ), viewOffsetMs( 0 ), viewCount( 0 ), leafCount( 0 ),
                 viewedLeafCount( 0 ), childCount( 0 ), index( 0 ), parentIndex( 0 ), rating( 0 ),
                 browsable( false ) {}

        bool Watched() const { return leafCount ? viewedLeafCount >= leafCount : viewCount > 0; }
        bool Playable() const { return type == "movie" || type == "episode" || type == "clip"; }
    };

    struct Result
    {
        bool              ok;
        std::string       error;
        std::wstring      title;    // MediaContainer title (library / show name)
        std::vector<Item> items;
        int               total;    // items available (for paging), or items.size()

        Result() : ok( false ), total( 0 ) {}
    };

    // A row on the home screen ("Continue Watching", "Recently Added Movies", ...).
    struct Hub
    {
        std::wstring      title;
        std::string       key;
        std::vector<Item> items;
    };

    // An audio or subtitle track of a file.
    struct Stream
    {
        int          id;            // Plex stream id
        int          streamType;    // 2 audio, 3 subtitles
        int          index;         // index inside the container (for direct play)
        int          channels;
        std::string  codec;
        std::string  language;      // language code (eng, hun, ...)
        std::wstring title;         // "English Dolby Digital 640 kbps (AC3)" - the full description
        std::wstring shortTitle;    // "English (AC3 5.1)"
        bool         selected;

        Stream() : id( 0 ), streamType( 0 ), index( -1 ), channels( 0 ), selected( false ) {}
    };

    // What's needed to play one video: the file part on the server, its formats and tracks.
    struct Media
    {
        bool                ok;
        std::string         error;
        std::string         ratingKey;
        std::string         partKey;      // /library/parts/{id}/{ts}/file.mkv
        std::string         partId;
        int                 mediaIndex;   // which version of the item (<Media> block) this is
        bool                hasPreviews;  // the server made timeline preview pictures (Part indexes="sd")
        std::string         container;    // mkv, mp4, avi, ...
        std::string         videoCodec;   // h264, hevc, mpeg4, ...
        std::string         audioCodec;   // aac, ac3, dca, ...
        std::wstring        title;
        int                 width;
        int                 height;
        int                 bitrate;      // kbps
        double              duration;     // seconds
        double              viewOffset;   // resume point, seconds
        std::vector<Stream> streams;

        Media() : ok( false ), mediaIndex( 0 ), hasPreviews( false ), width( 0 ), height( 0 ), bitrate( 0 ), duration( 0 ), viewOffset( 0 ) {}

        const Stream* SelectedStream( int streamType ) const
        {
            for( size_t i = 0; i < streams.size(); ++i )
                if( streams[i].streamType == streamType && streams[i].selected )
                    return &streams[i];
            return 0;
        }
    };

    class Client
    {
    public:
        // baseUrl: chosen server connection, e.g. https://1-2-3-4.<hash>.plex.direct:32400
        Client( const std::string& baseUrl, const std::string& token, const std::string& clientId )
            : m_base( baseUrl ), m_token( token ), m_clientId( clientId ) {}

        // Quick reachability check (/identity) used to pick a connection; returns the server version.
        bool Ping( unsigned long timeoutMs, std::wstring& version, std::string& error );

        // A short-lived token that only works on this server (for requests sent unencrypted).
        bool TransientToken( std::string& token );

        // Library sections (Movies, TV Shows, ...).
        Result Sections();

        // Any browse path: /library/sections/{id}/all, /library/metadata/{id}/children, ...
        // start/count page through big libraries (count 0 = server default).
        Result Browse( const std::string& path, int start = 0, int count = 0 );

        // Home screen rows: Continue Watching, Recently Added, ...
        bool   Hubs( std::vector<Hub>& hubs, std::string& error );

        // Full details of one item (summary, artwork, ...).
        Result Details( const std::string& ratingKey );

        // Media details for a movie/episode key (/library/metadata/{id}).
        // maxHeight: tallest H.264 version the 360 plays itself; picks that version if any.
        Media  GetMedia( const std::string& key, int maxHeight );

        // Chooses the audio/subtitle track the server uses (0 subtitles = off).
        bool   SelectStreams( const std::string& partId, int audioStreamId, int subtitleStreamId );

        // Playback progress for Continue Watching. state: playing, paused, stopped.
        void   Timeline( const std::string& ratingKey, const char* state, double seconds, double duration );

        // Mark watched / unwatched.
        void   SetWatched( const std::string& ratingKey, bool watched );

        // URL of an on-the-fly conversion to H.264 (max height/kbps) + stereo AAC in MKV,
        // starting at offsetSeconds. burnSubtitles draws the selected subtitles into the picture.
        std::string TranscodeUrl( const std::string& key, double offsetSeconds, int maxHeight, int maxKbps,
                                  const std::string& session, bool burnSubtitles = false, int mediaIndex = 0 ) const;
        std::string TranscodeHeaders() const;
        void        StopTranscode( const std::string& session );

        // Headers for any request to this server (token, client identification).
        std::string Headers() const;

        const std::string& Base() const  { return m_base; }
        const std::string& Token() const { return m_token; }

    private:
        Result      Fetch( const std::string& path, std::string& xml, int start = 0, int count = 0 );

        std::string m_base;
        std::string m_token;
        std::string m_clientId;
    };
}
