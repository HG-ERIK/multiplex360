#include "PlayerScreen.h"
#include <math.h>
#include <stdio.h>
#include "Draw.h"
#include "Log.h"

namespace
{
    const DWORD COLOR_TEXT     = 0xffeeeeee;
    const DWORD COLOR_DIM      = 0xff8a8a8a;

    const DWORD OSD_HIDE_MS    = 4000;
    const DWORD HOLD_MS        = 400;    // Left/Right held longer than this repeats
    const DWORD SEEK_COMMIT_MS = 700;    // taps add up; the jump happens after this pause
    const DWORD SLOW_COMMIT_MS = 1200;
    const int   TAP_SECONDS    = 10;
}

std::wstring FormatTime( double s )
{
    if( s < 0 )
        s = 0;
    unsigned t = (unsigned)s;
    wchar_t buf[32];
    if( t >= 3600 )
        swprintf_s( buf, L"%u:%02u:%02u", t / 3600, ( t / 60 ) % 60, t % 60 );
    else
        swprintf_s( buf, L"%u:%02u", t / 60, t % 60 );
    return buf;
}

PlayerScreen::PlayerScreen()
    : m_player( NULL ), m_font( NULL ), m_titleFont( NULL ), m_cpu( NULL ), m_host( NULL ), m_accent( 0xffe5a00d ),
      m_prevStick( 0 ), m_osdTick( 0 ), m_osdHidden( false ), m_seekPending( false ), m_seekTarget( 0 ),
      m_seekIdleTick( 0 ), m_holdDir( 0 ), m_holdStart( 0 ), m_holdLastTick( 0 ), m_menuOpen( false ), m_menuPage( 0 ),
      m_menuSel( 0 ), m_statsTick( 0 ), m_statsShown( 0 ), m_statsBytes( 0 ), m_displayFps( 0 ), m_mbps( 0 ),
      m_noticeTick( 0 )
{
}

void PlayerScreen::Init( FFPlayer* player, Font* font, Font* titleFont, CpuMeter* cpu, PlayerHost* host, DWORD accent )
{
    m_player = player;
    m_font = font;
    m_titleFont = titleFont;
    m_cpu = cpu;
    m_host = host;
    m_accent = accent;
}

void PlayerScreen::Reset()
{
    m_seekPending = false;
    m_holdDir = 0;
    m_menuOpen = false;
    m_osdHidden = false;
    m_osdTick = GetTickCount();
}

void PlayerScreen::ShowFakeSeek( double target )
{
    DWORD now = GetTickCount();
    m_osdTick = now;
    m_seekPending = true;
    m_seekTarget = target;
    m_seekIdleTick = now;
}

void PlayerScreen::Notice( const std::wstring& text )
{
    m_notice = text;
    m_noticeTick = GetTickCount();
}

void PlayerScreen::CommitSeek()
{
    m_seekPending = false;
    Log::Write( "Seek to %.0f s (from %.0f s)", m_seekTarget, m_player->Position() );
    m_host->Seek( m_seekTarget );
}

