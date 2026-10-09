#include <xtl.h>
#include <d3dx9.h>
#include <stdio.h>
#include <math.h>
#include "Font.h"
#include "Log.h"

namespace
{
    IDirect3DDevice9*            g_device = NULL;
    IDirect3DVertexShader9*      g_vs = NULL;
    IDirect3DPixelShader9*       g_ps = NULL;
    IDirect3DVertexDeclaration9* g_decl = NULL;
    float                        g_screenW = 1280, g_screenH = 720;
    float                        g_squeeze = 1.0f;     // 0.75 on a 4:3 TV, which squeezes our 16:9 picture

    // Half-pixel shift keeps glyphs on the pixel grid.
    const char* VS =
        "float4 scale : register( c0 );\n"     // 2/width, 2/height
        "struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };\n"
        "V main( V v )\n"
        "{\n"
        "    V o = v;\n"
        "    o.pos = float4( ( v.pos.x - 0.5 ) * scale.x - 1.0, 1.0 - ( v.pos.y - 0.5 ) * scale.y, 0, 1 );\n"
        "    return o;\n"
        "}\n";
    const char* PS =
        "sampler2D glyphs : register( s0 );\n"
        "float4 main( float2 uv : TEXCOORD0, float4 color : COLOR0 ) : COLOR { return tex2D( glyphs, uv ) * color; }\n";

