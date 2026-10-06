#pragma once
#include <string>
#include <vector>
#include <map>

// Minimal XML for Plex replies: element attributes by tag name.
namespace Xml
{
    typedef std::map<std::string, std::string> Attributes;

    std::vector<Attributes> FindElements( const std::string& xml, const char* tag );

    // Attributes of the first element with this tag (empty map if none).
    Attributes FindFirst( const std::string& xml, const char* tag );

    std::wstring Utf8ToWide( const std::string& s );
}
