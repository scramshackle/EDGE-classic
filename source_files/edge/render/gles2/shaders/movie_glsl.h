#pragma once

static const char kMovieVertexSource[] =
    "attribute vec3 a_position;\n"
    "attribute vec4 a_texture_coordinates;\n"
    "\n"
    "uniform mat4 u_model_view;\n"
    "uniform mat4 u_projection;\n"
    "\n"
    "varying vec2 v_texture_coordinates;\n"
    "\n"
    "void main()\n"
    "{\n"
    "    v_texture_coordinates = a_texture_coordinates.xy;\n"
    "\n"
    "    gl_Position = u_projection * u_model_view * vec4(a_position, 1.0);\n"
    "}\n"
    ;

static const char kMovieFragmentSource[] =
    "uniform sampler2D u_texture_y;\n"
    "uniform sampler2D u_texture_cb;\n"
    "uniform sampler2D u_texture_cr;\n"
    "\n"
    "uniform vec2 u_luma_scale;\n"
    "uniform vec2 u_chroma_scale;\n"
    "\n"
    "varying vec2 v_texture_coordinates;\n"
    "\n"
    "const mat4 kRec601 = mat4(1.16438,  0.00000,  1.59603, -0.87079,\n"
    "                          1.16438, -0.39176, -0.81297,  0.52959,\n"
    "                          1.16438,  2.01723,  0.00000, -1.08139,\n"
    "                          0.00000,  0.00000,  0.00000,  1.00000);\n"
    "\n"
    "void main()\n"
    "{\n"
    "    float y  = texture2D(u_texture_y, v_texture_coordinates * u_luma_scale).r;\n"
    "    float cb = texture2D(u_texture_cb, v_texture_coordinates * u_chroma_scale).r;\n"
    "    float cr = texture2D(u_texture_cr, v_texture_coordinates * u_chroma_scale).r;\n"
    "\n"
    "    gl_FragColor = vec4(y, cb, cr, 1.0) * kRec601;\n"
    "}\n"
    ;