    struct Vertex
    {
        float x, y, z, w;
        float u, v;
        float r, g, b, a;
    };
    const D3DVERTEXELEMENT9 ELEMENTS[] =
    {
        { 0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };

    const int MAX_GLYPHS_PER_DRAW = 256;
    Vertex g_vertices[MAX_GLYPHS_PER_DRAW * 6];

    bool Compile( const char* src, const char* profile, ID3DXBuffer** code )
    {
        ID3DXBuffer* errors = NULL;
        HRESULT hr = D3DXCompileShader( src, (UINT)strlen( src ), NULL, NULL, "main", profile, 0, code, &errors, NULL );
        if( FAILED( hr ) )
            Log::Write( "Font shader: %s", errors ? (const char*)errors->GetBufferPointer() : "?" );
        if( errors )
            errors->Release();
        return SUCCEEDED( hr );
    }

    WORD Read16( const BYTE*& p )
    {
        WORD v = (WORD)( ( p[0] << 8 ) | p[1] );
        p += 2;
        return v;
    }
}

void Font::Startup( IDirect3DDevice9* device )
{
    g_device = device;
    // A 4:3 TV shows the 1280x720 picture squeezed sideways; draw letters narrower so they look normal.
    XVIDEO_MODE video;
    XGetVideoMode( &video );
    g_squeeze = video.fIsWideScreen ? 1.0f : 0.75f;
    D3DDISPLAYMODE mode;
    if( SUCCEEDED( device->GetDisplayMode( 0, &mode ) ) && mode.Width > 0 )
    {
        g_screenW = (float)mode.Width;
        g_screenH = (float)mode.Height;
    }
    ID3DXBuffer* code = NULL;
    if( Compile( VS, "vs_3_0", &code ) )
    {
        device->CreateVertexShader( (const DWORD*)code->GetBufferPointer(), &g_vs );
        code->Release();
    }
    if( Compile( PS, "ps_3_0", &code ) )
    {
        device->CreatePixelShader( (const DWORD*)code->GetBufferPointer(), &g_ps );
        code->Release();
    }
    device->CreateVertexDeclaration( ELEMENTS, &g_decl );
}

Font::Font()
    : m_texture( NULL ), m_glyphs( NULL ), m_translator( NULL ), m_maxChar( 0 ), m_cellHeight( 0 ), m_lineAdvance( 0 ),
      m_texW( 1 ), m_texH( 1 ), m_scaleX( g_squeeze ), m_scaleY( 1 ), m_beginCount( 0 )
{
    SetWindow( 0, 0, (LONG)g_screenW, (LONG)g_screenH );
}

Font::~Font()
{
    if( m_texture )
        m_texture->Release();
    delete[] m_glyphs;
    delete[] m_translator;
}

void Font::SetWindow( LONG x1, LONG y1, LONG x2, LONG y2 )
{
    m_window.x1 = x1;
    m_window.y1 = y1;
    m_window.x2 = x2;
    m_window.y2 = y2;
}

void Font::SetScaleFactors( float x, float y )
{
    m_scaleX = x * g_squeeze;
    m_scaleY = y;
}

HRESULT Font::Create( const char* path )
{
    m_scaleX = g_squeeze;
    if( !g_device || !g_vs || !g_ps || !g_decl )
        return E_FAIL;
    FILE* f = fopen( path, "rb" );
    if( !f )
    {
        Log::Write( "Font: can't open %s", path );
        return E_FAIL;
    }
    fseek( f, 0, SEEK_END );
    long size = ftell( f );
    fseek( f, 0, SEEK_SET );
    BYTE* data = new BYTE[size];
    size_t got = fread( data, 1, size, f );
    fclose( f );

    HRESULT hr = E_FAIL;
    const BYTE* p = data;
    const BYTE* end = data + got;
    if( got > 20 && memcmp( p, "M3FN", 4 ) == 0 && p[7] == 1 )
    {
        p += 8;
        m_cellHeight = Read16( p );
        m_lineAdvance = Read16( p );
        m_texW = Read16( p );
        m_texH = Read16( p );
        m_maxChar = Read16( p );
        int count = Read16( p );
        size_t need = ( m_maxChar + 1 ) * 2 + count * 10 + (size_t)m_texW * m_texH * 4;
        if( count > 0 && (size_t)( end - p ) >= need )
        {
            m_translator = new WORD[m_maxChar + 1];
            for( int i = 0; i <= m_maxChar; ++i )
            {
                m_translator[i] = Read16( p );
                if( m_translator[i] >= count )
                    m_translator[i] = 0;
            }
            m_glyphs = new Glyph[count];
            for( int i = 0; i < count; ++i )
            {
                m_glyphs[i].x = Read16( p );
                m_glyphs[i].y = Read16( p );
                m_glyphs[i].width = Read16( p );
                m_glyphs[i].offset = (SHORT)Read16( p );
                m_glyphs[i].advance = Read16( p );
            }
            // A, R, G, B bytes are exactly a big-endian D3DFMT_A8R8G8B8 texel.
            if( SUCCEEDED( g_device->CreateTexture( m_texW, m_texH, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT,
                                                    &m_texture, NULL ) ) )
            {
                D3DLOCKED_RECT lr;
                if( SUCCEEDED( m_texture->LockRect( 0, &lr, NULL, 0 ) ) )
                {
                    for( int y = 0; y < m_texH; ++y )
                        memcpy( (BYTE*)lr.pBits + y * lr.Pitch, p + (size_t)y * m_texW * 4, m_texW * 4 );
                    m_texture->UnlockRect( 0 );
                    hr = S_OK;
                }
            }
        }
    }
    delete[] data;
    if( FAILED( hr ) )
        Log::Write( "Font: %s is not a valid font file", path );
    return hr;
}

const Font::Glyph& Font::GlyphFor( wchar_t c ) const
{
    int index = (int)c <= m_maxChar ? m_translator[c] : m_translator['?' <= m_maxChar ? '?' : 0];
    return m_glyphs[index];
}

void Font::Begin()
{
    ++m_beginCount;
}

void Font::End()
{
    if( m_beginCount > 0 && --m_beginCount == 0 )
        Restore();
}

// Set on every DrawText: other drawing may have changed the state.
void Font::Apply()
{
    float scale[4] = { 2.0f / g_screenW, 2.0f / g_screenH, 0, 0 };
    g_device->SetVertexDeclaration( g_decl );
    g_device->SetVertexShader( g_vs );
    g_device->SetPixelShader( g_ps );
    g_device->SetVertexShaderConstantF( 0, scale, 1 );
    g_device->SetTexture( 0, m_texture );
    g_device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
    g_device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
    g_device->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
    g_device->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
    g_device->SetRenderState( D3DRS_ZENABLE, FALSE );
    g_device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    g_device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
    g_device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
    g_device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
    g_device->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
    g_device->SetRenderState( D3DRS_ALPHATESTENABLE, TRUE );
    g_device->SetRenderState( D3DRS_ALPHAREF, 0x08 );
    g_device->SetRenderState( D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL );
}

void Font::Restore()
{
    g_device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
    g_device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
    g_device->SetTexture( 0, NULL );
    g_device->SetVertexShader( NULL );
    g_device->SetPixelShader( NULL );
}

void Font::GetTextExtent( const wchar_t* text, float* width, float* height, BOOL firstLineOnly ) const
{
    int lineWidth = 0, widest = 0, lines = 1;
    for( ; text && *text; ++text )
    {
        if( *text == L'\n' )
        {
            if( firstLineOnly )
                break;
            lineWidth = 0;
            ++lines;
            continue;
        }
        if( *text == L'\r' )
            continue;
        const Glyph& g = GlyphFor( *text );
        lineWidth += g.offset + g.advance;
        if( lineWidth > widest )
            widest = lineWidth;
    }
    *width = widest * m_scaleX;
    *height = ( m_cellHeight + ( lines - 1 ) * m_lineAdvance ) * m_scaleY;
}

float Font::GetTextWidth( const wchar_t* text ) const
{
    float w, h;
    GetTextExtent( text, &w, &h );
    return w;
}

void Font::DrawText( float x, float y, DWORD color, const wchar_t* text, DWORD flags, float maxWidth )
{
    if( !text || !*text || !m_texture )
        return;

    if( x < 0 || ( ( flags & FONT_RIGHT ) && x <= 0 ) )
        x += (float)( m_window.x2 - m_window.x1 );
    if( y < 0 )
        y += (float)( m_window.y2 - m_window.y1 );
    float originX = x + m_window.x1;
    float cursorX = floorf( x ) + m_window.x1;
    float cursorY = floorf( y ) + m_window.y1;

    if( flags & FONT_TRUNCATED )
    {
        float w, h;
        GetTextExtent( text, &w, &h, TRUE );
        if( maxWidth <= 0 || w <= maxWidth )
            flags &= ~FONT_TRUNCATED;
    }
    const Glyph& dot = GlyphFor( L'.' );
    float dotsWidth = m_scaleX * 3.0f * ( dot.offset + dot.advance );

    float r = ( ( color >> 16 ) & 255 ) / 255.0f, g = ( ( color >> 8 ) & 255 ) / 255.0f;
    float b = ( color & 255 ) / 255.0f, a = ( color >> 24 ) / 255.0f;
    float h = m_cellHeight * m_scaleY;
    float invW = 1.0f / m_texW, invH = 1.0f / m_texH;

    Apply();
    float limit = originX + maxWidth;       // where truncated text must end
    int quads = 0, dotsLeft = 0;
    bool lineStart = true;
    for( ;; )
    {
        wchar_t c;
        if( dotsLeft )
            c = L'.';
        else
        {
            if( !*text )
                break;
            if( lineStart )
            {
                if( flags & ( FONT_RIGHT | FONT_CENTER_X ) )
                {
                    float w, lh;
                    GetTextExtent( text, &w, &lh, TRUE );
                    if( ( flags & FONT_TRUNCATED ) && w > maxWidth )
                        w = maxWidth;
                    cursorX = floorf( ( flags & FONT_RIGHT ) ? originX - w : originX - w * 0.5f );
                    limit = cursorX + maxWidth;
                }
                lineStart = false;
            }
            c = *text++;
            if( c == L'\n' )
            {
                cursorX = originX;
                cursorY += m_lineAdvance * m_scaleY;
                lineStart = true;
                continue;
            }
            if( c == L'\r' )
                continue;
        }

        const Glyph& gl = GlyphFor( c );
        float offset = m_scaleX * gl.offset, width = m_scaleX * gl.width;
        if( !dotsLeft && ( flags & FONT_TRUNCATED ) && cursorX + offset + width + dotsWidth > limit )
        {
            dotsLeft = 3;       // no room for this letter and "...": end with the dots
            continue;
        }

        float x0 = cursorX + offset, x1 = x0 + width, y0 = cursorY, y1 = cursorY + h;
        float u0 = gl.x * invW, u1 = ( gl.x + gl.width ) * invW;
        float v0 = gl.y * invH, v1 = ( gl.y + m_cellHeight ) * invH;
        Vertex* q = &g_vertices[quads * 6];
        Vertex corners[4] = { { x0, y0, 0, 1, u0, v0, r, g, b, a }, { x1, y0, 0, 1, u1, v0, r, g, b, a },
                              { x0, y1, 0, 1, u0, v1, r, g, b, a }, { x1, y1, 0, 1, u1, v1, r, g, b, a } };
        q[0] = corners[0]; q[1] = corners[1]; q[2] = corners[2];
        q[3] = corners[1]; q[4] = corners[3]; q[5] = corners[2];
        cursorX += offset + m_scaleX * gl.advance;

        if( ++quads == MAX_GLYPHS_PER_DRAW )
        {
            g_device->DrawPrimitiveUP( D3DPT_TRIANGLELIST, quads * 2, g_vertices, sizeof( Vertex ) );
            quads = 0;
        }
        if( dotsLeft && --dotsLeft == 0 )
            break;
    }
    if( quads )
        g_device->DrawPrimitiveUP( D3DPT_TRIANGLELIST, quads * 2, g_vertices, sizeof( Vertex ) );
    if( m_beginCount == 0 )
        Restore();
}
