#include "descriptor_set.h"

layout(
  local_size_x = 64,
  local_size_y = 1,
  local_size_z = 1) in;

layout(binding = 0, scalar)
uniform layers_t {
    uvec4 c1, c2, c3, c4;
};

#define A_GPU 1
#define A_GLSL 1
#if FSR_EASU_USE_FP16
#define A_HALF 1
#endif
#include "ffx_a.h"

#if FSR_EASU_USE_FP16
#define FSR_EASU_H 1
f16vec4 FsrEasuRH(vec2 p) { return f16vec4(textureGather(s_samplers[0], p, 0)); }
f16vec4 FsrEasuGH(vec2 p) { return f16vec4(textureGather(s_samplers[0], p, 1)); }
f16vec4 FsrEasuBH(vec2 p) { return f16vec4(textureGather(s_samplers[0], p, 2)); }
#else
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return AF4(textureGather(s_samplers[0], p, 0)); }
AF4 FsrEasuGF(AF2 p) { return AF4(textureGather(s_samplers[0], p, 1)); }
AF4 FsrEasuBF(AF2 p) { return AF4(textureGather(s_samplers[0], p, 2)); }
#endif
#include "ffx_fsr1.h"

void easuPass(uvec2 pos)
{
#if FSR_EASU_USE_FP16
    f16vec3 color;
    FsrEasuH(color, pos, c1, c2, c3, c4);
#else
    vec3 color;
    FsrEasuF(color, pos, c1, c2, c3, c4);
#endif
    imageStore(dst, ivec2(pos), vec4(color, 1));
}

void main()
{
    // AMD recommends to use this swizzle and to process 4 pixels per invocation
    // for better cache utilisation.
    uvec2 pos = ARmp8x8(gl_LocalInvocationID.x) + uvec2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);
    easuPass(pos);
    pos.x += 8u;
    easuPass(pos);
    pos.y += 8u;
    easuPass(pos);
    pos.x -= 8u;
    easuPass(pos);
}
