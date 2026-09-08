#include "DRMVendorQuirks.h"

#include <xf86drm.h>

namespace gamescope
{
	DrmVendorQuirks DrmVendorQuirksForDriver( std::string_view svDriverName )
	{
		DrmVendorQuirks quirks{};

		if ( svDriverName == "nvidia-drm" )
		{
			quirks.bNeedsModesetLinkDown = true;
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
