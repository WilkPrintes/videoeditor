#version 440

// Pass 2 ("display"): the float frame, fitted into the viewer.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D image;

void main()
{
    fragColor = vec4(clamp(texture(image, v_uv).rgb, 0.0, 1.0), 1.0);
}