void PlayerScreen::Update( Pad* pad )
{
    // Left stick acts as the D-pad.
    WORD stick = 0;
    if( pad->sThumbLX > 16000 )  stick |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if( pad->sThumbLX < -16000 ) stick |= XINPUT_GAMEPAD_DPAD_LEFT;
    if( pad->sThumbLY > 16000 )  stick |= XINPUT_GAMEPAD_DPAD_UP;
    if( pad->sThumbLY < -16000 ) stick |= XINPUT_GAMEPAD_DPAD_DOWN;
    WORD pressed = pad->wPressedButtons | ( stick & ~m_prevStick );
    WORD held = pad->wButtons | stick;
    m_prevStick = stick;
    DWORD now = GetTickCount();

    // While (re)starting only B (cancel) works.
    if( !m_player->IsActive() )
    {
        if( pressed & XINPUT_GAMEPAD_B )
        {
            Log::Write( "Start cancelled" );
            m_host->CancelStart();
        }
        return;
    }

    if( m_host->ShowStats() && !m_cpu->Running() )
        m_cpu->Start();
    else if( !m_host->ShowStats() && m_cpu->Running() )
        m_cpu->Stop();

    if( m_menuOpen )
    {
        UpdateMenu( pressed );
        m_osdTick = now;
        return;
    }
    if( pressed & XINPUT_GAMEPAD_Y )
    {
        m_menuOpen = true;
        m_seekPending = false;
        m_holdDir = 0;
        m_menuPage = 0;
        m_menuSel = 0;
        return;
    }
    if( pressed & XINPUT_GAMEPAD_X )
    {
        m_player->SetAspect( ( m_player->Aspect() + 1 ) % FFPlayer::ASPECT_COUNT );
        Notice( std::wstring( L"Aspect ratio: " ) + FFPlayer::AspectName( m_player->Aspect() ) );
        Log::Write( "Aspect mode %d", m_player->Aspect() );
        m_host->AspectChanged();
        return;
    }

    // Up toggles the timeline, Down hides it, any other input shows it.
    bool wasVisible = !m_osdHidden && ( now - m_osdTick < OSD_HIDE_MS || m_player->IsPaused() || m_seekPending );
    bool hide = !m_seekPending && ( ( pressed & XINPUT_GAMEPAD_DPAD_DOWN ) ||
                                    ( ( pressed & XINPUT_GAMEPAD_DPAD_UP ) && wasVisible ) );
    if( hide )
    {
        m_osdTick = 0;
        m_osdHidden = true;         // stays hidden even while paused, until the next input
    }
    else if( pressed || ( held & ( XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT ) ) )
    {
        m_osdTick = now;
        m_osdHidden = false;
    }

    // B: cancel the seek, else hide the timeline, else leave.
    if( pressed & XINPUT_GAMEPAD_B )
    {
        if( m_seekPending )
            m_seekPending = false;
        else if( wasVisible )
        {
            m_osdTick = 0;
            m_osdHidden = true;
        }
        else
            m_host->Leave();
        return;
    }
    if( pressed & ( XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START ) )
    {
        if( m_seekPending )
        {
            m_holdDir = 0;
            CommitSeek();           // A jumps to the chosen spot right away
            return;
        }
        m_player->TogglePause();
    }

    double duration = m_player->Duration() > 0 ? m_player->Duration() : m_host->DurationHint();

    // Left/Right: tap = 10 s, hold = repeat. Seeks on release (or A).
    int dir = 0;
    if( pressed & XINPUT_GAMEPAD_DPAD_RIGHT ) dir = 1;
    if( pressed & XINPUT_GAMEPAD_DPAD_LEFT )  dir = -1;
    if( dir )
    {
        if( !m_seekPending )
        {
            m_seekPending = true;
            m_seekTarget = m_player->Position();
        }
        m_seekTarget += dir * TAP_SECONDS;
        m_holdDir = dir;
        m_holdStart = m_holdLastTick = now;
    }
    if( m_holdDir )
    {
        WORD key = m_holdDir > 0 ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
        if( held & key )
        {
            // ~7 steps a second: 10 s, then 30 s after 2 s, 60 s after 4 s, more in long films.
            DWORD heldFor = now - m_holdStart;
            if( heldFor > HOLD_MS && now - m_holdLastTick >= 140 )
            {
                double step = heldFor < 2000 ? 10 : heldFor < 4000 ? 30 : 60;
                if( heldFor >= 6000 && duration / 70 > step )
                    step = duration / 70;
                m_seekTarget += m_holdDir * step;
                m_holdLastTick = now;
            }
        }
        else
        {
            m_holdDir = 0;
            m_seekIdleTick = now;
        }
    }
    if( pressed & ( XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER ) )
    {
        if( !m_seekPending )
        {
            m_seekPending = true;
            m_seekTarget = m_player->Position();
        }
        m_seekTarget += ( pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER ) ? 300 : -300;
        m_seekIdleTick = now;
    }

    if( m_seekPending )
    {
        if( m_seekTarget < 0 )
            m_seekTarget = 0;
        if( duration > 0 && m_seekTarget > duration - 5 )
            m_seekTarget = duration - 5;
        if( !m_holdDir && now - m_seekIdleTick > ( m_host->SlowSeek() ? SLOW_COMMIT_MS : SEEK_COMMIT_MS ) )
            CommitSeek();
    }
}

