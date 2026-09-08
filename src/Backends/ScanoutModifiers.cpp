#include "ScanoutModifiers.h"

#include <algorithm>
#include <drm_fourcc.h>

namespace gamescope
{
	namespace
	{
		bool Contains( std::span<const uint64_t> set, uint64_t ulModifier )
		{
			return std::find( set.begin(), set.end(), ulModifier ) != set.end();
		}

		// DRM_FORMAT_MOD_INVALID means "unspecified layout". It cannot be
		// negotiated: neither side can promise anything about it, so it has no
		// place in an intersection.
		std::vector<uint64_t> WithoutInvalid( std::span<const uint64_t> set )
		{
			std::vector<uint64_t> out;
			out.reserve( set.size() );
			for ( uint64_t ulModifier : set )
			{
				if ( ulModifier == DRM_FORMAT_MOD_INVALID )
					continue;
				if ( Contains( out, ulModifier ) )
					continue;
				out.push_back( ulModifier );
			}
			return out;
		}
	}

	ScanoutModifierChoice NegotiateScanoutModifiers( const ScanoutModifierRequest &request )
	{
		ScanoutModifierChoice choice;

		// KMS order is the driver's own preference order, so start from it and
		// keep it.
		std::vector<uint64_t> candidates = WithoutInvalid( request.kmsPrimary );
		if ( candidates.empty() )
		{
			choice.sRejectReason = "no KMS plane modifiers for the primary format";
			return choice;
		}

		const size_t uAfterKms = candidates.size();

		std::erase_if( candidates, [ & ]( uint64_t ulModifier )
		{
			return !Contains( request.vulkanPrimary, ulModifier );
		} );

		if ( candidates.empty() )
		{
			choice.sRejectReason = "no modifier is both KMS-flippable and Vulkan-importable for the primary format (" +
				std::to_string( uAfterKms ) + " KMS candidates, none understood by Vulkan)";
			return choice;
		}

		if ( request.bNeedsOverlay )
		{
			std::erase_if( candidates, [ & ]( uint64_t ulModifier )
			{
				return !Contains( request.kmsOverlay, ulModifier ) ||
				       !Contains( request.vulkanOverlay, ulModifier );
			} );

			if ( candidates.empty() )
			{
				choice.sRejectReason = "no modifier satisfies both the primary and the partial-overlay format";
				return choice;
			}
		}

		if ( !request.bAllowLinearFallback )
		{
			std::erase( candidates, DRM_FORMAT_MOD_LINEAR );
			if ( candidates.empty() )
			{
				choice.sRejectReason = "only LINEAR survived the intersection and the caller disallowed it";
				return choice;
			}
		}

		// LINEAR is a valid last resort, never a preference. If anything else
		// survived, try that first.
		std::stable_partition( candidates.begin(), candidates.end(), []( uint64_t ulModifier )
		{
			return ulModifier != DRM_FORMAT_MOD_LINEAR;
		} );

		// Degraded means LINEAR is all we could agree on -- however we got here.
		// The caller is expected to say so loudly: it is a real performance
		// cliff, not a detail.
		choice.bDegradedToLinear =
			candidates.size() == 1 && candidates.front() == DRM_FORMAT_MOD_LINEAR;

		choice.candidates = std::move( candidates );
		return choice;
	}
}
