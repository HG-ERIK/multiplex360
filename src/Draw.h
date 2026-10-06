#pragma once
#include <xtl.h>

// Alpha-blended rectangles and images in screen pixels.
namespace Draw
{
    bool Startup( IDirect3DDevice9* device, int screenWidth, int screenHeight );

    // color is ARGB; alpha < 255 blends over what's already there.
    void Rect( float x0, float y0, float x1, float y1, DWORD color );

    // Texture stretched over the rectangle, multiplied by tint (ARGB; 0xFFFFFFFF = as is).
    void Image( IDirect3DTexture9* tex, float x0, float y0, float x1, float y1, DWORD tint = 0xFFFFFFFF );

    // Outline of the given thickness inside the rectangle.
    void Frame( float x0, float y0, float x1, float y1, float thickness, DWORD color );
}
