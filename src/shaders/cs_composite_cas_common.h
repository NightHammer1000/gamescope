// CAS composite pass: sharpens the bicubic-upscaled base layer (in
// s_samplers[0], written by cs_bicubic) with AMD FidelityFX CAS, then
// composites the remaining layers exactly like the RCAS pass does.
//
// CAS is (C) AMD, MIT -- see ffx_cas.h. This wrapper mirrors
// cs_composite_rcas_common.h; u_c1 carries the CAS sharpness as float bits
// rather than an FsrRcasCon constant.

#include "descriptor_set.h"

layout(
  local_size_x = 64,
  local_size_y = 1,
  local_size_z = 1) in;

layout(binding = 0, scalar)
uniform layers_t {
    uvec2 u_layer0Offset;
    vec2 u_scale[VKR_MAX_LAYERS - 1];
    vec2 u_offset[VKR_MAX_LAYERS - 1];
    float u_opacity[VKR_MAX_LAYERS];
    mat3x4 u_ctm[VKR_MAX_LAYERS];
    uint u_borderMask;
    uint u_frameId;
    uint u_c1;

    uint u_shaderFilter;
    uint u_alphaMode;

    // hdr
    float u_linearToNits;
    float u_nitsToLinear;
    float u_itmSdrNits;
    float u_itmTargetNits;

    uint u_rotation;
};

#include "composite.h"

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

AF3 CasLoad(ASU2 p) { return texelFetch(s_samplers[0], p, 0).rgb; }
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_cas.h"

void runCas(out vec3 outputValue, uvec2 pos)
{
    // Sharpen-only CasSetup, done GPU-side: only con1 matters when
    // noScaling is true. Matches CasSetup() in ffx_cas.h.
    float sharpness = clamp(uintBitsToFloat(u_c1), 0.0f, 1.0f);
    float sharp = -1.0f / mix(8.0f, 5.0f, sharpness);
    uvec4 con0 = uvec4(0u);
    uvec4 con1 = uvec4(floatBitsToUint(sharp), packHalf2x16(vec2(sharp, 0.0f)), floatBitsToUint(8.0f), 0u);

    CasFilter(outputValue.r, outputValue.g, outputValue.b, pos, con0, con1, true);
}

vec4 sampleLayer(uint layerIdx, vec2 uv)
{
    if ((c_ycbcrMask & (1 << layerIdx)) != 0)
        return sampleLayerEx(s_ycbcr_samplers[layerIdx], layerIdx - 1, layerIdx, uv, false);
    return sampleLayerEx(s_samplers[layerIdx], layerIdx - 1, layerIdx, uv, true);
}

void casComposite(uvec2 pos)
{
    vec3 outputValue = vec3(0.0f);

    if (c_fsr_simple_output) {
        runCas(outputValue, pos);
        imageStore(dst, ivec2(pos), vec4(outputValue, 0));
        return;
    }

    if (checkDebugFlag(compositedebug_PlaneBorders))
        outputValue = vec3(1.0f, 0.0f, 0.0f);

    if (c_layerCount > 0) {
        // this is actually signed, underflow will be filtered out by the branch below
        uvec2 casPos = pos + u_layer0Offset;
        uvec2 layer0Extent = uvec2(textureSize(s_samplers[0], 0));

        if (all(lessThan(casPos, layer0Extent))) {
            runCas(outputValue, casPos);

            uint colorspace = get_layer_colorspace(0);
            if (colorspace == colorspace_linear)
            {
                // Like RCAS: CAS works in encoded space, not through an sRGB view.
                colorspace = colorspace_sRGB;
            }

            outputValue.rgb = colorspace_plane_degamma_tf(outputValue.rgb, colorspace);
            outputValue.rgb = (vec4(outputValue.rgb, 1.0f) * u_ctm[0]).rgb;
            outputValue.rgb = apply_layer_color_mgmt(outputValue.rgb, 0, colorspace);
            outputValue *= u_opacity[0];
        }
    }

    if (c_layerCount > 1) {
        vec2 uv = vec2(pos);

        for (int i = 1; i < c_layerCount; i++) {
            vec4 layerColor = sampleLayer(i, uv);
            outputValue = BlendLayer(i, outputValue, layerColor, u_opacity[i]);
        }
    }

    outputValue = encodeOutputColor(outputValue);
    imageStore(dst, ivec2(pos), vec4(outputValue, 0));

    if (checkDebugFlag(compositedebug_Markers))
        compositing_debug(pos, u_rotation);
}

void main()
{
    uvec2 pos = ARmp8x8(gl_LocalInvocationID.x) + uvec2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);
    casComposite(pos);
    pos.x += 8u;
    casComposite(pos);
    pos.y += 8u;
    casComposite(pos);
    pos.x -= 8u;
    casComposite(pos);
}
