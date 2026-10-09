//--------------------------------------------------------------------------------------
// Multiplex 360 - unofficial Plex client for the Xbox 360.
//--------------------------------------------------------------------------------------
#include <xtl.h>
#include <winsockx.h>
#include "App.h"
#include "Font.h"
#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>
#include <memory>
#include "Config.h"
#include "Http.h"
#include "PlexApi.h"
#include "PlexTv.h"
#include "Log.h"
#include "FFPlayer.h"
#include "ffglue.h"
#include "Draw.h"
#include "Browser.h"
#include "Tasks.h"
#include "ImageCache.h"
#include "CpuMeter.h"
#include "Guard.h"
#include "Quality.h"
#include "PlayerScreen.h"

namespace
{
    const char* const AUTH_PATH = "game:\\auth.ini";

    const DWORD COLOR_BG_TOP     = 0xff1f1f1f;
    const DWORD COLOR_BG_BOTTOM  = 0xff0a0a0a;
    const DWORD COLOR_ACCENT     = 0xffe5a00d;   // Plex orange
    const DWORD COLOR_TEXT       = 0xffeeeeee;
    const DWORD COLOR_DIM        = 0xff8a8a8a;
    const DWORD COLOR_ERROR      = 0xffff6060;


    const DWORD PING_TIMEOUT_MS  = 4000;
    const DWORD PIN_POLL_MS      = 2000;
    const DWORD PIN_GIVE_UP_MS   = 15 * 60 * 1000;


    // Work handed to the network thread; only one job runs at a time.
    struct Job
    {
        enum Kind { NONE, STARTUP, MEDIA };

        Kind          kind;
        std::string   path;
        double        offset;     // MEDIA: where to start playing
        std::wstring  title;
        Plex::Result  result;
        Plex::Media   media;
        volatile LONG done;

        Job() : kind( NONE ), offset( 0 ), done( 0 ) {}
    };

    // A connection candidate: the URL to try and how to describe it on screen.
    struct Candidate
    {
        std::string  url;
        std::wstring label;
    };

    std::wstring Widen( const std::string& s )
    {
        return std::wstring( s.begin(), s.end() );
    }

    std::string NewClientId()
    {
        BYTE bytes[8];
        XNetRandom( bytes, sizeof( bytes ) );
        char id[32] = "xbox360-";
        for( int i = 0; i < 8; ++i )
            sprintf_s( id + 8 + i * 2, 3, "%02x", bytes[i] );
        return id;
    }
}

class PlexApp : public App, public PlayerHost
{
public:
    PlexApp() : m_client( NULL ), m_sessionReady( false ), m_timelineTick( 0 ), m_lastPosition( 0 ),
                m_transcoding( false ), m_restarting( false ), m_reconnects( 0 ), m_startGeneration( 0 ), m_menuHeight( 720 ),
                m_qualityMode( QUALITY_AUTO ), m_qualityPreset( 1 ), m_burnSubs( false ), m_frameStart( 0 ),
                m_renderMs( 0 ), m_thread( NULL ), m_notify( NULL ), m_busy( false ) {}

private:
    virtual HRESULT Initialize();
    virtual HRESULT Update();
    virtual HRESULT Render();

    void         StartJob( Job::Kind kind, const std::string& path, const std::wstring& title );
    void         FinishJob();
    static DWORD WINAPI JobThread( LPVOID param );

    bool         SignIn( std::string& error );
    bool         ConnectToServer( std::string& error );
    std::vector<Candidate> Candidates( const PlexTv::Server& server ) const;

    void         SetStatus( const std::wstring& s );
    std::wstring Status();
    void         SetPinCode( const std::string& code );
    std::string  PinCode();

    void         RenderSignIn( const std::string& code, const D3DRECT& safe );

    Font               m_font;
    Font               m_titleFont;
    Config             m_cfg;
    AuthStore          m_auth;
    Plex::Client*      m_client;
    Browser            m_browser;
    TaskQueue          m_api;           // background Plex requests for the browser
    ImageCache         m_images;
    UserSettings       m_settings;
    bool               m_sessionReady;  // signed in, connected, browser showing
    DWORD              m_timelineTick;  // last progress report to the server
    double             m_lastPosition;  // playback position, kept after the player stops

    void         PlayItem( const Plex::Item& item, double start );
    void         SignOut();
    void         SwitchUser( const std::string& token, const std::wstring& name );
    void         ApplySettings();
    void         ReportTimeline( const char* state );
    std::string        m_localIp;
    std::wstring       m_username;
    std::wstring       m_serverLabel;   // "My Server - Remote (internet)"
    std::string        m_error;

    CRITICAL_SECTION   m_lock;
    std::wstring       m_status;
    std::string        m_pinCode;

    Job                m_job;

    FFPlayer           m_player;
    PlayerScreen       m_screen;
    CpuMeter           m_cpu;
    std::string        m_lanBase;      // http://<LAN ip>:port of the server, if it has one

    void         StartPlayback( const std::string& key, const Plex::Media& media, double offset );
    void         StopPlayback();
    void         UpdatePlayer( Pad* pad );

    // What is playing, so a transcode can be restarted at a new time for seeking.
    std::string        m_playKey;
    Plex::Media        m_playMedia;
    std::wstring       m_playTitle;
    bool               m_transcoding;
    std::string        m_session;

    // While the stream (re)starts the player screen stays up with a message.
    void         RestartPlayback( double position, bool selectStreams, const std::wstring& message );
    bool               m_restarting;
    int                m_reconnects;        // automatic restarts after a lost connection, this video
    int                m_startGeneration;   // bumps on every start/cancel; stale callbacks see a different value
    std::wstring       m_restartMessage;

    // PlayerHost
    virtual void         Seek( double seconds );
    virtual void         Leave();
    virtual void         CancelStart();
    virtual bool         SlowSeek() const { return m_transcoding; }
    virtual double       DurationHint() const { return m_playMedia.duration; }
    virtual std::wstring Title() const { return m_playTitle; }
    virtual std::wstring StartMessage() const { return m_restartMessage; }
    virtual std::wstring Tag() const;
    virtual IDirect3DTexture9* Preview( double seconds );
    virtual bool         ShowStats() const { return m_settings.stats; }
    virtual std::wstring StatsMode() const;
    virtual int          Brightness() const { return m_settings.brightness; }
    virtual void         BuildMenu( int page, MenuPage& out );
    virtual MenuMove     Choose( int page, int sel );
    virtual MenuMove     Back( int page );
    virtual void         SliderStep( int page, int step );
    virtual void         AspectChanged();

