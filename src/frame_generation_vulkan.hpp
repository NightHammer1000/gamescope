#pragma once

#include <cstdint>

#include "rendervulkan.hpp"

// Record one FidelityFX Optical Flow v5 dispatch. The source image is the raw
// pre-upscale game frame. Calls are nonblocking; false means the previous work
// is still in flight or the GPU/format is unsupported.
bool vulkan_frame_generation_record_optical_flow(
	CVulkanCmdBuffer *cmdBuffer,
	gamescope::Rc<CVulkanTexture> source,
	uint32_t flowScalePercent,
	bool reset );

// Run the optical-flow-only FI vector field, midpoint, SPD inpainting pyramid,
// and final inpainting after a successful optical-flow record. The returned
// FP16 texture remains in raw pre-FSR dimensions.
gamescope::Rc<CVulkanTexture> vulkan_frame_generation_record_interpolation(
	CVulkanCmdBuffer *cmdBuffer,
	gamescope::Rc<CVulkanTexture> previous,
	gamescope::Rc<CVulkanTexture> current,
	bool reset );

// Associate the most recently recorded work with Gamescope's submission
// timeline so descriptor and image resources are never reused while in flight.
void vulkan_frame_generation_notify_submit( uint64_t sequence );
bool vulkan_frame_generation_work_complete();
void vulkan_frame_generation_reset_optical_flow();
bool vulkan_frame_generation_ab_enabled();
