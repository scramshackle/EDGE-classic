#include "gles2_program.h"

#include <string.h>

#include <vector>

#include "epi.h"
#include "epi_math.h"
#include "epi_vector.h"
#include "gles2_lights.h"
#include "i_system.h"
#include "r_backend.h"
#include "r_colormap.h"
#include "r_lightgrid.h"
#include "shaders/model_glsl.h"
#include "shaders/movie_glsl.h"
#include "shaders/oit_glsl.h"
#include "shaders/world_glsl.h"

Gles2Program      gles2_program;
Gles2ModelProgram gles2_model_program;
Gles2MovieProgram gles2_movie_program;
Gles2OitProgram   gles2_oit_program;

static constexpr int kColorLookupTiles = 8;

static GLuint color_lookup_textures[kColorLookupMaximum] = {};
static GLuint color_lookup_bound                         = 0;

static void BindColorLookupTexture(GLuint texture)
{
    GLint previous_unit = GL_TEXTURE0;

    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_unit);

    glActiveTexture(GL_TEXTURE0 + kGles2TextureUnitColorLookup);
    glBindTexture(GL_TEXTURE_2D, texture);
    glActiveTexture((GLenum)previous_unit);

    color_lookup_bound = texture;
}

static bool UseColorLookup(int slot)
{
    GLuint texture = (slot > 0 && slot < kColorLookupMaximum) ? color_lookup_textures[slot] : 0;

    if (texture != 0 && texture != color_lookup_bound)
        BindColorLookupTexture(texture);

    return texture != 0;
}

void Gles2UploadColorLookup(int slot, const uint8_t *pixels)
{
    if (slot <= 0 || slot >= kColorLookupMaximum || !pixels)
        return;

    int atlas_size = kColorLookupSize * kColorLookupTiles;

    std::vector<uint8_t> packed((size_t)atlas_size * atlas_size * 4);

    for (int b = 0; b < kColorLookupSize; b++)
    {
        int tile_x = (b % kColorLookupTiles) * kColorLookupSize;
        int tile_y = (b / kColorLookupTiles) * kColorLookupSize;

        for (int g = 0; g < kColorLookupSize; g++)
        {
            const uint8_t *source = pixels + ((size_t)b * kColorLookupSize + g) * kColorLookupSize * 4;
            uint8_t       *dest   = packed.data() + ((size_t)(tile_y + g) * atlas_size + tile_x) * 4;

            memcpy(dest, source, (size_t)kColorLookupSize * 4);
        }
    }

    if (color_lookup_textures[slot] == 0)
        glGenTextures(1, &color_lookup_textures[slot]);

    GLint previous_unit = GL_TEXTURE0;

    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_unit);

    glActiveTexture(GL_TEXTURE0 + kGles2TextureUnitColorLookup);
    glBindTexture(GL_TEXTURE_2D, color_lookup_textures[slot]);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas_size, atlas_size, 0, GL_RGBA, GL_UNSIGNED_BYTE, packed.data());

    glActiveTexture((GLenum)previous_unit);

    color_lookup_bound = color_lookup_textures[slot];
}

