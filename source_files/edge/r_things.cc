//----------------------------------------------------------------------------
//  EDGE OpenGL Rendering (Things)
//----------------------------------------------------------------------------
//
//  Copyright (c) 1999-2024 The EDGE Team.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//----------------------------------------------------------------------------
//
//  Based on the DOOM source code, released by Id Software under the
//  following copyright:
//
//    Copyright (C) 1993-1996 by id Software, Inc.
//
//----------------------------------------------------------------------------

#include "edge_profiling.h"
#include <limits.h>
#include <math.h>
#include <string.h>

#include <map>
#include <unordered_map>
#include <vector>

#include "epi_math.h"
#include "r_lightgrid.h"
#include "coal.h"
#include "dm_defs.h"
#include "dm_state.h"
#include "epi_color.h"
#include "epi_file.h"
#include "epi_filesystem.h"
#include "epi_str_compare.h"
#include "epi_str_util.h"
#include "g_game.h" //current_map
#include "i_defs_gl.h"
#include "im_data.h"
#include "im_funcs.h"
#include "m_misc.h" // !!!! model test
#include "n_network.h"
#include "p_local.h"
#include "r_atlas.h"
#include "r_colormap.h"
#include "r_defs.h"
#include "r_draw.h"
#include "r_effects.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_md2.h"
#include "r_mdl.h"
#include "r_mirror.h"
#include "r_misc.h"
#include "r_modes.h"
#include "r_render.h"
#include "r_shader.h"
#include "r_static.h"
#include "r_texgl.h"
#include "r_things.h"
#include "r_units.h"
#include "script/compat/lua_compat.h"
#include "vm_coal.h"
#include "w_model.h"
#include "w_sprite.h"

extern coal::VM *ui_vm;
extern double    COALGetFloat(coal::VM *vm, const char *mod_name, const char *var_name);
extern bool erraticism_active;

EDGE_DEFINE_CONSOLE_VARIABLE(crosshair_image, "None", kConsoleVariableFlagArchive)
EDGE_DEFINE_CONSOLE_VARIABLE(crosshair_color, "0",
                             kConsoleVariableFlagArchive) // 0 .. 7
EDGE_DEFINE_CONSOLE_VARIABLE(crosshair_size, "16.0",
                             kConsoleVariableFlagArchive) // pixels on a 320x200 screen
EDGE_DEFINE_CONSOLE_VARIABLE(crosshair_brightness, "1.0",
                             kConsoleVariableFlagArchive) // 1.0 is normal

float sprite_skew;

extern MapObject *view_camera_map_object;
extern float      widescreen_view_width_multiplier;

// The minimum distance between player and a visible sprite.
static constexpr float kMinimumSpriteDistance = 4.0f;

std::map<std::string, unsigned int> available_crosshairs;

void CollectCrosshairs()
{
    // Add the default (none) option first so it takes precedence
    // over an overlay that might somehow have the same file stem
    available_crosshairs.emplace("None", 0);

    // Check for overlays
    std::vector<epi::DirectoryEntry> chd;
    std::string                      crosshair_dir = epi::PathAppend(home_directory, "crosshairs");

    // Create home directory overlays folder if it doesn't aleady exist
    if (!epi::IsDirectory(crosshair_dir))
        epi::MakeDirectory(crosshair_dir);

    chd.clear();

    if (!ReadDirectory(chd, crosshair_dir, "*.png"))
    {
        LogWarning("CollectCrosshairs: Failed to read '%s' directory!\n", crosshair_dir.c_str());
    }
    else
    {
        for (size_t i = 0; i < chd.size(); i++)
        {
            if (!chd[i].is_dir)
            {
                std::string filename = epi::GetStem(chd[i].name);
                if (!available_crosshairs.count(filename))
                {
                    epi::File *ovimg_file = epi::FileOpen(chd[i].name, epi::kFileAccessRead | epi::kFileAccessBinary);
                    if (ovimg_file)
                    {
                        ImageData *ovimg_data = LoadImageData(ovimg_file);
                        if (ovimg_data)
                        {
                            unsigned int tex_id = UploadTexture(ovimg_data, kUploadSmooth, (1 << 30));
                            available_crosshairs.emplace(filename, tex_id);
                            delete ovimg_data;
                        }
                        delete ovimg_file;
                    }
                }
            }
        }
    }
    chd.clear();
    if (!ReadDirectory(chd, crosshair_dir, "*.tga"))
    {
        LogWarning("CollectCrosshairs: Failed to read '%s' directory!\n", crosshair_dir.c_str());
    }
    else
    {
        for (size_t i = 0; i < chd.size(); i++)
        {
            if (!chd[i].is_dir)
            {
                std::string filename = epi::GetStem(chd[i].name);
                if (!available_crosshairs.count(filename))
                {
                    epi::File *ovimg_file = epi::FileOpen(chd[i].name, epi::kFileAccessRead | epi::kFileAccessBinary);
                    if (ovimg_file)
                    {
                        ImageData *ovimg_data = LoadImageData(ovimg_file);
                        if (ovimg_data)
                        {
                            unsigned int tex_id = UploadTexture(ovimg_data, kUploadSmooth, (1 << 30));
                            available_crosshairs.emplace(filename, tex_id);
                            delete ovimg_data;
                        }
                        delete ovimg_file;
                    }
                }
            }
        }
    }

    if (home_directory != game_directory)
    {
        chd.clear();

        // Read the program directory, but only add names we haven't encountered yet
        crosshair_dir = epi::PathAppend(game_directory, "crosshairs");

        if (!ReadDirectory(chd, crosshair_dir, "*.png"))
        {
            LogWarning("CollectCrosshairs: Failed to read '%s' directory!\n", crosshair_dir.c_str());
        }
        else
        {
            for (size_t i = 0; i < chd.size(); i++)
            {
                if (!chd[i].is_dir)
                {
                    std::string filename = epi::GetStem(chd[i].name);
                    if (!available_crosshairs.count(filename))
                    {
                        epi::File *ovimg_file =
                            epi::FileOpen(chd[i].name, epi::kFileAccessRead | epi::kFileAccessBinary);
                        if (ovimg_file)
                        {
                            ImageData *ovimg_data = LoadImageData(ovimg_file);
                            if (ovimg_data)
                            {
                                unsigned int tex_id = UploadTexture(ovimg_data, kUploadSmooth, (1 << 30));
                                available_crosshairs.emplace(filename, tex_id);
                                delete ovimg_data;
                            }
                            delete ovimg_file;
                        }
                    }
                }
            }
        }
        chd.clear();
        if (!ReadDirectory(chd, crosshair_dir, "*.tga"))
        {
            LogWarning("CollectCrosshairs: Failed to read '%s' directory!\n", crosshair_dir.c_str());
        }
        else
        {
            for (size_t i = 0; i < chd.size(); i++)
            {
                if (!chd[i].is_dir)
                {
                    std::string filename = epi::GetStem(chd[i].name);
                    if (!available_crosshairs.count(filename))
                    {
                        epi::File *ovimg_file =
                            epi::FileOpen(chd[i].name, epi::kFileAccessRead | epi::kFileAccessBinary);
                        if (ovimg_file)
                        {
                            ImageData *ovimg_data = LoadImageData(ovimg_file);
                            if (ovimg_data)
                            {
                                unsigned int tex_id = UploadTexture(ovimg_data, kUploadSmooth, (1 << 30));
                                available_crosshairs.emplace(filename, tex_id);
                                delete ovimg_data;
                            }
                            delete ovimg_file;
                        }
                    }
                }
            }
        }
    }

    // Check for previously saved overlay CVAR; revert if not present anymore
    if (!available_crosshairs.count(crosshair_image.s_))
    {
        crosshair_image = "None";
    }
}

inline BlendingMode GetThingBlending(float alpha, ImageOpacity opacity, int32_t hyper_flags = 0)
{
    BlendingMode blending = kBlendingMasked;

    if (alpha >= 0.11f && opacity != kOpacityComplex)
        blending = kBlendingLess;

    if (alpha < 0.99 || opacity == kOpacityComplex)
        blending = (BlendingMode)(blending | kBlendingAlpha);

    if (hyper_flags & kHyperFlagNoZBufferUpdate)
        blending = (BlendingMode)(blending | kBlendingNoZBuffer);

    return blending;
}

static float GetHoverDeltaZ(MapObject *mo, float bob_mult = 0)
{
    if (time_stop_active || erraticism_active)
        return mo->phase_;

    // compute a different phase for different objects
    BAMAngle phase = (BAMAngle)(long long)mo;
    phase ^= (BAMAngle)(phase << 19);
    phase += (BAMAngle)(level_time_elapsed << (kBAMAngleBits - 6));

    mo->phase_ = epi::BAMSin(phase);

    if (mo->hyper_flags_ & kHyperFlagHover)
        mo->phase_ *= 4.0f;
    else if (bob_mult > 0)
        mo->phase_ *= (mo->height_ * 0.5 * bob_mult);

    return mo->phase_;
}

struct PlayerSpriteCoordinateData
{
    HMM_Vec3 vertices[4];
    HMM_Vec2 texture_coordinates[4];
    HMM_Vec3 light_position;

