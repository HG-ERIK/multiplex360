#pragma once

// Timestamped log to the debug output and game:\plexlog.txt.
namespace Log
{
    void Open( const char* path );
    void Write( const char* fmt, ... );

    // Also send each line over UDP (tools\loglisten.ps1). Call after the network is up.
    void SetRemote( const char* ip, int port );
}