static GLuint CompileStage(GLenum stage, const char *source, const char *label, const char *defines = "")
{
    GLuint shader = glCreateShader(stage);

    if (!shader)
    {
        FatalError("Gles2Program: glCreateShader failed for %s\n", label);
    }

    const char *strings[3] = {Gles2ShaderPreamble(stage == GL_FRAGMENT_SHADER), defines, source};

    glShaderSource(shader, 3, strings, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    if (compiled != GL_TRUE)
    {
        GLint log_length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
        {
            glGetShaderInfoLog(shader, log_length, nullptr, log);
        }

        FatalError("Gles2Program: %s failed to compile:\n%s\n", label, log);
    }

    return shader;
}

void Gles2Program::LinkVariant(int32_t variant, const char *defines)
{
    GLuint vertex_shader   = CompileStage(GL_VERTEX_SHADER, kWorldVertexSource, "world.vert.glsl");
    GLuint fragment_shader = CompileStage(GL_FRAGMENT_SHADER, kWorldFragmentSource, "world.frag.glsl", defines);

    GLuint program = glCreateProgram();

    if (!program)
    {
        FatalError("Gles2Program: glCreateProgram failed\n");
    }

    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);

    glBindAttribLocation(program, kGles2AttributePosition, "a_position");
    glBindAttribLocation(program, kGles2AttributeTextureCoordinates, "a_texture_coordinates");
    glBindAttribLocation(program, kGles2AttributeColor, "a_color");
    glBindAttribLocation(program, kGles2AttributeSpriteOrigin, "a_sprite_origin");
    glBindAttribLocation(program, kGles2AttributeSpriteExtent, "a_sprite_extent");
    glBindAttribLocation(program, kGles2AttributeSpriteFuzz, "a_sprite_fuzz");
    glBindAttribLocation(program, kGles2AttributeSpriteLight, "a_sprite_light");

    glLinkProgram(program);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint log_length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
        {
            glGetProgramInfoLog(program, log_length, nullptr, log);
        }

        FatalError("Gles2Program: world program failed to link:\n%s\n", log);
    }

    glDetachShader(program, vertex_shader);
    glDetachShader(program, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    locations_.model_view_projection = glGetUniformLocation(program, "u_model_view_projection");
    locations_.model_view            = glGetUniformLocation(program, "u_model_view");
    locations_.texture0              = glGetUniformLocation(program, "u_texture0");
    locations_.texture1              = glGetUniformLocation(program, "u_texture1");
    locations_.multi_texture         = glGetUniformLocation(program, "u_multi_texture");
    locations_.light_falloff         = glGetUniformLocation(program, "u_light_falloff");
    locations_.world_lit             = glGetUniformLocation(program, "u_world_lit");
    locations_.light_data            = glGetUniformLocation(program, "u_light_data");
    locations_.light_headers         = glGetUniformLocation(program, "u_light_headers");
    locations_.light_indices         = glGetUniformLocation(program, "u_light_indices");
    locations_.light_list            = glGetUniformLocation(program, "u_light_list");
    locations_.light_view            = glGetUniformLocation(program, "u_light_view");
    locations_.light_cluster         = glGetUniformLocation(program, "u_light_cluster");
    locations_.light_bounds_min      = glGetUniformLocation(program, "u_light_bounds_min");
    locations_.light_bounds_range    = glGetUniformLocation(program, "u_light_bounds_range");
    locations_.light_radius_scale    = glGetUniformLocation(program, "u_light_radius_scale");
    locations_.light_data_step       = glGetUniformLocation(program, "u_light_data_step");
    locations_.glow_count            = glGetUniformLocation(program, "u_glow_count");
    locations_.glow_plane            = glGetUniformLocation(program, "u_glow_plane");
    locations_.glow_color            = glGetUniformLocation(program, "u_glow_color");
    locations_.glow_additive         = glGetUniformLocation(program, "u_glow_additive");
    locations_.line_mode             = glGetUniformLocation(program, "u_line_mode");
    locations_.skip_rgb              = glGetUniformLocation(program, "u_skip_rgb");
    locations_.alpha_test            = glGetUniformLocation(program, "u_alpha_test");
    locations_.fog_mode              = glGetUniformLocation(program, "u_fog_mode");
    locations_.fog_color             = glGetUniformLocation(program, "u_fog_color");
    locations_.fog_density           = glGetUniformLocation(program, "u_fog_density");
    locations_.fog_start             = glGetUniformLocation(program, "u_fog_start");
    locations_.fog_end               = glGetUniformLocation(program, "u_fog_end");

    locations_.sky_pass               = glGetUniformLocation(program, "u_sky_pass");
    locations_.oit_mode               = glGetUniformLocation(program, "u_oit_mode");
    locations_.oit_scale              = glGetUniformLocation(program, "u_oit_scale");
    locations_.color_lookup           = glGetUniformLocation(program, "u_color_lookup");
    locations_.color_lookup_enabled   = glGetUniformLocation(program, "u_color_lookup_enabled");
    locations_.whiten                 = glGetUniformLocation(program, "u_whiten");
    locations_.blur                   = glGetUniformLocation(program, "u_blur");
    locations_.texture_offset         = glGetUniformLocation(program, "u_texture_offset");
    locations_.light_row_offset       = glGetUniformLocation(program, "u_light_row_offset");
    locations_.sprite_mode            = glGetUniformLocation(program, "u_sprite_mode");
    locations_.sprite_whites          = glGetUniformLocation(program, "u_sprite_whites");
    locations_.sprite_lighting        = glGetUniformLocation(program, "u_sprite_lighting");
    locations_.sprite_view0           = glGetUniformLocation(program, "u_sprite_view0");
    locations_.sprite_view1           = glGetUniformLocation(program, "u_sprite_view1");
    locations_.liquid                 = glGetUniformLocation(program, "u_liquid");
    locations_.light_depth            = glGetUniformLocation(program, "u_light_depth");
    locations_.view_tint              = glGetUniformLocation(program, "u_view_tint");
    locations_.sky_fog_depth          = glGetUniformLocation(program, "u_sky_fog_depth");
    locations_.sky_inverse_projection = glGetUniformLocation(program, "u_sky_inverse_projection");
    locations_.sky_inverse_view       = glGetUniformLocation(program, "u_sky_inverse_view");
    locations_.sky_viewport           = glGetUniformLocation(program, "u_sky_viewport");
    locations_.sky_stretch_mode       = glGetUniformLocation(program, "u_sky_stretch_mode");
    locations_.sky_u_scale            = glGetUniformLocation(program, "u_sky_u_scale");
    locations_.sky_ty                 = glGetUniformLocation(program, "u_sky_ty");
    locations_.sky_u_offset           = glGetUniformLocation(program, "u_sky_u_offset");
    locations_.sky_v_offset           = glGetUniformLocation(program, "u_sky_v_offset");
    locations_.sky_band               = glGetUniformLocation(program, "u_sky_band");
    locations_.sky_fade_exponent      = glGetUniformLocation(program, "u_sky_fade_exponent");
    locations_.sky_cap_top            = glGetUniformLocation(program, "u_sky_cap_top");
    locations_.sky_cap_bottom         = glGetUniformLocation(program, "u_sky_cap_bottom");
    locations_.sky_horizon_shift      = glGetUniformLocation(program, "u_sky_horizon_shift");
    locations_.sky_is_box             = glGetUniformLocation(program, "u_sky_is_box");
    locations_.sky_geometry           = glGetUniformLocation(program, "u_sky_geometry");
    locations_.sky_cube               = glGetUniformLocation(program, "u_sky_cube");

    glUseProgram(program);

    glUniform1i(locations_.texture0, kGles2TextureUnit0);
    glUniform1i(locations_.texture1, kGles2TextureUnit1);
    glUniform1i(locations_.sky_cube, kGles2TextureUnitSkyCube);
    glUniform1i(locations_.light_data, kGles2TextureUnitLightData);
    glUniform1i(locations_.light_headers, kGles2TextureUnitLightHeaders);
    glUniform1i(locations_.light_indices, kGles2TextureUnitLightIndices);
    glUniform1i(locations_.color_lookup, kGles2TextureUnitColorLookup);

    programs_[variant]          = program;
    variant_locations_[variant] = locations_;
}