    ColorMixer colors[4];
};

static void DLIT_PSprite(MapObject *mo, void *dataptr)
{
    PlayerSpriteCoordinateData *data = (PlayerSpriteCoordinateData *)dataptr;

    EPI_ASSERT(mo->dynamic_light_.shader);

    mo->dynamic_light_.shader->Sample(data->colors + 0, data->light_position.X, data->light_position.Y,
                                      data->light_position.Z);
}

static int GetMulticolMaxRGB(ColorMixer *cols, int num, bool additive)
{
    int result = 0;

    for (; num > 0; num--, cols++)
    {
        int mx = additive ? cols->add_MAX() : cols->mod_MAX();

        result = HMM_MAX(result, mx);
    }

    return result;
}

static void RenderPSprite(PlayerSprite *psp, int which, Player *player, RegionProperties *props, const State *state)
{
    if (state->flags & kStateFrameFlagModel)
        return;

    // determine sprite patch
    bool         flip;
    const Image *image = GetOtherSprite(state->sprite, state->frame, &flip);

    if (!image)
        return;

    GLuint tex_id = ImageCache(image, false);

    float w     = image->ScaledWidth();
    float h     = image->ScaledHeight();
    float right = 1.0f;
    float top   = 1.0f;
    float ratio = 1.0f;

    bool is_fuzzy = (player->map_object_->flags_ & kMapObjectFlagFuzzy) ? true : false;

    float trans = player->map_object_->visibility_;

    if (is_fuzzy && player->powers_[kPowerTypePartInvisTranslucent] > 0)
    {
        is_fuzzy = false;
        trans *= 0.3f;
    }

    if (which == kPlayerSpriteCrosshair)
    {
        if (!player->weapons_[player->ready_weapon_].info->ignore_crosshair_scaling_)
            ratio = crosshair_size.f_ / w;

        w *= ratio;
        h *= ratio;
        is_fuzzy = false;
        trans    = 1.0f;
    }

    // Lobo: no sense having the zoom crosshair fuzzy
    if (which == kPlayerSpriteWeapon && view_is_zoomed && player->weapons_[player->ready_weapon_].info->zoom_state_ > 0)
    {
        is_fuzzy = false;
        trans    = 1.0f;
    }

    trans *= psp->visibility;

    if (trans <= 0)
        return;

    float tex_top_h = top;  /// ## 1.00f; // 0.98;
    float tex_bot_h = 0.0f; /// ## 1.00f - top;  // 1.02 - bottom;

    float tex_x1 = 0.002f;
    float tex_x2 = right - 0.002f;

    if (flip)
    {
        tex_x1 = right - tex_x1;
        tex_x2 = right - tex_x2;
    }

    float coord_W = 320.0f * widescreen_view_width_multiplier;
    float coord_H = 200.0f;

    float psp_x, psp_y;

    if (!console_active && !paused && !menu_active && !rts_menu_active)
    {
        psp_x = HMM_Lerp(psp->old_screen_x, fractional_tic, psp->screen_x);
        psp_y = HMM_Lerp(psp->old_screen_y, fractional_tic, psp->screen_y);
    }
    else
    {
        psp_x = psp->screen_x;
        psp_y = psp->screen_y;
    }

    float tx1 = (coord_W - w) / 2.0 + psp_x - image->ScaledOffsetX();
    float tx2 = tx1 + w;

    float ty1 = -psp_y + image->ScaledOffsetY() - ((h - image->ScaledHeight()) * 0.5f);

    if (LuaUseLuaHUD())
    {
        // Lobo 2022: Apply sprite Y offset, mainly for Heretic weapons.
        if ((state->flags & kStateFrameFlagWeapon) && (player->ready_weapon_ >= 0))
            ty1 += LuaGetFloat(LuaGetGlobalVM(), "hud", "universal_y_adjust") +
                   player->weapons_[player->ready_weapon_].info->y_adjust_;
    }
    else
    {
        // Lobo 2022: Apply sprite Y offset, mainly for Heretic weapons.
        if ((state->flags & kStateFrameFlagWeapon) && (player->ready_weapon_ >= 0))
            ty1 += COALGetFloat(ui_vm, "hud", "universal_y_adjust") +
                   player->weapons_[player->ready_weapon_].info->y_adjust_;
    }

    float ty2 = ty1 + h;

    float x1b, y1b, x1t, y1t, x2b, y2b, x2t, y2t; // screen coords

    x1b = x1t = view_window_width * tx1 / coord_W;
    x2b = x2t = view_window_width * tx2 / coord_W;

    y1b = y2b = view_window_height * ty1 / coord_H;
    y1t = y2t = view_window_height * ty2 / coord_H;

    // clip psprite to view window
    render_state->Enable(GL_SCISSOR_TEST);

    render_state->Scissor(view_window_x, view_window_y, view_window_width, view_window_height);

    x1b = (float)view_window_x + x1b;
    x1t = (float)view_window_x + x1t;
    x2t = (float)view_window_x + x2t;
    x2b = (float)view_window_x + x2b;

    y1b = (float)view_window_y + y1b - 1;
    y1t = (float)view_window_y + y1t - 1;
    y2t = (float)view_window_y + y2t - 1;
    y2b = (float)view_window_y + y2b - 1;

    PlayerSpriteCoordinateData data;

    data.vertices[0] = {{x1b, y1b, 0}};
    data.vertices[1] = {{x1t, y1t, 0}};
    data.vertices[2] = {{x2t, y1t, 0}};
    data.vertices[3] = {{x2b, y2b, 0}};

    data.texture_coordinates[0] = {{tex_x1, tex_bot_h}};
    data.texture_coordinates[1] = {{tex_x1, tex_top_h}};
    data.texture_coordinates[2] = {{tex_x2, tex_top_h}};
    data.texture_coordinates[3] = {{tex_x2, tex_bot_h}};

    float away = 120.0;

    data.light_position.X = player->map_object_->x + view_cosine * away;
    data.light_position.Y = player->map_object_->y + view_sine * away;
    data.light_position.Z =
        player->map_object_->z + player->map_object_->height_ * player->map_object_->info_->shotheight_;

    data.colors[0].Clear();

    BlendingMode blending = kBlendingMasked;

    if (trans >= 0.11f && image->opacity_ != kOpacityComplex)
        blending = kBlendingLess;

    if (trans < 0.99 || image->opacity_ == kOpacityComplex)
        blending = (BlendingMode)(blending | kBlendingAlpha);

    if (is_fuzzy)
    {
        blending = (BlendingMode)(kBlendingMasked | kBlendingAlpha);
        trans    = 1.0f;
    }

    RGBAColor fc_to_use = player->map_object_->sector_->properties.fog_color;
    float     fd_to_use = player->map_object_->sector_->properties.fog_density;
    // check for DDFLEVL fog
    if (fc_to_use == kRGBANoValue)
    {
        if (EDGE_IMAGE_IS_SKY(player->map_object_->sector_->ceiling))
        {
            fc_to_use = current_map->outdoor_fog_color_;
            fd_to_use = 0.01f * current_map->outdoor_fog_density_;
        }
        else
        {
            fc_to_use = current_map->indoor_fog_color_;
            fd_to_use = 0.01f * current_map->indoor_fog_density_;
        }
    }

    if (!is_fuzzy)
    {
        AbstractShader *shader =
            GetColormapShader(props, player->map_object_->info_->force_fullbright_ ? 255 : state->bright,
                              player->map_object_->sector_);

        shader->Sample(data.colors + 0, data.light_position.X, data.light_position.Y, data.light_position.Z);

        if (fc_to_use != kRGBANoValue)
        {
            int       mix_factor = RoundToInteger(255.0f * (fd_to_use * 75));
            RGBAColor mixme =
                epi::MixRGBA(epi::MakeRGBAClamped(data.colors[0].modulate_red_, data.colors[0].modulate_green_,
                                                  data.colors[0].modulate_blue_),
                             fc_to_use, mix_factor);
            data.colors[0].modulate_red_   = epi::GetRGBARed(mixme);
            data.colors[0].modulate_green_ = epi::GetRGBAGreen(mixme);
            data.colors[0].modulate_blue_  = epi::GetRGBABlue(mixme);
            mixme                          = epi::MixRGBA(
                epi::MakeRGBAClamped(data.colors[0].add_red_, data.colors[0].add_green_, data.colors[0].add_blue_),
                fc_to_use, mix_factor);
            data.colors[0].add_red_   = epi::GetRGBARed(mixme);
            data.colors[0].add_green_ = epi::GetRGBAGreen(mixme);
            data.colors[0].add_blue_  = epi::GetRGBABlue(mixme);
        }

        if (use_dynamic_lights && render_view_extra_light < 250)
        {
            data.light_position.X = player->map_object_->x + view_cosine * 24;
            data.light_position.Y = player->map_object_->y + view_sine * 24;

            float r = 96;

            for (int index = 0; index < LightGridSampleTotal(); index++)
            {
                MapObject *light = LightGridSampleLight(index);

                if (!light)
                    continue;

                float reach = light->dynamic_light_.r;

                if (fabs(light->x - data.light_position.X) >= reach ||
                    fabs(light->y - data.light_position.Y) >= reach ||
                    fabs(MapObjectMidZ(light) - data.light_position.Z) >= reach)
                    continue;

                DLIT_PSprite(light, &data);
            }

            SectorGlowIterator(player->map_object_->sector_, data.light_position.X - r,
                               data.light_position.Y - r, player->map_object_->z, data.light_position.X + r,
                               data.light_position.Y + r, player->map_object_->z + player->map_object_->height_,
                               DLIT_PSprite, &data);
        }
    }

    // FIXME: sample at least TWO points (left and right edges)
    data.colors[1] = data.colors[0];
    data.colors[2] = data.colors[0];
    data.colors[3] = data.colors[0];

    /* draw the weapon */

    int saved_lookup = render_unit_color_lookup;

    if (which == kPlayerSpriteCrosshair)
        render_unit_color_lookup = 0;

    StartUnitBatch(false);

    int num_pass = is_fuzzy ? 1 : (detail_level > 0 ? 4 : 3);

    for (int pass = 0; pass < num_pass; pass++)
    {
        if (pass == 1)
        {
            blending = (BlendingMode)(blending & ~kBlendingAlpha);
            blending = (BlendingMode)(blending | kBlendingAdd);
        }

        bool is_additive = (pass > 0 && pass == num_pass - 1);

        if (pass > 0 && pass < num_pass - 1)
        {
            if (GetMulticolMaxRGB(data.colors, 4, false) <= 0)
                continue;
        }
        else if (is_additive)
        {
            if (GetMulticolMaxRGB(data.colors, 4, true) <= 0)
                continue;
        }

        GLuint fuzz_tex = is_fuzzy ? ImageCache(fuzz_image, false) : 0;

        RendererVertex *glvert =
            BeginRenderUnit(GL_QUADS, 4, is_additive ? (GLuint)kTextureEnvironmentSkipRGB : GL_MODULATE, tex_id,
                            is_fuzzy ? GL_MODULATE : (GLuint)kTextureEnvironmentDisable, fuzz_tex, pass, blending,
                            pass > 0 ? kRGBANoValue : fc_to_use, fd_to_use);

        for (int v_idx = 0; v_idx < 4; v_idx++)
        {
            RendererVertex *dest = glvert + v_idx;

            dest->position               = data.vertices[v_idx];
            dest->texture_coordinates[0] = data.texture_coordinates[v_idx];

            if (is_fuzzy)
            {
                dest->texture_coordinates[1].X = dest->position.X / (float)current_screen_width;
                dest->texture_coordinates[1].Y = dest->position.Y / (float)current_screen_height;

                FuzzAdjust(&dest->texture_coordinates[1], player->map_object_);

                dest->rgba = kRGBABlack;
            }
            else if (!is_additive)
            {
                dest->rgba = epi::MakeRGBAClamped(data.colors[v_idx].modulate_red_ * render_view_red_multiplier,
                                                  (data.colors[v_idx].modulate_green_ * render_view_green_multiplier),
                                                  data.colors[v_idx].modulate_blue_ * render_view_blue_multiplier);

                data.colors[v_idx].modulate_red_ -= 256;
                data.colors[v_idx].modulate_green_ -= 256;
                data.colors[v_idx].modulate_blue_ -= 256;
            }
            else
            {
                dest->rgba = epi::MakeRGBAClamped(data.colors[v_idx].add_red_ * render_view_red_multiplier,
                                                  (data.colors[v_idx].add_green_ * render_view_green_multiplier),
                                                  data.colors[v_idx].add_blue_ * render_view_blue_multiplier);
            }

            epi::SetRGBAAlpha(dest->rgba, trans);
        }

        EndRenderUnit(4);
    }

    FinishUnitBatch();

    render_unit_color_lookup = saved_lookup;

    render_state->Disable(GL_SCISSOR_TEST);
}

