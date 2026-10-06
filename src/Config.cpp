#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include "Config.h"

namespace
{
    std::string Trim( const std::string& s )
    {
        size_t b = s.find_first_not_of( " \t\r\n" );
        if( b == std::string::npos )
            return std::string();
        size_t e = s.find_last_not_of( " \t\r\n" );
        return s.substr( b, e - b + 1 );
    }

    bool ReadIni( const char* path, std::map<std::string, std::string>& values )
    {
        FILE* f = fopen( path, "rb" );
        if( !f )
            return false;

        char line[1024];
        while( fgets( line, sizeof( line ), f ) )
        {
            std::string s = Trim( line );
            if( s.empty() || s[0] == '#' || s[0] == ';' )
                continue;
            size_t eq = s.find( '=' );
            if( eq != std::string::npos )
                values[Trim( s.substr( 0, eq ) )] = Trim( s.substr( eq + 1 ) );
        }
        fclose( f );
        return true;
    }
}

void Config::Load( const char* path )
{
    std::map<std::string, std::string> v;
    if( !ReadIni( path, v ) )
        return;
    if( !v["connection"].empty() )
        connection = v["connection"];
    serverName = v["servername"];
    autoplay = v["autoplay"];
    logTo = v["logto"];
    testSeek = atoi( v["testseek"].c_str() );
    testOsd = v["testosd"] == "1";
    testNav = v["testnav"];
    autoplayStart = atoi( v["autoplaystart"].c_str() );
    benchmark = atoi( v["benchmark"].c_str() );
    testCrash = atoi( v["testcrash"].c_str() );
    testHang = atoi( v["testhang"].c_str() );
    if( !v["threads"].empty() )
        threads = atoi( v["threads"].c_str() );
    if( !v["quality"].empty() )
        quality = atoi( v["quality"].c_str() );
    if( !v["kbps"].empty() )
        kbps = atoi( v["kbps"].c_str() );
    if( !v["directmax"].empty() )
        directMax = atoi( v["directmax"].c_str() );
}

bool AuthStore::Load( const char* path )
{
    std::map<std::string, std::string> v;
    if( !ReadIni( path, v ) )
        return false;
    clientId = v["clientid"];
    token = v["token"];
    return true;
}

bool AuthStore::Save( const char* path ) const
{
    FILE* f = fopen( path, "wb" );
    if( !f )
        return false;
    fprintf( f, "# Plex sign-in for this console. Delete this file to sign out.\r\n" );
    fprintf( f, "clientid=%s\r\ntoken=%s\r\n", clientId.c_str(), token.c_str() );
    fclose( f );
    return true;
}
