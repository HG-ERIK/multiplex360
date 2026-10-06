#pragma once
#include <xtl.h>

class App
{
public:
    App();
    virtual ~App() {}

    void Run();

    IDirect3DDevice9*     m_pd3dDevice;
    D3DPRESENT_PARAMETERS m_d3dpp;      // set the size / present interval before Run()

protected:
    virtual HRESULT Initialize() = 0;
    virtual HRESULT Update() = 0;
    virtual HRESULT Render() = 0;
};

// The title-safe area: 10% in from each edge, where TVs never crop.
D3DRECT SafeArea();

// Background filled with a vertical gradient (top colour to bottom colour, ARGB).
void FillGradient( DWORD top, DWORD bottom );

// All controllers merged; left stick outside the dead zone, scaled to -1..1.
struct Pad
{
    WORD  wButtons;
    WORD  wPressedButtons;
    SHORT sThumbLX, sThumbLY;
    float fX1, fY1;
};
Pad* ReadPads();
