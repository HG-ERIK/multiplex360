#include <stdlib.h>
#include <string.h>
#include "Xml.h"

namespace
{
    void AppendUtf8( std::string& out, unsigned long cp )
    {
        if( cp < 0x80 )
            out += (char)cp;
        else if( cp < 0x800 )
        {
            out += (char)( 0xC0 | ( cp >> 6 ) );
            out += (char)( 0x80 | ( cp & 0x3F ) );
        }
        else if( cp < 0x10000 )
        {
            out += (char)( 0xE0 | ( cp >> 12 ) );
            out += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
            out += (char)( 0x80 | ( cp & 0x3F ) );
        }
        else
        {
            out += (char)( 0xF0 | ( cp >> 18 ) );
            out += (char)( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
            out += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
            out += (char)( 0x80 | ( cp & 0x3F ) );
        }
    }

    std::string DecodeEntities( const std::string& in )
    {
        std::string out;
        out.reserve( in.size() );
        for( size_t i = 0; i < in.size(); ++i )
        {
            if( in[i] != '&' )
            {
                out += in[i];
                continue;
            }
            size_t semi = in.find( ';', i );
            if( semi == std::string::npos || semi - i > 10 )
            {
                out += in[i];
                continue;
            }
            std::string ent = in.substr( i + 1, semi - i - 1 );
            if( ent == "amp" )       out += '&';
            else if( ent == "lt" )   out += '<';
            else if( ent == "gt" )   out += '>';
            else if( ent == "quot" ) out += '"';
            else if( ent == "apos" ) out += '\'';
            else if( !ent.empty() && ent[0] == '#' )
            {
                unsigned long cp = ( ent.size() > 1 && ( ent[1] == 'x' || ent[1] == 'X' ) )
                                   ? strtoul( ent.c_str() + 2, NULL, 16 )
                                   : strtoul( ent.c_str() + 1, NULL, 10 );
                AppendUtf8( out, cp );
            }
            else
            {
                out += in.substr( i, semi - i + 1 );
            }
            i = semi;
        }
        return out;
    }

    bool IsSpace( char c ) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

    // Parses attributes from the text between the tag name and '>' / '/>'.
    Xml::Attributes ParseAttributes( const std::string& xml, size_t pos, size_t end )
    {
        Xml::Attributes attrs;
        while( pos < end )
        {
            while( pos < end && IsSpace( xml[pos] ) )
                ++pos;
            size_t nameStart = pos;
            while( pos < end && xml[pos] != '=' && !IsSpace( xml[pos] ) && xml[pos] != '/' )
                ++pos;
            std::string name = xml.substr( nameStart, pos - nameStart );
            while( pos < end && xml[pos] != '=' )
                ++pos;
            if( pos >= end )
                break;
            ++pos;
            while( pos < end && IsSpace( xml[pos] ) )
                ++pos;
            if( pos >= end || ( xml[pos] != '"' && xml[pos] != '\'' ) )
                break;
            char quote = xml[pos++];
            size_t valEnd = xml.find( quote, pos );
            if( valEnd == std::string::npos || valEnd > end )
                break;
            if( !name.empty() )
                attrs[name] = DecodeEntities( xml.substr( pos, valEnd - pos ) );
            pos = valEnd + 1;
        }
        return attrs;
    }

    // Finds the end of a start tag, skipping '>' characters inside quoted values.
    size_t FindTagEnd( const std::string& xml, size_t pos )
    {
        char quote = 0;
        for( ; pos < xml.size(); ++pos )
        {
            char c = xml[pos];
            if( quote )
            {
                if( c == quote )
                    quote = 0;
            }
            else if( c == '"' || c == '\'' )
                quote = c;
            else if( c == '>' )
                return pos;
        }
        return std::string::npos;
    }
}

std::vector<Xml::Attributes> Xml::FindElements( const std::string& xml, const char* tag )
{
    std::vector<Attributes> result;
    std::string open = std::string( "<" ) + tag;
    size_t pos = 0;
    while( ( pos = xml.find( open, pos ) ) != std::string::npos )
    {
        size_t nameEnd = pos + open.size();
        if( nameEnd < xml.size() && !IsSpace( xml[nameEnd] ) && xml[nameEnd] != '>' && xml[nameEnd] != '/' )
        {
            pos = nameEnd;   // a longer tag name that merely starts with this one
            continue;
        }
        size_t end = FindTagEnd( xml, nameEnd );
        if( end == std::string::npos )
            break;
        result.push_back( ParseAttributes( xml, nameEnd, end ) );
        pos = end + 1;
    }
    return result;
}

Xml::Attributes Xml::FindFirst( const std::string& xml, const char* tag )
{
    std::vector<Attributes> all = FindElements( xml, tag );
    return all.empty() ? Attributes() : all[0];
}

std::wstring Xml::Utf8ToWide( const std::string& s )
{
    std::wstring out;
    out.reserve( s.size() );
    for( size_t i = 0; i < s.size(); )
    {
        unsigned char c = (unsigned char)s[i];
        unsigned long cp;
        int extra;
        if( c < 0x80 )                { cp = c;        extra = 0; }
        else if( ( c >> 5 ) == 0x6 )  { cp = c & 0x1F; extra = 1; }
        else if( ( c >> 4 ) == 0xE )  { cp = c & 0x0F; extra = 2; }
        else if( ( c >> 3 ) == 0x1E ) { cp = c & 0x07; extra = 3; }
        else                          { cp = '?';      extra = 0; }
        ++i;
        for( int k = 0; k < extra && i < s.size(); ++k, ++i )
            cp = ( cp << 6 ) | ( (unsigned char)s[i] & 0x3F );
        out += ( cp <= 0xFFFF ) ? (wchar_t)cp : L'?';
    }
    return out;
}