    // Options menu pages.
    enum { PAGE_MAIN, PAGE_QUALITY, PAGE_BITRATE, PAGE_AUDIO, PAGE_SUBS, PAGE_BRIGHTNESS, PAGE_ASPECT };
    enum { ROW_QUALITY, ROW_AUDIO, ROW_SUBS, ROW_BRIGHTNESS, ROW_ASPECT, ROW_STATS };
    enum { QUALITY_AUTO, QUALITY_ORIGINAL, QUALITY_PRESET };
    std::vector<int> TrackList( int streamType ) const;   // indexes into m_playMedia.streams; -1 = subtitles off
    std::wstring TrackName( int streamType ) const;
    std::wstring QualityName() const;
    int                m_menuHeight;        // resolution whose bitrates PAGE_BITRATE lists
    int                m_qualityMode;       // QUALITY_*: this video's choice (resets per video)
    int                m_qualityPreset;     // QUALITY_PRESETS index when QUALITY_PRESET
    bool               m_burnSubs;

    DWORD              m_frameStart;
    float              m_renderMs;
    void         RenderPlayer( const D3DRECT& safe );
    void         EndFrame();
    HANDLE             m_thread;
    HANDLE             m_notify;        // system notifications (Guide open/closed, ...)
    bool               m_busy;
};


// Separate function: __try can't live in a function with C++ destructors.
static void RunGuarded( PlexApp& app )
{
    __try
    {
        app.Run();
    }
    __except( Guard_Filter( GetExceptionCode(), GetExceptionInformation(), "main" ) )
    {
    }
}

VOID __cdecl main()
{
    Log::Open( "game:\\plexlog.txt" );
    Log::Write( "Multiplex 360 starting" );

    PlexApp app;
    // vsync: no tearing, and no extra frames competing with the decoder.
    app.m_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    Guard_Start();
    RunGuarded( app );
}


HRESULT PlexApp::Initialize()
{
    InitializeCriticalSection( &m_lock );

    m_notify = XNotifyCreateListener( XNOTIFY_SYSTEM );
    Draw::Startup( m_pd3dDevice, m_d3dpp.BackBufferWidth, m_d3dpp.BackBufferHeight );
    Font::Startup( m_pd3dDevice );
    if( FAILED( m_font.Create( "game:\\Media\\Fonts\\Text_16.fnt" ) ) ||
        FAILED( m_titleFont.Create( "game:\\Media\\Fonts\\Text_22.fnt" ) ) )
    {
        Log::Write( "Font load failed" );
        return E_FAIL;
    }
    m_font.SetWindow( SafeArea() );
    m_titleFont.SetWindow( SafeArea() );
    m_screen.Init( &m_player, &m_font, &m_titleFont, &m_cpu, this, COLOR_ACCENT );

    m_cfg.Load( "game:\\plex.ini" );
    Log::Write( "Connection mode: %s", m_cfg.connection.c_str() );

    // settings.ini (Settings screen) overrides plex.ini.
    m_settings.quality = m_cfg.quality;
    m_settings.kbps = m_cfg.kbps;
    m_settings.directMax = m_cfg.directMax;
    m_settings.Load( "game:\\settings.ini" );

    static const DWORD apiCpu[1] = { 1 };
    m_api.Start( apiCpu, 1 );
    m_images.Startup( m_pd3dDevice );
    BrowserHost host;
    host.play = [this]( const Plex::Item& item, double start ) { PlayItem( item, start ); };
    host.signOut = [this]() { SignOut(); };
    host.exitApp = []() {
        Log::Write( "Exit to dashboard (sidebar)" );
        XLaunchNewImage( XLAUNCH_KEYWORD_DEFAULT_APP, 0 );
    };
    host.switchUser = [this]( const std::string& token, const std::wstring& name ) { SwitchUser( token, name ); };
    host.settingsChanged = [this]() { ApplySettings(); };
    m_browser.Startup( m_pd3dDevice, &m_font, &m_titleFont, SafeArea(), &m_api, &m_images, &m_settings,
                       host );

    StartJob( Job::STARTUP, "", L"" );
    return S_OK;
}


void PlexApp::SetStatus( const std::wstring& s )
{
    EnterCriticalSection( &m_lock );
    m_status = s;
    LeaveCriticalSection( &m_lock );
}

std::wstring PlexApp::Status()
{
    EnterCriticalSection( &m_lock );
    std::wstring s = m_status;
    LeaveCriticalSection( &m_lock );
    return s;
}

void PlexApp::SetPinCode( const std::string& code )
{
    EnterCriticalSection( &m_lock );
    m_pinCode = code;
    LeaveCriticalSection( &m_lock );
}

std::string PlexApp::PinCode()
{
    EnterCriticalSection( &m_lock );
    std::string s = m_pinCode;
    LeaveCriticalSection( &m_lock );
    return s;
}


void PlexApp::StartJob( Job::Kind kind, const std::string& path, const std::wstring& title )
{
    m_job.kind = kind;
    m_job.path = path;
    m_job.title = title;
    m_job.result = Plex::Result();
    m_job.media = Plex::Media();
    m_job.done = 0;
    m_busy = true;
    m_error.clear();
    SetStatus( kind == Job::STARTUP ? ( m_client ? L"Loading libraries..." : L"Starting network..." ) : L"Loading video..." );

    m_thread = Guard_CreateThread( 0, JobThread, this, CREATE_SUSPENDED );
    XSetThreadProcessor( m_thread, 2 );   // keep the render thread's core free
    ResumeThread( m_thread );
}


bool PlexApp::SignIn( std::string& error )
{
    m_auth.Load( AUTH_PATH );
    if( m_auth.clientId.empty() )
    {
        m_auth.clientId = NewClientId();
        m_auth.Save( AUTH_PATH );
    }

    if( !m_auth.token.empty() )
    {
        SetStatus( L"Checking Plex sign-in..." );
        bool invalid;
        if( PlexTv::CheckToken( m_auth.clientId, m_auth.token, m_username, invalid, error ) )
            return true;
        if( !invalid )
            return false;   // plex.tv unreachable: keep the token, let the user retry
        Log::Write( "Saved token rejected; signing in again" );
        m_auth.token.clear();
        m_auth.Save( AUTH_PATH );
    }

    SetStatus( L"Getting a sign-in code from plex.tv..." );
    PlexTv::Pin pin;
    if( !PlexTv::CreatePin( m_auth.clientId, pin, error ) )
        return false;
    SetPinCode( pin.code );
    SetStatus( L"Waiting for you to enter the code..." );

    DWORD start = GetTickCount();
    std::string token;
    PlexTv::PinState state = PlexTv::PIN_WAITING;
    while( state == PlexTv::PIN_WAITING && GetTickCount() - start < PIN_GIVE_UP_MS )
    {
        Sleep( PIN_POLL_MS );
        state = PlexTv::CheckPin( m_auth.clientId, pin, token, error );
    }
    SetPinCode( "" );

    if( state != PlexTv::PIN_LINKED )
    {
        if( state == PlexTv::PIN_WAITING )
            error = "Sign-in timed out";
        return false;
    }

    Log::Write( "Signed in to plex.tv" );
    m_auth.token = token;
    if( !m_auth.Save( AUTH_PATH ) )
        Log::Write( "Could not save %s", AUTH_PATH );

    bool invalid;
    PlexTv::CheckToken( m_auth.clientId, m_auth.token, m_username, invalid, error );
    error.clear();
    return true;
}


