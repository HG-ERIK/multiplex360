#include <stdlib.h>
#include <stdio.h>
#include "PlexTv.h"
#include "Http.h"
#include "Xml.h"
#include "Log.h"

namespace
{
    std::string Attr( const Xml::Attributes& a, const char* name )
    {
        Xml::Attributes::const_iterator it = a.find( name );
        return it == a.end() ? std::string() : it->second;
    }

    std::string HttpError( const Http::Response& r )
    {
        if( r.status == 0 )
            return r.error;
        char buf[64];
        sprintf_s( buf, "plex.tv replied HTTP %d", r.status );
        return buf;
    }
}

std::string PlexTv::ClientHeaders( const std::string& clientId )
{
    return "X-Plex-Product: Multiplex 360\r\n"
           "X-Plex-Version: 0.2\r\n"
           // "Xbox 360" makes the server apply its old built-in profile for the official 360 app,
           // which rejects our MKV transcode (HTTP 400); Generic takes the profile we send.
           "X-Plex-Platform: Generic\r\n"
           "X-Plex-Device: Xbox 360\r\n"
           "X-Plex-Device-Name: Xbox 360\r\n"
           "X-Plex-Client-Identifier: " + clientId + "\r\n";
}

bool PlexTv::CreatePin( const std::string& clientId, Pin& pin, std::string& error )
{
    Http::Response r = Http::Request( "POST", "https://plex.tv/api/v2/pins?strong=false",
                                      ClientHeaders( clientId ) );
    if( r.status != 201 && r.status != 200 )
    {
        error = HttpError( r );
        return false;
    }
    Xml::Attributes a = Xml::FindFirst( r.body, "pin" );
    pin.id = Attr( a, "id" );
    pin.code = Attr( a, "code" );
    if( pin.id.empty() || pin.code.empty() )
    {
        error = "plex.tv sent no link code";
        return false;
    }
    Log::Write( "Link code created (pin %s)", pin.id.c_str() );
    return true;
}

PlexTv::PinState PlexTv::CheckPin( const std::string& clientId, const Pin& pin, std::string& token,
                                   std::string& error )
{
    Http::Response r = Http::Get( "https://plex.tv/api/v2/pins/" + pin.id, ClientHeaders( clientId ) );
    if( r.status == 404 )
    {
        error = "The link code expired";
        return PIN_FAILED;
    }
    if( r.status != 200 )
    {
        // Network hiccups while waiting aren't fatal; keep polling.
        Log::Write( "Pin poll: %s", HttpError( r ).c_str() );
        return PIN_WAITING;
    }
    token = Attr( Xml::FindFirst( r.body, "pin" ), "authToken" );
    return token.empty() ? PIN_WAITING : PIN_LINKED;
}

bool PlexTv::CheckToken( const std::string& clientId, const std::string& token, std::wstring& username,
                         bool& invalid, std::string& error )
{
    invalid = false;
    Http::Response r = Http::Get( "https://plex.tv/api/v2/user",
                                  ClientHeaders( clientId ) + "X-Plex-Token: " + token + "\r\n" );
    if( r.status == 401 )
    {
        invalid = true;
        error = "Plex sign-in expired";
        return false;
    }
    if( r.status != 200 )
    {
        error = HttpError( r );
        return false;
    }
    Xml::Attributes user = Xml::FindFirst( r.body, "user" );
    username = Xml::Utf8ToWide( Attr( user, "username" ) );
    if( username.empty() )
        username = Xml::Utf8ToWide( Attr( user, "title" ) );
    return true;
}

