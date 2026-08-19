#pragma once

static const char kOitVertexSource[] =
    "attribute vec3 a_position;\n"
    "attribute vec4 a_texture_coordinates;\n"
    "\n"
    "varying vec2 v_texture_coordinates;\n"
    "\n"
    "void main()\n"
    "{\n"
    "    v_texture_coordinates = a_texture_coordinates.xy;\n"
    "\n"
    "    gl_Position = vec4(a_position, 1.0);\n"
    "}\n"
    ;

static const char kOitFragmentSource[] =
    "uniform sampler2D u_accumulation;\n"
    "uniform sampler2D u_revealage;\n"
    "\n"
    "uniform float u_oit_scale;\n"
    "\n"
    "varying vec2 v_texture_coordinates;\n"
    "\n"
    "void main()\n"
    "{\n"
    "    vec4 accumulation = texture2D(u_accumulation, v_texture_coordinates);\n"
    "\n"
    "    float revealage = texture2D(u_revealage, v_texture_coordinates).a;\n"
    "\n"
    "    accumulation /= u_oit_scale;\n"
    "\n"
    "    float weight = max(accumulation.a, 1e-5);\n"
    "\n"
    "    gl_FragColor = vec4(accumulation.rgb / weight, 1.0 - revealage);\n"
    "}\n"
    ;