std::vector<Candidate> PlexApp::Candidates( const PlexTv::Server& server ) const
{
    const std::string& mode = m_cfg.connection;
    bool wantLocal  = mode == "auto" || mode == "local";
    bool wantRemote = mode == "auto" || mode == "remote";
    bool wantRelay  = mode == "auto" || mode == "relay";

    std::vector<Candidate> out;
    // LAN first, then internet, then Plex Relay.
    for( int pass = 0; pass < 3; ++pass )
    {
        for( size_t i = 0; i < server.connections.size(); ++i )
        {
            const PlexTv::Connection& c = server.connections[i];
            if( pass == 0 && wantLocal && c.local && !c.relay )
            {
                Candidate secure = { c.uri, L"Home network" };
                out.push_back( secure );
                // Some routers' DNS rebinding protection blocks plex.direct names for LAN
                // addresses, so also try the plain LAN address.
                char url[64];
                sprintf_s( url, "http://%s:%d", c.address.c_str(), c.port );
                Candidate plain = { url, L"Home network" };
                out.push_back( plain );
            }
            if( pass == 1 && wantRemote && !c.local && !c.relay )
            {
                Candidate remote = { c.uri, L"Internet" };
                out.push_back( remote );
            }
            if( pass == 2 && wantRelay && c.relay )
            {
                Candidate relay = { c.uri, L"Plex Relay (slow)" };
                out.push_back( relay );
            }
        }
    }
    return out;
}


bool PlexApp::ConnectToServer( std::string& error )
{
    SetStatus( L"Finding your Plex server..." );
    std::vector<PlexTv::Server> servers;
    if( !PlexTv::GetServers( m_auth.clientId, m_auth.token, servers, error ) )
        return false;

    // Pick the server named in plex.ini, else the first one this account owns.
    const PlexTv::Server* server = NULL;
    std::wstring wantName = Widen( m_cfg.serverName );
    for( size_t i = 0; i < servers.size() && !server; ++i )
        if( wantName.empty() ? servers[i].owned : servers[i].name == wantName )
            server = &servers[i];
    if( !server )
        server = &servers[0];

    std::vector<Candidate> candidates = Candidates( *server );
    if( candidates.empty() )
    {
        error = "No usable connection for this server (connection=" + m_cfg.connection + ")";
        return false;
    }

    for( size_t i = 0; i < candidates.size(); ++i )
    {
        SetStatus( L"Connecting to " + server->name + L" (" + candidates[i].label + L")..." );
        Plex::Client* client = new Plex::Client( candidates[i].url, server->accessToken, m_auth.clientId );
        std::wstring version;
        std::string pingError;
        Http::Url u;
        u.Parse( candidates[i].url );
        if( client->Ping( PING_TIMEOUT_MS, version, pingError ) )
        {
            Log::Write( "Using %s connection %s://%s:%d", u.https ? "secure" : "plain",
                        u.https ? "https" : "http", u.host.c_str(), u.port );
            m_client = client;
            for( size_t k = 0; k < server->connections.size(); ++k )
            {
                const PlexTv::Connection& c = server->connections[k];
                if( c.local && !c.relay )
                {
                    char lan[64];
                    sprintf_s( lan, "http://%s:%d", c.address.c_str(), c.port );
                    m_lanBase = lan;
                }
            }
            m_serverLabel = server->name + L" - " + candidates[i].label +
                            ( u.https ? L"" : L", unencrypted" ) + L" - Plex " + version;
            return true;
        }
        Log::Write( "Connection %s:%d failed: %s", u.host.c_str(), u.port, pingError.c_str() );
        delete client;
    }

    error = "Could not reach the server on any connection. Is Remote Access on in the server settings?";
    return false;
}


DWORD WINAPI PlexApp::JobThread( LPVOID param )
{
    PlexApp* app = (PlexApp*)param;
    Job& job = app->m_job;

    switch( job.kind )
    {
    case Job::STARTUP:
        {
            std::string err;
            if( !app->m_client )
            {
                if( Http::Startup( err, app->m_localIp ) && !app->m_cfg.logTo.empty() )
                {
                    std::string to = app->m_cfg.logTo;
                    size_t colon = to.find( ':' );
                    Log::SetRemote( to.substr( 0, colon ).c_str(),
                                    colon == std::string::npos ? 5555 : atoi( to.c_str() + colon + 1 ) );
                }
                if( !err.empty() ||
                    !app->SignIn( err ) ||
                    !app->ConnectToServer( err ) )
                {
                    job.result.error = err;
                    break;
                }
            }
            app->SetStatus( L"Loading libraries..." );
            job.result = app->m_client->Sections();
        }
        break;
    case Job::MEDIA:
        job.media = app->m_client->GetMedia( job.path, app->m_settings.directMax );
        break;
    default:
        break;
    }

    InterlockedExchange( &job.done, 1 );
    return 0;
}


void PlexApp::FinishJob()
{
    WaitForSingleObject( m_thread, INFINITE );
    CloseHandle( m_thread );
    m_thread = NULL;
    m_busy = false;
    SetStatus( L"" );

    if( m_job.kind == Job::MEDIA )
    {
        if( m_job.media.ok )
            StartPlayback( m_job.path, m_job.media, m_job.offset );
        else
            m_browser.Toast( Widen( m_job.media.error ) );
        return;
    }

    Plex::Result& r = m_job.result;
    if( !r.ok )
    {
        m_error = r.error;
        Log::Write( "Error: %s", r.error.c_str() );
        return;
    }

    m_browser.SetSession( m_client, m_auth.clientId, m_auth.token, m_username, m_serverLabel, r.items );
    m_sessionReady = true;

    if( !m_cfg.autoplay.empty() )
    {
        Log::Write( "Autoplay %s from %d s", m_cfg.autoplay.c_str(), m_cfg.autoplayStart );
        m_job.offset = m_cfg.autoplayStart;
        StartJob( Job::MEDIA, "/library/metadata/" + m_cfg.autoplay, L"" );
    }
}


