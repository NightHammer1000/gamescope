#pragma once

#include <cstdint>

#include "rendervulkan.hpp"

// Record one FidelityFX Optical Flow v5 dispatch. The source image is the raw
// pre-upscale game frame. Calls are nonblocking; false means the previous work
// is still in flight or the GPU/format is unsupported.
bool vulkan_frame_generation_record_optical_flow(
	CVulkanCmdBuffer *cmdBuffer,
	gamescope::Rc<CVulkanTexture> source,
	GamescopeAppTextureColorspace colorspace,
	uint32_t flowScalePercent,
	bool reset );

// Run the optical-flow-only FI vector field, midpoint, SPD inpainting pyramid,
// and final inpainting after a successful optical-flow record. The output
// remains in raw pre-FSR dimensions. SDR remains in its source encoding; HDR
// is converted to linear scRGB for interpolation and inpainting. When supplied,
// finalOutput is used for the complete midpoint/inpainting chain and returned.
gamescope::Rc<CVulkanTexture> vulkan_frame_generation_record_interpolation(
	CVulkanCmdBuffer *cmdBuffer,
	gamescope::Rc<CVulkanTexture> previous,
	gamescope::Rc<CVulkanTexture> current,
	bool reset,
	gamescope::Rc<CVulkanTexture> finalOutput = nullptr );

// Associate the most recently recorded work with Gamescope's submission
// timeline so descriptor and image resources are never reused while in flight.
void vulkan_frame_generation_notify_submit( uint64_t sequence );
bool vulkan_frame_generation_work_complete();
void vulkan_frame_generation_reset_optical_flow();
bool vulkan_frame_generation_ab_enabled();
bool vulkan_frame_generation_direct_output_enabled();

void vulkan_frame_generation_record_output_timestamp(
	CVulkanCmdBuffer *cmdBuffer, bool usedFsr );
FrameGenerationGpuTimings vulkan_frame_generation_get_gpu_timings();
uint64_t vulkan_frame_generation_get_scene_cut_copy_count();
