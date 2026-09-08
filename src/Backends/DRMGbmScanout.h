#pragma once

#include <cstdint>
#include <span>

struct gbm_device;
struct wlr_dmabuf_attributes;

namespace gamescope
{
	// Allocate every compositor-owned scanout buffer through the DRM driver's
	// KMS-aware GBM allocator. Vulkan imports these buffers for rendering, but
	// never decides their memory placement.
	class CGbmScanoutAllocator
	{
	public:
		~CGbmScanoutAllocator();

		bool Init( int nDrmFd );
		void Shutdown();

		bool IsAvailable() const { return m_pGbmDevice != nullptr; }

		// On failure, *pDmaBuf is left zeroed and owns nothing.
		bool CreateScanoutDmabuf( uint32_t uWidth, uint32_t uHeight, uint32_t uDrmFormat,
		                          std::span<const uint64_t> ulModifiers,
		                          wlr_dmabuf_attributes *pDmaBuf );

	private:
		gbm_device *m_pGbmDevice = nullptr;
	};
}
