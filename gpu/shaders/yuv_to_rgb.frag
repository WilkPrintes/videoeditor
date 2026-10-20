#version 440

// Pass 1 ("process"): planar Y'CbCr -> R'G'B' in float, written to an RGBA16F target.
// This is where the effect/colour node graph will run later; it is already float.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 1) uniform ColorParams {
    vec4 row0;   // matrix rows, range expansion folded in
    vec4 row1;
    vec4 row2;
    vec4 offset; // black level / chroma zero of the encoded signal
} color;

layout(binding = 2) uniform sampler2D planeY;
layout(binding = 3) uniform sampler2D planeU;
layout(binding = 4) uniform sampler2D planeV;

void main()
{
    vec3 ycc = vec3(texture(planeY, v_uv).r, texture(planeU, v_uv).r, texture(planeV, v_uv).r) - color.offset.xyz;
    vec3 rgb = vec3(dot(color.row0.xyz, ycc), dot(color.row1.xyz, ycc), dot(color.row2.xyz, ycc));
    fragColor = vec4(rgb, 1.0);
}
