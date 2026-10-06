#pragma once
#include <string>

// Optional settings from game:\plex.ini (key=value lines, # comments).
struct Config
{
    std::string connection;   // auto (default), local, remote or relay - forces a connection type
    std::string serverName;   // pick this server if the account has several; empty = first owned
    std::string autoplay;     // testing: metadata id to start playing right after connecting
    int         threads;      // video decode threads, 1-4 (default 4)
    int         directMax;    // tallest video played without Plex converting it (default 720)
    int         quality;      // height Plex converts to when a file can't be played directly (480/576/720)
    int         kbps;         // bitrate of those conversions, and the most a file may have to play directly
    int         testSeek;     // testing: seek by this many seconds every 12 s (3 times)
    bool        testOsd;      // testing: keep the player bar visible with a fake scrub target
    std::string testNav;      // testing: keys pressed one per 1.5 s (U D L R A B X Y), e.g. "R A"
    int         autoplayStart; // testing: where autoplay starts, seconds
    int         benchmark;    // testing: stop after this many seconds of playback and log a summary
    int         testCrash;    // testing: crash on purpose this many seconds after start (checks Guard)
    int         testHang;     // testing: freeze the main loop this many seconds after start
    std::string logTo;        // debugging: ip:port that receives log lines over UDP

    Config() : connection( "auto" ), threads( 4 ), directMax( 720 ), quality( 720 ), kbps( 4000 ), testSeek( 0 ),
               testOsd( false ), autoplayStart( 0 ), benchmark( 0 ), testCrash( 0 ), testHang( 0 ) {}

    // A missing file is fine: defaults apply.
    void Load( const char* path );
};

// Sign-in state saved on the console in game:\auth.ini.
struct AuthStore
{
    std::string clientId;     // random, generated on first run; identifies this console to Plex
    std::string token;        // plex.tv account token from the link-code sign-in

    bool Load( const char* path );
    bool Save( const char* path ) const;
};
