#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gamescope
{
	// A scanout buffer has to satisfy three separate owners at once:
	//
	//   the KMS plane      -- it must be flippable
	//   Vulkan             -- we render the composite into it
	//   the allocator      -- GBM has to be able to produce it
	//
	// Historically the code intersected only the first of those and discovered
	// the second by watching an import fail. Negotiate explicitly instead, so a
	// failure names which set was empty rather than surfacing as a mystery
	// further down.
	//
	// GBM is not represented here: it is asked last, by handing it this
	// candidate list, and its answer is checked against the list.
	struct ScanoutModifierRequest
	{
		std::span<const uint64_t> kmsPrimary;
		std::span<const uint64_t> vulkanPrimary;

		// The partial-overlay image aliases the same buffer with another
		// format, so its constraints have to be met by the same modifier.
		bool bNeedsOverlay = false;
		std::span<const uint64_t> kmsOverlay;
		std::span<const uint64_t> vulkanOverlay;

		// LINEAR is correct everywhere and slow everywhere. Only reachable
		// when nothing better survives the intersection.
		bool bAllowLinearFallback = true;
	};

	struct ScanoutModifierChoice
	{
		// Ordered best-first. Empty means no viable modifier.
		std::vector<uint64_t> candidates;

		// Set when the intersection was empty and we dropped to LINEAR. The
		// caller is expected to say so loudly: this is a real performance
		// cliff, not a detail.
		bool bDegradedToLinear = false;

		// Populated only when candidates is empty. Says which constraint
		// emptied the set, for the log.
		std::string sRejectReason;

		bool bOk() const { return !candidates.empty(); }
	};

	ScanoutModifierChoice NegotiateScanoutModifiers( const ScanoutModifierRequest &request );
}