HRESULT PlexApp::Update()
{
    if( m_busy && m_job.done )
        FinishJob();

    Pad* pad = ReadPads();

    Guard_Beat();

    // Log system notifications (Guide open/close etc.).
    DWORD notifyId;
    ULONG_PTR notifyParam;
    while( m_notify && XNotifyGetNext( m_notify, 0, &notifyId, &notifyParam ) )
        Log::Write( "System notification 0x%08X (param %u)", notifyId, (unsigned)notifyParam );

    // testcrash= / testhang=: crash or freeze on purpose to check that Guard recovers.
    static DWORD s_startTick = GetTickCount();
    if( m_cfg.testCrash > 0 && GetTickCount() - s_startTick > (DWORD)m_cfg.testCrash * 1000 )
    {
        Log::Write( "Test crash now" );
        RaiseException( 0xE0000001, 0, 0, NULL );
    }
    if( m_cfg.testHang > 0 && GetTickCount() - s_startTick > (DWORD)m_cfg.testHang * 1000 )
    {
        Log::Write( "Test hang now" );
        for( ;; )
            Sleep( 1000 );
    }

    // testnav=: scripted key presses, one every 1.5 s.
    static size_t s_navPos = 0;
    static DWORD s_navTick = 0;
    static DWORD s_holdUntil = 0;
    if( m_sessionReady && !m_busy && s_navPos < m_cfg.testNav.size() && GetTickCount() - s_navTick > 1500 )
    {
        s_navTick = GetTickCount();
        char k = m_cfg.testNav[s_navPos++];
        WORD key = k == 'U' ? XINPUT_GAMEPAD_DPAD_UP : k == 'D' ? XINPUT_GAMEPAD_DPAD_DOWN :
                   k == 'L' ? XINPUT_GAMEPAD_DPAD_LEFT : k == 'R' ? XINPUT_GAMEPAD_DPAD_RIGHT :
                   k == 'A' ? XINPUT_GAMEPAD_A : k == 'B' ? XINPUT_GAMEPAD_B : k == 'X' ? XINPUT_GAMEPAD_X :
                   k == 'Y' ? XINPUT_GAMEPAD_Y : k == 'H' ? XINPUT_GAMEPAD_DPAD_RIGHT : 0;
        if( key )
        {
            Log::Write( "Test key %c", k );
            pad->wPressedButtons |= key;
        }
        if( k == 'H' )
            s_holdUntil = GetTickCount() + 3000;    // H: hold Right for 3 s
    }
    if( GetTickCount() < s_holdUntil )
        pad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    WORD pressed = pad->wPressedButtons;

    if( m_player.IsActive() || m_restarting )
    {
        m_api.Pump();       // replies for a restart in progress arrive here
        UpdatePlayer( pad );
        return S_OK;
    }
    if( !m_playKey.empty() )
    {
        // Playback ended (finished, stopped or failed).
        if( m_player.Failed() && m_player.Error() == "Connection lost" && m_reconnects < 5 )
        {
            // The player's own reconnecting gave up (~20 s); start the stream again from there.
            double pos = m_player.Position();
            ++m_reconnects;
            Log::Write( "Connection lost at %.1f s, restarting (%d)", pos, m_reconnects );
            RestartPlayback( pos, false, L"Connection lost, reconnecting..." );
            return S_OK;
        }
        if( m_player.Failed() )
        {
            Log::Write( "Playback gave up: %s", m_player.Error().c_str() );
            m_browser.Toast( L"Playback failed: " + Widen( m_player.Error() ) );
        }
        ReportTimeline( "stopped" );
        if( m_transcoding && !m_session.empty() )
        {
            Plex::Client client = *m_client;
            std::string session = m_session;
            m_api.Post( [client, session]() mutable { client.StopTranscode( session ); }, std::function<void()>() );
        }
        m_transcoding = false;
        m_playKey.clear();
        m_browser.Refresh();
    }

    if( pressed & XINPUT_GAMEPAD_BACK )
    {
        Log::Write( "Exit to dashboard" );
        XLaunchNewImage( XLAUNCH_KEYWORD_DEFAULT_APP, 0 );
    }

    if( m_sessionReady )
    {
        m_api.Pump();
        if( !m_busy )
            m_browser.Update( pad );
        return S_OK;
    }

    if( !m_busy && ( pressed & XINPUT_GAMEPAD_Y ) )
        StartJob( Job::STARTUP, "", L"" );
    return S_OK;
}

void PlexApp::RenderSignIn( const std::string& code, const D3DRECT& safe )
{
    float cx = (float)( safe.x2 - safe.x1 ) / 2.0f;

    m_titleFont.Begin();
    m_titleFont.DrawText( cx, 130, COLOR_TEXT, L"Sign in to Plex", FONT_CENTER_X );
    m_titleFont.DrawText( cx, 190, COLOR_DIM, L"On your phone or computer, go to", FONT_CENTER_X );
    m_titleFont.SetScaleFactors( 1.6f, 1.6f );
    m_titleFont.DrawText( cx, 225, COLOR_ACCENT, L"plex.tv/link", FONT_CENTER_X );
    m_titleFont.SetScaleFactors( 1.0f, 1.0f );
    m_titleFont.DrawText( cx, 300, COLOR_DIM, L"and enter this code:", FONT_CENTER_X );
    m_titleFont.SetScaleFactors( 3.5f, 3.5f );
    m_titleFont.DrawText( cx, 335, COLOR_TEXT, Widen( code ).c_str(), FONT_CENTER_X );
    m_titleFont.SetScaleFactors( 1.0f, 1.0f );
    m_titleFont.End();
}


HRESULT PlexApp::Render()
{
    m_frameStart = GetTickCount();
    D3DRECT safe = SafeArea();
    if( m_player.IsActive() || m_restarting )
    {
        RenderPlayer( safe );
        EndFrame();
        return S_OK;
    }

    if( m_sessionReady )
    {
        m_browser.Render();
        if( m_busy )
        {
            Draw::Rect( 0, 0, 1280, 720, 0x90000000 );
            m_titleFont.Begin();
            m_titleFont.DrawText( (float)( safe.x2 - safe.x1 ) / 2, 320, COLOR_TEXT, Status().c_str(), FONT_CENTER_X );
            m_titleFont.End();
        }
        EndFrame();
        return S_OK;
    }

    FillGradient( COLOR_BG_TOP, COLOR_BG_BOTTOM );
    float width = (float)( safe.x2 - safe.x1 );
    std::string pinCode = PinCode();
    std::wstring status = Status();

    m_titleFont.Begin();
    m_titleFont.SetScaleFactors( 2.2f, 2.2f );
    m_titleFont.DrawText( width / 2, pinCode.empty() ? 230.0f : 20.0f, COLOR_ACCENT, L"MULTIPLEX 360", FONT_CENTER_X );
    m_titleFont.SetScaleFactors( 1.0f, 1.0f );
    m_titleFont.End();

    if( !pinCode.empty() )
        RenderSignIn( pinCode, safe );

    m_font.Begin();
    if( !m_error.empty() )
    {
        m_font.DrawText( width / 2, 340, COLOR_ERROR, Widen( m_error ).c_str(), FONT_CENTER_X );
        m_font.DrawText( width / 2, 372, COLOR_DIM, L"Press Y to retry      BACK to exit", FONT_CENTER_X );
    }
    else if( !status.empty() )
        m_font.DrawText( width / 2, pinCode.empty() ? 340.0f : 560.0f, COLOR_DIM, status.c_str(), FONT_CENTER_X );
    if( !m_localIp.empty() )
        m_font.DrawText( width, (float)( safe.y2 - safe.y1 ) - 28, 0xff505050, ( L"Console " + Widen( m_localIp ) ).c_str(),
                         FONT_RIGHT );
    m_font.End();

    EndFrame();
    return S_OK;
}

