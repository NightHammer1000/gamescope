# AMD FidelityFX shader sources

Gamescope's frame-generation path compiles the seven Vulkan/GLSL Optical Flow
v5 passes and the vector-field, SPD inpainting-pyramid, and final-inpainting
parts of Frame Interpolation from the pinned FidelityFX SDK submodule.
Gamescope owns Vulkan resources, dispatch scheduling, interpolation, and
presentation; it does not link the SDK's host framework.

`cs_ffx_frameinterpolation_midpoint.comp` is a Gamescope adaptation of the
SDK's frame-interpolation shader. It deliberately removes all game motion
vector, depth, reconstruction, distortion, and HUD-less inputs, retaining the
optical-flow midpoint and inpainting-weight logic. It is provided under the
same MIT terms reproduced in that source file.

The imported source is pinned to FidelityFX SDK 1.1.4, the final upstream SDK
release containing the Vulkan/GLSL frame-generation implementation. The files
are licensed under the MIT license included in the submodule and retain AMD's
original copyright notices.
