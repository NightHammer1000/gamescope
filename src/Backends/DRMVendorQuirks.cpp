#include "DRMVendorQuirks.h"

#include <xf86drm.h>

namespace gamescope
{
	DrmVendorQuirks DrmVendorQuirksForDriver( std::string_view svDriverName )
	{
		DrmVendorQuirks quirks{};

		// nvidia-drm's display engine will not scan out memory that did not
		// come from its own KMS-aware allocator. Client buffers never do, and
		// neither does anything Vulkan exported for us.
		if ( svDriverName == "nvidia-drm" )
		{
			quirks.bCanDirectScanoutClientBuffers = false;
			quirks.bRequiresGbmScanoutAllocation = true;
		}

		return quirks;
	}

	DrmVendorQuirks DetectDrmVendorQuirks( int nDrmFd )
	{
		DrmVendorQuirks quirks{};

		if ( drmVersion *pVersion = drmGetVersion( nDrmFd ) )
		{
			if ( pVersion->name )
				quirks = DrmVendorQuirksForDriver( pVersion->name );
			drmFreeVersion( pVersion );
		}

		return quirks;
	}
}
