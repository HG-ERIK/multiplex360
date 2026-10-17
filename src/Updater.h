#pragma once
#include <xtl.h>
#include <string>

class Font;

// This build's version, compared with the newest release on GitHub (HG-ERIK/multiplex360).
#define MULTIPLEX_VERSION "0.4"

// Updates from the newest GitHub release: at start it looks for a newer version in the background; if
// there is one it asks (with the release notes), then downloads that release's default.xex next to the
// running one (default.new), checks it, swaps it in (the old one stays as default.old) and starts it.
// plex.ini, settings.ini and the sign-in are other files and stay as they are.
namespace Updater
{
    void Start( const std::string& from = std::string() );   // after the network is up; once. from: testing URL

    // While it shows something (the question, the download, an error) it takes the controller:
    bool Active();
    void Input( WORD pressed );         // A: update, B: later
    void Render( Font& font, Font& title, float width );
}