void PlayerScreen::Render( const D3DRECT& safe, float renderMs )
{
    DWORD now = GetTickCount();
    if( !m_player->IsActive() )
    {
        float cx = (float)( safe.x2 - safe.x1 ) / 2;
        static const wchar_t* const dots[4] = { L"", L".", L"..", L"..." };
        std::wstring title = m_host->Title();
        m_titleFont->Begin();
        m_titleFont->DrawText( cx, 300, COLOR_TEXT, title.c_str(), FONT_CENTER_X | FONT_TRUNCATED,
                               (float)( safe.x2 - safe.x1 ) - 40 );
        m_titleFont->End();
        std::wstring msg = m_host->StartMessage();
        while( !msg.empty() && msg[msg.size() - 1] == L'.' )
            msg.erase( msg.size() - 1 );
        m_font->Begin();
        m_font->DrawText( cx, 350, m_accent, ( msg + dots[( now / 400 ) % 4] ).c_str(), FONT_CENTER_X );
        m_font->DrawText( cx, 400, COLOR_DIM, L"B  Cancel", FONT_CENTER_X );
        m_font->End();
        return;
    }
    bool buffering = m_player->IsBuffering();
    bool visible = !m_menuOpen && ( ( !m_osdHidden && ( now - m_osdTick < OSD_HIDE_MS || m_player->IsPaused() ) ) ||
                                    m_seekPending || buffering );
    // Subtitles move up above the controls while they're showing.
    m_host->DrawOverlay( visible ? (float)safe.y2 - 240.0f : (float)safe.y2 - 28.0f );
    if( m_host->ShowStats() )
        RenderStats( renderMs );
    if( !m_notice.empty() && now - m_noticeTick < 2000 && !m_menuOpen )
    {
        float nw = 0, nh = 0;
        m_titleFont->GetTextExtent( m_notice.c_str(), &nw, &nh );
        float cx = 640, top = (float)safe.y1 + 10;
        Draw::Rect( cx - nw / 2 - 24, top, cx + nw / 2 + 24, top + nh + 20, 0xC0000000 );
        D3DRECT window;
        m_titleFont->GetWindow( window );
        m_titleFont->SetWindow( 0, 0, 1280, 720 );
        m_titleFont->Begin();
        m_titleFont->DrawText( cx, top + 10, COLOR_TEXT, m_notice.c_str(), FONT_CENTER_X );
        m_titleFont->End();
        m_titleFont->SetWindow( window );
    }
    if( m_menuOpen )
    {
        RenderMenu();
        return;
    }
    if( !visible )
        return;

    const float sw = 1280, sh = 720;
    float left = (float)safe.x1, right = (float)safe.x2, width = right - left;
    float barY = (float)safe.y2 - 58.0f;

    // Gradient behind the controls.
    const float fadeTop = barY - 170.0f, fadeBottom = barY - 40.0f;
    const int strips = 26;
    for( int i = 0; i < strips; ++i )
    {
        float y0 = fadeTop + ( fadeBottom - fadeTop ) * i / strips;
        float y1 = fadeTop + ( fadeBottom - fadeTop ) * ( i + 1 ) / strips;
        DWORD alpha = (DWORD)( 180.0f * ( i + 1 ) / strips );
        Draw::Rect( 0, y0, sw, y1, alpha << 24 );
    }
    Draw::Rect( 0, fadeBottom, sw, sh, 0xB4000000 );

    double duration = m_player->Duration() > 0 ? m_player->Duration() : m_host->DurationHint();
    double pos = m_player->Position();
    double shown = m_seekPending ? m_seekTarget : pos;
    float fPos = duration > 0 ? (float)( pos / duration ) : 0.0f;
    float fShown = duration > 0 ? (float)( shown / duration ) : 0.0f;
    if( fPos > 1 ) fPos = 1;
    if( fShown > 1 ) fShown = 1;
    Draw::Rect( left, barY, right, barY + 6, 0x60FFFFFF );
    Draw::Rect( left, barY, left + width * fPos, barY + 6, m_accent );
    if( m_seekPending )
        Draw::Rect( left + width * min( fPos, fShown ), barY, left + width * max( fPos, fShown ), barY + 6,
                    0xC0FFFFFF );
    float knobX = left + width * fShown;
    Draw::Rect( knobX - 8, barY - 5, knobX + 8, barY + 11, 0xFFFFFFFF );

    IDirect3DTexture9* thumb = m_seekPending ? m_host->Preview( m_seekTarget ) : NULL;
    if( thumb )
    {
        float px = knobX - 160;
        if( px < left ) px = left;
        if( px > right - 320 ) px = right - 320;
        float py = barY - 290;
        Draw::Rect( px - 3, py - 3, px + 323, py + 183, 0xFF000000 );
        Draw::Image( thumb, px, py, px + 320, py + 180 );
        Draw::Frame( px - 3, py - 3, px + 323, py + 183, 2, 0xFFFFFFFF );
    }

    float ty = barY - safe.y1;
    std::wstring title = m_host->Title(), tag = m_host->Tag();
    float tagW = 0, titleW = 0, th = 0;
    m_font->GetTextExtent( tag.c_str(), &tagW, &th );
    m_titleFont->GetTextExtent( title.c_str(), &titleW, &th );
    float titleMax = width - tagW - 40;
    m_titleFont->Begin();
    m_titleFont->DrawText( 0, ty - 80, COLOR_TEXT, title.c_str(), titleW > titleMax ? FONT_TRUNCATED : 0, titleMax );
    if( m_seekPending )
    {
        double delta = m_seekTarget - pos;
        std::wstring label = FormatTime( m_seekTarget ) + L"   (" + ( delta >= 0 ? L"+" : L"-" ) +
                             FormatTime( fabs( delta ) ) + L")";
        float lx = knobX - left;
        if( lx < 120 ) lx = 120;
        if( lx > width - 120 ) lx = width - 120;
        m_titleFont->DrawText( lx, ty - 40, COLOR_TEXT, label.c_str(), FONT_CENTER_X );
    }
    m_titleFont->End();

    m_font->Begin();
    m_font->DrawText( 0, ty + 14, COLOR_TEXT, FormatTime( pos ).c_str() );
    m_font->DrawText( width, ty + 14, COLOR_DIM, FormatTime( duration ).c_str(), FONT_RIGHT );
    std::wstring state = buffering ? L"Buffering..." : m_player->IsPaused() ? L"Paused" : L"";
    if( !state.empty() )
        m_font->DrawText( width / 2, ty + 14, m_accent, state.c_str(), FONT_CENTER_X );
    if( !tag.empty() )
        m_font->DrawText( width, ty - 74, COLOR_DIM, tag.c_str(), FONT_RIGHT );
    m_font->DrawText( 0, ty + 40, COLOR_DIM, m_seekPending ?
                      L"Left/Right  Move (hold = faster)      A  Jump here      B  Cancel" :
                      L"Left/Right  Seek      A  Pause      LB/RB  5 min      Down  Hide      X  Aspect      Y  Options      B  Back" );
    m_font->End();
}