//--------------------------------------------------------------------------------------
// Playback
//--------------------------------------------------------------------------------------
namespace
{

    bool OneOf( const std::string& s, const char* const* list )
    {
        for( ; *list; ++list )
            if( s == *list )
                return true;
        return false;
    }

    // Decode cost grows with bitrate, so heavy files are converted even at a playable size.
    bool CanDirectPlay( const Plex::Media& m, int maxHeight, int maxKbps )
    {
        static const char* const video[] = { "h264", "mpeg4", 0 };
        static const char* const audio[] = { "aac", "ac3", "eac3", "mp3", "mp2", "dca", "", 0 };
        static const char* const containers[] = { "mkv", "mp4", "m4v", "mov", "avi", "mpegts", "ts", 0 };
        return OneOf( m.videoCodec, video ) && OneOf( m.audioCodec, audio ) && OneOf( m.container, containers ) &&
               m.height > 0 && m.height <= maxHeight && ( m.bitrate == 0 || m.bitrate <= maxKbps );
    }
}

void PlexApp::StartPlayback( const std::string& key, const Plex::Media& media, double offset )
{
    if( !m_player.Startup( m_pd3dDevice ) )
    {
        m_error = "Video playback is unavailable on this console";
        return;
    }
    fg_set_decode_threads( m_cfg.threads );
    // Server calls go through m_api (one worker, in order) so the UI never blocks on them.
    m_player.Stop();
    bool endingTranscode = m_transcoding && !m_session.empty();
    if( endingTranscode )
    {
        Plex::Client client = *m_client;
        std::string session = m_session;
        m_api.Post( [client, session]() mutable { client.StopTranscode( session ); }, std::function<void()>() );
    }
    if( !m_restarting )
        m_restartMessage = L"Starting...";
    int generation = ++m_startGeneration;

    m_playKey = key;
    m_playMedia = media;
    m_playTitle = media.title;
    m_screen.Reset();
    std::vector<std::string> urls;
    std::vector<std::wstring> labels;

    const Plex::Stream* audio = media.SelectedStream( 2 );
    m_burnSubs = media.SelectedStream( 3 ) != NULL;     // Plex draws subtitles into the picture
    fg_set_audio_track( audio ? audio->index : -1 );
    if( m_qualityMode == QUALITY_ORIGINAL && !m_burnSubs )
        m_transcoding = false;
    else if( m_qualityMode == QUALITY_PRESET || m_burnSubs )
        m_transcoding = true;
    else
    {
        // At home the decoder is the limit, not the network: up to 8 Mbit/s plays smoothly.
        // 1080p H.264 only decodes at ~9 fps on Xenon, so directMax stays at 720.
        bool home = m_serverLabel.find( L"Home network" ) != std::wstring::npos;
        int maxKbps = home ? ( m_settings.kbps > 8000 ? m_settings.kbps : 8000 ) : m_settings.kbps;
        m_transcoding = !CanDirectPlay( media, m_settings.directMax, maxKbps );
    }
    Log::Write( "Playback decision: %s (quality mode %d, burn subtitles %d, server %ls)",
                m_transcoding ? "transcode" : "direct", m_qualityMode, m_burnSubs ? 1 : 0, m_serverLabel.c_str() );
    m_player.SetBrightness( m_settings.brightness * 0.02f );
    m_player.SetAspect( m_settings.aspect );
    m_timelineTick = GetTickCount();
    if( !m_transcoding )
    {
        // At home the file comes over plain http (HTTPS decryption caps out near 8 Mbit/s on
        // Xenon), so it carries a transient token, not the account token. New session id per
        // play, or the server may still apply our last transcode-only session and return 503.
        bool lan = !m_lanBase.empty() && m_serverLabel.find( L"Home network" ) != std::wstring::npos;
        std::string base = m_client->Base(), lanBase = m_lanBase, partKey = media.partKey;
        std::string clientHeaders = PlexTv::ClientHeaders( m_auth.clientId ) + "X-Plex-Session-Identifier: " +
                                    NewClientId().substr( 8 ) + "\r\n";
        Log::Write( "Direct play %s (%s %s/%s %dp %d kbps)", key.c_str(), media.container.c_str(),
                    media.videoCodec.c_str(), media.audioCodec.c_str(), media.height, media.bitrate );

        // Runs after any transcode stop queued above (one stream per device).
        m_restarting = true;
        Plex::Client client = *m_client;
        std::shared_ptr<std::string> transient( new std::string );
        m_api.Post( [client, lan, transient]() mutable {
                        if( lan && !client.TransientToken( *transient ) )
                            Log::Write( "No transient token; streaming over HTTPS only" );
                    },
                    [this, generation, lan, base, lanBase, partKey, clientHeaders, transient, offset]() {
                        if( generation != m_startGeneration || !m_restarting )
                            return;
                        std::vector<std::string> urls;
                        std::vector<std::wstring> labels;
                        std::string token = m_client->Token();
                        if( lan && !transient->empty() )
                        {
                            urls.push_back( lanBase + partKey );
                            labels.push_back( L"direct play (LAN)" );
                            token = *transient;
                        }
                        urls.push_back( base + partKey );
                        labels.push_back( L"direct play" );
                        m_player.Open( urls, labels, clientHeaders + "X-Plex-Token: " + token + "\r\n", offset );
                        m_restarting = false;
                    } );
        return;
    }

    // Converted by the server to H.264 + stereo AAC.
    int height = m_settings.quality, kbps = m_settings.kbps;
    if( m_qualityMode == QUALITY_PRESET )
    {
        height = QUALITY_PRESETS[m_qualityPreset].height;
        kbps = QUALITY_PRESETS[m_qualityPreset].kbps;
    }
    // Fixed session id: a new transcode replaces a leftover one instead of getting 503.
    m_session = m_auth.clientId;
    std::string url = m_client->TranscodeUrl( key, offset, height, kbps, m_session, m_burnSubs, media.mediaIndex );
    // /start returns 400 without a /decision first.
    std::string decision = url;
    decision.replace( decision.find( "/start?" ), 7, "/decision?" );
    urls.push_back( url );
    wchar_t label[64];
    swprintf_s( label, L"converted by Plex to %dp %g Mbps", height, kbps / 1000.0 );
    labels.push_back( label );
    Log::Write( "Transcode %s from %.0f s at %dp %d kbps (%s %s/%s %dp %d kbps)", key.c_str(), offset, height, kbps,
                media.container.c_str(), media.videoCodec.c_str(), media.audioCodec.c_str(), media.height, media.bitrate );

    m_restarting = true;
    std::string headers = m_client->TranscodeHeaders();
    double duration = media.duration;
    m_api.Post( [decision, headers]() {
                    Http::Response d = Http::Get( decision, headers, 10000 );
                    Log::Write( "Transcode decision -> %d", d.status );
                },
                [this, generation, urls, labels, headers, offset, duration]() {
                    if( generation != m_startGeneration || !m_restarting )
                        return;     // cancelled, or replaced by a newer start
                    m_player.Open( urls, labels, headers, 0.0, offset, duration );
                    m_restarting = false;
                } );
}