static const RGBAColor crosshair_colors[8] = {kRGBALightGray, kRGBABlue,    kRGBAGreen,  kRGBACyan,
                                              kRGBARed,       kRGBAFuchsia, kRGBAYellow, kRGBADarkOrange};

static void DrawStdCrossHair(void)
{
    if (crosshair_size.f_ < 0.1 || crosshair_brightness.f_ < 0.1)
        return;

    unsigned int tex_id = available_crosshairs[crosshair_image.s_];

    if (!tex_id)
        return;

    RGBAColor color = crosshair_colors[crosshair_color.d_ & 7];

    float intensity = 1.0f * crosshair_brightness.f_;

    RGBAColor unit_col =
        epi::MakeRGBA((uint8_t)(epi::GetRGBARed(color) * intensity), (uint8_t)(epi::GetRGBAGreen(color) * intensity),
                      (uint8_t)(epi::GetRGBABlue(color) * intensity));

    float x = view_window_x + view_window_width / 2;
    float y = view_window_y + view_window_height / 2;

    float w = RoundToInteger(current_screen_width * crosshair_size.f_ / 640.0f);

    StartUnitBatch(false);

    RendererVertex *glvert =
        BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, tex_id, (GLuint)kTextureEnvironmentDisable, 0, 0, kBlendingAdd);

    glvert->rgba                     = unit_col;
    glvert->position                 = {{x - w, y - w, 0.0f}};
    glvert++->texture_coordinates[0] = {{0.0f, 0.0f}};
    glvert->rgba                     = unit_col;
    glvert->position                 = {{x - w, y + w, 0.0f}};
    glvert++->texture_coordinates[0] = {{0.0f, 1.0f}};
    glvert->rgba                     = unit_col;
    glvert->position                 = {{x + w, y + w, 0.0f}};
    glvert++->texture_coordinates[0] = {{1.0f, 1.0f}};
    glvert->rgba                     = unit_col;
    glvert->position                 = {{x + w, y - w, 0.0f}};
    glvert++->texture_coordinates[0] = {{1.0f, 0.0f}};

    EndRenderUnit(4);

    FinishUnitBatch();
}

void RenderWeaponSprites(Player *p)
{
    // special handling for zoom: show viewfinder
    if (view_is_zoomed)
    {
        PlayerSprite *psp = &p->player_sprites_[kPlayerSpriteWeapon];

        WeaponDefinition *w = p->weapons_[p->ready_weapon_].info;

        // 2023.06.13 - If zoom state missing but weapon can zoom, allow the
        // regular psprite drawing routines to occur (old EDGE behavior)
        if (w->zoom_state_ > 0)
        {
            RenderPSprite(psp, kPlayerSpriteWeapon, p, view_properties, states + w->zoom_state_);
            return;
        }
    }

    // add all active player_sprites_
    // Note: order is significant

    // Lobo 2022:
    // Allow changing the order of weapon sprite
    // rendering so that FLASH states are
    // drawn in front of the WEAPON states
    if (!p->weapons_[p->ready_weapon_].info->render_invert_)
    {
        for (int i = 0; i < kTotalPlayerSpriteTypes; i++) // normal
        {
            PlayerSprite *psp = &p->player_sprites_[i];

            if (psp->state == 0)
                continue;

            RenderPSprite(psp, i, p, view_properties, psp->state);
        }
    }
    else
    {
        for (int i = kTotalPlayerSpriteTypes - 1; i >= 0; i--) // go backwards
        {
            PlayerSprite *psp = &p->player_sprites_[i];

            if (psp->state == 0)
                continue;

            RenderPSprite(psp, i, p, view_properties, psp->state);
        }
    }
}

void RenderCrosshair(Player *p)
{
    if (view_is_zoomed && p->weapons_[p->ready_weapon_].info->zoom_state_ > 0)
    {
        // Only skip crosshair if there is a dedicated zoom state, which
        // should be providing its own
        return;
    }
    else
    {
        PlayerSprite *psp = &p->player_sprites_[kPlayerSpriteCrosshair];

        if (p->ready_weapon_ >= 0 && psp->state != 0)
            return;
    }

    if (p->health_ > 0)
    {
        int saved_lookup         = render_unit_color_lookup;
        render_unit_color_lookup = 0;

        DrawStdCrossHair();

        render_unit_color_lookup = saved_lookup;
    }
}

