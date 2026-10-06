#pragma once
#include <xtl.h>
#include <string>
#include <map>
#include "Tasks.h"

// Posters and artwork. Get() returns NULL and starts a download if not loaded yet; LRU-bounded.
class ImageCache
{
public:
    ImageCache();
    ~ImageCache();

    void Startup( IDirect3DDevice9* device );

    // Base URL of the server's photo transcoder and the headers (token) it needs.
    void SetServer( const std::string& base, const std::string& headers );

    // thumb: the item's thumb/art path (e.g. /library/metadata/12/thumb/1690000000).
    // The server scales it to w x h.
    IDirect3DTexture9* Get( const std::string& thumb, int w, int h );

    // Render thread, once per frame: turns finished downloads into textures.
    void Pump();

    // Drops queued downloads (e.g. when leaving a screen). Loaded images stay cached.
    void CancelPending();

    void Clear();

    size_t Count() const { return m_entries.size(); }
    size_t Bytes() const { return m_bytes; }

private:
    struct Entry
    {
        IDirect3DTexture9* texture;
        DWORD              lastUsed;
        bool               loading;
        bool               failed;
        size_t             bytes;
    };

    void Evict();

    IDirect3DDevice9*            m_device;
    TaskQueue                    m_tasks;
    std::string                  m_base;
    std::string                  m_headers;
    std::map<std::string, Entry> m_entries;
    size_t                       m_bytes;
    int                          m_generation;   // bumps on CancelPending/Clear
};
