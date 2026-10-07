#include "r_atlas.h"

#include <string.h>

#include <vector>

#include "HandmadeMath.h"
#include "epi.h"
#include "im_data.h"
#include "r_backend.h"
#include "r_image.h"
#include "r_state.h"
#include "r_things.h"
#include "stb_rect_pack.h"
#include "w_sprite.h"

static constexpr int kAtlasMaximumPageSize  = 4096;
static constexpr int kAtlasMaximumImageSize = 1024;
static constexpr int kAtlasGutter           = 1;

struct AtlasPage
{
    GLuint                  texture;
    int                     size;
    bool                    smooth;
    stbrp_context           context;
    std::vector<stbrp_node> nodes;
};

struct AtlasPending
{
    Image     *image;
    ImageData *data;
};

static std::vector<AtlasPage *> atlas_pages;
static std::vector<Image *>     atlas_images;

static int AtlasPageSize(void)
{
    return HMM_MIN(kAtlasMaximumPageSize, render_backend->GetMaxTextureSize());
}

static int CreateAtlasPage(bool smooth)
{
    AtlasPage *page = new AtlasPage;

    page->size   = AtlasPageSize();
    page->smooth = smooth;

    page->nodes.resize((size_t)page->size);

    stbrp_init_target(&page->context, page->size, page->size, page->nodes.data(), page->size);

    render_state->GenTextures(1, &page->texture);
    render_state->BindTexture(page->texture);

    render_state->TextureWrapS(GL_CLAMP_TO_EDGE);
    render_state->TextureWrapT(GL_CLAMP_TO_EDGE);

    texture_clamp_s.emplace(page->texture, GL_CLAMP_TO_EDGE);
    texture_clamp_t.emplace(page->texture, GL_CLAMP_TO_EDGE);

    render_state->TextureMagFilter(smooth ? GL_LINEAR : GL_NEAREST);
    render_state->TextureMinFilter(smooth ? GL_LINEAR : GL_NEAREST);

    render_state->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, page->size, page->size, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             nullptr);

    render_state->FinishTextures(1, &page->texture);

    atlas_pages.push_back(page);

    return (int)atlas_pages.size() - 1;
}

static void UploadAtlasImage(AtlasPage *page, const stbrp_rect &rect, const ImageData *data)
{
    int width  = rect.w;
    int height = rect.h;

    std::vector<uint8_t> block((size_t)width * (size_t)height * 4);

    for (int y = 0; y < height; y++)
    {
        int source_y = HMM_Clamp(0, y - kAtlasGutter, data->height_ - 1);

        for (int x = 0; x < width; x++)
        {
            int source_x = HMM_Clamp(0, x - kAtlasGutter, data->width_ - 1);

            memcpy(&block[((size_t)y * (size_t)width + (size_t)x) * 4], data->PixelAt(source_x, source_y), 4);
        }
    }

    render_state->PixelStorei(GL_UNPACK_ALIGNMENT, 1);

    render_state->BindTexture(page->texture);

    render_state->TexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                                block.data());
}

static bool PackOnPage(int page_index, std::vector<AtlasPending> &pending, std::vector<stbrp_rect> &rects)
{
    AtlasPage *page = atlas_pages[(size_t)page_index];

    for (size_t i = 0; i < pending.size(); i++)
    {
        rects[i].id         = (int)i;
        rects[i].w          = pending[i].data->width_ + kAtlasGutter * 2;
        rects[i].h          = pending[i].data->height_ + kAtlasGutter * 2;
        rects[i].was_packed = 0;
    }

    return stbrp_pack_rects(&page->context, rects.data(), (int)rects.size()) != 0;
}

static void CommitPackedImages(int page_index, std::vector<AtlasPending> &pending, std::vector<stbrp_rect> &rects)
{
    AtlasPage *page = atlas_pages[(size_t)page_index];

    float size = (float)page->size;

    for (size_t i = 0; i < rects.size(); i++)
    {
        if (!rects[i].was_packed)
            continue;

        AtlasPending &entry = pending[(size_t)rects[i].id];

        UploadAtlasImage(page, rects[i], entry.data);

        Image *rim = entry.image;

        rim->atlas_page_         = page_index;
        rim->atlas_rectangle_[0] = (rects[i].x + kAtlasGutter) / size;
        rim->atlas_rectangle_[1] = (rects[i].y + kAtlasGutter) / size;
        rim->atlas_rectangle_[2] = (rects[i].x + kAtlasGutter + entry.data->width_) / size;
        rim->atlas_rectangle_[3] = (rects[i].y + kAtlasGutter + entry.data->height_) / size;
    }
}