bool PlexTv::GetServers( const std::string& clientId, const std::string& token,
                         std::vector<Server>& servers, std::string& error )
{
    Http::Response r = Http::Get( "https://clients.plex.tv/api/v2/resources?includeHttps=1&includeRelay=1",
                                  ClientHeaders( clientId ) + "X-Plex-Token: " + token + "\r\n" );
    if( r.status != 200 )
    {
        error = HttpError( r );
        return false;
    }

    // Each <resource> holds its own <connections>; split the reply per resource.
    const std::string& xml = r.body;
    size_t pos = 0;
    while( ( pos = xml.find( "<resource ", pos ) ) != std::string::npos )
    {
        size_t end = xml.find( "</resource>", pos );
        std::string chunk = xml.substr( pos, end == std::string::npos ? std::string::npos : end - pos );
        pos += 10;

        Xml::Attributes res = Xml::FindFirst( chunk, "resource" );
        if( Attr( res, "provides" ).find( "server" ) == std::string::npos )
            continue;

        Server server;
        server.name = Xml::Utf8ToWide( Attr( res, "name" ) );
        server.accessToken = Attr( res, "accessToken" );
        server.owned = Attr( res, "owned" ) == "1";

        std::vector<Xml::Attributes> conns = Xml::FindElements( chunk, "connection" );
        for( size_t i = 0; i < conns.size(); ++i )
        {
            if( Attr( conns[i], "IPv6" ) == "1" )
                continue;   // the 360 stack is IPv4 only
            Connection c;
            c.uri = Attr( conns[i], "uri" );
            c.address = Attr( conns[i], "address" );
            c.port = atoi( Attr( conns[i], "port" ).c_str() );
            c.local = Attr( conns[i], "local" ) == "1";
            c.relay = Attr( conns[i], "relay" ) == "1";
            server.connections.push_back( c );
        }
        Log::Write( "Server '%s': %u connections, owned=%d", Attr( res, "name" ).c_str(),
                    (unsigned)server.connections.size(), server.owned ? 1 : 0 );
        servers.push_back( server );
    }

    if( servers.empty() )
    {
        error = "No Plex servers on this account";
        return false;
    }
    return true;
}

bool PlexTv::HomeUsers( const std::string& clientId, const std::string& token, std::vector<HomeUser>& users,
                        std::string& error )
{
    Http::Response r = Http::Get( "https://plex.tv/api/v2/home/users",
                                  ClientHeaders( clientId ) + "X-Plex-Token: " + token + "\r\n" );
    if( r.status != 200 )
    {
        error = HttpError( r );
        return false;
    }
    std::vector<Xml::Attributes> list = Xml::FindElements( r.body, "user" );
    for( size_t i = 0; i < list.size(); ++i )
    {
        HomeUser u;
        u.uuid = Attr( list[i], "uuid" );
        u.title = Xml::Utf8ToWide( Attr( list[i], "title" ) );
        u.thumb = Attr( list[i], "thumb" );
        u.isProtected = Attr( list[i], "protected" ) == "1" || Attr( list[i], "protected" ) == "true";
        u.admin = Attr( list[i], "admin" ) == "1" || Attr( list[i], "admin" ) == "true";
        if( !u.uuid.empty() )
            users.push_back( u );
    }
    Log::Write( "Home users: %u", (unsigned)users.size() );
    return true;
}

bool PlexTv::SwitchUser( const std::string& clientId, const std::string& token, const std::string& uuid,
                         const std::string& pin, std::string& newToken, std::string& error )
{
    std::string url = "https://plex.tv/api/v2/home/users/" + uuid + "/switch";
    if( !pin.empty() )
        url += "?pin=" + Http::Escape( pin );
    Http::Response r = Http::Request( "POST", url, ClientHeaders( clientId ) + "X-Plex-Token: " + token + "\r\n" );
    if( r.status == 401 || r.status == 403 )
    {
        error = "Wrong PIN";
        return false;
    }
    if( r.status != 200 && r.status != 201 )
    {
        error = HttpError( r );
        return false;
    }
    newToken = Attr( Xml::FindFirst( r.body, "user" ), "authToken" );
    if( newToken.empty() )
    {
        error = "plex.tv sent no token for that profile";
        return false;
    }
    return true;
}
