#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include "Browser.h"
#include "Quality.h"
#include "Draw.h"
#include "Log.h"

namespace
{
    const DWORD C_BG        = 0xFF141414;
    const DWORD C_SIDEBAR   = 0xF0101010;
    const DWORD C_PANEL     = 0xFF1F1F1F;
    const DWORD C_PLACEHOLD = 0xFF2B2B2B;
    const DWORD C_ACCENT    = 0xFFE5A00D;   // Plex orange
    const DWORD C_TEXT      = 0xFFF0F0F0;
    const DWORD C_DIM       = 0xFF9A9A9A;
    const DWORD C_FAINT     = 0xFF606060;
    const DWORD C_ERROR     = 0xFFFF6A6A;

    const float SIDEBAR_W   = 250.0f;
    const float CONTENT_X   = 290.0f;
    const float CONTENT_R   = 1216.0f;
    const float TOP_Y       = 56.0f;

    const float HUB_POSTER_W = 150.0f, HUB_POSTER_H = 225.0f, HUB_GAP = 22.0f, HUB_ROW_H = 345.0f;
    const int   GRID_COLS    = 6;
    const float GRID_POSTER_W = 130.0f, GRID_POSTER_H = 195.0f, GRID_ROW_H = 258.0f;
    const int   LIBRARY_PAGE = 120;

    const DWORD REPEAT_DELAY_MS = 380, REPEAT_RATE_MS = 110;

    const char* const SETTINGS_PATH = "game:\\settings.ini";

    enum SettingRow { SET_ACCOUNT, SET_SWITCH, SET_SIGNOUT, SET_SERVER, SET_QUALITY, SET_DIRECT,
                      SET_BRIGHTNESS, SET_STATS, SET_COUNT };

    std::wstring Widen( const std::string& s )
    {
        return std::wstring( s.begin(), s.end() );
    }

