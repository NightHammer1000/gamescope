// Catmull-Rom bicubic upscale of layer 0 into the intermediate image.
// First half of the BCAS filter; cs_composite_cas sharpens the result.
// Runs in encoded (gamma) space like the classic emulator-scene BCAS.

#include "descriptor_set.h"

layout(
  local_size_x = 8,
  local_size_y = 8,
  local_size_z = 1) in;

// Catmull-Rom weight for |t| <= 1 and 1 < |t| <= 2 (a = -0.5).
float cweight(float t)
{
    t = abs(t);
    if (t <= 1.0f)
        return 1.5f * t * t * t - 2.5f * t * t + 1.0f;
    if (t <= 2.0f)
        return -0.5f * t * t * t + 2.5f * t * t - 4.0f * t + 2.0f;
    return 0.0f;
}

void main()
{
    uvec2 coord = gl_GlobalInvocationID.xy;
    uvec2 outSize = uvec2(imageSize(dst));
    if (coord.x >= outSize.x || coord.y >= outSize.y)
        return;

    ivec2 inSize = textureSize(s_samplers[0], 0);

    // Source position of this output pixel.
    vec2 src = (vec2(coord) + vec2(0.5f)) * vec2(inSize) / vec2(outSize) - vec2(0.5f);
    ivec2 base = ivec2(floor(src));
    vec2 f = src - vec2(base);

    vec3 sum = vec3(0.0f);
    float wsum = 0.0f;
    for (int y = -1; y <= 2; y++)
    {
        float wy = cweight(float(y) - f.y);
        for (int x = -1; x <= 2; x++)
        {
            float w = cweight(float(x) - f.x) * wy;
            ivec2 p = clamp(base + ivec2(x, y), ivec2(0), inSize - ivec2(1));
            sum += texelFetch(s_samplers[0], p, 0).rgb * w;
            wsum += w;
        }
    }

    vec3 color = clamp(sum / wsum, vec3(0.0f), vec3(1.0f));
    imageStore(dst, ivec2(coord), vec4(color, 1.0f));
}