void RenderWeaponModel(Player *p)
{
    if (view_is_zoomed && p->weapons_[p->ready_weapon_].info->zoom_state_ > 0)
        return;

    PlayerSprite *psp = &p->player_sprites_[kPlayerSpriteWeapon];

    if (!(psp->state->flags & kStateFrameFlagModel))
        return;

    WeaponDefinition *w = p->weapons_[p->ready_weapon_].info;

    ModelDefinition *md = GetModel(psp->state->sprite);

    int skin_num = p->weapons_[p->ready_weapon_].model_skin;

    const Image *skin_img = md->skins_[skin_num];

    if (!skin_img && md->md2_model_)
    {
        skin_img = ImageForDummySkin();
    }

    float psp_x, psp_y;

    if (!console_active && !paused && !menu_active && !erraticism_active && !rts_menu_active)
    {
        psp_x = HMM_Lerp(psp->old_screen_x, fractional_tic, psp->screen_x);
        psp_y = HMM_Lerp(psp->old_screen_y, fractional_tic, psp->screen_y);
    }
    else
    {
        psp_x = psp->screen_x;
        psp_y = psp->screen_y;
    }

    float x = view_x + view_right.X * psp_x / 8.0;
    float y = view_y + view_right.Y * psp_x / 8.0;
    float z = view_z + view_right.Z * psp_x / 8.0;

    x -= view_up.X * psp_y / 10.0;
    y -= view_up.Y * psp_y / 10.0;
    z -= view_up.Z * psp_y / 10.0;

    x += view_forward.X * w->model_forward_;
    y += view_forward.Y * w->model_forward_;
    z += view_forward.Z * w->model_forward_;

    x += view_right.X * w->model_side_;
    y += view_right.Y * w->model_side_;
    z += view_right.Z * w->model_side_;

    int   last_frame = psp->state->frame;
    float lerp       = 0.0;

    if (p->weapon_last_frame_ >= 0)
    {
        EPI_ASSERT(psp->state);
        EPI_ASSERT(psp->state->tics > 1);

        last_frame = p->weapon_last_frame_;
        if (!console_active && !paused && !menu_active && !erraticism_active && !rts_menu_active)
            lerp = ((float)psp->state->tics - psp->tics + fractional_tic) / (float)(psp->state->tics);
        else
            lerp = (psp->state->tics - psp->tics + 1) / (float)(psp->state->tics);
        lerp = HMM_Clamp(0, lerp, 1);
    }

    float bias = 0.0f;

    if (LuaUseLuaHUD())
    {
        bias =
            LuaGetFloat(LuaGetGlobalVM(), "hud", "universal_y_adjust") + p->weapons_[p->ready_weapon_].info->y_adjust_;
    }
    else
    {
        bias = COALGetFloat(ui_vm, "hud", "universal_y_adjust") + p->weapons_[p->ready_weapon_].info->y_adjust_;
    }

    bias /= 5;
    bias += w->model_bias_;

    if (md->md2_model_)
        MD2RenderModel(md->md2_model_, skin_img, true, last_frame, psp->state->frame, lerp, x, y, z, p->map_object_,
                       view_properties, 1.0f /* scale */, w->model_aspect_, bias, w->model_rotate_);
    else if (md->mdl_model_)
        MDLRenderModel(md->mdl_model_, true, last_frame, psp->state->frame, lerp, x, y, z, p->map_object_,
                       view_properties, 1.0f /* scale */, w->model_aspect_, bias, w->model_rotate_);
}

// ============================================================================
// RendererBSP START
// ============================================================================

int sprite_kludge = 0;

static inline void LinkDrawThingIntoView(DrawThing *dthing)
{
    int32_t active_mirrors = active_mirror_set.TotalActive();

    if (active_mirrors > 0)
        active_mirror_set.PushThing(active_mirrors - 1, dthing);
    else
        draw_thing_list.push_back(dthing);
}

static const Image *RendererGetThingSprite2(MapObject *mo, float mx, float my, bool *flip)
{
    // Note: can return nullptr for no image.

    // decide which patch to use for sprite relative to player
    EPI_ASSERT(mo->state_);

    if (mo->state_->sprite == 0)
        return nullptr;

    SpriteFrame *frame = GetSpriteFrame(mo->state_->sprite, mo->state_->frame);

    if (!frame)
    {
        // show dummy sprite for missing frame
        (*flip) = false;
        return ImageForDummySprite();
    }

    int rot = 0;

    if (frame->rotations_ >= 8)
    {
        BAMAngle ang;

        if (mo->interpolate_ && !console_active && !paused && !menu_active && !erraticism_active && !rts_menu_active)
            ang = epi::BAMInterpolate(mo->old_angle_, mo->angle_, fractional_tic);
        else
            ang = mo->angle_;

        active_mirror_set.Angle(ang);

        BAMAngle from_view = PointToAngle(view_x, view_y, mx, my);

        ang = from_view - ang + kBAMAngle180;

        if (active_mirror_set.Reflective())
            ang = (BAMAngle)0 - ang;

        if (frame->rotations_ == 16)
            rot = (ang + (kBAMAngle45 / 4)) >> (kBAMAngleBits - 4);
        else
            rot = (ang + (kBAMAngle45 / 2)) >> (kBAMAngleBits - 3);
    }

    EPI_ASSERT(0 <= rot && rot < 16);

    (*flip) = frame->flip_[rot] ? true : false;

    if (active_mirror_set.Reflective())
        (*flip) = !(*flip);

    if (!frame->images_[rot])
    {
        // show dummy sprite for missing rotation
        (*flip) = false;
        return ImageForDummySprite();
    }

    return frame->images_[rot];
}

const Image *GetOtherSprite(int spritenum, int framenum, bool *flip)
{
    /* Used for non-object stuff, like weapons and finale */

    if (spritenum == 0)
        return nullptr;

    SpriteFrame *frame = GetSpriteFrame(spritenum, framenum);

    if (!frame || !frame->images_[0])
    {
        (*flip) = false;
        return ImageForDummySprite();
    }

    *flip = frame->flip_[0] ? true : false;

    return frame->images_[0];
}

static void RendererClipSpriteVertically(DrawThing *dthing)
{
    float z = dthing->map_z + (dthing->map_object->height_ * 0.5f);

    dthing->properties = GetPointProperties(dthing->map_object->sector_, z);

    LinkDrawThingIntoView(dthing);
}

static bool ThingSectorReached(const MapObject *mo)
{
    if (SectorReachedThisView(mo->sector_))
        return true;

    for (const TouchNode *tn = mo->touch_sectors_; tn; tn = tn->map_object_next)
    {
        if (tn->sector && SectorReachedThisView(tn->sector))
            return true;
    }

    return false;
}

void BSPWalkThing(MapObject *mo)
{
    /* Visit a single thing that exists in the current subsector */

    EPI_ASSERT(mo->state_);

    // ignore the camera itself
    if (mo == view_camera_map_object && active_mirror_set.TotalActive() == 0)
        return;

    // ignore invisible things
    if (epi::AlmostEquals(mo->visibility_, 0.0f))
        return;

    bool is_model = (mo->state_->flags & kStateFrameFlagModel) ? true : false;

    // transform the origin point
    float mx, my, mz, fz;

    // position interpolation

    // This applies to kStateFrameFlagModel and kMapObjectFlagFloat
    if (mo->interpolation_number_ > 0)
    {
        if (mo->interpolate_ && !console_active && !paused && !menu_active && !erraticism_active && !rts_menu_active)
        {
            float along = (float)(mo->interpolation_position_ - 1 + fractional_tic) / mo->interpolation_number_;
            mx          = HMM_Lerp(mo->interpolation_from_.X, along, mo->x);
            my          = HMM_Lerp(mo->interpolation_from_.Y, along, mo->y);
            mz          = HMM_Lerp(mo->interpolation_from_.Z, along, mo->z);
            fz          = HMM_Lerp(mo->old_floor_z_, fractional_tic, mo->floor_z_);
        }
        else
        {
            float along = (float)(mo->interpolation_position_ - 1) / mo->interpolation_number_;
            mx          = HMM_Lerp(mo->interpolation_from_.X, along, mo->x);
            my          = HMM_Lerp(mo->interpolation_from_.Y, along, mo->y);
            mz          = HMM_Lerp(mo->interpolation_from_.Z, along, mo->z);
            fz          = mo->floor_z_;
        }
    }
    else
    {
        if (mo->interpolate_ && !console_active && !paused && !menu_active && !erraticism_active && !rts_menu_active)
        {
            mx = HMM_Lerp(mo->old_x_, fractional_tic, mo->x);
            my = HMM_Lerp(mo->old_y_, fractional_tic, mo->y);
            mz = HMM_Lerp(mo->old_z_, fractional_tic, mo->z);
            fz = HMM_Lerp(mo->old_floor_z_, fractional_tic, mo->floor_z_);
        }
        else
        {
            mx = mo->x;
            my = mo->y;
            mz = mo->z;
            fz = mo->floor_z_;
        }
    }

    float view_mx = mx;
    float view_my = my;

    active_mirror_set.Coordinate(view_mx, view_my);

    float tr_x = view_mx - view_x;
    float tr_y = view_my - view_y;

    float tz = tr_x * view_cosine + tr_y * view_sine;

    // thing is behind view plane?
    if (!is_model)
    {
        if (clip_scope != kBAMAngle180 && tz <= 0)
            return;
    }
    else
    {
        ModelDefinition *md = GetModel(mo->state_->sprite);
        EPI_ASSERT(md);
        if (clip_scope != kBAMAngle180 && tz < -(md->radius_ * mo->scale_))
        {
            return;
        }
    }

    float tx = tr_x * view_sine - tr_y * view_cosine;

    // too far off the side?
    // -ES- 1999/03/13 Fixed clipping to work with large FOVs (up to 176 deg)
    // rejects all sprites where angle>176 deg (arctan 32), since those
    // sprites would result in overflow in future calculations
    if (!is_model && (tz >= kMinimumSpriteDistance) && ((fabs(tx) / 32) > tz))
        return;

    float   sink_mult = 0;
    float   bob_mult  = 0;
    Sector *cur_sec   = mo->sector_;
    if (!cur_sec->extrafloor_used && !cur_sec->height_sector && epi::AlmostEquals(mz, cur_sec->floor_height))
    {
        if (!(mo->flags_ & kMapObjectFlagNoGravity))
        {
            sink_mult = cur_sec->sink_depth;
            bob_mult  = cur_sec->bob_depth;
        }
    }

    float hover_dz = 0;

    if (mo->hyper_flags_ & kHyperFlagHover ||
        ((mo->flags_ & kMapObjectFlagSpecial || mo->flags_ & kMapObjectFlagCorpse) && bob_mult > 0))
        hover_dz = GetHoverDeltaZ(mo, bob_mult);

    if (sink_mult > 0)
        hover_dz -= (mo->height_ * 0.5 * sink_mult);

    bool         spr_flip = false;
    const Image *image    = nullptr;

    if (!is_model)
    {
        image = RendererGetThingSprite2(mo, view_mx, view_my, &spr_flip);

        if (!image)
            return;
    }

    // Dasho: feels like we can figure this out up above
    if (!(mo->hyper_flags_ & kHyperFlagHover || (sink_mult > 0 || bob_mult > 0)))
        hover_dz = 0;

    // create new draw thing

    DrawThing *dthing       = GetDrawThing();
    dthing->map_object      = nullptr;
    dthing->properties      = nullptr;

    dthing->map_object = mo;
    dthing->map_x      = mx;
    dthing->map_y      = my;
    dthing->map_z      = mz;

    dthing->is_model   = is_model;

    dthing->image = image;
    dthing->flip  = spr_flip;

    dthing->translated_z = tz;
    dthing->floor_z      = fz;
    dthing->hover_dz     = hover_dz;
    dthing->sink_mult    = sink_mult;

    RendererClipSpriteVertically(dthing);
}