    template<typename T> T Clamp( T v, T lo, T hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

    std::string PosterOf( const Plex::Item& it )
    {
        if( it.type == "episode" && !it.grandparentThumb.empty() )
            return it.grandparentThumb;
        if( it.type == "season" && it.thumb.empty() )
            return it.parentThumb;
        return it.thumb;
    }

    // The fonts cover Latin-1, Latin Extended-A, dashes, quotes, bullet, ellipsis and €.
    std::wstring Displayable( const std::wstring& s )
    {
        std::wstring out( s );
        for( size_t i = 0; i < out.size(); ++i )
        {
            wchar_t c = out[i];
            if( c == 0x2212 || c == 0x2010 || c == 0x2011 )
                out[i] = L'-';
            else if( c > 0x17F && !( c >= 0x2013 && c <= 0x2014 ) && !( c >= 0x2018 && c <= 0x201E ) &&
                     c != 0x2022 && c != 0x2026 && c != 0x20AC )
                out[i] = L'?';
        }
        return out;
    }

    std::wstring TitleOf( const Plex::Item& it )
    {
        return it.type == "episode" && !it.grandparentTitle.empty() ? it.grandparentTitle : it.title;
    }
}


//--------------------------------------------------------------------------------------
// Settings
//--------------------------------------------------------------------------------------
void UserSettings::Load( const char* path )
{
    FILE* f = fopen( path, "rb" );
    if( !f )
        return;
    char line[256];
    while( fgets( line, sizeof( line ), f ) )
    {
        char key[64];
        int value;
        if( sscanf_s( line, "%63[^=]=%d", key, (unsigned)sizeof( key ), &value ) != 2 )
            continue;
        std::string k( key );
        if( k == "quality" )         quality = value;
        else if( k == "kbps" )       kbps = value;
        else if( k == "directmax" )  directMax = value;
        else if( k == "stats" )      stats = value != 0;
        else if( k == "brightness" ) brightness = value;
    }
    fclose( f );
    int preset = NearestQualityPreset( quality, kbps );
    quality = QUALITY_PRESETS[preset].height;
    kbps = QUALITY_PRESETS[preset].kbps;
}

void UserSettings::Save( const char* path ) const
{
    FILE* f = fopen( path, "wb" );
    if( !f )
        return;
    fprintf( f, "quality=%d\r\nkbps=%d\r\ndirectmax=%d\r\nstats=%d\r\nbrightness=%d\r\n", quality, kbps, directMax,
             stats ? 1 : 0, brightness );
    fclose( f );
}


//--------------------------------------------------------------------------------------
// Setup
//--------------------------------------------------------------------------------------
Browser::Browser()
    : m_device( NULL ), m_font( NULL ), m_titleFont( NULL ), m_tasks( NULL ), m_images( NULL ), m_settings( NULL ),
      m_client( NULL ), m_nextId( 1 ), m_sidebarFocus( false ), m_sidebarSel( 0 ), m_toastTick( 0 ),
      m_repeatTick( 0 ), m_repeatKey( 0 )
{
}

void Browser::Startup( IDirect3DDevice9* device, Font* font, Font* titleFont, const D3DRECT& safe,
                       TaskQueue* tasks, ImageCache* images, UserSettings* settings, const BrowserHost& host )
{
    m_device = device;
    m_font = font;
    m_titleFont = titleFont;
    m_safe = safe;
    m_tasks = tasks;
    m_images = images;
    m_settings = settings;
    m_host = host;
}

void Browser::SetSession( Plex::Client* client, const std::string& clientId, const std::string& accountToken,
                          const std::wstring& userName, const std::wstring& serverLabel,
                          const std::vector<Plex::Item>& sections )
{
    m_client = client;
    m_clientId = clientId;
    m_accountToken = accountToken;
    m_userName = userName;
    m_serverLabel = serverLabel;
    m_sections.clear();
    for( size_t i = 0; i < sections.size(); ++i )
        if( sections[i].type == "movie" || sections[i].type == "show" )
            m_sections.push_back( sections[i] );
    m_images->SetServer( client->Base(), client->Headers() );

    m_stack.clear();
    m_sidebarFocus = false;
    m_sidebarSel = 0;
    Push( HOME );
}


//--------------------------------------------------------------------------------------
// Navigation
//--------------------------------------------------------------------------------------
Browser::Screen* Browser::FindScreen( int id )
{
    for( size_t i = 0; i < m_stack.size(); ++i )
        if( m_stack[i].id == id )
            return &m_stack[i];
    return NULL;
}

void Browser::Push( ScreenType type )
{
    Screen s;
    s.type = type;
    s.id = m_nextId++;
    m_stack.push_back( s );
    m_images->CancelPending();
    Screen& t = Top();
    switch( type )
    {
    case HOME:     LoadHome( t ); break;
    case USERS:    LoadUsers( t ); break;
    default:       break;
    }
}

void Browser::Pop()
{
    if( m_stack.size() <= 1 )
        return;
    m_tasks->Cancel( Top().id );
    m_stack.pop_back();
    m_images->CancelPending();
}

void Browser::OpenSection( int index )
{
    // Library screens replace each other (and Home) instead of stacking up.
    while( m_stack.size() > 1 )
        m_stack.pop_back();
    if( index < 0 )
    {
        m_stack.clear();
        Push( HOME );
        return;
    }
    m_stack.clear();
    Push( LIBRARY );
    Screen& s = Top();
    s.title = m_sections[index].title;
    s.path = "/library/sections/" + m_sections[index].sectionId + "/all?sort=titleSort";
    LoadLibrary( s, false );
}

void Browser::OpenItem( const Plex::Item& source )
{
    // Copy: 'source' may live in the screen stack, which Push() can reallocate.
    const Plex::Item item = source;
    if( item.type == "movie" || item.type == "show" )
    {
        Push( DETAILS );
        Top().item = item;
        LoadDetails( Top() );
    }
    else if( item.type == "season" )
    {
        Push( SEASON );
        Top().item = item;
        LoadSeason( Top() );
    }
    else if( item.type == "episode" )
    {
        m_host.play( item, item.viewOffsetMs / 1000.0 );
    }
    else
        Toast( L"This kind of item isn't supported yet" );
}

void Browser::Refresh()
{
    if( m_stack.empty() )
        return;
    Screen& s = Top();
    switch( s.type )
    {
    case HOME:    LoadHome( s ); break;
    case DETAILS: LoadDetails( s ); break;
    case SEASON:  LoadSeason( s ); break;
    default:      break;
    }
}

void Browser::Toast( const std::wstring& text )
{
    m_toast = text;
    m_toastTick = GetTickCount();
}


//--------------------------------------------------------------------------------------
// Loading (work runs on the API thread; results are applied only if the screen still exists)
//--------------------------------------------------------------------------------------
void Browser::LoadHome( Screen& s )
{
    s.loading = s.hubs.empty();
    int id = s.id;
    Plex::Client client = *m_client;
    std::vector<Plex::Hub>* hubs = new std::vector<Plex::Hub>;
    std::string* error = new std::string;
    Browser* self = this;
    m_tasks->Post(
        [client, hubs, error]() mutable { client.Hubs( *hubs, *error ); },
        [self, id, hubs, error]()
        {
            Screen* s = self->FindScreen( id );
            if( s )
            {
                s->loading = false;
                s->error = *error;
                s->hubs.swap( *hubs );
                s->rowScroll.assign( s->hubs.size(), 0 );
                if( s->row >= (int)s->hubs.size() )
                    s->row = 0;
                s->col = 0;
            }
            delete hubs;
            delete error;
        },
        id );
}

void Browser::LoadLibrary( Screen& s, bool more )
{
    if( more )
    {
        if( s.loadingMore || (int)s.items.size() >= s.total )
            return;
        s.loadingMore = true;
    }
    else
        s.loading = true;

    int id = s.id;
    int start = more ? (int)s.items.size() : 0;
    std::string path = s.path;
    Plex::Client client = *m_client;
    Plex::Result* r = new Plex::Result;
    Browser* self = this;
    m_tasks->Post(
        [client, path, start, r]() mutable { *r = client.Browse( path, start, LIBRARY_PAGE ); },
        [self, id, more, r]()
        {
            Screen* s = self->FindScreen( id );
            if( s )
            {
                s->loading = false;
                s->loadingMore = false;
                if( !r->ok )
                    s->error = r->error;
                else
                {
                    if( !more )
                        s->items.clear();
                    s->items.insert( s->items.end(), r->items.begin(), r->items.end() );
                    s->total = r->total;
                }
            }
            delete r;
        },
        id );
}

void Browser::LoadDetails( Screen& s )
{
    int id = s.id;
    std::string ratingKey = s.item.ratingKey;
    bool isShow = s.item.type == "show";
    Plex::Client client = *m_client;
    Plex::Result* details = new Plex::Result;
    Plex::Result* children = new Plex::Result;
    Browser* self = this;
    m_tasks->Post(
        [client, ratingKey, isShow, details, children]() mutable
        {
            *details = client.Details( ratingKey );
            if( isShow )
                *children = client.Browse( "/library/metadata/" + ratingKey + "/children" );
        },
        [self, id, details, children]()
        {
            Screen* s = self->FindScreen( id );
            if( s )
            {
                if( details->ok && !details->items.empty() )
                {
                    s->item = details->items[0];
                    Log::Write( "Details: %u items, type %s, title %u chars, thumb '%s', summary %u chars",
                                (unsigned)details->items.size(), s->item.type.c_str(), (unsigned)s->item.title.size(),
                                s->item.thumb.c_str(), (unsigned)s->item.summary.size() );
                }
                else if( !details->ok )
                    s->error = details->error;
                if( children->ok )
                {
                    s->children.clear();
                    for( size_t i = 0; i < children->items.size(); ++i )
                        if( children->items[i].type == "season" )
                            s->children.push_back( children->items[i] );
                }
            }
            delete details;
            delete children;
        },
        id );
}

void Browser::LoadSeason( Screen& s )
{
    s.loading = s.items.empty();
    int id = s.id;
    std::string ratingKey = s.item.ratingKey;
    Plex::Client client = *m_client;
    Plex::Result* r = new Plex::Result;
    Browser* self = this;
    m_tasks->Post(
        [client, ratingKey, r]() mutable { *r = client.Browse( "/library/metadata/" + ratingKey + "/children" ); },
        [self, id, r]()
        {
            Screen* s = self->FindScreen( id );
            if( s )
            {
                s->loading = false;
                if( !r->ok )
                    s->error = r->error;
                else
                {
                    s->items = r->items;
                    s->total = (int)s->items.size();
                    if( s->sel == 0 )
                        for( size_t i = 0; i < s->items.size(); ++i )
                            if( !s->items[i].Watched() )
                            {
                                s->sel = (int)i;
                                break;
                            }
                }
            }
            delete r;
        },
        id );
}

void Browser::LoadUsers( Screen& s )
{
    s.loading = true;
    s.title = L"Switch user";
    int id = s.id;
    std::string clientId = m_clientId, token = m_accountToken;
    std::vector<PlexTv::HomeUser>* users = new std::vector<PlexTv::HomeUser>;
    std::string* error = new std::string;
    Browser* self = this;
    m_tasks->Post(
        [clientId, token, users, error]() { PlexTv::HomeUsers( clientId, token, *users, *error ); },
        [self, id, users, error]()
        {
            Screen* s = self->FindScreen( id );
            if( s )
            {
                s->loading = false;
                s->error = users->empty() && error->empty() ? "This account has no other Plex Home users" : *error;
                s->users.swap( *users );
            }
            delete users;
            delete error;
        },
        id );
}


//--------------------------------------------------------------------------------------
// Input
//--------------------------------------------------------------------------------------
void Browser::Update( Pad* pad )
{
    if( m_stack.empty() )
        return;
    m_images->Pump();

    const WORD dirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
                      XINPUT_GAMEPAD_DPAD_RIGHT;
    WORD pressed = pad->wPressedButtons;
    WORD heldDir = pad->wButtons & dirs;
    if( pad->fY1 > 0.6f )  heldDir |= XINPUT_GAMEPAD_DPAD_UP;
    if( pad->fY1 < -0.6f ) heldDir |= XINPUT_GAMEPAD_DPAD_DOWN;
    if( pad->fX1 < -0.6f ) heldDir |= XINPUT_GAMEPAD_DPAD_LEFT;
    if( pad->fX1 > 0.6f )  heldDir |= XINPUT_GAMEPAD_DPAD_RIGHT;
    DWORD now = GetTickCount();
    if( heldDir != m_repeatKey )
    {
        if( heldDir & ~m_repeatKey )
            pressed |= heldDir & ~m_repeatKey;    // stick just moved
        m_repeatKey = heldDir;
        m_repeatTick = now + REPEAT_DELAY_MS;
    }
    else if( heldDir && now >= m_repeatTick )
    {
        pressed |= heldDir;
        m_repeatTick = now + REPEAT_RATE_MS;
    }
    if( !pressed )
        return;

    Screen& s = Top();
    bool sidebarScreen = s.type == HOME || s.type == LIBRARY || s.type == SETTINGS;
    if( m_sidebarFocus && sidebarScreen )
    {
        UpdateSidebar( pressed );
        return;
    }
    if( ( pressed & XINPUT_GAMEPAD_B ) && s.type != PIN )
    {
        if( m_stack.size() > 1 )
            Pop();
        else if( sidebarScreen )
            m_sidebarFocus = true;
        return;
    }

    switch( s.type )
    {
    case HOME:     UpdateHome( s, pressed ); break;
    case LIBRARY:  UpdateGrid( s, pressed ); break;
    case DETAILS:  UpdateDetails( s, pressed ); break;
    case SEASON:   UpdateSeason( s, pressed ); break;
    case SETTINGS: UpdateSettings( s, pressed ); break;
    case USERS:    UpdateUsers( s, pressed ); break;
    case PIN:      UpdatePin( s, pressed ); break;
    }
}

void Browser::UpdateSidebar( WORD pressed )
{
    int count = (int)m_sections.size() + 2;
    int switchRow = count, exitRow = count + 1;
    if( pressed & XINPUT_GAMEPAD_DPAD_DOWN ) m_sidebarSel = Clamp( m_sidebarSel + 1, 0, exitRow );
    if( pressed & XINPUT_GAMEPAD_DPAD_UP )   m_sidebarSel = Clamp( m_sidebarSel - 1, 0, exitRow );
    if( m_sidebarSel >= switchRow )
    {
        if( pressed & XINPUT_GAMEPAD_A )
        {
            if( m_sidebarSel == exitRow && m_host.exitApp )
                m_host.exitApp();
            else if( m_sidebarSel == switchRow )
            {
                m_sidebarFocus = false;
                Push( USERS );      // the same "Who's watching?" screen as in Settings
            }
        }
        if( pressed & XINPUT_GAMEPAD_B )
            m_sidebarFocus = false;
        return;
    }
    if( pressed & ( XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_RIGHT ) )
    {
        m_sidebarFocus = false;
        bool same = ( m_sidebarSel == 0 && Top().type == HOME ) ||
                    ( m_sidebarSel == count - 1 && Top().type == SETTINGS );
        if( same && ( pressed & XINPUT_GAMEPAD_DPAD_RIGHT ) )
            return;
        if( m_sidebarSel == 0 )
            OpenSection( -1 );
        else if( m_sidebarSel == count - 1 )
        {
            m_stack.clear();
            Push( SETTINGS );
            Top().title = L"Settings";
        }
        else
            OpenSection( m_sidebarSel - 1 );
    }
    if( pressed & XINPUT_GAMEPAD_B )
        m_sidebarFocus = false;
}

void Browser::UpdateHome( Screen& s, WORD pressed )
{
    if( s.hubs.empty() )
    {
        if( pressed & XINPUT_GAMEPAD_DPAD_LEFT )
            m_sidebarFocus = true;
        if( pressed & XINPUT_GAMEPAD_Y )
            LoadHome( s );
        return;
    }
    int rows = (int)s.hubs.size();
    if( pressed & XINPUT_GAMEPAD_DPAD_DOWN ) s.row = Clamp( s.row + 1, 0, rows - 1 );
    if( pressed & XINPUT_GAMEPAD_DPAD_UP )   s.row = Clamp( s.row - 1, 0, rows - 1 );
    int cols = (int)s.hubs[s.row].items.size();
    if( pressed & ( XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN ) )
        s.col = Clamp( s.rowScroll[s.row], 0, cols - 1 );
    if( pressed & XINPUT_GAMEPAD_DPAD_RIGHT )
        s.col = Clamp( s.col + 1, 0, cols - 1 );
    if( pressed & XINPUT_GAMEPAD_DPAD_LEFT )
    {
        if( s.col == 0 )
            m_sidebarFocus = true;
        else
            --s.col;
    }
    int visible = (int)( ( CONTENT_R - CONTENT_X + HUB_GAP ) / ( HUB_POSTER_W + HUB_GAP ) );
    int& scroll = s.rowScroll[s.row];
    if( s.col < scroll )
        scroll = s.col;
    if( s.col >= scroll + visible )
        scroll = s.col - visible + 1;

    if( pressed & XINPUT_GAMEPAD_A )
        OpenItem( s.hubs[s.row].items[s.col] );
    if( pressed & XINPUT_GAMEPAD_Y )
        LoadHome( s );
}

void Browser::UpdateGrid( Screen& s, WORD pressed )
{
    int count = (int)s.items.size();
    if( count == 0 )
    {
        if( pressed & XINPUT_GAMEPAD_DPAD_LEFT )
            m_sidebarFocus = true;
        return;
    }
    int col = s.sel % GRID_COLS;
    if( pressed & XINPUT_GAMEPAD_DPAD_RIGHT ) s.sel = Clamp( s.sel + 1, 0, count - 1 );
    if( pressed & XINPUT_GAMEPAD_DPAD_LEFT )
    {
        if( col == 0 )
            m_sidebarFocus = true;
        else
            --s.sel;
    }
    if( pressed & XINPUT_GAMEPAD_DPAD_DOWN )  s.sel = Clamp( s.sel + GRID_COLS, 0, count - 1 );
    if( pressed & XINPUT_GAMEPAD_DPAD_UP )    s.sel = Clamp( s.sel - GRID_COLS, 0, count - 1 );
    if( pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER ) s.sel = Clamp( s.sel + GRID_COLS * 3, 0, count - 1 );
    if( pressed & XINPUT_GAMEPAD_LEFT_SHOULDER )  s.sel = Clamp( s.sel - GRID_COLS * 3, 0, count - 1 );

    int row = s.sel / GRID_COLS;
    if( row < s.scroll )
        s.scroll = row;
    if( row > s.scroll + 1 )
        s.scroll = row - 1;
    if( s.sel > count - GRID_COLS * 3 )
        LoadLibrary( s, true );

    if( pressed & XINPUT_GAMEPAD_A )
        OpenItem( s.items[s.sel] );
}

void Browser::UpdateDetails( Screen& s, WORD pressed )
{
    const Plex::Item& it = s.item;
    if( it.type == "show" )
    {
        int n = (int)s.children.size();
        if( n == 0 )
            return;
        if( pressed & XINPUT_GAMEPAD_DPAD_RIGHT ) s.button = Clamp( s.button + 1, 0, n - 1 );
        if( pressed & XINPUT_GAMEPAD_DPAD_LEFT )  s.button = Clamp( s.button - 1, 0, n - 1 );
        if( pressed & XINPUT_GAMEPAD_A )
            OpenItem( s.children[s.button] );
        return;
    }

    bool resume = it.viewOffsetMs > 0;
    int n = resume ? 3 : 2;
    if( pressed & XINPUT_GAMEPAD_DPAD_RIGHT ) s.button = Clamp( s.button + 1, 0, n - 1 );
    if( pressed & XINPUT_GAMEPAD_DPAD_LEFT )  s.button = Clamp( s.button - 1, 0, n - 1 );
    if( pressed & XINPUT_GAMEPAD_A )
    {
        int b = resume ? s.button : s.button + 1;
        if( b == 0 )
            m_host.play( it, it.viewOffsetMs / 1000.0 );
        else if( b == 1 )
            m_host.play( it, 0.0 );
        else
        {
            bool watched = !it.Watched();
            Plex::Client client = *m_client;
            std::string key = it.ratingKey;
            int id = s.id;
            Browser* self = this;
            m_tasks->Post( [client, key, watched]() mutable { client.SetWatched( key, watched ); },
                           [self, id]()
                           {
                               Screen* sc = self->FindScreen( id );
                               if( sc )
                                   self->LoadDetails( *sc );
                           },
                           id );
            Toast( watched ? L"Marked as watched" : L"Marked as unwatched" );
        }
    }
}

void Browser::UpdateSeason( Screen& s, WORD pressed )
{
    int count = (int)s.items.size();
    if( count == 0 )
        return;
    if( pressed & XINPUT_GAMEPAD_DPAD_DOWN ) s.sel = Clamp( s.sel + 1, 0, count - 1 );
    if( pressed & XINPUT_GAMEPAD_DPAD_UP )   s.sel = Clamp( s.sel - 1, 0, count - 1 );
    if( pressed & XINPUT_GAMEPAD_A )
    {
        const Plex::Item& ep = s.items[s.sel];
        m_host.play( ep, ep.viewOffsetMs / 1000.0 );
    }
    if( pressed & XINPUT_GAMEPAD_X )
    {
        m_host.play( s.items[s.sel], 0.0 );
    }
}

void Browser::UpdateSettings( Screen& s, WORD pressed )
{
    if( pressed & XINPUT_GAMEPAD_DPAD_DOWN ) s.sel = Clamp( s.sel + 1, 0, (int)SET_COUNT - 1 );
    if( pressed & XINPUT_GAMEPAD_DPAD_UP )   s.sel = Clamp( s.sel - 1, 0, (int)SET_COUNT - 1 );

    int step = 0;
    if( pressed & XINPUT_GAMEPAD_DPAD_RIGHT ) step = 1;
    if( pressed & ( XINPUT_GAMEPAD_DPAD_LEFT ) )
    {
        if( s.sel == SET_ACCOUNT || s.sel == SET_SWITCH || s.sel == SET_SIGNOUT || s.sel == SET_SERVER )
        {
            m_sidebarFocus = true;
            return;
        }
        step = -1;
    }
    if( pressed & XINPUT_GAMEPAD_A )
    {
        if( s.sel == SET_SWITCH )
        {
            Push( USERS );
            return;
        }
        if( s.sel == SET_SIGNOUT )
        {
            m_host.signOut();
            return;
        }
        step = 1;
    }
    if( !step )
        return;

    UserSettings& u = *m_settings;
    static const int directs[] = { 480, 576, 720, 1080 };
    switch( s.sel )
    {
    case SET_QUALITY:
        {
            int i = Clamp( NearestQualityPreset( u.quality, u.kbps ) + step, 0, QUALITY_PRESET_COUNT - 1 );
            u.quality = QUALITY_PRESETS[i].height;
            u.kbps = QUALITY_PRESETS[i].kbps;
        }
        break;
    case SET_DIRECT:
        {
            int i = 0;
            while( i < 4 && directs[i] != u.directMax ) ++i;
            u.directMax = directs[Clamp( ( i < 4 ? i : 2 ) + step, 0, 3 )];
        }
        break;
    case SET_BRIGHTNESS:
        u.brightness = Clamp( u.brightness + step, -5, 10 );
        break;
    case SET_STATS:
        u.stats = !u.stats;
        break;
    default:
        return;
    }
    u.Save( SETTINGS_PATH );
    if( m_host.settingsChanged )
        m_host.settingsChanged();
}

void Browser::UpdateUsers( Screen& s, WORD pressed )
{
    int count = (int)s.users.size();
    if( count == 0 )
        return;
    if( pressed & XINPUT_GAMEPAD_DPAD_RIGHT ) s.sel = Clamp( s.sel + 1, 0, count - 1 );
    if( pressed & XINPUT_GAMEPAD_DPAD_LEFT )  s.sel = Clamp( s.sel - 1, 0, count - 1 );
    if( pressed & XINPUT_GAMEPAD_A )
    {
        PlexTv::HomeUser u = s.users[s.sel];   // copy: Push() may move the stack
        if( u.isProtected )
        {
            Push( PIN );
            Top().pinUser = u;
            Top().title = L"Enter PIN for " + u.title;
            return;
        }
        Screen pin;
        pin.pinUser = u;
        UpdatePin( pin, 0xFFFF );   // switch right away, no PIN
    }
}

void Browser::UpdatePin( Screen& s, WORD pressed )
{
    bool submit = pressed == 0xFFFF;   // no-PIN profile
    if( !submit )
    {
        if( pressed & XINPUT_GAMEPAD_B )
        {
            Pop();
            return;
        }
        if( pressed & XINPUT_GAMEPAD_DPAD_UP )    s.digits[s.digitPos] = ( s.digits[s.digitPos] + 1 ) % 10;
        if( pressed & XINPUT_GAMEPAD_DPAD_DOWN )  s.digits[s.digitPos] = ( s.digits[s.digitPos] + 9 ) % 10;
        if( pressed & XINPUT_GAMEPAD_DPAD_RIGHT ) s.digitPos = Clamp( s.digitPos + 1, 0, 3 );
        if( pressed & XINPUT_GAMEPAD_DPAD_LEFT )  s.digitPos = Clamp( s.digitPos - 1, 0, 3 );
        submit = ( pressed & XINPUT_GAMEPAD_A ) != 0;
    }
    if( !submit )
        return;

    char pin[8] = "";
    if( s.pinUser.isProtected )
        sprintf_s( pin, "%d%d%d%d", s.digits[0], s.digits[1], s.digits[2], s.digits[3] );
    std::string clientId = m_clientId, token = m_accountToken, uuid = s.pinUser.uuid, pinStr = pin;
    std::wstring name = s.pinUser.title;
    std::string* newToken = new std::string;
    std::string* error = new std::string;
    Browser* self = this;
    Toast( L"Switching to " + name + L"..." );
    m_tasks->Post(
        [clientId, token, uuid, pinStr, newToken, error]()
        { PlexTv::SwitchUser( clientId, token, uuid, pinStr, *newToken, *error ); },
        [self, newToken, error, name]()
        {
            if( newToken->empty() )
                self->Toast( L"Could not switch: " + Widen( *error ) );
            else
                self->m_host.switchUser( *newToken, name );
            delete newToken;
            delete error;
        },
        0 );
}


//--------------------------------------------------------------------------------------
// Drawing helpers
//--------------------------------------------------------------------------------------
void Browser::Text( Font* font, float x, float y, DWORD color, const std::wstring& s, float scale, DWORD flags,
                    float maxWidth )
{
    if( s.empty() )
        return;
    // Absolute screen coordinates (the font treats negatives as offsets from the far edge).
    D3DRECT window;
    font->GetWindow( window );
    font->SetWindow( 0, 0, 1280, 720 );
    font->Begin();
    font->SetScaleFactors( scale, scale );
    font->DrawText( x, y, color, Displayable( s ).c_str(), flags | ( maxWidth > 0 ? FONT_TRUNCATED : 0 ), maxWidth );
    font->SetScaleFactors( 1.0f, 1.0f );
    font->End();
    font->SetWindow( window );
}

int Browser::WrapText( Font* font, float x, float y, float width, int maxLines, DWORD color,
                       const std::wstring& text, float scale )
{
    std::wstring s = Displayable( text );
    D3DRECT window;
    font->GetWindow( window );
    font->SetWindow( 0, 0, 1280, 720 );
    font->Begin();
    font->SetScaleFactors( scale, scale );
    float lineH = font->GetFontHeight() * scale;
    int lines = 0;
    size_t pos = 0;
    while( pos < s.size() && lines < maxLines )
    {
        size_t end = pos, lastFit = pos;
        std::wstring line;
        while( end <= s.size() )
        {
            size_t next = s.find_first_of( L" \n", end );
            if( next == std::wstring::npos )
                next = s.size();
            std::wstring candidate = s.substr( pos, next - pos );
            if( font->GetTextWidth( candidate.c_str() ) > width && lastFit > pos )   // width includes the scale
                break;
            lastFit = next;
            line = candidate;
            if( next >= s.size() || s[next] == L'\n' )
            {
                lastFit = next;
                break;
            }
            end = next + 1;
        }
        bool last = lines == maxLines - 1 && lastFit < s.size();
        if( last )
            line += L"...";
        font->DrawText( x, y + lines * lineH, color, line.c_str(), FONT_TRUNCATED, width );
        ++lines;
        pos = lastFit + 1;
    }
    font->SetScaleFactors( 1.0f, 1.0f );
    font->End();
    font->SetWindow( window );
    return lines;
}

std::wstring Browser::Duration( int ms ) const
{
    int min = ms / 60000;
    wchar_t buf[32];
    if( min >= 60 )
        swprintf_s( buf, L"%d h %d min", min / 60, min % 60 );
    else
        swprintf_s( buf, L"%d min", min );
    return buf;
}

void Browser::Poster( const Plex::Item& it, float x, float y, float w, float h, bool focused, bool wide )
{
    std::string thumb = wide ? it.thumb : PosterOf( it );
    IDirect3DTexture9* tex = m_images->Get( thumb, (int)w, (int)h );
    if( focused )
        Draw::Rect( x - 5, y - 5, x + w + 5, y + h + 5, C_ACCENT );
    if( tex )
        Draw::Image( tex, x, y, x + w, y + h );
    else
    {
        Draw::Rect( x, y, x + w, y + h, C_PLACEHOLD );
        WrapText( m_font, x + 8, y + h / 2 - 20, w - 16, 3, C_DIM, TitleOf( it ), 0.8f );
    }

    if( it.viewOffsetMs > 0 && it.durationMs > 0 )
    {
        float f = Clamp( (float)it.viewOffsetMs / it.durationMs, 0.0f, 1.0f );
        Draw::Rect( x, y + h - 6, x + w, y + h, 0xC0000000 );
        Draw::Rect( x, y + h - 6, x + w * f, y + h, C_ACCENT );
    }
    int unwatched = it.leafCount - it.viewedLeafCount;
    if( ( it.type == "show" || it.type == "season" ) && unwatched > 0 )
    {
        wchar_t n[16];
        swprintf_s( n, L"%d", unwatched );
        float bw = unwatched > 99 ? 40.0f : ( unwatched > 9 ? 32.0f : 24.0f );
        Draw::Rect( x + w - bw, y, x + w, y + 24, C_ACCENT );
        Text( m_font, x + w - bw / 2, y + 1, 0xFF000000, n, 0.8f, FONT_CENTER_X );
    }
    else if( ( it.type == "movie" || it.type == "episode" ) && it.viewCount == 0 && it.viewOffsetMs == 0 )
    {
        Draw::Rect( x + w - 14, y, x + w, y + 14, C_ACCENT );
    }
}

void Browser::RenderBackground( const std::string& art )
{
    Draw::Rect( 0, 0, 1280, 720, C_BG );
    IDirect3DTexture9* tex = art.empty() ? NULL : m_images->Get( art, 640, 360 );
    if( tex )
    {
        Draw::Image( tex, 0, 0, 1280, 720, 0xFF4A4A4A );   // darkened
        for( int i = 0; i < 16; ++i )
            Draw::Rect( i * 60.0f, 0, i * 60.0f + 60, 720, (DWORD)( 200 - i * 12 ) << 24 );
    }
}

void Browser::RenderLoading( Screen& s )
{
    if( s.loading )
        Text( m_titleFont, CONTENT_X, 340, C_DIM, L"Loading..." );
    else if( !s.error.empty() )
    {
        Text( m_font, CONTENT_X, 340, C_ERROR, Widen( s.error ) );
        Text( m_font, CONTENT_X, 372, C_DIM, L"Press Y to retry" );
    }
}


//--------------------------------------------------------------------------------------
// Screens
//--------------------------------------------------------------------------------------
void Browser::Render()
{
    if( m_stack.empty() )
        return;
    Screen& s = Top();

    switch( s.type )
    {
    case HOME:     RenderBackground( "" ); RenderHome( s ); break;
    case LIBRARY:  RenderBackground( "" ); RenderGrid( s ); break;
    case DETAILS:  RenderBackground( s.item.art ); RenderDetails( s ); break;
    case SEASON:   RenderBackground( s.item.art ); RenderSeason( s ); break;
    case SETTINGS: RenderBackground( "" ); RenderSettings( s ); break;
    case USERS:    RenderBackground( "" ); RenderUsers( s ); break;
    case PIN:      RenderBackground( "" ); RenderPin( s ); break;
    }
    if( s.type == HOME || s.type == LIBRARY || s.type == SETTINGS )
        RenderSidebar();

    if( !m_toast.empty() && GetTickCount() - m_toastTick < 3000 )
    {
        float w = m_font->GetTextWidth( m_toast.c_str() ) + 40;
        Draw::Rect( 640 - w / 2, 640, 640 + w / 2, 680, 0xE0303030 );
        Text( m_font, 640, 648, C_TEXT, m_toast, 1.0f, FONT_CENTER_X );
    }
}

void Browser::RenderSidebar()
{
    Draw::Rect( 0, 0, SIDEBAR_W, 720, m_sidebarFocus ? 0xFA181818 : C_SIDEBAR );
    Text( m_titleFont, 60, 32, C_ACCENT, L"MULTIPLEX", 0.95f );
    Text( m_titleFont, 60, 64, C_TEXT, L"360", 0.85f );

    float y = 120;
    int count = (int)m_sections.size() + 2;
    for( int i = 0; i < count; ++i )
    {
        std::wstring label = i == 0 ? L"Home" : ( i == count - 1 ? L"Settings" : m_sections[i - 1].title );
        bool active = ( i == 0 && Top().type == HOME ) || ( i == count - 1 && Top().type == SETTINGS ) ||
                      ( Top().type == LIBRARY && i > 0 && i < count - 1 && Top().title == label );
        bool focused = m_sidebarFocus && i == m_sidebarSel;
        if( focused )
            Draw::Rect( 40, y - 6, SIDEBAR_W - 12, y + 34, 0xFF2E2E2E );
        if( active )
            Draw::Rect( 40, y - 6, 45, y + 34, C_ACCENT );
        Text( m_titleFont, 60, y, focused ? C_TEXT : ( active ? C_ACCENT : C_DIM ), label, 0.95f, 0,
              SIDEBAR_W - 80 );
        y += 50;
        if( i == count - 2 )
            y += 20;
    }

    static const wchar_t* const bottom[2] = { L"Switch user", L"Exit" };
    for( int i = 0; i < 2; ++i )
    {
        float by = 550.0f + i * 50;
        bool focused = m_sidebarFocus && m_sidebarSel == count + i;
        if( focused )
            Draw::Rect( 40, by - 6, SIDEBAR_W - 12, by + 34, 0xFF2E2E2E );
        Text( m_titleFont, 60, by, focused ? C_TEXT : C_DIM, bottom[i], 0.95f, 0, SIDEBAR_W - 80 );
    }

    Text( m_font, 64, 660, C_FAINT, m_userName, 0.85f, 0, SIDEBAR_W - 80 );
}

void Browser::RenderHome( Screen& s )
{
    if( s.hubs.empty() )
    {
        RenderLoading( s );
        if( !s.loading && s.error.empty() )
            Text( m_font, CONTENT_X, 340, C_DIM, L"Nothing here yet" );
        return;
    }

    int first = Clamp( s.row - ( s.row == (int)s.hubs.size() - 1 && s.row > 0 ? 1 : 0 ), 0, (int)s.hubs.size() - 1 );
    float y = TOP_Y;
    for( int r = first; r < (int)s.hubs.size() && y < 700; ++r )
    {
        const Plex::Hub& hub = s.hubs[r];
        Text( m_titleFont, CONTENT_X, y, r == s.row && !m_sidebarFocus ? C_TEXT : C_DIM, hub.title );
        float py = y + 40;
        int scroll = s.rowScroll[r];
        for( int c = scroll; c < (int)hub.items.size(); ++c )
        {
            float px = CONTENT_X + ( c - scroll ) * ( HUB_POSTER_W + HUB_GAP );
            if( px > 1280 )
                break;   // the next poster may peek in at the edge, like Plex
            const Plex::Item& it = hub.items[c];
            bool focused = !m_sidebarFocus && r == s.row && c == s.col;
            Poster( it, px, py, HUB_POSTER_W, HUB_POSTER_H, focused );
            if( focused )
            {
                Text( m_font, px, py + HUB_POSTER_H + 10, C_TEXT, TitleOf( it ), 0.9f, 0, HUB_POSTER_W + 120 );
                std::wstring sub = it.type == "episode" ? it.subtitle + L"  " + it.title : it.subtitle;
                Text( m_font, px, py + HUB_POSTER_H + 34, C_DIM, sub, 0.8f, 0, HUB_POSTER_W + 120 );
            }
        }
        y += HUB_ROW_H;
    }
}

void Browser::RenderGrid( Screen& s )
{
    Text( m_titleFont, CONTENT_X, TOP_Y - 8, C_TEXT, s.title, 1.2f );
    if( s.items.empty() )
    {
        RenderLoading( s );
        return;
    }
    wchar_t count[32];
    swprintf_s( count, L"%d / %d", s.sel + 1, s.total );
    Text( m_font, CONTENT_R, TOP_Y, C_DIM, count, 0.9f, FONT_RIGHT );

    float cellW = ( CONTENT_R - CONTENT_X ) / GRID_COLS;
    float top = TOP_Y + 52;
    for( int i = s.scroll * GRID_COLS; i < (int)s.items.size(); ++i )
    {
        int row = i / GRID_COLS - s.scroll;
        if( row > 2 )
            break;
        float px = CONTENT_X + ( i % GRID_COLS ) * cellW;
        float py = top + row * GRID_ROW_H;
        bool focused = !m_sidebarFocus && i == s.sel;
        const Plex::Item& it = s.items[i];
        Poster( it, px, py, GRID_POSTER_W, GRID_POSTER_H, focused );
        Text( m_font, px, py + GRID_POSTER_H + 8, focused ? C_TEXT : C_DIM, it.title, 0.8f, 0, GRID_POSTER_W );
        if( focused )
            Text( m_font, px, py + GRID_POSTER_H + 28, C_FAINT, it.subtitle, 0.75f, 0, GRID_POSTER_W );
    }
}

void Browser::RenderDetails( Screen& s )
{
    const Plex::Item& it = s.item;
    float px = 100, py = 90, pw = 260, ph = 390;
    Poster( it, px, py, pw, ph, false );

    float tx = px + pw + 50, tw = CONTENT_R - tx;
    Text( m_titleFont, tx, py, C_TEXT, it.title, 1.5f, 0, tw );

    std::wstring meta;
    wchar_t buf[64];
    if( it.year )
    {
        swprintf_s( buf, L"%d", it.year );
        meta += buf;
    }
    if( it.durationMs )
        meta += ( meta.empty() ? L"" : L"   " ) + Duration( it.durationMs );
    if( !it.contentRating.empty() )
        meta += ( meta.empty() ? L"" : L"   " ) + Widen( it.contentRating );
    if( it.rating > 0 )
    {
        swprintf_s( buf, L"   %.1f / 10", it.rating );
        meta += buf;
    }
    if( it.type == "show" && it.childCount )
    {
        swprintf_s( buf, L"   %d seasons", it.childCount );
        meta += buf;
    }
    Text( m_font, tx, py + 56, C_DIM, meta );

    WrapText( m_font, tx, py + 100, tw, it.type == "show" ? 5 : 8, C_TEXT, it.summary, 0.95f );

    if( it.type == "show" )
    {
        Text( m_titleFont, tx, 420, C_DIM, L"Seasons" );
        for( size_t i = 0; i < s.children.size(); ++i )
        {
            float x = tx + i * 130.0f - ( s.button > 5 ? ( s.button - 5 ) * 130.0f : 0 );
            if( x < tx - 10 || x > 1250 )
                continue;
            Poster( s.children[i], x, 460, 110, 165, (int)i == s.button );
            Text( m_font, x, 632, (int)i == s.button ? C_TEXT : C_DIM, s.children[i].title, 0.75f, 0, 115 );
        }
        if( s.children.empty() )
            Text( m_font, tx, 470, C_DIM, L"Loading seasons..." );
        return;
    }

    std::vector<std::wstring> buttons;
    if( it.viewOffsetMs > 0 )
    {
        std::wstring at;
        int sec = it.viewOffsetMs / 1000;
        swprintf_s( buf, sec >= 3600 ? L"Resume from %d:%02d:%02d" : L"Resume from %d:%02d",
                    sec >= 3600 ? sec / 3600 : sec / 60, sec >= 3600 ? ( sec / 60 ) % 60 : sec % 60, sec % 60 );
        buttons.push_back( buf );
        buttons.push_back( L"Play from start" );
    }
    else
        buttons.push_back( L"Play" );
    buttons.push_back( it.Watched() ? L"Mark unwatched" : L"Mark watched" );

    float bx = tx, by = 560;
    for( size_t i = 0; i < buttons.size(); ++i )
    {
        float w = m_titleFont->GetTextWidth( buttons[i].c_str() ) * 0.9f + 50;
        bool focused = (int)i == s.button;
        Draw::Rect( bx, by, bx + w, by + 50, focused ? C_ACCENT : 0xC0303030 );
        Text( m_titleFont, bx + w / 2, by + 11, focused ? 0xFF000000 : C_TEXT, buttons[i], 0.9f, FONT_CENTER_X );
        bx += w + 20;
    }
    if( it.viewOffsetMs > 0 && it.durationMs > 0 )
    {
        float f = Clamp( (float)it.viewOffsetMs / it.durationMs, 0.0f, 1.0f );
        Draw::Rect( tx, by + 64, tx + 400, by + 69, 0x80FFFFFF );
        Draw::Rect( tx, by + 64, tx + 400 * f, by + 69, C_ACCENT );
    }
}

void Browser::RenderSeason( Screen& s )
{
    const Plex::Item& season = s.item;
    Poster( season, 100, 90, 200, 300, false );
    Text( m_titleFont, 100, 410, C_TEXT, season.parentTitle, 1.0f, 0, 220 );
    Text( m_font, 100, 445, C_DIM, season.title, 1.0f, 0, 220 );
    Text( m_font, 100, 640, C_FAINT, L"A  Play      X  From start", 0.8f );

    if( s.items.empty() )
    {
        RenderLoading( s );
        return;
    }
    const float rowH = 140, x = 350, thumbW = 192, thumbH = 108;
    int first = Clamp( s.sel - 1, 0, (int)s.items.size() - 1 );
    for( int i = first; i < (int)s.items.size(); ++i )
    {
        float y = 70 + ( i - first ) * rowH;
        if( y > 640 )
            break;
        const Plex::Item& ep = s.items[i];
        bool focused = i == s.sel;
        if( focused )
            Draw::Rect( x - 14, y - 10, CONTENT_R + 10, y + thumbH + 14, 0xC0262626 );
        Poster( ep, x, y, thumbW, thumbH, focused, true );
        wchar_t head[32];
        swprintf_s( head, L"%d.  ", ep.index );
        float tx = x + thumbW + 24;
        Text( m_titleFont, tx, y, focused ? C_TEXT : C_DIM, head + ep.title, 0.95f, 0, CONTENT_R - tx );
        std::wstring meta = Duration( ep.durationMs );
        if( ep.Watched() )
            meta += L"   Watched";
        else if( ep.viewOffsetMs > 0 )
            meta += L"   " + Duration( ep.durationMs - ep.viewOffsetMs ) + L" left";
        Text( m_font, tx, y + 32, C_FAINT, meta, 0.85f );
        WrapText( m_font, tx, y + 58, CONTENT_R - tx, 2, focused ? C_DIM : C_FAINT, ep.summary, 0.8f );
    }
}

void Browser::RenderSettings( Screen& s )
{
    Text( m_titleFont, CONTENT_X, TOP_Y - 8, C_TEXT, L"Settings", 1.2f );
    const UserSettings& u = *m_settings;
    wchar_t buf[64];
    for( int i = 0; i < SET_COUNT; ++i )
    {
        std::wstring label, value;
        switch( i )
        {
        case SET_ACCOUNT:    label = L"Signed in as";  value = m_userName; break;
        case SET_SWITCH:     label = L"Switch Plex Home user"; break;
        case SET_SIGNOUT:    label = L"Sign out"; break;
        case SET_SERVER:     label = L"Server";        value = m_serverLabel; break;
        case SET_QUALITY:
            label = L"Conversion quality";
            swprintf_s( buf, L"<  %dp  -  %g Mbps  >", u.quality, u.kbps / 1000.0 );
            value = buf;
            break;
        case SET_DIRECT:
            label = L"Play files directly up to";
            swprintf_s( buf, L"<  %dp  >", u.directMax );
            value = buf;
            break;
        case SET_BRIGHTNESS:
            label = L"Video brightness";
            swprintf_s( buf, L"<  %+d  >", u.brightness );
            value = buf;
            break;
        case SET_STATS:      label = L"Stats for nerds"; value = u.stats ? L"<  On  >" : L"<  Off  >"; break;
        }
        float y = TOP_Y + 60 + i * 56.0f + ( i >= SET_QUALITY ? 24 : 0 );
        bool focused = !m_sidebarFocus && i == s.sel;
        if( focused )
            Draw::Rect( CONTENT_X - 16, y - 10, CONTENT_R + 10, y + 38, 0xFF2A2A2A );
        Text( m_titleFont, CONTENT_X, y, focused ? C_TEXT : C_DIM, label, 0.95f );
        Text( m_titleFont, CONTENT_R - 10, y, focused ? C_ACCENT : C_DIM, value, 0.95f, FONT_RIGHT, 500 );
    }
    Text( m_font, CONTENT_X, 660, C_FAINT,
          L"Videos above the direct-play limit or bitrate are converted by your Plex server.", 0.8f );
}

void Browser::RenderUsers( Screen& s )
{
    Text( m_titleFont, 100, TOP_Y - 8, C_TEXT, L"Who's watching?", 1.3f );
    if( s.users.empty() )
    {
        if( s.loading )
            Text( m_titleFont, 100, 340, C_DIM, L"Loading..." );
        else
            Text( m_font, 100, 340, C_ERROR, Widen( s.error ) );
        return;
    }
    float size = 150, gap = 50;
    float total = s.users.size() * ( size + gap ) - gap;
    float x0 = 640 - total / 2;
    for( size_t i = 0; i < s.users.size(); ++i )
    {
        float x = x0 + i * ( size + gap ), y = 250;
        bool focused = (int)i == s.sel;
        if( focused )
            Draw::Rect( x - 6, y - 6, x + size + 6, y + size + 6, C_ACCENT );
        IDirect3DTexture9* tex = m_images->Get( s.users[i].thumb, (int)size, (int)size );
        if( tex )
            Draw::Image( tex, x, y, x + size, y + size );
        else
            Draw::Rect( x, y, x + size, y + size, C_PLACEHOLD );
        Text( m_titleFont, x + size / 2, y + size + 16, focused ? C_TEXT : C_DIM, s.users[i].title, 0.95f,
              FONT_CENTER_X );
        if( s.users[i].isProtected )
            Text( m_font, x + size / 2, y + size + 48, C_FAINT, L"PIN", 0.8f, FONT_CENTER_X );
    }
}

void Browser::RenderPin( Screen& s )
{
    Text( m_titleFont, 640, 200, C_TEXT, s.title, 1.1f, FONT_CENTER_X );
    for( int i = 0; i < 4; ++i )
    {
        float x = 640 - 2 * 90 + i * 90 + 10, y = 290;
        bool focused = i == s.digitPos;
        Draw::Rect( x, y, x + 70, y + 90, focused ? C_ACCENT : 0xFF2E2E2E );
        wchar_t d[4];
        swprintf_s( d, L"%d", s.digits[i] );
        Text( m_titleFont, x + 35, y + 22, focused ? 0xFF000000 : C_TEXT, d, 1.6f, FONT_CENTER_X );
    }
    Text( m_font, 640, 420, C_DIM, L"Up/Down change digit      Left/Right move      A  OK      B  Cancel", 0.9f,
          FONT_CENTER_X );
}
