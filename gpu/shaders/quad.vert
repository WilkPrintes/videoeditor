#version 440

// Shared by both viewer passes: a textured quad, scaled/offset in NDC.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform QuadParams {
    vec4 rect;   // xy = scale, zw = offset (NDC)
    vec4 flags;  // x = flip v (sampling a render target on a Y-up framebuffer API)
} quad;

void main()
{
    v_uv = vec2(texcoord.x, quad.flags.x > 0.5 ? 1.0 - texcoord.y : texcoord.y);
    gl_Position = vec4(position * quad.rect.xy + quad.rect.zw, 0.0, 1.0);
}