static void RenderModel(DrawThing *dthing)
{
    MapObject *mo = dthing->map_object;

    ModelDefinition *md = GetModel(mo->state_->sprite);

    const Image *skin_img = md->skins_[mo->model_skin_];

    if (!skin_img && md->md2_model_)
    {
        // LogDebug("Render model: no skin %d\n", mo->model_skin);
        skin_img = ImageForDummySkin();
    }

    float z = dthing->map_z + dthing->hover_dz;

    int   last_frame = mo->state_->frame;
    float lerp       = 0.0;

    if (mo->model_last_frame_ >= 0)
    {
        last_frame = mo->model_last_frame_;

        EPI_ASSERT(mo->state_->tics > 1);
        if (mo->interpolate_ && !console_active && !paused && !menu_active && !erraticism_active && !rts_menu_active)
            lerp = ((float)mo->state_->tics - mo->tics_ + fractional_tic) / (float)(mo->state_->tics);
        else
            lerp = (mo->state_->tics - mo->tics_ + 1) / (float)(mo->state_->tics);
        lerp = HMM_Clamp(0, lerp, 1);
    }

    if (md->md2_model_)
        MD2RenderModel(md->md2_model_, skin_img, false, last_frame, mo->state_->frame, lerp, dthing->map_x,
                       dthing->map_y, z, mo, mo->region_properties_, mo->model_scale_, mo->model_aspect_,
                       mo->info_->model_bias_, mo->info_->model_rotate_);
    else if (md->mdl_model_)
        MDLRenderModel(md->mdl_model_, false, last_frame, mo->state_->frame, lerp, dthing->map_x, dthing->map_y, z, mo,
                       mo->region_properties_, mo->model_scale_, mo->model_aspect_, mo->info_->model_bias_,
                       mo->info_->model_rotate_);
}

struct SpriteBatchKey
{
    GLuint                  texture;
    GLuint                  fuzz_texture;
    BlendingMode            blending;
    RGBAColor               fog_color;
    float                   fog_density;
    int                     color_lookup;
    int                     glow_set;
    const SpriteLightTable *light_table;
    uint8_t                 alpha;
    bool                    world_lit;
};

struct SpriteBatch
{
    SpriteBatchKey              key;
    std::vector<SpriteInstance> instances;
};

static std::vector<SpriteBatch>             sprite_batches;
static size_t                               sprite_batches_used = 0;
static std::unordered_map<uint64_t, size_t> sprite_batch_index;
static uint64_t                             sprite_batch_last_hash  = 0;
static size_t                               sprite_batch_last_index = SIZE_MAX;

static uint64_t HashSpriteBatchValue(uint64_t hash, uint64_t value)
{
    hash ^= value;
    hash *= 1099511628211ULL;

    return hash;
}

static uint64_t HashSpriteBatchKey(const SpriteBatchKey &key)
{
    uint32_t density_bits = 0;

    memcpy(&density_bits, &key.fog_density, sizeof(density_bits));

    uint64_t hash = 14695981039346656037ULL;

    hash = HashSpriteBatchValue(hash, key.texture);
    hash = HashSpriteBatchValue(hash, key.fuzz_texture);
    hash = HashSpriteBatchValue(hash, (uint64_t)key.blending);
    hash = HashSpriteBatchValue(hash, key.fog_color);
    hash = HashSpriteBatchValue(hash, density_bits);
    hash = HashSpriteBatchValue(hash, (uint64_t)(int64_t)key.color_lookup);
    hash = HashSpriteBatchValue(hash, (uint64_t)(int64_t)key.glow_set);
    hash = HashSpriteBatchValue(hash, (uint64_t)(uintptr_t)key.light_table);
    hash = HashSpriteBatchValue(hash, key.alpha);
    hash = HashSpriteBatchValue(hash, key.world_lit ? 1 : 0);

    return hash;
}

static bool SpriteBatchKeysMatch(const SpriteBatchKey &a, const SpriteBatchKey &b)
{
    return a.texture == b.texture && a.fuzz_texture == b.fuzz_texture && a.blending == b.blending &&
           a.fog_color == b.fog_color && epi::AlmostEquals(a.fog_density, b.fog_density) &&
           a.color_lookup == b.color_lookup && a.glow_set == b.glow_set && a.light_table == b.light_table &&
           a.alpha == b.alpha && a.world_lit == b.world_lit;
}

static void AddSpriteToBatch(const SpriteBatchKey &key, const SpriteInstance &instance)
{
    uint64_t hash = HashSpriteBatchKey(key);

    if (sprite_batch_last_index != SIZE_MAX && hash == sprite_batch_last_hash &&
        SpriteBatchKeysMatch(sprite_batches[sprite_batch_last_index].key, key))
    {
        sprite_batches[sprite_batch_last_index].instances.push_back(instance);
        return;
    }

    uint64_t probe = hash;

    for (;;)
    {
        std::unordered_map<uint64_t, size_t>::iterator found = sprite_batch_index.find(probe);

        if (found == sprite_batch_index.end())
        {
            if (sprite_batches_used == sprite_batches.size())
                sprite_batches.emplace_back();

            SpriteBatch &batch = sprite_batches[sprite_batches_used];

            batch.key = key;
            batch.instances.clear();
            batch.instances.push_back(instance);

            sprite_batch_index[probe] = sprite_batches_used;

            sprite_batch_last_hash  = hash;
            sprite_batch_last_index = sprite_batches_used;

            sprite_batches_used++;
            return;
        }

        SpriteBatch &batch = sprite_batches[found->second];

        if (SpriteBatchKeysMatch(batch.key, key))
        {
            batch.instances.push_back(instance);

            sprite_batch_last_hash  = hash;
            sprite_batch_last_index = found->second;
            return;
        }

        probe++;
    }
}

static void FlushSpriteBatches(void)
{
    for (size_t i = 0; i < sprite_batches_used; i++)
    {
        SpriteBatch &batch = sprite_batches[i];

        int count = (int)batch.instances.size();

        if (count == 0)
            continue;

        int first = 0;

        SpriteInstance *destination = ReserveSpriteInstances(count, &first);

        memcpy(destination, batch.instances.data(), sizeof(SpriteInstance) * (size_t)count);

        const SpriteBatchKey &key = batch.key;

        AddSpriteRenderUnit(first, count, key.texture, key.fuzz_texture, key.blending, key.fog_color,
                            key.fog_density, key.color_lookup, key.world_lit, key.glow_set, key.light_table,
                            key.alpha);

        batch.instances.clear();
    }

    sprite_batches_used = 0;
    sprite_batch_index.clear();

    sprite_batch_last_index = SIZE_MAX;
}

struct SpriteSource
{
    MapObject        *map_object;
    const Image      *image;
    RegionProperties *properties;
    float             map_x;
    float             map_y;
    float             map_z;
    float             floor_z;
    float             hover_dz;
    float             sink_mult;
    bool              flip;
    bool              resident;
};

