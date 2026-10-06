#pragma once
#include <string>
#include <vector>

// plex.tv account APIs: link-code sign-in and finding the user's servers.
namespace PlexTv
{
    // X-Plex-* identification headers sent on every request (plex.tv and servers).
    std::string ClientHeaders( const std::string& clientId );

    struct Pin
    {
        std::string id;
        std::string code;   // 4 characters, entered at plex.tv/link
    };

    bool CreatePin( const std::string& clientId, Pin& pin, std::string& error );

    enum PinState { PIN_WAITING, PIN_LINKED, PIN_FAILED };
    PinState CheckPin( const std::string& clientId, const Pin& pin, std::string& token, std::string& error );

    // false + invalid=true when plex.tv rejects the token (signed out / revoked).
    bool CheckToken( const std::string& clientId, const std::string& token, std::wstring& username,
                     bool& invalid, std::string& error );

    struct Connection
    {
        std::string uri;        // https://a-b-c-d.<hash>.plex.direct:port
        std::string address;    // plain IP
        int         port;
        bool        local;
        bool        relay;
    };

    struct Server
    {
        std::wstring            name;
        std::string             accessToken;
        bool                    owned;
        std::vector<Connection> connections;
    };

    bool GetServers( const std::string& clientId, const std::string& token,
                     std::vector<Server>& servers, std::string& error );

    // Plex Home: the profiles on this account.
    struct HomeUser
    {
        std::string  uuid;
        std::wstring title;
        std::string  thumb;       // avatar URL (https://plex.tv/...)
        bool         isProtected; // needs a PIN to switch to
        bool         admin;
    };

    bool HomeUsers( const std::string& clientId, const std::string& token, std::vector<HomeUser>& users,
                    std::string& error );

    // Switches to another Home profile; returns that profile's token.
    bool SwitchUser( const std::string& clientId, const std::string& token, const std::string& uuid,
                     const std::string& pin, std::string& newToken, std::string& error );
}
