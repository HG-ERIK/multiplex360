#include <xtl.h>
#include <d3dx9.h>
#include "Draw.h"
#include "Log.h"

namespace
{
    IDirect3DDevice9*            g_device = NULL;
    IDirect3DVertexShader9*      g_vs = NULL;
    IDirect3DPixelShader9*       g_ps = NULL;
    IDirect3DVertexDeclaration9* g_decl = NULL;
    IDirect3DVertexShader9*      g_vsTex = NULL;
    IDirect3DPixelShader9*       g_psTex = NULL;
    IDirect3DVertexDeclaration9* g_declTex = NULL;
    float                        g_width = 1280, g_height = 720;

    const char* VS =
        "float4 main( float4 pos : POSITION ) : POSITION { return pos; }\n";
    const char* PS =
        "float4 color : register( c0 );\n"
        "float4 main() : COLOR { return color; }\n";

    const char* VS_TEX =
        "struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "V main( V v ) { return v; }\n";
    const char* PS_TEX =
        "sampler2D tex : register( s0 );\n"
        "float4 tint : register( c0 );\n"
        "float4 main( float2 uv : TEXCOORD0 ) : COLOR { return tex2D( tex, uv ) * tint; }\n";

    const D3DVERTEXELEMENT9 ELEMENTS_TEX[] =
    {
        { 0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };

    void ToColor( DWORD color, float* c )
    {
        c[0] = ( ( color >> 16 ) & 255 ) / 255.0f;
        c[1] = ( ( color >> 8 ) & 255 ) / 255.0f;
        c[2] = ( color & 255 ) / 255.0f;
        c[3] = ( color >> 24 ) / 255.0f;
    }

    const D3DVERTEXELEMENT9 ELEMENTS[] =
    {
        { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        D3DDECL_END()
    };

    bool Compile( const char* src, const char* profile, ID3DXBuffer** code )
    {
        ID3DXBuffer* errors = NULL;
        HRESULT hr = D3DXCompileShader( src, (UINT)strlen( src ), NULL, NULL, "main", profile, 0, code,
                                        &errors, NULL );
        if( FAILED( hr ) )
            Log::Write( "Draw shader: %s", errors ? (const char*)errors->GetBufferPointer() : "?" );
        if( errors )
            errors->Release();
        return SUCCEEDED( hr );
    }
}

bool Draw::Startup( IDirect3DDevice9* device, int screenWidth, int screenHeight )
{
    g_device = device;
    g_width = (float)screenWidth;
    g_height = (float)screenHeight;

    ID3DXBuffer* code = NULL;
    if( !Compile( VS, "vs_3_0", &code ) )
        return false;
    device->CreateVertexShader( (const DWORD*)code->GetBufferPointer(), &g_vs );
    code->Release();
    if( !Compile( PS, "ps_3_0", &code ) )
        return false;
    device->CreatePixelShader( (const DWORD*)code->GetBufferPointer(), &g_ps );
    code->Release();
    device->CreateVertexDeclaration( ELEMENTS, &g_decl );

    if( !Compile( VS_TEX, "vs_3_0", &code ) )
        return false;
    device->CreateVertexShader( (const DWORD*)code->GetBufferPointer(), &g_vsTex );
    code->Release();
    if( !Compile( PS_TEX, "ps_3_0", &code ) )
        return false;
    device->CreatePixelShader( (const DWORD*)code->GetBufferPointer(), &g_psTex );
    code->Release();
    device->CreateVertexDeclaration( ELEMENTS_TEX, &g_declTex );
    return g_vs && g_ps && g_decl && g_vsTex && g_psTex && g_declTex;
}

void Draw::Image( IDirect3DTexture9* tex, float x0, float y0, float x1, float y1, DWORD tint )
{
    if( !g_psTex || !tex )
        return;
    float l = x0 / g_width * 2.0f - 1.0f, r = x1 / g_width * 2.0f - 1.0f;
    float t = 1.0f - y0 / g_height * 2.0f, b = 1.0f - y1 / g_height * 2.0f;
    float quad[4][6] = { { l, t, 0, 1, 0, 0 }, { r, t, 0, 1, 1, 0 }, { l, b, 0, 1, 0, 1 }, { r, b, 0, 1, 1, 1 } };
    float c[4];
    ToColor( tint, c );

    g_device->SetVertexDeclaration( g_declTex );
    g_device->SetVertexShader( g_vsTex );
    g_device->SetPixelShader( g_psTex );
    g_device->SetPixelShaderConstantF( 0, c, 1 );
    g_device->SetTexture( 0, tex );
    g_device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
    g_device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
    g_device->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
    g_device->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
    g_device->SetRenderState( D3DRS_ZENABLE, FALSE );
    g_device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    g_device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
    g_device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
    g_device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
    g_device->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, quad, sizeof( quad[0] ) );
    g_device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
    g_device->SetTexture( 0, NULL );
    g_device->SetVertexShader( NULL );
    g_device->SetPixelShader( NULL );
}

void Draw::Frame( float x0, float y0, float x1, float y1, float k, DWORD color )
{
    Rect( x0, y0, x1, y0 + k, color );
    Rect( x0, y1 - k, x1, y1, color );
    Rect( x0, y0 + k, x0 + k, y1 - k, color );
    Rect( x1 - k, y0 + k, x1, y1 - k, color );
}

void Draw::Rect( float x0, float y0, float x1, float y1, DWORD color )
{
    if( !g_ps )
        return;

    // Pixels to clip space.
    float l = x0 / g_width * 2.0f - 1.0f, r = x1 / g_width * 2.0f - 1.0f;
    float t = 1.0f - y0 / g_height * 2.0f, b = 1.0f - y1 / g_height * 2.0f;
    float quad[4][4] = { { l, t, 0, 1 }, { r, t, 0, 1 }, { l, b, 0, 1 }, { r, b, 0, 1 } };
    float c[4];
    ToColor( color, c );

    g_device->SetVertexDeclaration( g_decl );
    g_device->SetVertexShader( g_vs );
    g_device->SetPixelShader( g_ps );
    g_device->SetPixelShaderConstantF( 0, c, 1 );
    g_device->SetRenderState( D3DRS_ZENABLE, FALSE );
    g_device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    g_device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
    g_device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
    g_device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
    g_device->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, quad, sizeof( quad[0] ) );
    g_device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
    g_device->SetVertexShader( NULL );
    g_device->SetPixelShader( NULL );
}
