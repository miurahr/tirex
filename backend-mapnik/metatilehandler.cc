/*
 * Tirex Tile Rendering System
 *
 * Mapnik rendering backend
 *
 * Originally written by Jochen Topf & Frederik Ramm.
 *
 */

#include "metatilehandler.h"
#include "renderrequest.h"
#include "renderresponse.h"

#include "sys/time.h"
#include <boost/filesystem.hpp>
#include <mapnik/image_util.hpp>
#include <limits.h>
#include <iostream>
#include <fstream>

#include <mapnik/version.hpp>
#include <mapnik/map.hpp>
#include <mapnik/datasource_cache.hpp>
#include <mapnik/font_engine_freetype.hpp>
#include <mapnik/agg_renderer.hpp>
#include <mapnik/image_util.hpp>
#include <mapnik/load_map.hpp>
#include <mapnik/box2d.hpp>

#if MAPNIK_VERSION >= 300000
# include <mapnik/datasource.hpp>
# include <mapnik/projection.hpp>
#endif

#define MERCATOR_WIDTH 40075016.685578488
#define MERCATOR_OFFSET 20037508.342789244

MetatileHandler::MetatileHandler(const std::string& tiledir, const std::string& stylefile, unsigned int tilesize, double scalefactor, int buffersize, unsigned int mtrowcol) :
    mTileWidth(tilesize),
    mTileHeight(tilesize),
    mMetaTileRows(mtrowcol),
    mMetaTileColumns(mtrowcol),
    mBufferSize(buffersize),
    mScaleFactor(scalefactor),
    mTileDir(tiledir)
{
    load_map(mMap, stylefile);

    fourpow[0] = 1;
    twopow[0] = 1;
    for (unsigned int i = 1; i < MAXZOOM; i++)
    {
        fourpow[i] = 4 * fourpow[i-1];
        twopow[i] = 2 * twopow[i-1];
    }
}

MetatileHandler::~MetatileHandler()
{
}

