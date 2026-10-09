#pragma once
#include <xtl.h>
#include <string>
#include <vector>
#include "App.h"
#include "Font.h"
#include "FFPlayer.h"
#include "CpuMeter.h"

std::wstring FormatTime( double seconds );

struct MenuEntry
{
    std::wstring label, value;
    bool         checked;       // the current choice
    bool         opens;         // leads to another list
    MenuEntry() : checked( false ), opens( false ) {}
};

// One page of the options menu. A slider page shows a bar instead of entries.
struct MenuPage
{
    std::wstring           title;
    std::vector<MenuEntry> entries;
    bool                   slider;
    int                    value, minValue, maxValue;
    MenuPage() : slider( false ), value( 0 ), minValue( 0 ), maxValue( 1 ) {}
};

// Where a menu action leads.
struct MenuMove
{
    enum Kind { STAY, OPEN, CLOSE } kind;
    int page, sel;
    static MenuMove Stay()                      { MenuMove m = { STAY, 0, 0 }; return m; }
    static MenuMove Close()                     { MenuMove m = { CLOSE, 0, 0 }; return m; }
    static MenuMove Open( int page, int sel )   { MenuMove m = { OPEN, page, sel }; return m; }
};

// What the playback screen needs from the app around it. Page 0 is the main menu page.
class PlayerHost
{
public:
    virtual ~PlayerHost() {}
    virtual void         Seek( double seconds ) = 0;
    virtual void         Leave() = 0;                       // B with nothing left to close
    virtual void         CancelStart() = 0;                 // B while the stream (re)starts
    virtual bool         SlowSeek() const { return false; } // seeking restarts the stream
    virtual double       DurationHint() const { return 0; }
    virtual std::wstring Title() const = 0;
    virtual std::wstring StartMessage() const = 0;          // shown while the stream (re)starts
    virtual std::wstring Tag() const = 0;                   // right of the title, e.g. "Original"
    virtual IDirect3DTexture9* Preview( double ) { return NULL; }
    virtual bool         ShowStats() const = 0;
    virtual std::wstring StatsMode() const = 0;
    virtual int          Brightness() const = 0;
    virtual void         DrawOverlay( float bottom ) {}     // e.g. subtitles, ending above y = bottom

    virtual void         BuildMenu( int page, MenuPage& out ) = 0;
    virtual MenuMove     Choose( int page, int sel ) = 0;
    virtual MenuMove     Back( int page ) = 0;
    virtual void         SliderStep( int page, int step ) {}
    virtual void         AspectChanged() {}                 // X changed the aspect mode: save it
};

// The playback screen shared by both apps: timeline, seeking, options menu, stats.
class PlayerScreen
{
public:
    PlayerScreen();
    void Init( FFPlayer* player, Font* font, Font* titleFont, CpuMeter* cpu, PlayerHost* host, DWORD accent );

    void Reset();                           // a new video starts
    void Update( Pad* pad );                // after FFPlayer::Update()
    void Render( const D3DRECT& safe, float renderMs );
    void ShowFakeSeek( double target );     // testing: keep the bar up with a seek target

private:
    void UpdateMenu( WORD pressed );
    void RenderMenu();
    void RenderStats( float renderMs );
    void CommitSeek();
    void Notice( const std::wstring& text );

    FFPlayer*   m_player;
    Font*       m_font;
    Font*       m_titleFont;
    CpuMeter*   m_cpu;
    PlayerHost* m_host;
    DWORD       m_accent;

    WORD        m_prevStick;
    DWORD       m_osdTick;
    bool        m_osdHidden;
    bool        m_seekPending;
    double      m_seekTarget;
    DWORD       m_seekIdleTick;
    int         m_holdDir;
    DWORD       m_holdStart, m_holdLastTick;

    bool        m_menuOpen;
    int         m_menuPage, m_menuSel;

    DWORD       m_statsTick;
    long        m_statsShown;
    __int64     m_statsBytes;
    float       m_displayFps, m_mbps;

    std::wstring m_notice;                  // e.g. "Aspect ratio: Zoom", top of the screen for a moment
    DWORD       m_noticeTick;
};