bool Gles2Program::Init()
{
    static const char *variant_defines[kGles2WorldVariants] = {
        "",
        "#define EDGE_NO_DISCARD 1\n",
        "#define EDGE_NO_EFFECTS 1\n",
        "#define EDGE_NO_EFFECTS 1\n#define EDGE_NO_DISCARD 1\n",
        "#define EDGE_NO_EFFECTS 1\n#define EDGE_NO_LIGHTS 1\n",
        "#define EDGE_NO_EFFECTS 1\n#define EDGE_NO_LIGHTS 1\n#define EDGE_NO_DISCARD 1\n"};

    for (int32_t i = kGles2WorldVariants - 1; i >= 0; i--)
        LinkVariant(i, variant_defines[i]);

    variant_ = 0;

    return true;
}

void Gles2Program::Shutdown()
{
    for (int32_t i = 0; i < kGles2WorldVariants; i++)
    {
        if (programs_[i])
        {
            glDeleteProgram(programs_[i]);
            programs_[i] = 0;
        }
    }
}

void Gles2Program::Use()
{
    glUseProgram(programs_[variant_]);
}

void Gles2Program::SelectVariant(bool opaque)
{
    bool effects = shadow_blur_.x > 0.0f || shadow_liquid_.x > 0.5f || shadow_color_lookup_enabled_ > 0.5f ||
                   shadow_whiten_ > 0.5f;

    bool lights = shadow_world_lit_ > 0.5f || shadow_glow_active_;

    int32_t variant = 0;

    if (!effects)
        variant = lights ? 2 : 4;

    if (opaque)
        variant++;

    if (variant == variant_)
        return;

    variant_   = variant;
    locations_ = variant_locations_[variant];

    glUseProgram(programs_[variant]);

    ResendUniforms();
}

void Gles2Program::ResendUniforms()
{
    glUniformMatrix4fv(locations_.model_view_projection, 1, GL_FALSE, (const GLfloat *)&shadow_model_view_projection_);
    glUniformMatrix4fv(locations_.model_view, 1, GL_FALSE, (const GLfloat *)&shadow_model_view_);

    glUniform1f(locations_.multi_texture, shadow_multi_texture_);
    glUniform1f(locations_.light_falloff, shadow_light_falloff_);
    glUniform1f(locations_.world_lit, shadow_world_lit_);
    glUniform1f(locations_.line_mode, shadow_line_mode_);
    glUniform1f(locations_.skip_rgb, shadow_skip_rgb_);
    glUniform1f(locations_.alpha_test, shadow_alpha_test_);
    glUniform1f(locations_.fog_mode, shadow_fog_mode_);
    glUniform1f(locations_.fog_density, shadow_fog_density_);
    glUniform1f(locations_.fog_start, shadow_fog_start_);
    glUniform1f(locations_.fog_end, shadow_fog_end_);
    glUniform4f(locations_.fog_color, shadow_fog_color_[0], shadow_fog_color_[1], shadow_fog_color_[2],
                shadow_fog_color_[3]);
    glUniform1f(locations_.sky_pass, shadow_sky_pass_);
    glUniform1f(locations_.light_depth, shadow_light_depth_);
    glUniform1f(locations_.oit_mode, shadow_oit_mode_);
    glUniform1f(locations_.oit_scale, shadow_oit_scale_);
    glUniform2f(locations_.texture_offset, shadow_texture_offset_.x, shadow_texture_offset_.y);
    glUniform1f(locations_.light_row_offset, shadow_light_row_offset_);
    glUniform1f(locations_.sprite_mode, shadow_sprite_mode_);
    glUniform4f(locations_.sprite_view0, shadow_sprite_view_[0].x, shadow_sprite_view_[0].y, shadow_sprite_view_[0].z,
                shadow_sprite_view_[0].w);
    glUniform4f(locations_.sprite_view1, shadow_sprite_view_[1].x, shadow_sprite_view_[1].y, shadow_sprite_view_[1].z,
                shadow_sprite_view_[1].w);
    glUniform4f(locations_.liquid, shadow_liquid_.x, shadow_liquid_.y, shadow_liquid_.z, shadow_liquid_.w);
    glUniform1f(locations_.color_lookup_enabled, shadow_color_lookup_enabled_);
    glUniform1f(locations_.whiten, shadow_whiten_);
    glUniform4f(locations_.blur, shadow_blur_.x, shadow_blur_.y, shadow_blur_.z, shadow_blur_.w);

    if (locations_.view_tint >= 0)
        glUniform4f(locations_.view_tint, shadow_view_tint_[0], shadow_view_tint_[1], shadow_view_tint_[2], 1.0f);

    if (shadow_sprite_light_table_)
    {
        glUniform4fv(locations_.sprite_whites, kSpriteLightLevels, &shadow_sprite_light_table_->whites[0][0]);
        glUniform4fv(locations_.sprite_lighting, 1, shadow_sprite_light_table_->parameters);
    }

    if (shadow_glow_set_ != -2)
        UploadGlowSet(shadow_glow_set_);

    if (shadow_light_grid_)
        UploadLightGrid(shadow_light_grid_);

    if (shadow_sky_pass_valid_)
        UploadSkyPass(shadow_sky_pass_info_);

    uniform_update_count_ += 32;
}

void Gles2Program::SetFloat(GLint location, float &shadow, float value)
{
    if (epi::AlmostEquals(shadow, value))
    {
        return;
    }

    shadow = value;

    glUniform1f(location, value);

    uniform_update_count_++;
}

void Gles2Program::SetModelViewProjection(const epi::Mat4 &matrix)
{
    if (memcmp(&shadow_model_view_projection_, &matrix, sizeof(epi::Mat4)) == 0)
    {
        return;
    }

    shadow_model_view_projection_ = matrix;

    glUniformMatrix4fv(locations_.model_view_projection, 1, GL_FALSE, (const GLfloat *)&matrix);

    uniform_update_count_++;
}

