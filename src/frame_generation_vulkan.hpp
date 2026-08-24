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

// Associate the most recently recorded work with Gamescope's submission
// timeline so descriptor and image resources are never reused while in flight.
void vulkan_frame_generation_notify_submit( uint64_t sequence );
bool vulkan_frame_generation_work_complete();
void vulkan_frame_generation_reset_optical_flow();
