#include "App.h"
#include "Draw.h"
#include "Log.h"

namespace
{
    LONG g_width = 1280, g_height = 720;
}

App::App() : m_pd3dDevice( NULL )
{
    ZeroMemory( &m_d3dpp, sizeof( m_d3dpp ) );
    m_d3dpp.BackBufferWidth = 1280;         // always 720p; the console scales to the TV
    m_d3dpp.BackBufferHeight = 720;
    m_d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;
    m_d3dpp.BackBufferCount = 1;
    m_d3dpp.MultiSampleType = D3DMULTISAMPLE_NONE;
    m_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    m_d3dpp.EnableAutoDepthStencil = TRUE;
    m_d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;
    m_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

void App::Run()
{
    IDirect3D9* d3d = Direct3DCreate9( D3D_SDK_VERSION );
    HRESULT hr = d3d ? d3d->CreateDevice( 0, D3DDEVTYPE_HAL, NULL, 0, &m_d3dpp, &m_pd3dDevice ) : E_FAIL;
    if( d3d )
        d3d->Release();
    if( FAILED( hr ) )
    {
        Log::Write( "Could not create the Direct3D device (0x%08X)", hr );
        return;
    }
    g_width = m_d3dpp.BackBufferWidth;
    g_height = m_d3dpp.BackBufferHeight;
    if( FAILED( Initialize() ) )
    {
        Log::Write( "Initialize failed" );
        return;
    }
    for( ;; )
    {
        Update();
        Render();
    }
}

D3DRECT SafeArea()
{
    D3DRECT rc = { g_width / 10, g_height / 10, g_width * 9 / 10, g_height * 9 / 10 };
    return rc;
}

void FillGradient( DWORD top, DWORD bottom )
{
    const int bands = g_height / 4;
    for( int i = 0; i < bands; ++i )
    {
        float t = ( i + 0.5f ) / bands;
        DWORD c = 0xFF000000;
        for( int shift = 0; shift < 24; shift += 8 )
        {
            float a = (float)( ( top >> shift ) & 255 ), b = (float)( ( bottom >> shift ) & 255 );
            c |= (DWORD)( a + ( b - a ) * t + 0.5f ) << shift;
        }
        Draw::Rect( 0, (float)( i * g_height / bands ), (float)g_width, (float)( ( i + 1 ) * g_height / bands ), c );
    }
}

Pad* ReadPads()
{
    static Pad pad;
    static WORD last = 0;
    const SHORT DEAD = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    WORD buttons = 0;
    int lx = 0, ly = 0;
    for( DWORD i = 0; i < XUSER_MAX_COUNT; ++i )
    {
        XINPUT_STATE st;
        if( XInputGetState( i, &st ) != ERROR_SUCCESS )
            continue;
        buttons |= st.Gamepad.wButtons;
        if( st.Gamepad.sThumbLX > DEAD || st.Gamepad.sThumbLX < -DEAD )
            lx = st.Gamepad.sThumbLX;
        if( st.Gamepad.sThumbLY > DEAD || st.Gamepad.sThumbLY < -DEAD )
            ly = st.Gamepad.sThumbLY;
    }
    pad.wButtons = buttons;
    pad.wPressedButtons = buttons & ~last;
    last = buttons;
    pad.sThumbLX = (SHORT)lx;
    pad.sThumbLY = (SHORT)ly;
    pad.fX1 = lx > 0 ? ( lx - DEAD ) / ( 32767.0f - DEAD ) : lx < 0 ? ( lx + DEAD ) / ( 32768.0f - DEAD ) : 0.0f;
    pad.fY1 = ly > 0 ? ( ly - DEAD ) / ( 32767.0f - DEAD ) : ly < 0 ? ( ly + DEAD ) / ( 32768.0f - DEAD ) : 0.0f;
    return &pad;
}