void PlayerScreen::UpdateMenu( WORD pressed )
{
    // Y closes; B/Left goes back a level.
    if( pressed & XINPUT_GAMEPAD_Y )
    {
        m_menuOpen = false;
        return;
    }
    MenuPage page;
    m_host->BuildMenu( m_menuPage, page );
    MenuMove move = MenuMove::Stay();
    if( page.slider )
    {
        int step = ( pressed & XINPUT_GAMEPAD_DPAD_RIGHT ) ? 1 : ( pressed & XINPUT_GAMEPAD_DPAD_LEFT ) ? -1 : 0;
        if( step )
            m_host->SliderStep( m_menuPage, step );
        if( pressed & ( XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B ) )
            move = m_host->Back( m_menuPage );
    }
    else
    {
        int count = (int)page.entries.size();
        if( ( pressed & XINPUT_GAMEPAD_DPAD_DOWN ) && m_menuSel < count - 1 ) ++m_menuSel;
        if( ( pressed & XINPUT_GAMEPAD_DPAD_UP ) && m_menuSel > 0 )           --m_menuSel;

        if( pressed & ( XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_RIGHT ) )
        {
            if( m_menuSel < count && ( ( pressed & XINPUT_GAMEPAD_A ) || page.entries[m_menuSel].opens ) )
                move = m_host->Choose( m_menuPage, m_menuSel );
        }
        else if( pressed & ( XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_LEFT ) )
            move = m_host->Back( m_menuPage );
    }
    if( move.kind == MenuMove::OPEN )
    {
        m_menuPage = move.page;
        m_menuSel = move.sel;
    }
    else if( move.kind == MenuMove::CLOSE )
        m_menuOpen = false;
}