const NetworkResponse *MetatileHandler::handleRequest(const NetworkRequest *request) 
{
    debug(">> MetatileHandler::handleRequest");
    timeval start, end;
    gettimeofday(&start, NULL);

    int x = request->getParam("x", -1);
    int y = request->getParam("y", -1);
    int z = request->getParam("z", -1);

    if (x % mMetaTileColumns)
    {
        error("given value for 'x' (%d) is not divisible by %d", x, mMetaTileColumns);
        return NetworkResponse::makeErrorResponse(request, "invalid value for x");
    }

    if (y % mMetaTileRows)
    {
        error("given value for 'y' (%d) is not divisible by %d", y, mMetaTileRows);
        return NetworkResponse::makeErrorResponse(request, "invalid value for y");
    }

    unsigned int mtc = mMetaTileColumns;
    if (mtc > fourpow[z]) mtc = fourpow[z];
    unsigned int mtr = mMetaTileRows;
    if (mtr > fourpow[z]) mtr = fourpow[z];

    RenderRequest rr;

    // compute render extent in epsg:3857 which the database is likely to use.
    // note that if the database should use something different, this will be
    // taken care of later.

    rr.west = x * MERCATOR_WIDTH / twopow[z] - MERCATOR_OFFSET;
    rr.east = (x + mtc) * MERCATOR_WIDTH / twopow[z] - MERCATOR_OFFSET;
    rr.north = (twopow[z] - y) * MERCATOR_WIDTH / twopow[z] - MERCATOR_OFFSET;
    rr.south = (twopow[z] - y - mtr) * MERCATOR_WIDTH / twopow[z] - MERCATOR_OFFSET;
    rr.scale_factor = mScaleFactor;
    rr.buffer_size = mBufferSize;

    // we specify the bbox in epsg:3857, and we also want our image returned
    // in this projection. 
    rr.bbox_srs = 3857;
    rr.srs = 3857;

    rr.width = mTileWidth * mtc;
    rr.height = mTileHeight * mtr;

    std::string map = request->getParam("map", "default");

    updateStatus("rendering z=%d x=%d y=%d map=%s", z, x, y, map.c_str());
    const RenderResponse *rrs = render(&rr);
    updateStatus("idle");

    NetworkResponse *resp;

    if (!rrs)
    {
        return NetworkResponse::makeErrorResponse(request, "renderer internal error");
    }
    else
    {
        meta_layout m;
        entry offsets[mMetaTileRows * mMetaTileColumns];
        std::vector<std::string> rawpng(mMetaTileRows * mMetaTileColumns);
        memset(&m, 0, sizeof(m));
        memset(&offsets, 0, sizeof(offsets));

        // it seems that mod_tile expects us to always put the theoretical
        // number of tiles in this meta tile, not the real number (in standard
        // setup, only zoom levels 3+ will have 64 tiles, 0-2 have less)
        // m.count = mtr * mtc;
        m.count = mMetaTileRows * mMetaTileColumns;
        memcpy(m.magic, "META", 4);
        m.x = x;
        m.y = y;
        m.z = z;
        size_t offset = sizeof(m) + sizeof(offsets);
        int index = 0;

        char metafilename[PATH_MAX];
        xyz_to_meta(metafilename, PATH_MAX, mTileDir.c_str(), x, y, z);
        if (!mkdirp(mTileDir.c_str(), x, y, z))
        {
            return NetworkResponse::makeErrorResponse(request, "renderer internal error");
        }

        char tmpfilename[PATH_MAX];
        snprintf(tmpfilename, PATH_MAX, "%s.%d.tmp", metafilename, getpid());
        std::ofstream outfile(tmpfilename, std::ios::out | std::ios::binary | std::ios::trunc);
        outfile.write(reinterpret_cast<const char*>(&m), sizeof(m));

        for (unsigned int col = 0; col < mMetaTileColumns; col++)
        {
            for (unsigned int row = 0; row < mMetaTileRows; row++)
            {
                if ((col < mtc) && (row < mtr))
                {
                    mapnik::image_view<mapnik::image_data_32> view(col * mTileWidth, 
                        row * mTileHeight, mTileWidth, mTileHeight, rrs->image->data());
                    rawpng[index] = mapnik::save_to_string(view, "png256");
                    offsets[index].offset = offset;
                    offset += offsets[index].size = rawpng[index].length();
                }
                else
                {
                    offsets[index].offset = 0;
                    offsets[index].size = 0;
                }
                index++;
            }
        }

        outfile.write(reinterpret_cast<const char*>(&offsets), sizeof(offsets));

        for (int i=0; i < index; i++)
        {
            outfile.write(rawpng[i].data(), rawpng[i].size());
        }

        outfile.close();
        delete rrs;
        rename(tmpfilename, metafilename);
        debug("created %s", metafilename);

        resp = new NetworkResponse(request);
        resp->setParam("map", map);
        resp->setParam("result", "ok");
        resp->setParam("x", x);
        resp->setParam("y", y);
        resp->setParam("z", z);
        resp->setParam("metatile", metafilename);
        gettimeofday(&end, NULL);
        char buffer[20];
        snprintf(buffer, 20, "%ld", (end.tv_sec-start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000);
        resp->setParam("render_time", buffer);
    }
    debug("<< MetatileHandler::handleRequest");
    return resp;
}

void MetatileHandler::xyz_to_meta(char *path, size_t len, const char *tile_dir, int x, int y, int z) const
{
    unsigned char i, hash[5];

    for (i=0; i<5; i++) {
        hash[i] = ((x & 0x0f) << 4) | (y & 0x0f);
        x >>= 4;
        y >>= 4;
    }
    snprintf(path, len, "%s/%d/%u/%u/%u/%u/%u.meta", tile_dir, z, hash[4], hash[3], hash[2], hash[1], hash[0]);
    return;
}

bool MetatileHandler::mkdirp(const char *tile_dir, int x, int y, int z) const
{
    unsigned char i, hash[5];
    char path[PATH_MAX];

    for (i=0; i<5; i++) {
        hash[i] = ((x & 0x0f) << 4) | (y & 0x0f);
        x >>= 4;
        y >>= 4;
    }
    snprintf(path, PATH_MAX, "%s/%d/%u/%u/%u/%u", tile_dir, z, hash[4], hash[3], hash[2], hash[1]);
    try
    {
        boost::filesystem::create_directories(path);
    }
    catch(std::exception const& ex)
    {
        error("cannot create directory %s: %s", path, ex.what());
        return false;
    }
    return true;
}

const RenderResponse *MetatileHandler::render(const RenderRequest *rr) 
{
    debug(">> MetatileHandler::render");
    char init[255];

    sprintf(init, "+init=epsg:%d", rr->srs);
    // commented out - rely on proper SRS specification in map.xml
    // mMap.set_srs(init);

    double west = rr->west;
    double south = rr->south;
    double east = rr->east;
    double north = rr->north;

    if (rr->srs != rr->bbox_srs)
    {
        mapnik::projection p1(init);
        sprintf(init, "+init=epsg:%d", rr->bbox_srs);
        mapnik::projection p0(init);
        mapnik::proj_transform pt(p0, p1);
        double z = 0.0;
        pt.forward(west, south, z);
        z = 0.0;
        pt.forward(east, north, z);

        debug("rendering for %f,%f - %f,%f in SRS %d (projected from %f,%f - %f,%f in SRS %d) to %dx%dpx",
            west, south, east, north, rr->srs, rr->west, rr->south, rr->east, rr->north, rr->bbox_srs, rr->width, rr->height);
    }
    else
    {
        debug("rendering area %f,%f - %f,%f in SRS %d to %dx%d px",
            west, south, east, north, rr->srs, rr->width, rr->height);
    }

    mapnik::box2d<double> bbox(west, south, east, north);
    mMap.resize(rr->width, rr->height);
    mMap.zoom_to_box(bbox);
    if (rr->buffer_size > -1)
    {
        mMap.set_buffer_size(rr->buffer_size);
    }
    else if (mMap.buffer_size() < 128)
    {
        mMap.set_buffer_size(128);
    }

    debug("width: %d, height:%d", rr->width, rr->height);
    RenderResponse *resp = new RenderResponse();
    resp->image = new mapnik::image_32(rr->width, rr->height);
    mapnik::agg_renderer<mapnik::image_32> renderer(mMap, *(resp->image), rr->scale_factor, 0u, 0u);
    try
    {
        renderer.apply();
    }
    catch (mapnik::datasource_exception const& dex)
    {
        delete resp;
        resp = NULL;
        error("Mapnik datasource exception: %s", dex.what());
    }
    catch (std::exception const& ex)
    {
        delete resp;
        resp = NULL;
        error("Mapnik config error: %s", ex.what());
    }
    debug("<< MetatileHandler::render");

    return resp;
}