static bool BuildSpriteInstance(const SpriteSource &source, SpriteInstance *instance, SpriteBatchKey *key)
{
    MapObject   *mo    = source.map_object;
    const Image *image = source.image;

    bool is_fuzzy = (mo->flags_ & kMapObjectFlagFuzzy) ? true : false;

    float trans = mo->visibility_;

    if (trans <= 0)
        return false;

    AtlasRegion region;

    GLuint tex_id;

    if (AtlasSpriteRegion(mo->state_->sprite, image, &region))
    {
        tex_id = region.texture;
    }
    else
    {
        tex_id = ImageCache(image, false);

        region.rectangle[0] = 0.0f;
        region.rectangle[1] = 0.0f;
        region.rectangle[2] = 1.0f;
        region.rectangle[3] = 1.0f;
    }

    float region_width  = region.rectangle[2] - region.rectangle[0];
    float region_height = region.rectangle[3] - region.rectangle[1];

    // calculate edges of the shape
    float sprite_width  = image->ScaledWidth();
    float sprite_height = image->ScaledHeight();
    float side_offset   = image->ScaledOffsetX();
    float top_offset    = image->ScaledOffsetY();

    if (source.flip)
        side_offset = -side_offset;

    float xscale = mo->scale_ * mo->aspect_;

    float pos1 = (sprite_width / -2.0f - side_offset) * xscale;
    float pos2 = (sprite_width / +2.0f - side_offset) * xscale;

    float gzt = 0;
    float gzb = 0;
    float fz  = source.floor_z;

    switch (mo->info_->yalign_)
    {
    case SpriteYAlignmentTopDown:
        gzt = source.map_z + mo->height_ + top_offset * mo->scale_;
        gzb = gzt - sprite_height * mo->scale_;
        break;

    case SpriteYAlignmentMiddle: {
        float _mz = source.map_z + mo->height_ * 0.5 + top_offset * mo->scale_;
        float dz  = sprite_height * 0.5 * mo->scale_;

        gzt = _mz + dz;
        gzb = _mz - dz;
        break;
    }

    case SpriteYAlignmentBottomUp:
    default:
        gzb = source.map_z + top_offset * mo->scale_;
        gzt = gzb + sprite_height * mo->scale_;
        break;
    }

    gzt += source.hover_dz;
    gzb += source.hover_dz;

    if ((mo->flags_ & kMapObjectFlagFuzzy) ||
        ((mo->hyper_flags_ & kHyperFlagHover) && epi::AlmostEquals(source.sink_mult, 0.0f)))
    {
        /* nothing, don't adjust clipping */
    }
    // Lobo: new FLOOR_CLIP flag
    else if (mo->hyper_flags_ & kHyperFlagFloorClip || source.sink_mult > 0)
    {
        /* nothing, don't adjust clipping */
    }
    else if (sprite_kludge == 0 && gzb < fz)
    {
        // explosion ?
        if (mo->info_->flags_ & kMapObjectFlagMissile)
        {
            /* nothing, don't adjust clipping */
        }
        else
        {
            // Dasho - The sprite boundaries are clipped by the floor; this checks
            // the actual visible portion of the image to see if we need to do any adjustments.
            float diff = (float)image->real_bottom_ * image->scale_y_ * mo->scale_;
            if (gzb + diff < fz)
            {
                gzt += fz - (gzb + diff);
                gzb = fz - diff;
            }
        }
    }

    if (gzb >= gzt)
        return false;

    BlendingMode blending = GetThingBlending(trans, (ImageOpacity)image->opacity_, mo->hyper_flags_);

    if (is_fuzzy)
        blending = (BlendingMode)(blending | kBlendingAlpha);

    float h = image->ScaledHeight();

    // MLook: tilt sprites so they look better
    float skew2 = gzt - gzb;

    if (mo->radius_ >= 1.0f && h > mo->radius_)
        skew2 = mo->radius_;

    float tex_x1 = 0.001f;
    float tex_x2 = 1.0f - 0.001f;

    EPI_ASSERT(h > 0);

    float tex_y1 = 0;
    float tex_y2 = (gzt - gzb) / (h * mo->scale_);

    if (source.flip)
    {
        float temp = tex_x2;
        tex_x1     = 1.0f - tex_x1;
        tex_x2     = 1.0f - temp;
    }

    float    fuzz_mul = 0;
    HMM_Vec2 fuzz_add = {{0, 0}};

    if (is_fuzzy)
    {
        blending = (BlendingMode)(kBlendingMasked | kBlendingAlpha);
        trans    = 1.0f;

        float dist = ApproximateDistance(mo->x - view_x, mo->y - view_y, mo->z - view_z);

        fuzz_mul = 0.8 / HMM_Clamp(20, dist, 700);

        FuzzAdjust(&fuzz_add, mo);
    }

    int                     light_level = 0;
    const SpriteLightTable *light_table = nullptr;

    if (!is_fuzzy)
        light_table = GetSpriteLightTable(source.properties, mo->info_->force_fullbright_ ? 255 : mo->state_->bright,
                                          mo->sector_, &light_level);

    RGBAColor fc_to_use = mo->sector_->properties.fog_color;
    float     fd_to_use = mo->sector_->properties.fog_density;
    // check for DDFLEVL fog
    if (fc_to_use == kRGBANoValue)
    {
        if (EDGE_IMAGE_IS_SKY(mo->sector_->ceiling))
        {
            fc_to_use = current_map->outdoor_fog_color_;
            fd_to_use = 0.01f * current_map->outdoor_fog_density_;
        }
        else
        {
            fc_to_use = current_map->indoor_fog_color_;
            fd_to_use = 0.01f * current_map->indoor_fog_density_;
        }
    }

    float tint_r = 1.0f;
    float tint_g = 1.0f;
    float tint_b = 1.0f;

    int color_lookup = render_unit_color_lookup;

    if (!render_view_effect_colormap && !ColormapTintFactors(mo->info_->palremap_, &tint_r, &tint_g, &tint_b))
        color_lookup = ColorLookupForColormap(mo->info_->palremap_);

    bool sprite_lit = !is_fuzzy && use_dynamic_lights && render_view_extra_light < 250;

    float flags = (source.resident ? kSpriteInstanceMirrorFlip : 0.0f) + (is_fuzzy ? kSpriteInstanceFuzzy : 0.0f);

    instance->origin[0] = source.map_x;
    instance->origin[1] = source.map_y;
    instance->origin[2] = gzb;
    instance->origin[3] = gzt;

    instance->extent[0] = pos1;
    instance->extent[1] = pos2;
    instance->extent[2] = skew2;
    instance->extent[3] = flags;

    instance->texture_coordinates[0] = region.rectangle[0] + tex_x1 * region_width;
    instance->texture_coordinates[1] = region.rectangle[1] + tex_y1 * region_height;
    instance->texture_coordinates[2] = region.rectangle[0] + tex_x2 * region_width;
    instance->texture_coordinates[3] = region.rectangle[1] + tex_y2 * region_height;

    instance->fuzz[0] = mo->radius_ * 2 * fuzz_mul;
    instance->fuzz[1] = mo->height_ * fuzz_mul;
    instance->fuzz[2] = fuzz_add.X;
    instance->fuzz[3] = fuzz_add.Y;

    instance->light[0] = (float)light_level;
    instance->light[1] = 0.0f;

    if (is_fuzzy)
        instance->rgba = kRGBABlack;
    else
        instance->rgba = epi::MakeRGBAClamped((int)(tint_r * 255.0f), (int)(tint_g * 255.0f), (int)(tint_b * 255.0f));

    epi::SetRGBAAlpha(instance->rgba, trans);

    instance->padding = 0;

    key->texture      = tex_id;
    key->fuzz_texture = is_fuzzy ? ImageCache(fuzz_image, false) : 0;
    key->blending     = blending;
    key->fog_color    = fc_to_use;
    key->fog_density  = fd_to_use;
    key->color_lookup = color_lookup;
    key->glow_set     = sprite_lit ? LightGridGlowSetForSector(mo->sector_) : -1;
    key->light_table  = light_table;
    key->alpha        = epi::GetRGBAAlpha(instance->rgba);
    key->world_lit    = sprite_lit;

    return true;
}

struct ResidentBatch
{
    SpriteBatchKey              key;
    std::vector<SpriteInstance> instances;
    std::vector<MapObject *>    owners;
    std::vector<int>            free_slots;
    uint32_t                    buffer          = 0;
    int                         buffer_capacity = 0;
    int                         dirty_low       = INT_MAX;
    int                         dirty_high      = -1;
    int                         live            = 0;
    bool                        transparent     = false;
};

struct ResidentSignature
{
    const Colormap *effect_colormap;
    int             color_lookup;
    int             dynamic_lights;
    bool            extra_full;
    int             brightness;
};

static std::vector<ResidentBatch>           resident_batches;
static std::unordered_map<uint64_t, size_t> resident_batch_index;
static int                                  resident_sector_total = -1;
static std::vector<MapObject *>             dynamic_things;
static std::vector<MapObject *>             resident_candidates;
static ResidentSignature                    resident_signature;
static bool                                 resident_ready = false;
static bool                                 resident_invalid = false;

extern ConsoleVariable sector_brightness_correction;

