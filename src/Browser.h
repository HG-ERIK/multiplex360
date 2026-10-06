#pragma once
#include <xtl.h>
#include "App.h"
#include "Font.h"
#include <string>
#include <vector>
#include <functional>
#include "PlexApi.h"
#include "PlexTv.h"
#include "Tasks.h"
#include "ImageCache.h"

// User-adjustable settings, saved to game:\settings.ini.
struct UserSettings
{
    int  quality;       // height Plex converts to: 480, 576 or 720
    int  kbps;          // conversion bitrate, also the direct-play limit
    int  directMax;     // tallest file played without conversion
    bool stats;         // stats-for-nerds overlay during playback
    int  brightness;    // -5..+10, steps of 0.02 added to the picture

    UserSettings() : quality( 720 ), kbps( 4000 ), directMax( 720 ), stats( false ), brightness( 0 ) {}
    void Load( const char* path );
    void Save( const char* path ) const;
};

struct BrowserHost
{
    std::function<void( const Plex::Item& item, double startSeconds )> play;
    std::function<void()>                                             signOut;
    std::function<void( const std::string& token, const std::wstring& name )> switchUser;
    std::function<void()>                                             settingsChanged;
    std::function<void()>                                             exitApp;
};

class Browser
{
public:
    Browser();

    void Startup( IDirect3DDevice9* device, Font* font, Font* titleFont, const D3DRECT& safe,
                  TaskQueue* tasks, ImageCache* images, UserSettings* settings, const BrowserHost& host );

    // Called once connected (and again after switching user).
    void SetSession( Plex::Client* client, const std::string& clientId, const std::string& accountToken,
                     const std::wstring& userName, const std::wstring& serverLabel,
                     const std::vector<Plex::Item>& sections );

    void Update( Pad* pad );
    void Render();

    // After playback: refresh progress on the current screen.
    void Refresh();
    void Toast( const std::wstring& text );

private:
    enum ScreenType { HOME, LIBRARY, DETAILS, SEASON, SETTINGS, USERS, PIN };

    struct Screen
    {
        ScreenType               type;
        int                      id;          // tags async loads; stale results are dropped
        bool                     loading;
        std::string              error;
        std::wstring             title;

        // HOME
        std::vector<Plex::Hub>   hubs;
        int                      row, col;
        std::vector<int>         rowScroll;

        // LIBRARY / SEASON / USERS: a flat list of items
        std::string              path;
        std::vector<Plex::Item>  items;
        int                      total;
        bool                     loadingMore;
        int                      sel, scroll;

        // DETAILS
        Plex::Item               item;        // the movie/show itself
        std::vector<Plex::Item>  children;    // seasons of a show
        int                      button;      // focused button / season

        // USERS / PIN
        std::vector<PlexTv::HomeUser> users;
        PlexTv::HomeUser         pinUser;
        int                      digits[4];
        int                      digitPos;

        Screen() : type( HOME ), id( 0 ), loading( false ), row( 0 ), col( 0 ), total( 0 ), loadingMore( false ),
                   sel( 0 ), scroll( 0 ), button( 0 ), digitPos( 0 )
        {
            digits[0] = digits[1] = digits[2] = digits[3] = 0;
        }
    };

    // navigation
    void     Push( ScreenType type );
    void     Pop();
    Screen&  Top() { return m_stack.back(); }
    Screen*  FindScreen( int id );
    void     OpenItem( const Plex::Item& item );
    void     OpenSection( int index );

    // loading
    void     LoadHome( Screen& s );
    void     LoadLibrary( Screen& s, bool more );
    void     LoadDetails( Screen& s );
    void     LoadSeason( Screen& s );
    void     LoadUsers( Screen& s );

    // input per screen
    void     UpdateSidebar( WORD pressed );
    void     UpdateHome( Screen& s, WORD pressed );
    void     UpdateGrid( Screen& s, WORD pressed );
    void     UpdateDetails( Screen& s, WORD pressed );
    void     UpdateSeason( Screen& s, WORD pressed );
    void     UpdateSettings( Screen& s, WORD pressed );
    void     UpdateUsers( Screen& s, WORD pressed );
    void     UpdatePin( Screen& s, WORD pressed );

    // drawing
    void     RenderBackground( const std::string& art );
    void     RenderSidebar();
    void     RenderHome( Screen& s );
    void     RenderGrid( Screen& s );
    void     RenderDetails( Screen& s );
    void     RenderSeason( Screen& s );
    void     RenderSettings( Screen& s );
    void     RenderUsers( Screen& s );
    void     RenderPin( Screen& s );
    void     RenderLoading( Screen& s );
    void     Poster( const Plex::Item& item, float x, float y, float w, float h, bool focused, bool wide = false );
    void     Text( Font* font, float x, float y, DWORD color, const std::wstring& s, float scale = 1.0f,
                   DWORD flags = 0, float maxWidth = 0 );
    int      WrapText( Font* font, float x, float y, float width, int maxLines, DWORD color,
                       const std::wstring& s, float scale = 1.0f );
    std::wstring Duration( int ms ) const;

    IDirect3DDevice9*        m_device;
    Font*               m_font;
    Font*               m_titleFont;
    D3DRECT                  m_safe;
    TaskQueue*               m_tasks;
    ImageCache*              m_images;
    UserSettings*            m_settings;
    BrowserHost              m_host;

    Plex::Client*            m_client;
    std::string              m_clientId;
    std::string              m_accountToken;
    std::wstring             m_userName;
    std::wstring             m_serverLabel;
    std::vector<Plex::Item>  m_sections;

    std::vector<Screen>      m_stack;
    int                      m_nextId;
    bool                     m_sidebarFocus;
    int                      m_sidebarSel;    // Home, sections, Settings, Switch user, Exit
    std::wstring             m_toast;
    DWORD                    m_toastTick;
    DWORD                    m_repeatTick;    // held-direction auto repeat
    WORD                     m_repeatKey;
};
