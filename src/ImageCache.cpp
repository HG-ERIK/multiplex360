#include <xtl.h>
#include <d3dx9.h>
#include <stdio.h>
#include "ImageCache.h"
#include "Http.h"
#include "Log.h"

namespace
{
    const size_t MAX_IMAGES = 180;
    const int    TEXTURES_PER_FRAME = 3;   // JPEG decode happens on the render thread

    // Downloads run on hardware threads 1 and 3 (free while browsing).
    const DWORD IMAGE_CPUS[2] = { 1, 3 };

    struct Download
    {
        std::string key;
        std::string body;
        int         generation;
        bool        ok;
    };
}

ImageCache::ImageCache() : m_device( NULL ), m_bytes( 0 ), m_generation( 0 )
{
}

ImageCache::~ImageCache()
{
    Clear();
}

void ImageCache::Startup( IDirect3DDevice9* device )
{
    m_device = device;
    m_tasks.Start( IMAGE_CPUS, 2 );
}

void ImageCache::SetServer( const std::string& base, const std::string& headers )
{
    if( base != m_base )
        Clear();
    m_base = base;
    m_headers = headers;
}

IDirect3DTexture9* ImageCache::Get( const std::string& thumb, int w, int h )
{
    if( thumb.empty() || m_base.empty() )
        return NULL;

    char size[48];
    sprintf_s( size, "|%dx%d", w, h );
    std::string key = thumb + size;

    std::map<std::string, Entry>::iterator it = m_entries.find( key );
    if( it != m_entries.end() )
    {
        it->second.lastUsed = GetTickCount();
        return it->second.texture;
    }

    Entry e;
    e.texture = NULL;
    e.lastUsed = GetTickCount();
    e.loading = true;
    e.failed = false;
    e.bytes = 0;
    m_entries[key] = e;

    char query[96];
    sprintf_s( query, "&width=%d&height=%d&minSize=1&upscale=1", w, h );
    std::string url = m_base + "/photo/:/transcode?url=" + Http::Escape( thumb ) + query;

    Download* d = new Download;
    d->key = key;
    d->generation = m_generation;
    d->ok = false;
    std::string headers = m_headers;
    ImageCache* self = this;
    m_tasks.Post(
        [d, url, headers]()
        {
            Http::Response r = Http::Get( url, headers, 15000 );
            d->ok = r.status == 200 && !r.body.empty();
            d->body.swap( r.body );
        },
        [d, self]()
        {
            std::map<std::string, Entry>::iterator e = self->m_entries.find( d->key );
            if( e != self->m_entries.end() && d->generation == self->m_generation )
            {
                e->second.loading = false;
                IDirect3DTexture9* tex = NULL;
                if( d->ok &&
                    SUCCEEDED( D3DXCreateTextureFromFileInMemoryEx( self->m_device, d->body.data(),
                                   (UINT)d->body.size(), D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1, 0,
                                   D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, D3DX_FILTER_NONE, D3DX_FILTER_NONE,
                                   0, NULL, NULL, &tex ) ) )
                {
                    D3DSURFACE_DESC desc;
                    tex->GetLevelDesc( 0, &desc );
                    e->second.texture = tex;
                    e->second.bytes = desc.Width * desc.Height * 4;
                    self->m_bytes += e->second.bytes;
                }
                else
                    e->second.failed = true;
                self->Evict();
            }
            delete d;
        } );
    return NULL;
}

void ImageCache::Pump()
{
    m_tasks.Pump( TEXTURES_PER_FRAME );
}

void ImageCache::CancelPending()
{
    m_tasks.Cancel( 0 );
    // Entries still marked loading would never complete; forget them so they re-request.
    for( std::map<std::string, Entry>::iterator it = m_entries.begin(); it != m_entries.end(); )
    {
        if( it->second.loading )
            it = m_entries.erase( it );
        else
            ++it;
    }
    ++m_generation;
}

void ImageCache::Evict()
{
    while( m_entries.size() > MAX_IMAGES )
    {
        std::map<std::string, Entry>::iterator oldest = m_entries.end();
        for( std::map<std::string, Entry>::iterator it = m_entries.begin(); it != m_entries.end(); ++it )
            if( !it->second.loading && ( oldest == m_entries.end() || it->second.lastUsed < oldest->second.lastUsed ) )
                oldest = it;
        if( oldest == m_entries.end() )
            return;
        if( oldest->second.texture )
        {
            m_bytes -= oldest->second.bytes;
            // The GPU may still be drawing a recent frame with it; on the 360 freeing a texture
            // the GPU is reading crashes, so wait for it first.
            oldest->second.texture->BlockUntilNotBusy();
            oldest->second.texture->Release();
        }
        m_entries.erase( oldest );
    }
}

void ImageCache::Clear()
{
    m_tasks.Cancel( 0 );
    for( std::map<std::string, Entry>::iterator it = m_entries.begin(); it != m_entries.end(); ++it )
        if( it->second.texture )
        {
            it->second.texture->BlockUntilNotBusy();
            it->second.texture->Release();
        }
    m_entries.clear();
    m_bytes = 0;
    ++m_generation;
}
