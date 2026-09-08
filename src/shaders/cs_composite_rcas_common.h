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
#if FSR_RCAS_USE_FP16
#define A_HALF 1
#endif
#include "ffx_a.h"

#if FSR_RCAS_USE_FP16
#define FSR_RCAS_H 1
AH4 FsrRcasLoadH(ASW2 p) { return AH4(texelFetch(s_samplers[0], ivec2(p), 0)); }
void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}
#else
#define FSR_RCAS_F 1
vec4 FsrRcasLoadF(ivec2 p) { return texelFetch(s_samplers[0], p, 0); }
void FsrRcasInputF(inout float r, inout float g, inout float b) {}
#endif
#include "ffx_fsr1.h"

void runRcas(out vec3 outputValue, uvec2 pos)
{
#if FSR_RCAS_USE_FP16
    AH3 halfOutput;
    FsrRcasH(halfOutput.r, halfOutput.g, halfOutput.b, pos, u_c1.xxxx);
    outputValue = vec3(halfOutput);
#else
    FsrRcasF(outputValue.r, outputValue.g, outputValue.b, pos, u_c1.xxxx);
#endif
}

vec4 sampleLayer(uint layerIdx, vec2 uv)
{
    if ((c_ycbcrMask & (1 << layerIdx)) != 0)
        return sampleLayerEx(s_ycbcr_samplers[layerIdx], layerIdx - 1, layerIdx, uv, false);
    return sampleLayerEx(s_samplers[layerIdx], layerIdx - 1, layerIdx, uv, true);
}

void rcasComposite(uvec2 pos)
{
    vec3 outputValue = vec3(0.0f);

    if (c_fsr_simple_output) {
        runRcas(outputValue, pos);
        imageStore(dst, ivec2(pos), vec4(outputValue, 0));
        return;
    }

    if (checkDebugFlag(compositedebug_PlaneBorders))
        outputValue = vec3(1.0f, 0.0f, 0.0f);

    if (c_layerCount > 0) {
        // this is actually signed, underflow will be filtered out by the branch below
        uvec2 rcasPos = pos + u_layer0Offset;
        uvec2 layer0Extent = uvec2(textureSize(s_samplers[0], 0));

        if (all(lessThan(rcasPos, layer0Extent))) {
            runRcas(outputValue, rcasPos);

            uint colorspace = get_layer_colorspace(0);
            if (colorspace == colorspace_linear)
            {
                // We don't use an sRGB view for FSR due to the spaces RCAS works in.
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
    // AMD recommends to use this swizzle and to process 4 pixels per invocation
    // for better cache utilisation.
    uvec2 pos = ARmp8x8(gl_LocalInvocationID.x) + uvec2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);
    rcasComposite(pos);
    pos.x += 8u;
    rcasComposite(pos);
    pos.y += 8u;
    rcasComposite(pos);
    pos.x -= 8u;
    rcasComposite(pos);
}
