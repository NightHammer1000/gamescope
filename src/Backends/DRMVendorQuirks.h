#pragma once

#include <string_view>

namespace gamescope
{
	// Vendor differences live here, named, instead of dissolved into defaults
	// elsewhere in the backend. Add a field with a comment saying *why*, never
	// a bare bool.
	struct DrmVendorQuirks
	{
		// Whether a modeset must take the link fully down, let it settle, and
		// bring it back up as a separate commit.
		//
		// The usual sequence zeroes CRTC_ID / ACTIVE / MODE_ID and refills them
		// with the new mode in a single atomic request. We have no guarantee how
		// a driver sequences that internally, and on nvidia-drm the link appears
		// not to actually drop, so it never retrains cleanly -- which is the
		// corruption. Measured on a 5080 against a 4K TV: splitting it into a
		// real link-down commit, a settle, then a link-up brought the display up
		// clean 8 times out of 10 where the single-request path corrupted.
		//
		// This flag is only the *default* for drivers known to need it. A real
		// link drop also helps sinks whose HDMI 2.1 link training is unreliable
		// -- some AV receivers only negotiate VRR correctly once the link has
		// actually gone down -- and that is a property of the display, not the
		// GPU. Users can force it on anywhere with drm_modeset_link_down=1.
		bool bNeedsModesetLinkDown = false;
	};

	// Pure driver-name -> quirks mapping. Kept free of any fd or ioctl so it
	// can be tested without a GPU.
	DrmVendorQuirks DrmVendorQuirksForDriver( std::string_view svDriverName );

	// Queries the driver name off the fd and delegates to the above. Returns
	// permissive defaults if the name cannot be read.
	DrmVendorQuirks DetectDrmVendorQuirks( int nDrmFd );
}
