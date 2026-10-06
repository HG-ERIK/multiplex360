#pragma once
#include <xtl.h>

// Text alignment / truncation flags for Font::DrawText.
#define FONT_LEFT       0x00000000
#define FONT_RIGHT      0x00000001      // x is the right edge
#define FONT_CENTER_X   0x00000002      // x is the centre
#define FONT_TRUNCATED  0x00000008      // cut with "..." to fit maxWidth

// Bitmap font (.fnt from tools\makefont.ps1). Coordinates are relative to the window;
// negative x/y count from its right/bottom edge.
class Font
{
public:
    Font();
    ~Font();

    // Call once with the device before creating fonts.
    static void Startup( IDirect3DDevice9* device );

    HRESULT Create( const char* path );

    void  Begin();
    void  End();

    void  DrawText( float x, float y, DWORD color, const wchar_t* text, DWORD flags = 0, float maxWidth = 0 );

    // Width of the widest line and the height of all lines, with the scale applied.
    void  GetTextExtent( const wchar_t* text, float* width, float* height, BOOL firstLineOnly = FALSE ) const;
    float GetTextWidth( const wchar_t* text ) const;
    float GetFontHeight() const { return (float)m_lineAdvance; }     // line spacing, unscaled

    void  SetScaleFactors( float x, float y ) { m_scaleX = x; m_scaleY = y; }
    void  SetWindow( const D3DRECT& rc ) { m_window = rc; }
    void  SetWindow( LONG x1, LONG y1, LONG x2, LONG y2 );
    void  GetWindow( D3DRECT& rc ) const { rc = m_window; }

private:
    struct Glyph
    {
        WORD  x, y, width;
        SHORT offset;
        WORD  advance;
    };

    const Glyph& GlyphFor( wchar_t c ) const;
    void         Apply();      // render state for drawing text
    void         Restore();    // undo it (outside Begin/End, after each DrawText)

    IDirect3DTexture9* m_texture;
    Glyph*             m_glyphs;
    WORD*              m_translator;
    int                m_maxChar;
    int                m_cellHeight, m_lineAdvance, m_texW, m_texH;
    float              m_scaleX, m_scaleY;
    D3DRECT            m_window;
    int                m_beginCount;
};