static uint64_t HashFloats(uint64_t hash, const float *values, int count)
{
    for (int i = 0; i < count; i++)
    {
        uint32_t bits = 0;

        memcpy(&bits, &values[i], sizeof(bits));

        hash = HashSpriteBatchValue(hash, bits);
    }

    return hash;
}

static uint64_t HashThingRenderState(const MapObject *mo)
{
    float values[5] = {mo->x, mo->y, mo->z, mo->floor_z_, (float)mo->interpolation_number_};

    uint64_t hash = HashFloats(14695981039346656037ULL, values, 5);

    hash = HashSpriteBatchValue(hash, (uint64_t)(uintptr_t)mo->sector_);
    hash = HashSpriteBatchValue(hash, (uint64_t)(uintptr_t)mo->region_properties_);

    return hash;
}

static uint64_t HashThingLookState(const MapObject *mo)
{
    float values[5] = {mo->visibility_, mo->scale_, mo->aspect_, mo->height_, mo->radius_};

    uint64_t hash = HashFloats(14695981039346656037ULL, values, 5);

    hash = HashSpriteBatchValue(hash, (uint64_t)(uintptr_t)mo->state_);
    hash = HashSpriteBatchValue(hash, (uint64_t)(uint32_t)mo->flags_);
    hash = HashSpriteBatchValue(hash, (uint64_t)(uint32_t)mo->hyper_flags_);

    return hash;
}

static ResidentSignature CurrentResidentSignature(void)
{
    ResidentSignature signature;

    signature.effect_colormap = render_view_effect_colormap;
    signature.color_lookup    = render_unit_color_lookup;
    signature.dynamic_lights  = use_dynamic_lights;
    signature.extra_full      = render_view_extra_light >= 250;
    signature.brightness      = sector_brightness_correction.d_;

    return signature;
}

static bool ResidentSignaturesMatch(const ResidentSignature &a, const ResidentSignature &b)
{
    return a.effect_colormap == b.effect_colormap && a.color_lookup == b.color_lookup &&
           a.dynamic_lights == b.dynamic_lights && a.extra_full == b.extra_full && a.brightness == b.brightness;
}

static void DynamicThingAdd(MapObject *mo)
{
    if (mo->render_dynamic_index_ >= 0)
        return;

    mo->render_dynamic_index_ = (int)dynamic_things.size();

    dynamic_things.push_back(mo);
}

static void DynamicThingRemove(MapObject *mo)
{
    int index = mo->render_dynamic_index_;

    if (index < 0)
        return;

    MapObject *last = dynamic_things.back();

    dynamic_things[(size_t)index] = last;
    last->render_dynamic_index_   = index;

    dynamic_things.pop_back();

    mo->render_dynamic_index_ = -1;
}

static void MarkResidentSlot(ResidentBatch &batch, int slot)
{
    batch.dirty_low  = HMM_MIN(batch.dirty_low, slot);
    batch.dirty_high = HMM_MAX(batch.dirty_high, slot);
}

static void ResidentThingRemove(MapObject *mo)
{
    if (mo->render_resident_batch_ < 0)
        return;

    ResidentBatch &batch = resident_batches[(size_t)mo->render_resident_batch_];

    int slot = mo->render_resident_slot_;

    SpriteInstance &instance = batch.instances[(size_t)slot];

    instance.origin[3] = instance.origin[2];
    instance.extent[0] = 0.0f;
    instance.extent[1] = 0.0f;

    batch.owners[(size_t)slot] = nullptr;
    batch.free_slots.push_back(slot);
    batch.live--;

    MarkResidentSlot(batch, slot);

    mo->render_resident_batch_ = -1;
    mo->render_resident_slot_  = -1;
}

static size_t ResidentBatchFor(const SpriteBatchKey &key)
{
    uint64_t probe = HashSpriteBatchKey(key);

    for (;;)
    {
        std::unordered_map<uint64_t, size_t>::iterator found = resident_batch_index.find(probe);

        if (found == resident_batch_index.end())
        {
            resident_batches.emplace_back();

            ResidentBatch &batch = resident_batches.back();

            batch.key         = key;
            batch.transparent = (key.blending & kBlendingNoZBuffer) || (key.blending & kBlendingAlpha);

            resident_batch_index[probe] = resident_batches.size() - 1;

            return resident_batches.size() - 1;
        }

        if (SpriteBatchKeysMatch(resident_batches[found->second].key, key))
            return found->second;

        probe++;
    }
}

static void ResidentThingInsert(MapObject *mo, const SpriteBatchKey &key, const SpriteInstance &instance)
{
    size_t batch_index = ResidentBatchFor(key);

    ResidentBatch &batch = resident_batches[batch_index];

    int slot;

    if (!batch.free_slots.empty())
    {
        slot = batch.free_slots.back();
        batch.free_slots.pop_back();

        batch.instances[(size_t)slot] = instance;
        batch.owners[(size_t)slot]    = mo;
    }
    else
    {
        slot = (int)batch.instances.size();

        batch.instances.push_back(instance);
        batch.owners.push_back(mo);
    }

    batch.live++;

    MarkResidentSlot(batch, slot);

    mo->render_resident_batch_ = (int)batch_index;
    mo->render_resident_slot_  = slot;
}

static bool BuildResidentInstance(MapObject *mo, SpriteInstance *instance, SpriteBatchKey *key)
{
    if (mo->IsRemoved() || !mo->sector_ || !mo->state_ || mo->state_->sprite == 0)
        return false;

    if (mo == view_camera_map_object)
        return false;

    if (mo->state_->flags & kStateFrameFlagModel)
        return false;

    if ((mo->flags_ & (kMapObjectFlagFuzzy | kMapObjectFlagNoSector)) || (mo->hyper_flags_ & kHyperFlagHover))
        return false;

    if (mo->interpolation_number_ > 0 || epi::AlmostEquals(mo->visibility_, 0.0f))
        return false;

    Sector *sector = mo->sector_;

    if (sector->extrafloor_used || sector->height_sector)
        return false;

    if (epi::AlmostEquals(mo->z, sector->floor_height) && !(mo->flags_ & kMapObjectFlagNoGravity) &&
        (sector->sink_depth > 0 || sector->bob_depth > 0))
        return false;

    SpriteFrame *frame = GetSpriteFrame(mo->state_->sprite, mo->state_->frame);

    if (!frame || frame->rotations_ >= 8 || !frame->images_[0])
        return false;

    RegionProperties *properties = GetPointProperties(sector, mo->z + mo->height_ * 0.5f);

    if (properties != &sector->properties)
        return false;

    SpriteSource source;

    source.map_object = mo;
    source.image      = frame->images_[0];
    source.properties = properties;
    source.map_x      = mo->x;
    source.map_y      = mo->y;
    source.map_z      = mo->z;
    source.floor_z    = mo->floor_z_;
    source.hover_dz   = 0.0f;
    source.sink_mult  = 0.0f;
    source.flip       = frame->flip_[0] ? true : false;
    source.resident   = true;

    return BuildSpriteInstance(source, instance, key);
}

static void AddResidentCandidate(MapObject *mo)
{
    if (mo->render_candidate_)
        return;

    mo->render_candidate_ = true;

    resident_candidates.push_back(mo);
}

static void ClearResidentBatches(void)
{
    for (size_t i = 0; i < resident_batches.size(); i++)
    {
        if (resident_batches[i].buffer)
            DeleteStaticVertexBuffer(resident_batches[i].buffer);
    }

    resident_batches.clear();
    resident_batch_index.clear();
}

void ResidentThingsReset(void)
{
    ClearResidentBatches();

    resident_invalid = false;

    dynamic_things.clear();
    resident_candidates.clear();

    for (MapObject *mo = map_object_list_head; mo; mo = mo->next_)
    {
        mo->render_dynamic_index_  = -1;
        mo->render_resident_batch_ = -1;
        mo->render_resident_slot_  = -1;
        mo->render_candidate_      = false;
        mo->render_hash_           = HashThingRenderState(mo);
        mo->render_look_hash_      = HashThingLookState(mo);
        mo->render_quiet_tics_     = 2;

        DynamicThingAdd(mo);

        if (!mo->IsRemoved())
            AddResidentCandidate(mo);
    }

    resident_sector_total = total_level_sectors;

    resident_signature = CurrentResidentSignature();

    resident_ready = true;
}

void ThingRenderTic(MapObject *mo)
{
    if (!resident_ready)
        return;

    uint64_t hash = HashThingRenderState(mo);

    if (hash != mo->render_hash_)
    {
        mo->render_hash_       = hash;
        mo->render_look_hash_  = HashThingLookState(mo);
        mo->render_quiet_tics_ = 0;

        if (mo->render_resident_batch_ >= 0)
        {
            ResidentThingRemove(mo);
            DynamicThingAdd(mo);
        }

        return;
    }

    uint64_t look_hash = HashThingLookState(mo);

    if (look_hash != mo->render_look_hash_)
    {
        mo->render_look_hash_ = look_hash;

        if (mo->render_resident_batch_ >= 0)
            AddResidentCandidate(mo);
    }

    if (mo->render_quiet_tics_ >= 2)
        return;

    mo->render_quiet_tics_++;

    if (mo->render_quiet_tics_ == 2 && mo->render_resident_batch_ < 0)
        AddResidentCandidate(mo);
}