void PlexApp::RestartPlayback( double position, bool selectStreams, const std::wstring& message )
{
    m_restartMessage = message;
    m_restarting = true;
    m_player.Stop();
    if( !selectStreams )
    {
        StartPlayback( m_playKey, m_playMedia, position );
        return;
    }
    // The server keeps the track choice per file.
    const Plex::Stream* a = m_playMedia.SelectedStream( 2 );
    const Plex::Stream* s = m_playMedia.SelectedStream( 3 );
    Plex::Client client = *m_client;
    std::string partId = m_playMedia.partId;
    int audioId = a ? a->id : 0, subId = s ? s->id : 0;
    int generation = ++m_startGeneration;
    Log::Write( "Tracks changed: audio %d, subtitles %d", audioId, subId );
    m_api.Post( [client, partId, audioId, subId]() mutable { client.SelectStreams( partId, audioId, subId ); },
                [this, generation, position]() {
                    if( generation == m_startGeneration && m_restarting )
                        StartPlayback( m_playKey, m_playMedia, position );
                } );
}

void PlexApp::StopPlayback()
{
    m_player.Stop();
}

void PlexApp::PlayItem( const Plex::Item& item, double start )
{
    if( m_busy || !m_client )
        return;
    m_job.offset = start;
    m_qualityMode = QUALITY_AUTO;    // per-video choice from the options menu
    m_reconnects = 0;
    StartJob( Job::MEDIA, "/library/metadata/" + item.ratingKey, item.title );
}

void PlexApp::ReportTimeline( const char* state )
{
    if( !m_client || m_playMedia.ratingKey.empty() )
        return;
    Plex::Client client = *m_client;
    std::string key = m_playMedia.ratingKey, st = state;
    double pos = m_player.IsActive() ? m_player.Position() : m_lastPosition;
    double dur = m_playMedia.duration;
    m_api.Post( [client, key, st, pos, dur]() mutable { client.Timeline( key, st.c_str(), pos, dur ); },
                std::function<void()>() );
}

void PlexApp::ApplySettings()
{
    m_player.SetBrightness( m_settings.brightness * 0.02f );
    m_player.SetAspect( m_settings.aspect );
}

void PlexApp::SignOut()
{
    Log::Write( "Signing out" );
    m_auth.token.clear();
    m_auth.Save( AUTH_PATH );
    m_sessionReady = false;
    delete m_client;
    m_client = NULL;
    m_username.clear();
    m_images.Clear();
    StartJob( Job::STARTUP, "", L"" );
}

void PlexApp::SwitchUser( const std::string& token, const std::wstring& name )
{
    Log::Write( "Switching Plex Home user" );
    m_auth.token = token;
    m_auth.Save( AUTH_PATH );
    m_sessionReady = false;
    delete m_client;
    m_client = NULL;
    m_username = name;
    m_images.Clear();
    StartJob( Job::STARTUP, "", L"" );
}


//--------------------------------------------------------------------------------------
// Player screen (PlayerScreen does the timeline, seeking and menu; these are the Plex parts)
//--------------------------------------------------------------------------------------
void PlexApp::Seek( double target )
{
    if( !m_transcoding )
        m_player.SeekAbsolute( target );
    else
        RestartPlayback( target, false, L"Jumping to " + FormatTime( target ) );   // a live transcode restarts there
}

void PlexApp::Leave()
{
    StopPlayback();
}

void PlexApp::CancelStart()
{
    m_restarting = false;
    ++m_startGeneration;
}

std::wstring PlexApp::Tag() const
{
    if( !m_transcoding )
        return L"Original";
    int h = m_settings.quality, k = m_settings.kbps;
    if( m_qualityMode == QUALITY_PRESET )
    {
        h = QUALITY_PRESETS[m_qualityPreset].height;
        k = QUALITY_PRESETS[m_qualityPreset].kbps;
    }
    wchar_t buf[48];
    swprintf_s( buf, L"%dp  %g Mbps%s", h, k / 1000.0, m_burnSubs ? L"  (subtitles)" : L"" );
    return buf;
}

IDirect3DTexture9* PlexApp::Preview( double seconds )
{
    if( !m_playMedia.hasPreviews || m_playMedia.partId.empty() )
        return NULL;
    char path[96];
    sprintf_s( path, "/library/parts/%s/indexes/sd/%u", m_playMedia.partId.c_str(), (unsigned)( seconds / 10 ) * 10000u );
    m_images.Pump();
    return m_images.Get( path, 320, 180 );
}

std::wstring PlexApp::StatsMode() const
{
    wchar_t buf[64];
    swprintf_s( buf, L"%d decode threads   %s", m_cfg.threads, m_transcoding ? L"server transcoding" : L"direct play" );
    return buf;
}