void Gles2Program::SetModelView(const epi::Mat4 &matrix)
{
    if (memcmp(&shadow_model_view_, &matrix, sizeof(epi::Mat4)) == 0)
    {
        return;
    }

    shadow_model_view_ = matrix;

    glUniformMatrix4fv(locations_.model_view, 1, GL_FALSE, (const GLfloat *)&matrix);

    uniform_update_count_++;
}

void Gles2Program::SetMultiTexture(bool enabled)
{
    SetFloat(locations_.multi_texture, shadow_multi_texture_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetLightFalloff(bool enabled)
{
    SetFloat(locations_.light_falloff, shadow_light_falloff_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetWorldLit(bool enabled)
{
    SetFloat(locations_.world_lit, shadow_world_lit_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetLightGrid(const Gles2LightGridState *grid)
{
    if (!grid || !grid->active)
        return;

    shadow_light_grid_ = grid;

    UploadLightGrid(grid);
}

void Gles2Program::UploadLightGrid(const Gles2LightGridState *grid)
{
    if (!grid->active)
        return;

    glUniform4f(locations_.light_view, grid->view_origin[0], grid->view_origin[1], grid->header_texel_step[0],
                grid->header_texel_step[1]);

    glUniform4f(locations_.light_cluster, grid->cluster[0], grid->cluster[1], grid->cluster[2], grid->cluster[3]);

    glUniform4f(locations_.light_list, grid->list_width, grid->list_texel_step[0], grid->list_texel_step[1], 0.0f);

    glUniform3f(locations_.light_bounds_min, grid->bounds_minimum[0], grid->bounds_minimum[1], grid->bounds_minimum[2]);
    glUniform3f(locations_.light_bounds_range, grid->bounds_range[0], grid->bounds_range[1], grid->bounds_range[2]);

    glUniform1f(locations_.light_radius_scale, grid->radius_scale);
    glUniform1f(locations_.light_data_step, grid->data_texel_step);
}

void Gles2Program::SetGlowSet(int index)
{
    if (shadow_glow_set_ == index)
        return;

    shadow_glow_set_ = index;

    UploadGlowSet(index);
}

void Gles2Program::UploadGlowSet(int index)
{
    const LightGridGlowSet *set = LightGridGlowSetAt(index);

    shadow_glow_active_ = set && set->count > 0;

    if (!shadow_glow_active_)
    {
        glUniform1f(locations_.glow_count, 0.0f);
        return;
    }

    float planes[kLightGridMaximumGlows * 4];
    float colors[kLightGridMaximumGlows * 4];
    float additive[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    for (int i = 0; i < kLightGridMaximumGlows; i++)
    {
        const LightGridGlow &glow = set->glows[i];

        for (int e = 0; e < 4; e++)
            planes[i * 4 + e] = (i < set->count) ? glow.plane[e] : 0.0f;

        colors[i * 4 + 0] = (i < set->count) ? glow.color[0] / 255.0f : 0.0f;
        colors[i * 4 + 1] = (i < set->count) ? glow.color[1] / 255.0f : 0.0f;
        colors[i * 4 + 2] = (i < set->count) ? glow.color[2] / 255.0f : 0.0f;
        colors[i * 4 + 3] = (i < set->count) ? glow.radius : 1.0f;

        if (i < 4)
            additive[i] = (i < set->count) ? glow.additive : 0.0f;
    }

    glUniform4fv(locations_.glow_plane, kLightGridMaximumGlows, planes);
    glUniform4fv(locations_.glow_color, kLightGridMaximumGlows, colors);
    glUniform4fv(locations_.glow_additive, 1, additive);

    glUniform1f(locations_.glow_count, (float)set->count);
}

void Gles2Program::SetLineMode(bool enabled)
{
    SetFloat(locations_.line_mode, shadow_line_mode_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetSkipRGB(bool enabled)
{
    SetFloat(locations_.skip_rgb, shadow_skip_rgb_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetAlphaTest(float reference)
{
    SetFloat(locations_.alpha_test, shadow_alpha_test_, reference);
}

void Gles2Program::SetViewTint(float r, float g, float b)
{
    if (locations_.view_tint < 0)
        return;

    if (epi::AlmostEquals(shadow_view_tint_[0], r) && epi::AlmostEquals(shadow_view_tint_[1], g) &&
        epi::AlmostEquals(shadow_view_tint_[2], b))
        return;

    shadow_view_tint_[0] = r;
    shadow_view_tint_[1] = g;
    shadow_view_tint_[2] = b;

    glUniform4f(locations_.view_tint, r, g, b, 1.0f);

    uniform_update_count_++;
}

void Gles2Program::SetLightDepth(bool enabled)
{
    SetFloat(locations_.light_depth, shadow_light_depth_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetOit(float mode, float scale)
{
    SetFloat(locations_.oit_mode, shadow_oit_mode_, mode);
    SetFloat(locations_.oit_scale, shadow_oit_scale_, scale);
}

void Gles2Program::SetTextureOffset(const epi::Vec2 &offset)
{
    if (epi::AlmostEquals(shadow_texture_offset_.x, offset.x) && epi::AlmostEquals(shadow_texture_offset_.y, offset.y))
        return;

    shadow_texture_offset_ = offset;

    glUniform2f(locations_.texture_offset, offset.x, offset.y);
}

void Gles2Program::SetLightRowOffset(float offset)
{
    SetFloat(locations_.light_row_offset, shadow_light_row_offset_, offset);
}

void Gles2Program::SetSpriteMode(bool enabled)
{
    SetFloat(locations_.sprite_mode, shadow_sprite_mode_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetSpriteLightTable(const SpriteLightTable *light_table)
{
    if (!light_table || light_table == shadow_sprite_light_table_)
        return;

    shadow_sprite_light_table_ = light_table;

    glUniform4fv(locations_.sprite_whites, kSpriteLightLevels, &light_table->whites[0][0]);
    glUniform4fv(locations_.sprite_lighting, 1, light_table->parameters);

    uniform_update_count_++;
}

void Gles2Program::SetSpriteView(const epi::Vec4 view[2])
{
    if (!memcmp(shadow_sprite_view_, view, sizeof(shadow_sprite_view_)))
        return;

    shadow_sprite_view_[0] = view[0];
    shadow_sprite_view_[1] = view[1];

    glUniform4f(locations_.sprite_view0, view[0].x, view[0].y, view[0].z, view[0].w);
    glUniform4f(locations_.sprite_view1, view[1].x, view[1].y, view[1].z, view[1].w);

    uniform_update_count_++;
}

void Gles2Program::SetLiquid(const epi::Vec4 &liquid)
{
    if (epi::AlmostEquals(shadow_liquid_.x, liquid.x) && epi::AlmostEquals(shadow_liquid_.y, liquid.y) &&
        epi::AlmostEquals(shadow_liquid_.z, liquid.z) && epi::AlmostEquals(shadow_liquid_.w, liquid.w))
        return;

    shadow_liquid_ = liquid;

    glUniform4f(locations_.liquid, liquid.x, liquid.y, liquid.z, liquid.w);
}

void Gles2Program::SetColorLookup(int slot)
{
    SetFloat(locations_.color_lookup_enabled, shadow_color_lookup_enabled_, UseColorLookup(slot) ? 1.0f : 0.0f);
}

void Gles2Program::SetWhiten(bool enabled)
{
    SetFloat(locations_.whiten, shadow_whiten_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetBlur(const epi::Vec4 &blur)
{
    if (epi::AlmostEquals(shadow_blur_.x, blur.x) && epi::AlmostEquals(shadow_blur_.z, blur.z) &&
        epi::AlmostEquals(shadow_blur_.w, blur.w))
        return;

    shadow_blur_ = blur;

    glUniform4f(locations_.blur, blur.x, blur.y, blur.z, blur.w);
}

void Gles2Program::ForceOitReset()
{
    shadow_oit_mode_ = 0.0f;

    glUniform1f(locations_.oit_mode, 0.0f);
}

void Gles2Program::SetSkyPass(const SkyPassInfo *sky_pass)
{
    if (!sky_pass)
    {
        SetFloat(locations_.sky_pass, shadow_sky_pass_, 0.0f);
        return;
    }

    if (sky_pass->is_box && sky_pass->cube_texture)
    {
        glActiveTexture(GL_TEXTURE0 + kGles2TextureUnitSkyCube);
        glBindTexture(GL_TEXTURE_CUBE_MAP, sky_pass->cube_texture);
        glActiveTexture(GL_TEXTURE0 + kGles2TextureUnit0);
    }

    SetFloat(locations_.sky_pass, shadow_sky_pass_, 1.0f);

    shadow_sky_pass_info_  = *sky_pass;
    shadow_sky_pass_valid_ = true;

    UploadSkyPass(*sky_pass);
}

void Gles2Program::UploadSkyPass(const SkyPassInfo &sky_pass)
{
    glUniform1f(locations_.sky_is_box, sky_pass.is_box ? 1.0f : 0.0f);
    glUniform1f(locations_.sky_geometry, sky_pass.is_geometry ? 1.0f : 0.0f);

    glUniformMatrix4fv(locations_.sky_inverse_projection, 1, GL_FALSE, (const GLfloat *)&sky_pass.inverse_projection);
    glUniformMatrix4fv(locations_.sky_inverse_view, 1, GL_FALSE, (const GLfloat *)&sky_pass.inverse_view);

    float scale_x = render_backend->ActiveScaleX();
    float scale_y = render_backend->ActiveScaleY();

    glUniform4f(locations_.sky_viewport, sky_pass.viewport_origin.x * scale_x, sky_pass.viewport_origin.y * scale_y,
                sky_pass.viewport_size.x * scale_x, sky_pass.viewport_size.y * scale_y);

    glUniform1f(locations_.sky_stretch_mode, (float)sky_pass.stretch_mode);
    glUniform1f(locations_.sky_u_scale, sky_pass.u_scale);
    glUniform1f(locations_.sky_ty, sky_pass.ty);
    glUniform1f(locations_.sky_u_offset, sky_pass.u_offset);
    glUniform1f(locations_.sky_v_offset, sky_pass.v_offset);
    glUniform4f(locations_.sky_band, sky_pass.band_top.x, sky_pass.band_top.y, sky_pass.band_bottom.x,
                sky_pass.band_bottom.y);
    glUniform1f(locations_.sky_fade_exponent, sky_pass.fade_exponent);
    glUniform4f(locations_.sky_cap_top, epi::GetRGBARed(sky_pass.cap_top_color) / 255.0f,
                epi::GetRGBAGreen(sky_pass.cap_top_color) / 255.0f, epi::GetRGBABlue(sky_pass.cap_top_color) / 255.0f,
                1.0f);
    glUniform4f(locations_.sky_cap_bottom, epi::GetRGBARed(sky_pass.cap_bottom_color) / 255.0f,
                epi::GetRGBAGreen(sky_pass.cap_bottom_color) / 255.0f,
                epi::GetRGBABlue(sky_pass.cap_bottom_color) / 255.0f, 1.0f);
    glUniform1f(locations_.sky_horizon_shift, sky_pass.horizon_shift);
    glUniform1f(locations_.sky_fog_depth, sky_pass.fog_depth);

    uniform_update_count_ += 11;
}

void Gles2Program::SetFog(Gles2FogMode mode, float red, float green, float blue, float density, float start, float end)
{
    SetFloat(locations_.fog_mode, shadow_fog_mode_, (float)mode);

    if (mode == kGles2FogModeNone)
    {
        return;
    }

    if (!epi::AlmostEquals(shadow_fog_color_[0], red) || !epi::AlmostEquals(shadow_fog_color_[1], green) ||
        !epi::AlmostEquals(shadow_fog_color_[2], blue))
    {
        shadow_fog_color_[0] = red;
        shadow_fog_color_[1] = green;
        shadow_fog_color_[2] = blue;
        shadow_fog_color_[3] = 1.0f;

        glUniform4f(locations_.fog_color, red, green, blue, 1.0f);

        uniform_update_count_++;
    }

    SetFloat(locations_.fog_density, shadow_fog_density_, density);
    SetFloat(locations_.fog_start, shadow_fog_start_, start);
    SetFloat(locations_.fog_end, shadow_fog_end_, end);
}

bool Gles2ModelProgram::Init()
{
    GLuint vertex_shader   = CompileStage(GL_VERTEX_SHADER, kModelVertexSource, "model.vert.glsl");
    GLuint fragment_shader = CompileStage(GL_FRAGMENT_SHADER, kModelFragmentSource, "model.frag.glsl");

    program_ = glCreateProgram();

    if (!program_)
    {
        FatalError("Gles2ModelProgram: glCreateProgram failed\n");
    }

    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);

    glBindAttribLocation(program_, kGles2AttributeModelPositionFrame1, "a_position_frame1");
    glBindAttribLocation(program_, kGles2AttributeModelPositionFrame2, "a_position_frame2");
    glBindAttribLocation(program_, kGles2AttributeModelTextureCoordinates, "a_texture_coordinates");
    glBindAttribLocation(program_, kGles2AttributeModelNormalFrame1, "a_normal_frame1");
    glBindAttribLocation(program_, kGles2AttributeModelNormalFrame2, "a_normal_frame2");

    glLinkProgram(program_);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint log_length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
        {
            glGetProgramInfoLog(program_, log_length, nullptr, log);
        }

        FatalError("Gles2ModelProgram: model program failed to link:\n%s\n", log);
    }

    glDetachShader(program_, vertex_shader);
    glDetachShader(program_, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    uniform_model_view_projection_ = glGetUniformLocation(program_, "u_model_view_projection");
    uniform_model_view_            = glGetUniformLocation(program_, "u_model_view");
    uniform_model_transform_       = glGetUniformLocation(program_, "u_model_transform");
    uniform_world_lit_             = glGetUniformLocation(program_, "u_world_lit");
    uniform_light_data_            = glGetUniformLocation(program_, "u_light_data");
    uniform_light_headers_         = glGetUniformLocation(program_, "u_light_headers");
    uniform_light_indices_         = glGetUniformLocation(program_, "u_light_indices");
    uniform_light_view_            = glGetUniformLocation(program_, "u_light_view");
    uniform_light_cluster_         = glGetUniformLocation(program_, "u_light_cluster");
    uniform_light_list_            = glGetUniformLocation(program_, "u_light_list");
    uniform_light_bounds_min_      = glGetUniformLocation(program_, "u_light_bounds_min");
    uniform_light_bounds_range_    = glGetUniformLocation(program_, "u_light_bounds_range");
    uniform_light_radius_scale_    = glGetUniformLocation(program_, "u_light_radius_scale");
    uniform_light_data_step_       = glGetUniformLocation(program_, "u_light_data_step");
    uniform_glow_count_            = glGetUniformLocation(program_, "u_glow_count");
    uniform_glow_plane_            = glGetUniformLocation(program_, "u_glow_plane");
    uniform_glow_color_            = glGetUniformLocation(program_, "u_glow_color");
    uniform_glow_additive_         = glGetUniformLocation(program_, "u_glow_additive");
    uniform_lerp_                  = glGetUniformLocation(program_, "u_lerp");
    uniform_texture_scale_         = glGetUniformLocation(program_, "u_texture_scale");
    uniform_texture_offset_        = glGetUniformLocation(program_, "u_texture_offset");
    uniform_texture0_              = glGetUniformLocation(program_, "u_texture0");
    uniform_alpha_                 = glGetUniformLocation(program_, "u_alpha");
    uniform_alpha_test_            = glGetUniformLocation(program_, "u_alpha_test");
    uniform_model_whites_          = glGetUniformLocation(program_, "u_model_whites");
    uniform_model_lighting_        = glGetUniformLocation(program_, "u_model_lighting");
    uniform_model_light_           = glGetUniformLocation(program_, "u_model_light");
    uniform_model_tint_            = glGetUniformLocation(program_, "u_model_tint");
    uniform_fog_mode_              = glGetUniformLocation(program_, "u_fog_mode");
    uniform_fog_color_             = glGetUniformLocation(program_, "u_fog_color");
    uniform_fog_density_           = glGetUniformLocation(program_, "u_fog_density");
    uniform_fog_start_             = glGetUniformLocation(program_, "u_fog_start");
    uniform_fog_end_               = glGetUniformLocation(program_, "u_fog_end");
    uniform_oit_mode_              = glGetUniformLocation(program_, "u_oit_mode");
    uniform_oit_scale_             = glGetUniformLocation(program_, "u_oit_scale");
    uniform_color_lookup_          = glGetUniformLocation(program_, "u_color_lookup");
    uniform_color_lookup_enabled_  = glGetUniformLocation(program_, "u_color_lookup_enabled");

    glUseProgram(program_);
    glUniform1i(uniform_texture0_, kGles2TextureUnit0);
    glUniform1i(uniform_light_data_, kGles2TextureUnitLightData);
    glUniform1i(uniform_light_headers_, kGles2TextureUnitLightHeaders);
    glUniform1i(uniform_light_indices_, kGles2TextureUnitLightIndices);
    glUniform1i(uniform_color_lookup_, kGles2TextureUnitColorLookup);

    return true;
}

void Gles2ModelProgram::SetColorLookup(int slot)
{
    SetFloat(uniform_color_lookup_enabled_, shadow_color_lookup_enabled_, UseColorLookup(slot) ? 1.0f : 0.0f);
}

void Gles2ModelProgram::SetWorldLit(bool enabled)
{
    SetFloat(uniform_world_lit_, shadow_world_lit_, enabled ? 1.0f : 0.0f);
}

void Gles2ModelProgram::SetLightGrid(const Gles2LightGridState *grid)
{
    if (!grid || !grid->active)
        return;

    glUniform4f(uniform_light_view_, grid->view_origin[0], grid->view_origin[1], grid->header_texel_step[0],
                grid->header_texel_step[1]);

    glUniform4f(uniform_light_cluster_, grid->cluster[0], grid->cluster[1], grid->cluster[2], grid->cluster[3]);

    glUniform4f(uniform_light_list_, grid->list_width, grid->list_texel_step[0], grid->list_texel_step[1], 0.0f);

    glUniform3f(uniform_light_bounds_min_, grid->bounds_minimum[0], grid->bounds_minimum[1], grid->bounds_minimum[2]);
    glUniform3f(uniform_light_bounds_range_, grid->bounds_range[0], grid->bounds_range[1], grid->bounds_range[2]);

    glUniform1f(uniform_light_radius_scale_, grid->radius_scale);
    glUniform1f(uniform_light_data_step_, grid->data_texel_step);
}

void Gles2ModelProgram::SetGlowSet(int index)
{
    if (shadow_glow_set_ == index)
        return;

    shadow_glow_set_ = index;

    const LightGridGlowSet *set = LightGridGlowSetAt(index);

    if (!set || set->count <= 0)
    {
        glUniform1f(uniform_glow_count_, 0.0f);
        return;
    }

    float planes[kLightGridMaximumGlows * 4];
    float colors[kLightGridMaximumGlows * 4];
    float additive[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    for (int i = 0; i < kLightGridMaximumGlows; i++)
    {
        const LightGridGlow &glow = set->glows[i];

        for (int e = 0; e < 4; e++)
            planes[i * 4 + e] = (i < set->count) ? glow.plane[e] : 0.0f;

        colors[i * 4 + 0] = (i < set->count) ? glow.color[0] / 255.0f : 0.0f;
        colors[i * 4 + 1] = (i < set->count) ? glow.color[1] / 255.0f : 0.0f;
        colors[i * 4 + 2] = (i < set->count) ? glow.color[2] / 255.0f : 0.0f;
        colors[i * 4 + 3] = (i < set->count) ? glow.radius : 1.0f;

        if (i < 4)
            additive[i] = (i < set->count) ? glow.additive : 0.0f;
    }

    glUniform4fv(uniform_glow_plane_, kLightGridMaximumGlows, planes);
    glUniform4fv(uniform_glow_color_, kLightGridMaximumGlows, colors);
    glUniform4fv(uniform_glow_additive_, 1, additive);

    glUniform1f(uniform_glow_count_, (float)set->count);
}

void Gles2ModelProgram::Shutdown()
{
    if (program_)
    {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void Gles2ModelProgram::Use()
{
    glUseProgram(program_);
}

void Gles2ModelProgram::SetFloat(GLint location, float &shadow, float value)
{
    if (epi::AlmostEquals(shadow, value))
    {
        return;
    }

    shadow = value;

    glUniform1f(location, value);
}

void Gles2ModelProgram::SetMatrices(const epi::Mat4 &model_view_projection, const epi::Mat4 &model_view)
{
    glUniformMatrix4fv(uniform_model_view_projection_, 1, GL_FALSE, (const GLfloat *)&model_view_projection);
    glUniformMatrix4fv(uniform_model_view_, 1, GL_FALSE, (const GLfloat *)&model_view);
}

void Gles2ModelProgram::SetTransform(const epi::Mat4 &transform)
{
    glUniformMatrix4fv(uniform_model_transform_, 1, GL_FALSE, (const GLfloat *)&transform);
}

void Gles2ModelProgram::SetLerp(float lerp)
{
    SetFloat(uniform_lerp_, shadow_lerp_, lerp);
}

void Gles2ModelProgram::SetTextureTransform(const epi::Vec2 &scale, const epi::Vec2 &offset)
{
    glUniform2f(uniform_texture_scale_, scale.x, scale.y);
    glUniform2f(uniform_texture_offset_, offset.x, offset.y);
}

void Gles2ModelProgram::SetAlpha(float alpha)
{
    SetFloat(uniform_alpha_, shadow_alpha_, alpha);
}

void Gles2ModelProgram::SetAlphaTest(float reference)
{
    SetFloat(uniform_alpha_test_, shadow_alpha_test_, reference);
}

void Gles2ModelProgram::SetLighting(const ModelDrawInfo &info)
{
    if (info.light_table)
    {
        glUniform4fv(uniform_model_whites_, kSpriteLightLevels, &info.light_table->whites[0][0]);
        glUniform4fv(uniform_model_lighting_, 1, info.light_table->parameters);
    }

    glUniform4f(uniform_model_light_, info.light_level, info.light_fixed_depth, info.light_depth_fixed ? 1.0f : 0.0f,
                info.fuzzy ? 1.0f : 0.0f);
    glUniform4f(uniform_model_tint_, info.tint.x, info.tint.y, info.tint.z, 1.0f);
}

void Gles2ModelProgram::SetOit(float mode, float scale)
{
    SetFloat(uniform_oit_mode_, shadow_oit_mode_, mode);
    SetFloat(uniform_oit_scale_, shadow_oit_scale_, scale);
}

void Gles2ModelProgram::SetFog(Gles2FogMode mode, float red, float green, float blue, float density, float start,
                               float end)
{
    SetFloat(uniform_fog_mode_, shadow_fog_mode_, (float)mode);

    if (mode == kGles2FogModeNone)
    {
        return;
    }

    if (!epi::AlmostEquals(shadow_fog_color_[0], red) || !epi::AlmostEquals(shadow_fog_color_[1], green) ||
        !epi::AlmostEquals(shadow_fog_color_[2], blue))
    {
        shadow_fog_color_[0] = red;
        shadow_fog_color_[1] = green;
        shadow_fog_color_[2] = blue;
        shadow_fog_color_[3] = 1.0f;

        glUniform4f(uniform_fog_color_, red, green, blue, 1.0f);
    }

    SetFloat(uniform_fog_density_, shadow_fog_density_, density);
    SetFloat(uniform_fog_start_, shadow_fog_start_, start);
    SetFloat(uniform_fog_end_, shadow_fog_end_, end);
}

bool Gles2MovieProgram::Init()
{
    GLuint vertex_shader   = CompileStage(GL_VERTEX_SHADER, kMovieVertexSource, "movie.vert.glsl");
    GLuint fragment_shader = CompileStage(GL_FRAGMENT_SHADER, kMovieFragmentSource, "movie.frag.glsl");

    program_ = glCreateProgram();

    if (!program_)
        FatalError("Gles2MovieProgram: glCreateProgram failed\n");

    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);

    glBindAttribLocation(program_, kGles2AttributePosition, "a_position");
    glBindAttribLocation(program_, kGles2AttributeTextureCoordinates, "a_texture_coordinates");

    glLinkProgram(program_);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint log_length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
            glGetProgramInfoLog(program_, log_length, nullptr, log);

        FatalError("Gles2MovieProgram: movie program failed to link:\n%s\n", log);
    }

    glDetachShader(program_, vertex_shader);
    glDetachShader(program_, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    uniform_model_view_   = glGetUniformLocation(program_, "u_model_view");
    uniform_projection_   = glGetUniformLocation(program_, "u_projection");
    uniform_texture_y_    = glGetUniformLocation(program_, "u_texture_y");
    uniform_texture_cb_   = glGetUniformLocation(program_, "u_texture_cb");
    uniform_texture_cr_   = glGetUniformLocation(program_, "u_texture_cr");
    uniform_luma_scale_   = glGetUniformLocation(program_, "u_luma_scale");
    uniform_chroma_scale_ = glGetUniformLocation(program_, "u_chroma_scale");

    glUseProgram(program_);

    glUniform1i(uniform_texture_y_, 0);
    glUniform1i(uniform_texture_cb_, 1);
    glUniform1i(uniform_texture_cr_, 2);

    return true;
}

void Gles2MovieProgram::Shutdown()
{
    if (program_)
    {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void Gles2MovieProgram::Use()
{
    glUseProgram(program_);
}

void Gles2MovieProgram::SetMatrices(const epi::Mat4 &model_view, const epi::Mat4 &projection)
{
    glUniformMatrix4fv(uniform_model_view_, 1, GL_FALSE, (const GLfloat *)&model_view);
    glUniformMatrix4fv(uniform_projection_, 1, GL_FALSE, (const GLfloat *)&projection);
}

void Gles2MovieProgram::SetPlaneScales(float luma_x, float luma_y, float chroma_x, float chroma_y)
{
    glUniform2f(uniform_luma_scale_, luma_x, luma_y);
    glUniform2f(uniform_chroma_scale_, chroma_x, chroma_y);
}

bool Gles2OitProgram::Init()
{
    GLuint vertex_shader   = CompileStage(GL_VERTEX_SHADER, kOitVertexSource, "oit.vert.glsl");
    GLuint fragment_shader = CompileStage(GL_FRAGMENT_SHADER, kOitFragmentSource, "oit.frag.glsl");

    program_ = glCreateProgram();

    if (!program_)
    {
        FatalError("Gles2OitProgram: glCreateProgram failed\n");
    }

    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);

    glBindAttribLocation(program_, kGles2AttributePosition, "a_position");
    glBindAttribLocation(program_, kGles2AttributeTextureCoordinates, "a_texture_coordinates");

    glLinkProgram(program_);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint log_length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
            glGetProgramInfoLog(program_, log_length, nullptr, log);

        FatalError("Gles2OitProgram: composite program failed to link:\n%s\n", log);
    }

    glDetachShader(program_, vertex_shader);
    glDetachShader(program_, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    uniform_accumulation_ = glGetUniformLocation(program_, "u_accumulation");
    uniform_revealage_    = glGetUniformLocation(program_, "u_revealage");
    uniform_scale_        = glGetUniformLocation(program_, "u_oit_scale");

    glUseProgram(program_);

    glUniform1i(uniform_accumulation_, kGles2TextureUnit0);
    glUniform1i(uniform_revealage_, kGles2TextureUnit1);

    return true;
}

void Gles2OitProgram::Shutdown()
{
    if (program_)
    {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void Gles2OitProgram::Use()
{
    glUseProgram(program_);
}

void Gles2OitProgram::SetScale(float scale)
{
    if (epi::AlmostEquals(shadow_scale_, scale))
        return;

    shadow_scale_ = scale;

    glUniform1f(uniform_scale_, scale);
}