static void PackAtlasGroup(std::vector<AtlasPending> &pending, bool smooth)
{
    if (pending.empty())
        return;

    std::vector<stbrp_rect> rects(pending.size());

    int page_index = -1;

    for (int i = (int)atlas_pages.size() - 1; i >= 0; i--)
    {
        if (atlas_pages[(size_t)i]->smooth == smooth)
        {
            page_index = i;
            break;
        }
    }

    if (page_index >= 0 && PackOnPage(page_index, pending, rects))
    {
        CommitPackedImages(page_index, pending, rects);
        return;
    }

    page_index = CreateAtlasPage(smooth);

    if (PackOnPage(page_index, pending, rects))
    {
        CommitPackedImages(page_index, pending, rects);
        return;
    }

    for (;;)
    {
        CommitPackedImages(page_index, pending, rects);

        std::vector<AtlasPending> remaining;

        for (size_t i = 0; i < rects.size(); i++)
        {
            if (!rects[i].was_packed)
                remaining.push_back(pending[(size_t)rects[i].id]);
        }

        if (remaining.empty())
            return;

        pending.swap(remaining);
        rects.resize(pending.size());

        page_index = CreateAtlasPage(smooth);

        PackOnPage(page_index, pending, rects);
    }
}

static void AddAtlasImages(const std::vector<const Image *> &images)
{
    std::vector<AtlasPending> groups[2];

    for (size_t i = 0; i < images.size(); i++)
    {
        Image *rim = (Image *)images[i];

        if (rim->atlas_checked_)
            continue;

        rim->atlas_checked_ = true;
        rim->atlas_page_    = -1;

        atlas_images.push_back(rim);

        bool smooth = false;

        ImageData *data = LoadAtlasImageData(rim, &smooth);

        if (!data)
            continue;

        if (data->width_ > kAtlasMaximumImageSize || data->height_ > kAtlasMaximumImageSize)
        {
            delete data;
            continue;
        }

        AtlasPending entry;

        entry.image = rim;
        entry.data  = data;

        groups[smooth ? 1 : 0].push_back(entry);
    }

    for (int smooth = 0; smooth < 2; smooth++)
    {
        PackAtlasGroup(groups[smooth], smooth != 0);

        for (size_t i = 0; i < groups[smooth].size(); i++)
            delete groups[smooth][i].data;
    }
}

void AtlasPrecacheSprite(int sprite)
{
    std::vector<const Image *> images;

    GetSpriteImages(sprite, images);

    AddAtlasImages(images);
}

bool AtlasSpriteRegion(int sprite, const Image *image, AtlasRegion *region)
{
    if (!image->atlas_checked_)
    {
        if (sprite > 0)
            AtlasPrecacheSprite(sprite);

        if (!image->atlas_checked_)
        {
            std::vector<const Image *> single(1, image);

            AddAtlasImages(single);
        }
    }

    if (image->atlas_page_ < 0)
        return false;

    region->texture = atlas_pages[(size_t)image->atlas_page_]->texture;

    for (int i = 0; i < 4; i++)
        region->rectangle[i] = image->atlas_rectangle_[i];

    return true;
}

bool AtlasImageRegion(const Image *image, AtlasRegion *region)
{
    return AtlasSpriteRegion(0, image, region);
}

void AtlasClear(void)
{
    ResidentThingsInvalidate();

    for (size_t i = 0; i < atlas_images.size(); i++)
    {
        atlas_images[i]->atlas_checked_ = false;
        atlas_images[i]->atlas_page_    = -1;
    }

    atlas_images.clear();

    for (size_t i = 0; i < atlas_pages.size(); i++)
    {
        texture_clamp_s.erase(atlas_pages[i]->texture);
        texture_clamp_t.erase(atlas_pages[i]->texture);

        render_state->DeleteTexture(&atlas_pages[i]->texture);

        delete atlas_pages[i];
    }

    atlas_pages.clear();
}