void PlexApp::UpdatePlayer( Pad* pad )
{
    m_player.Update();

    // benchmark=N: play N seconds and log totals; -N: decode-only speed test.
    static DWORD s_benchStart = 0;
    static long s_benchFrom = 0;
    if( m_cfg.benchmark < 0 && m_player.IsActive() )
    {
        FFPlayer::Stats st;
        m_player.GetStats( st );
        if( !s_benchStart && st.decoded > 0 )
        {
            m_player.SetSpeedTest( true );
            s_benchStart = GetTickCount();
            s_benchFrom = st.decoded;
        }
        else if( s_benchStart && GetTickCount() - s_benchStart > (DWORD)-m_cfg.benchmark * 1000 )
        {
            float secs = ( GetTickCount() - s_benchStart ) / 1000.0f;
            Log::Write( "BENCHMARK %s speed: %.1f decoded fps over %.0f s (%ld frames, %.1f MB)", m_cfg.autoplay.c_str(),
                        ( st.decoded - s_benchFrom ) / secs, secs, st.decoded - s_benchFrom, st.bytesRead / 1048576.0 );
            m_player.SetSpeedTest( false );
            s_benchStart = 0;
            m_cfg.benchmark = 0;
            StopPlayback();
            return;
        }
    }
    if( m_cfg.benchmark > 0 && m_player.IsActive() && !m_player.IsBuffering() )
    {
        if( !s_benchStart )
        {
            s_benchStart = GetTickCount();
            double a, b, c, d;
            fg_profile( &a, &b, &c, &d, 1 );
        }
        else if( GetTickCount() - s_benchStart > (DWORD)m_cfg.benchmark * 1000 )
        {
            FFPlayer::Stats st;
            m_player.GetStats( st );
            float secs = ( GetTickCount() - s_benchStart ) / 1000.0f;
            Log::Write( "BENCHMARK %s: %.0f s, decoded %ld, shown %ld, dropped %ld, catch-ups %ld, %.1f MB",
                        m_cfg.autoplay.c_str(), secs, st.decoded, st.shown, st.dropped, st.catchups,
                        st.bytesRead / 1048576.0 );
            double cabac, recon, filter, wait;
            fg_profile( &cabac, &recon, &filter, &wait, 0 );
            double work = recon - wait;
            double total = cabac + work + filter;
            Log::Write( "BENCHMARK split per frame: cabac %.1f ms (%.0f%%), reconstruction %.1f ms (%.0f%%), loop filter %.1f ms (%.0f%%); "
                        "waiting for reference frames %.1f ms",
                        cabac / st.decoded, 100 * cabac / total, work / st.decoded, 100 * work / total,
                        filter / st.decoded, 100 * filter / total, wait / st.decoded );
            s_benchStart = 0;
            m_cfg.benchmark = 0;
            StopPlayback();
            return;
        }
    }
    if( m_player.IsActive() )
        m_lastPosition = m_player.Position();
    if( GetTickCount() - m_timelineTick > 10000 && m_player.IsActive() && !m_player.IsBuffering() )
    {
        m_timelineTick = GetTickCount();
        ReportTimeline( m_player.IsPaused() ? "paused" : "playing" );
    }

    // testseek=N: seek N seconds every 12 s, three times.
    static int s_testSeeks = 0;
    static DWORD s_testTick = GetTickCount();
    if( m_player.IsActive() && m_cfg.testSeek != 0 && s_testSeeks < 3 && GetTickCount() - s_testTick > 12000 )
    {
        s_testTick = GetTickCount();
        ++s_testSeeks;
        Log::Write( "Test seek %d: %+d s", s_testSeeks, m_cfg.testSeek );
        Seek( m_player.Position() + m_cfg.testSeek );
        return;
    }
    if( m_cfg.testOsd && m_player.IsActive() )
        m_screen.ShowFakeSeek( m_player.Position() + 754 );

    m_screen.Update( pad );
}

void PlexApp::RenderPlayer( const D3DRECT& safe )
{
    m_pd3dDevice->Clear( 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0 );
    D3DRECT full = { 0, 0, (LONG)m_d3dpp.BackBufferWidth, (LONG)m_d3dpp.BackBufferHeight };
    m_player.RenderFrame( full );
    m_screen.Render( safe, m_renderMs );
}

void PlexApp::EndFrame()
{
    m_renderMs = m_renderMs * 0.9f + ( GetTickCount() - m_frameStart ) * 0.1f;

    // Present() spins until vblank and starved hw thread 0's sibling; sleep most of the
    // frame first and wake ~3 ms before vblank.
    static DWORD s_lastPresent = 0;
    DWORD elapsed = GetTickCount() - s_lastPresent;
    if( elapsed < 13 )
        Sleep( 13 - elapsed );
    m_pd3dDevice->Present( NULL, NULL, NULL, NULL );
    s_lastPresent = GetTickCount();
}

//--------------------------------------------------------------------------------------
// Options menu
//--------------------------------------------------------------------------------------
std::wstring PlexApp::TrackName( int streamType ) const
{
    const Plex::Stream* s = m_playMedia.SelectedStream( streamType );
    if( !s )
        return streamType == 3 ? L"Off" : L"Default";
    if( !s->shortTitle.empty() )
        return s->shortTitle;
    return s->title.empty() ? Widen( s->language ) : s->title;
}

std::vector<int> PlexApp::TrackList( int streamType ) const
{
    std::vector<int> list;
    if( streamType == 3 )
        list.push_back( -1 );       // subtitles off
    for( size_t i = 0; i < m_playMedia.streams.size(); ++i )
        if( m_playMedia.streams[i].streamType == streamType )
            list.push_back( (int)i );
    return list;
}

std::wstring PlexApp::QualityName() const
{
    if( m_qualityMode == QUALITY_ORIGINAL )
        return m_burnSubs ? L"Original - converted for subtitles" : L"Original";
    if( m_qualityMode == QUALITY_PRESET )
    {
        wchar_t buf[48];
        swprintf_s( buf, L"%dp  %g Mbps", QUALITY_PRESETS[m_qualityPreset].height,
                    QUALITY_PRESETS[m_qualityPreset].kbps / 1000.0 );
        return buf;
    }
    return L"Auto";
}

void PlexApp::BuildMenu( int page, MenuPage& out )
{
    MenuEntry e;
    wchar_t buf[48];
    switch( page )
    {
    case PAGE_MAIN:
        out.title = L"Options";
        e.opens = true;
        e.label = L"Quality";    e.value = QualityName();  out.entries.push_back( e );
        e.label = L"Audio";      e.value = TrackName( 2 ); out.entries.push_back( e );
        e.label = L"Subtitles";  e.value = TrackName( 3 ); out.entries.push_back( e );
        swprintf_s( buf, L"%+d", m_settings.brightness );
        e.label = L"Brightness"; e.value = buf;            out.entries.push_back( e );
        e.label = L"Aspect ratio"; e.value = FFPlayer::AspectName( m_settings.aspect ); out.entries.push_back( e );
        e.label = L"Aspect ratio"; e.value = FFPlayer::AspectName( m_settings.aspect ); out.entries.push_back( e );
        e.opens = false;
        e.label = L"Stats for nerds"; e.value = m_settings.stats ? L"On" : L"Off"; out.entries.push_back( e );
        break;

    case PAGE_QUALITY:
        out.title = L"Quality";
        e.label = L"Auto";      e.value = L"recommended";     e.checked = m_qualityMode == QUALITY_AUTO;     out.entries.push_back( e );
        e.label = L"Original";
        e.value = m_burnSubs ? L"not with subtitles on" : L"no conversion";
        e.checked = m_qualityMode == QUALITY_ORIGINAL;
        out.entries.push_back( e );
        for( int i = 0; i < QUALITY_HEIGHT_COUNT; ++i )
        {
            swprintf_s( buf, L"%dp", QUALITY_HEIGHTS[i] );
            e.label = buf;
            e.value = L"";
            e.opens = true;
            e.checked = m_qualityMode == QUALITY_PRESET && QUALITY_PRESETS[m_qualityPreset].height == QUALITY_HEIGHTS[i];
            out.entries.push_back( e );
        }
        break;

    case PAGE_BITRATE:
        swprintf_s( buf, L"%dp", m_menuHeight );
        out.title = buf;
        for( int i = 0; i < QUALITY_PRESET_COUNT; ++i )
            if( QUALITY_PRESETS[i].height == m_menuHeight )
            {
                e.label = QUALITY_PRESETS[i].bitrateLabel;
                e.value = L"";
                e.checked = m_qualityMode == QUALITY_PRESET && m_qualityPreset == i;
                out.entries.push_back( e );
            }
        break;

    case PAGE_AUDIO:
    case PAGE_SUBS:
        {
            int type = page == PAGE_AUDIO ? 2 : 3;
            out.title = type == 2 ? L"Audio" : L"Subtitles";
            std::vector<int> list = TrackList( type );
            const Plex::Stream* selected = m_playMedia.SelectedStream( type );
            for( size_t i = 0; i < list.size(); ++i )
            {
                if( list[i] < 0 )
                {
                    e.label = L"Off";
                    e.checked = selected == NULL;
                }
                else
                {
                    const Plex::Stream& s = m_playMedia.streams[list[i]];
                    e.label = s.title.empty() ? Widen( s.language ) : s.title;
                    if( e.label.empty() )
                        e.label = L"Track";
                    e.checked = s.selected;
                }
                e.value = L"";
                out.entries.push_back( e );
            }
            if( out.entries.empty() )
            {
                e.label = L"No other tracks";
                e.checked = false;
                out.entries.push_back( e );
            }
        }
        break;

    case PAGE_BRIGHTNESS:
        out.title = L"Brightness";
        out.slider = true;
        out.value = m_settings.brightness;
        out.minValue = -5;
        out.maxValue = 10;
        break;

    case PAGE_ASPECT:
        out.title = L"Aspect ratio";
        for( int i = 0; i < FFPlayer::ASPECT_COUNT; ++i )
        {
            e.label = FFPlayer::AspectName( i );
            e.checked = i == m_settings.aspect;
            out.entries.push_back( e );
        }
        break;
    }
}

