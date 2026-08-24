#pragma once

#include <string_view>

namespace gamescope
{
	// Vendor differences live here, named, instead of dissolved into defaults
	// elsewhere in the backend. Add a field with a comment saying *why*, never
	// a bare bool.
	struct DrmVendorQuirks
	{
		// Whether a client-allocated buffer may be handed straight to a KMS
		// plane.
		//
		// Direct scanout is the copy gamescope exists to avoid, so this is true
		// everywhere it can possibly be true. On nvidia-drm it cannot: client
		// buffers come from the game's own allocator, and the display engine
		// will not scan out memory that did not come from NVIDIA's KMS-aware
		// one. There, every frame must be composited into a buffer we
		// allocated.
		bool bCanDirectScanoutClientBuffers = true;

		// Whether our own scanout buffers *must* come from GBM.
		//
		// Vulkan makes no promise that an exported image is KMS-scannable --
		// VK_EXT_image_drm_format_modifier negotiates layout, not placement --
		// and on nvidia-drm that promise is not kept, which is what corrupts
		// 4K/HDR output. GBM routes the allocation through the driver's own KMS
		// path, which has to satisfy the display engine.
		//
		// Where this is set, falling back to Vulkan allocation means falling
		// back into the bug, so it is treated as fatal rather than a fallback.
		bool bRequiresGbmScanoutAllocation = false;
	};

	// Pure driver-name -> quirks mapping. Kept free of any fd or ioctl so it
	// can be tested without a GPU.
	DrmVendorQuirks DrmVendorQuirksForDriver( std::string_view svDriverName );

	// Queries the driver name off the fd and delegates to the above. Returns
	// permissive defaults if the name cannot be read.
	DrmVendorQuirks DetectDrmVendorQuirks( int nDrmFd );
}