void PlayerScreen::RenderMenu()
{
    MenuPage page;
    m_host->BuildMenu( m_menuPage, page );

    const float x0 = 690, x1 = 1230, y0 = 70, rowH = 50;
    const int maxRows = 8;
    int count = (int)page.entries.size();
    int first = 0;
    if( count > maxRows )
    {
        first = m_menuSel - maxRows / 2;
        if( first < 0 ) first = 0;
        if( first > count - maxRows ) first = count - maxRows;
    }
    int shown = page.slider ? 2 : min( count, maxRows );
    float y1 = y0 + 84 + shown * rowH + 56;
    Draw::Rect( x0, y0, x1, y1, 0xEE141414 );
    Draw::Rect( x0, y0 + 66, x1, y0 + 67, 0x40FFFFFF );

    D3DRECT full = { 0, 0, 1280, 720 };
    m_titleFont->SetWindow( full );
    m_font->SetWindow( full );
    m_titleFont->Begin();
    m_titleFont->DrawText( x0 + 28, y0 + 18, COLOR_TEXT, ( m_menuPage == 0 ? page.title : L"<  " + page.title ).c_str() );
    m_titleFont->End();

    if( page.slider )
    {
        float bx0 = x0 + 40, bx1 = x1 - 40, by = y0 + 150;
        float f = (float)( page.value - page.minValue ) / (float)( page.maxValue - page.minValue );
        Draw::Rect( bx0, by, bx1, by + 6, 0x60FFFFFF );
        Draw::Rect( bx0, by, bx0 + ( bx1 - bx0 ) * f, by + 6, m_accent );
        float kx = bx0 + ( bx1 - bx0 ) * f;
        Draw::Rect( kx - 8, by - 6, kx + 8, by + 12, 0xFFFFFFFF );
        wchar_t buf[16];
        swprintf_s( buf, L"%+d", page.value );
        m_titleFont->Begin();
        m_titleFont->DrawText( ( x0 + x1 ) / 2, y0 + 90, COLOR_TEXT, buf, FONT_CENTER_X );
        m_titleFont->End();
        m_font->Begin();
        m_font->DrawText( x0 + 28, y1 - 40, COLOR_DIM, L"Left/Right  Adjust      A  Done" );
        m_font->End();
    }
    else
    {
        for( int i = first; i < first + shown; ++i )
        {
            const MenuEntry& e = page.entries[i];
            float y = y0 + 84 + ( i - first ) * rowH;
            bool sel = i == m_menuSel;
            if( sel )
            {
                Draw::Rect( x0 + 8, y - 8, x1 - 8, y + rowH - 14, 0xFF303030 );
                Draw::Rect( x0 + 8, y - 8, x0 + 13, y + rowH - 14, m_accent );
            }
            if( e.checked )
                Draw::Rect( x0 + 24, y + 9, x0 + 32, y + 17, m_accent );

            // The value only gets the room the label leaves.
            const float labelX = x0 + 44, right = x1 - 24;
            std::wstring value = e.value + ( e.opens ? L"  >" : L"" );
            float labelW = 0, h = 0, valueW = 0;
            m_font->GetTextExtent( e.label.c_str(), &labelW, &h );
            if( !value.empty() )
                m_font->GetTextExtent( value.c_str(), &valueW, &h );
            float labelMax = right - labelX - ( value.empty() ? 0 : min( valueW, ( right - labelX ) * 0.6f ) + 24 );
            m_font->Begin();
            m_font->DrawText( labelX, y, sel ? COLOR_TEXT : 0xFFBBBBBB, e.label.c_str(),
                              labelW > labelMax ? FONT_TRUNCATED : 0, labelMax );
            if( !value.empty() )
            {
                float valueMax = right - labelX - min( labelW, labelMax ) - 24;
                m_font->DrawText( right, y, sel ? m_accent : COLOR_DIM, value.c_str(),
                                  FONT_RIGHT | ( valueW > valueMax ? FONT_TRUNCATED : 0 ), valueMax );
            }
            m_font->End();
        }
        m_font->Begin();
        m_font->DrawText( x0 + 28, y1 - 40, COLOR_DIM,
                          m_menuPage == 0 ? L"A  Select      B  Close" : L"A  Select      B  Back      Y  Close" );
        m_font->End();
    }
    m_titleFont->SetWindow( SafeArea() );
    m_font->SetWindow( SafeArea() );
}