MenuMove PlexApp::Choose( int page, int sel )
{
    double pos = m_player.Position();
    switch( page )
    {
    case PAGE_MAIN:
        switch( sel )
        {
        case ROW_QUALITY:
            {
                int start = m_qualityMode == QUALITY_PRESET ? 2 : m_qualityMode;
                if( m_qualityMode == QUALITY_PRESET )
                    for( int i = 0; i < QUALITY_HEIGHT_COUNT; ++i )
                        if( QUALITY_HEIGHTS[i] == QUALITY_PRESETS[m_qualityPreset].height )
                            start = 2 + i;
                return MenuMove::Open( PAGE_QUALITY, start );
            }
        case ROW_AUDIO:
        case ROW_SUBS:
            {
                int sub = sel == ROW_AUDIO ? PAGE_AUDIO : PAGE_SUBS;
                MenuPage list;
                BuildMenu( sub, list );
                int start = 0;
                for( size_t i = 0; i < list.entries.size(); ++i )
                    if( list.entries[i].checked )
                        start = (int)i;
                return MenuMove::Open( sub, start );
            }
        case ROW_BRIGHTNESS:
            return MenuMove::Open( PAGE_BRIGHTNESS, 0 );
        case ROW_ASPECT:
            return MenuMove::Open( PAGE_ASPECT, m_settings.aspect );
        case ROW_STATS:
            m_settings.stats = !m_settings.stats;
            m_settings.Save( "game:\\settings.ini" );
            return MenuMove::Stay();
        }
        break;

    case PAGE_QUALITY:
        if( sel < 2 )
        {
            int mode = sel == 0 ? QUALITY_AUTO : QUALITY_ORIGINAL;
            if( mode != m_qualityMode )
            {
                m_qualityMode = mode;
                Log::Write( "Quality: %s", mode == QUALITY_AUTO ? "auto" : "original" );
                RestartPlayback( pos, false, L"Changing quality..." );
            }
            return MenuMove::Close();
        }
        else
        {
            m_menuHeight = QUALITY_HEIGHTS[sel - 2];
            int start = 0, n = 0;
            for( int i = 0; i < QUALITY_PRESET_COUNT; ++i )
                if( QUALITY_PRESETS[i].height == m_menuHeight )
                {
                    if( m_qualityMode == QUALITY_PRESET && m_qualityPreset == i )
                        start = n;
                    ++n;
                }
            return MenuMove::Open( PAGE_BITRATE, start );
        }

    case PAGE_BITRATE:
        {
            int n = 0, chosen = -1;
            for( int i = 0; i < QUALITY_PRESET_COUNT; ++i )
                if( QUALITY_PRESETS[i].height == m_menuHeight && n++ == sel )
                    chosen = i;
            if( chosen >= 0 && !( m_qualityMode == QUALITY_PRESET && m_qualityPreset == chosen ) )
            {
                m_qualityMode = QUALITY_PRESET;
                m_qualityPreset = chosen;
                Log::Write( "Quality: %dp %d kbps", QUALITY_PRESETS[chosen].height, QUALITY_PRESETS[chosen].kbps );
                RestartPlayback( pos, false, L"Changing quality..." );
            }
            return MenuMove::Close();
        }

    case PAGE_AUDIO:
    case PAGE_SUBS:
        {
            int type = page == PAGE_AUDIO ? 2 : 3;
            std::vector<int> list = TrackList( type );
            if( sel >= (int)list.size() )
                return MenuMove::Close();
            int pick = list[sel];
            bool changed = false;
            for( size_t i = 0; i < m_playMedia.streams.size(); ++i )
                if( m_playMedia.streams[i].streamType == type )
                {
                    bool on = (int)i == pick;
                    changed |= m_playMedia.streams[i].selected != on;
                    m_playMedia.streams[i].selected = on;
                }
            if( changed )
                RestartPlayback( pos, true, type == 2 ? L"Changing audio..." : L"Changing subtitles..." );
            return MenuMove::Close();
        }

    case PAGE_ASPECT:
        m_player.SetAspect( sel );
        AspectChanged();
        return MenuMove::Close();
    }
    return MenuMove::Stay();
}

void PlexApp::AspectChanged()
{
    m_settings.aspect = m_player.Aspect();
    m_settings.Save( "game:\\settings.ini" );
}

MenuMove PlexApp::Back( int page )
{
    switch( page )
    {
    case PAGE_BITRATE:
        for( int i = 0; i < QUALITY_HEIGHT_COUNT; ++i )
            if( QUALITY_HEIGHTS[i] == m_menuHeight )
                return MenuMove::Open( PAGE_QUALITY, 2 + i );
        return MenuMove::Open( PAGE_QUALITY, 0 );
    case PAGE_QUALITY:    return MenuMove::Open( PAGE_MAIN, ROW_QUALITY );
    case PAGE_AUDIO:      return MenuMove::Open( PAGE_MAIN, ROW_AUDIO );
    case PAGE_SUBS:       return MenuMove::Open( PAGE_MAIN, ROW_SUBS );
    case PAGE_BRIGHTNESS: return MenuMove::Open( PAGE_MAIN, ROW_BRIGHTNESS );
    case PAGE_ASPECT:     return MenuMove::Open( PAGE_MAIN, ROW_ASPECT );
    }
    return MenuMove::Close();
}

void PlexApp::SliderStep( int page, int step )
{
    if( page != PAGE_BRIGHTNESS )
        return;
    m_settings.brightness = max( -5, min( 10, m_settings.brightness + step ) );
    m_settings.Save( "game:\\settings.ini" );
    ApplySettings();
}