void ThingRenderSpawned(MapObject *mo)
{
    if (!resident_ready)
        return;

    mo->render_hash_       = HashThingRenderState(mo);
    mo->render_look_hash_  = HashThingLookState(mo);
    mo->render_quiet_tics_ = 0;

    DynamicThingAdd(mo);
}

void ThingRenderRemoved(MapObject *mo)
{
    if (!resident_ready)
        return;

    if (mo->render_resident_batch_ >= 0)
    {
        ResidentThingRemove(mo);
        DynamicThingAdd(mo);
    }
}

void ThingRenderDeleted(MapObject *mo)
{
    ResidentThingRemove(mo);
    DynamicThingRemove(mo);

    if (mo->render_candidate_)
    {
        for (size_t i = 0; i < resident_candidates.size(); i++)
        {
            if (resident_candidates[i] == mo)
            {
                resident_candidates[i] = resident_candidates.back();
                resident_candidates.pop_back();
                break;
            }
        }

        mo->render_candidate_ = false;
    }
}

void ResidentThingsInvalidate(void)
{
    resident_invalid = true;
}

static void RebuildResidentThing(MapObject *mo)
{
    SpriteInstance instance;
    SpriteBatchKey key;

    bool built = BuildResidentInstance(mo, &instance, &key);

    if (mo->render_resident_batch_ >= 0)
    {
        ResidentBatch &batch = resident_batches[(size_t)mo->render_resident_batch_];

        if (built && SpriteBatchKeysMatch(batch.key, key))
        {
            batch.instances[(size_t)mo->render_resident_slot_] = instance;

            MarkResidentSlot(batch, mo->render_resident_slot_);
            return;
        }

        ResidentThingRemove(mo);
        DynamicThingAdd(mo);
    }

    if (!built)
        return;

    ResidentThingInsert(mo, key, instance);

    DynamicThingRemove(mo);
}

static void UploadResidentBatches(void);

static void ApplyResidentSectorChanges(void)
{
    if (!resident_ready || resident_sector_total != total_level_sectors)
        return;

    const std::vector<StaticSectorChange> &changes = StaticSectorChanges();

    if (changes.empty())
        return;

    for (size_t i = 0; i < changes.size(); i++)
    {
        Sector *sector = level_sectors + changes[i].sector;

        for (MapObject *mo = sector->thing_list; mo; mo = mo->sector_next_)
        {
            if (mo->render_resident_batch_ >= 0)
                RebuildResidentThing(mo);
        }
    }

    UploadResidentBatches();
}

static void UpdateResidentThings(void)
{
    if (!resident_ready)
        return;

    if (resident_invalid || resident_sector_total != total_level_sectors ||
        !ResidentSignaturesMatch(resident_signature, CurrentResidentSignature()))
    {
        ResidentThingsReset();
    }

    for (size_t i = 0; i < resident_candidates.size(); i++)
    {
        MapObject *mo = resident_candidates[i];

        mo->render_candidate_ = false;

        if (mo->render_quiet_tics_ < 2)
            continue;

        RebuildResidentThing(mo);
    }

    resident_candidates.clear();

    UploadResidentBatches();
}

static void UploadResidentBatches(void)
{
    for (size_t i = 0; i < resident_batches.size(); i++)
    {
        ResidentBatch &batch = resident_batches[i];

        if (batch.dirty_high < batch.dirty_low)
            continue;

        int count = (int)batch.instances.size();

        if (!batch.buffer || batch.buffer_capacity < count)
        {
            if (batch.buffer)
                DeleteStaticVertexBuffer(batch.buffer);

            batch.buffer_capacity = HMM_MAX(256, count + count / 2);
            batch.buffer          = CreateSpriteInstanceBuffer(batch.instances.data(), count, batch.buffer_capacity);
        }
        else
        {
            UpdateSpriteInstanceBuffer(batch.buffer, batch.dirty_low, batch.instances.data() + batch.dirty_low,
                                       batch.dirty_high - batch.dirty_low + 1);
        }

        batch.dirty_low  = INT_MAX;
        batch.dirty_high = -1;
    }

    FlushStaticVertexUploads();
}

static void DrawResidentBatches(bool transparent)
{
    if (!resident_ready)
        return;

    for (size_t i = 0; i < resident_batches.size(); i++)
    {
        const ResidentBatch &batch = resident_batches[i];

        if (batch.live <= 0 || !batch.buffer || batch.transparent != transparent)
            continue;

        const SpriteBatchKey &key = batch.key;

        AddSpriteRenderUnit(0, (int)batch.instances.size(), key.texture, key.fuzz_texture, key.blending,
                            key.fog_color, key.fog_density, key.color_lookup, key.world_lit, key.glow_set,
                            key.light_table, key.alpha, batch.buffer);
    }
}

static bool render_thing_needs_transparent = false;

static bool RenderThing(DrawThing *dthing, bool solid)
{
    ec_frame_stats.draw_things++;

    if (dthing->is_model)
    {
        bool             is_solid = true;
        MapObject       *mo       = dthing->map_object;
        ModelDefinition *md       = GetModel(mo->state_->sprite);
        const Image     *skin_img = md->skins_[mo->model_skin_];

        if ((mo->flags_ & kMapObjectFlagFuzzy) || (mo->visibility_ < 0.99f) ||
            (skin_img && skin_img->opacity_ == kOpacityComplex) || mo->hyper_flags_ & kHyperFlagNoZBufferUpdate)
        {
            is_solid = false;
        }

        if (solid == is_solid)
        {
            RenderModel(dthing);
            return is_solid;
        }

        if (solid)
            render_thing_needs_transparent = true;

        return is_solid;
    }

    SpriteSource source;

    source.map_object = dthing->map_object;
    source.image      = dthing->image;
    source.properties = dthing->properties;
    source.map_x      = dthing->map_x;
    source.map_y      = dthing->map_y;
    source.map_z      = dthing->map_z;
    source.floor_z    = dthing->floor_z;
    source.hover_dz   = dthing->hover_dz;
    source.sink_mult  = dthing->sink_mult;
    source.flip       = dthing->flip;
    source.resident   = false;

    SpriteInstance instance;
    SpriteBatchKey key;

    if (!BuildSpriteInstance(source, &instance, &key))
        return false;

    bool transparent = (key.blending & kBlendingNoZBuffer) || (key.blending & kBlendingAlpha);

    if (solid)
    {
        if (transparent)
        {
            render_thing_needs_transparent = true;
            return false;
        }
    }
    else if (!transparent)
    {
        return false;
    }

    AddSpriteToBatch(key, instance);

    return solid;
}

void EnumerateViewThings(void)
{
    if (active_mirror_set.TotalActive() == 0)
        UpdateResidentThings();

    if (resident_ready)
    {
        for (size_t i = 0; i < dynamic_things.size(); i++)
        {
            MapObject *mo = dynamic_things[i];

            if (mo->IsRemoved() || !mo->sector_)
                continue;

            if (!ThingSectorReached(mo))
                continue;

            BSPWalkThing(mo);
        }

        return;
    }

    for (MapObject *mo = map_object_list_head; mo; mo = mo->next_)
    {
        if (mo->IsRemoved() || !mo->sector_)
            continue;

        if (!ThingSectorReached(mo))
            continue;

        BSPWalkThing(mo);
    }
}

static void SetSpriteViewParameters(void)
{
    float skew = (mirror_view.xy_scale >= 0.99f) ? sprite_skew : 0.0f;

    render_unit_sprite_view[0] = HMM_V4(mirror_view.sprite_right.X, mirror_view.sprite_right.Y,
                                        mirror_view.sprite_forward.X * skew, mirror_view.sprite_forward.Y * skew);
    render_unit_sprite_view[1] =
        HMM_V4(mirror_view.reflective ? 1.0f : 0.0f, (float)render_view_extra_light, 0.0f, 0.0f);
}

void RenderThings(std::vector<DrawThing *> &things, std::vector<DrawThing *> &transparent_things)
{
    SetSpriteViewParameters();

    if (active_mirror_set.TotalActive() == 0)
        ApplyResidentSectorChanges();

    transparent_things.clear();

    for (std::vector<DrawThing *>::iterator it = things.begin(); it != things.end(); it++)
    {
        render_thing_needs_transparent = false;

        RenderThing(*it, true);

        if (render_thing_needs_transparent)
            transparent_things.push_back(*it);
    }

    FlushSpriteBatches();

    DrawResidentBatches(false);
}

void RenderTransparentThings(const std::vector<DrawThing *> &transparent_things, bool models)
{
    SetSpriteViewParameters();

    for (size_t i = 0; i < transparent_things.size(); i++)
    {
        if (transparent_things[i]->is_model == models)
            RenderThing(transparent_things[i], false);
    }

    FlushSpriteBatches();

    if (!models)
        DrawResidentBatches(true);
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