void PlayerScreen::RenderStats( float renderMs )
{
    DWORD now = GetTickCount();
    FFPlayer::Stats st;
    m_player->GetStats( st );
    if( now - m_statsTick >= 1000 )
    {
        float secs = ( now - m_statsTick ) / 1000.0f;
        if( m_statsTick )
        {
            m_displayFps = ( st.shown - m_statsShown ) / secs;
            m_mbps = (float)( st.bytesRead - m_statsBytes ) * 8.0f / secs / 1e6f;
        }
        m_statsTick = now;
        m_statsShown = st.shown;
        m_statsBytes = st.bytesRead;
    }

    MEMORYSTATUS mem;
    GlobalMemoryStatus( &mem );

    wchar_t lines[9][160];
    swprintf_s( lines[0], L"Video     %s", m_player->MediaInfo().c_str() );
    swprintf_s( lines[1], L"Frames    %ld decoded   %ld shown   %ld dropped   %ld catch-ups%s", st.decoded, st.shown,
                st.dropped, st.catchups, st.skippingB ? L"   (skipping B-frames)" : L"" );
    swprintf_s( lines[2], L"Display   %.1f fps   render %.1f ms/frame", m_displayFps, renderMs );
    swprintf_s( lines[3], L"Buffers   video %d packets   audio %d packets   %d ms queued", st.videoPackets,
                st.audioPackets, st.audioBufferedMs );
    swprintf_s( lines[4], L"Input     %.2f Mbit/s   %.1f MB read", m_mbps, st.bytesRead / 1048576.0 );
    swprintf_s( lines[5], L"CPU       %2.0f%%  %2.0f%%  |  %2.0f%%  %2.0f%%  |  %2.0f%%  %2.0f%%     (core 0 | 1 | 2)",
                m_cpu->Busy( 0 ) * 100, m_cpu->Busy( 1 ) * 100, m_cpu->Busy( 2 ) * 100, m_cpu->Busy( 3 ) * 100,
                m_cpu->Busy( 4 ) * 100, m_cpu->Busy( 5 ) * 100 );
    swprintf_s( lines[6], L"Memory    %u MB free of %u MB", (unsigned)( mem.dwAvailPhys >> 20 ), (unsigned)( mem.dwTotalPhys >> 20 ) );
    swprintf_s( lines[7], L"Colour    %s   brightness %+d", m_player->ColorInfo().c_str(), m_host->Brightness() );
    swprintf_s( lines[8], L"Mode      %s", m_host->StatsMode().c_str() );

    Draw::Rect( 30, 30, 840, 30 + 22 + 9 * 24, 0xC0000000 );
    D3DRECT full = { 0, 0, 1280, 720 };
    m_font->SetWindow( full );
    m_font->Begin();
    m_font->SetScaleFactors( 0.85f, 0.85f );
    for( int i = 0; i < 9; ++i )
        m_font->DrawText( 44, 40.0f + i * 24, i == 5 ? m_accent : COLOR_TEXT, lines[i], FONT_TRUNCATED, 780 );
    m_font->SetScaleFactors( 1.0f, 1.0f );
    m_font->End();
    m_font->SetWindow( SafeArea() );
}
