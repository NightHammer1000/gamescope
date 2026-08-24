#include <catch2/catch_test_macros.hpp>

#include "Backends/ScanoutModifiers.h"
#include "Backends/DRMVendorQuirks.h"

#include <drm_fourcc.h>

using namespace gamescope;

// Stand-ins for real vendor modifiers. The negotiator only does set algebra, so
// the exact values don't matter -- only that they are distinct and not LINEAR.
static constexpr uint64_t kTiledA = 0x0100000000000001ull;
static constexpr uint64_t kTiledB = 0x0100000000000002ull;
static constexpr uint64_t kTiledC = 0x0100000000000003ull;

TEST_CASE("Scanout modifier negotiation keeps KMS preference order", "[scanout]") {
	const uint64_t kms[]    = { kTiledA, kTiledB, kTiledC };
	const uint64_t vulkan[] = { kTiledC, kTiledB, kTiledA };

	ScanoutModifierRequest request{ .kmsPrimary = kms, .vulkanPrimary = vulkan };
	ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

	REQUIRE( choice.bOk() );
	REQUIRE_FALSE( choice.bDegradedToLinear );
	// KMS order wins: it is the driver's own preference.
	REQUIRE( choice.candidates == std::vector<uint64_t>{ kTiledA, kTiledB, kTiledC } );
}

TEST_CASE("Scanout modifier negotiation drops what Vulkan cannot import", "[scanout]") {
	const uint64_t kms[]    = { kTiledA, kTiledB, kTiledC };
	const uint64_t vulkan[] = { kTiledB };

	ScanoutModifierRequest request{ .kmsPrimary = kms, .vulkanPrimary = vulkan };
	ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

	REQUIRE( choice.bOk() );
	REQUIRE( choice.candidates == std::vector<uint64_t>{ kTiledB } );
}

TEST_CASE("Scanout modifier negotiation never negotiates INVALID", "[scanout]") {
	const uint64_t kms[]    = { DRM_FORMAT_MOD_INVALID, kTiledA };
	const uint64_t vulkan[] = { DRM_FORMAT_MOD_INVALID, kTiledA };

	ScanoutModifierRequest request{ .kmsPrimary = kms, .vulkanPrimary = vulkan };
	ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

	REQUIRE( choice.candidates == std::vector<uint64_t>{ kTiledA } );
}

TEST_CASE("Scanout modifier negotiation deduplicates", "[scanout]") {
	const uint64_t kms[]    = { kTiledA, kTiledA, kTiledB, kTiledA };
	const uint64_t vulkan[] = { kTiledA, kTiledB };

	ScanoutModifierRequest request{ .kmsPrimary = kms, .vulkanPrimary = vulkan };
	ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

	REQUIRE( choice.candidates == std::vector<uint64_t>{ kTiledA, kTiledB } );
}

TEST_CASE("Scanout modifier negotiation treats LINEAR as a last resort", "[scanout]") {
	const uint64_t kms[]    = { DRM_FORMAT_MOD_LINEAR, kTiledA };
	const uint64_t vulkan[] = { DRM_FORMAT_MOD_LINEAR, kTiledA };

	ScanoutModifierRequest request{ .kmsPrimary = kms, .vulkanPrimary = vulkan };
	ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

	REQUIRE( choice.bOk() );
	REQUIRE_FALSE( choice.bDegradedToLinear );
	// Tiled first even though KMS advertised LINEAR ahead of it.
	REQUIRE( choice.candidates == std::vector<uint64_t>{ kTiledA, DRM_FORMAT_MOD_LINEAR } );
}

TEST_CASE("Scanout modifier negotiation narrows to the partial-overlay format", "[scanout]") {
	const uint64_t kmsPrimary[]    = { kTiledA, kTiledB };
	const uint64_t vulkanPrimary[] = { kTiledA, kTiledB };
	const uint64_t kmsOverlay[]    = { kTiledB };
	const uint64_t vulkanOverlay[] = { kTiledB };

	ScanoutModifierRequest request{
		.kmsPrimary = kmsPrimary,
		.vulkanPrimary = vulkanPrimary,
		.bNeedsOverlay = true,
		.kmsOverlay = kmsOverlay,
		.vulkanOverlay = vulkanOverlay,
	};
	ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

	REQUIRE( choice.bOk() );
	REQUIRE( choice.candidates == std::vector<uint64_t>{ kTiledB } );
}

TEST_CASE("Scanout modifier negotiation degrades to LINEAR only when nothing else survives", "[scanout]") {
	// KMS and Vulkan agree on nothing except LINEAR.
	const uint64_t kms[]    = { kTiledA, DRM_FORMAT_MOD_LINEAR };
	const uint64_t vulkan[] = { kTiledB, DRM_FORMAT_MOD_LINEAR };

	ScanoutModifierRequest request{ .kmsPrimary = kms, .vulkanPrimary = vulkan };
	ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

	REQUIRE( choice.bOk() );
	REQUIRE( choice.bDegradedToLinear );
	REQUIRE( choice.candidates == std::vector<uint64_t>{ DRM_FORMAT_MOD_LINEAR } );
}

TEST_CASE("Scanout modifier negotiation rejects rather than guessing", "[scanout]") {
	SECTION("nothing in common and no LINEAR anywhere") {
		const uint64_t kms[]    = { kTiledA };
		const uint64_t vulkan[] = { kTiledB };

		ScanoutModifierRequest request{ .kmsPrimary = kms, .vulkanPrimary = vulkan };
		ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

		REQUIRE_FALSE( choice.bOk() );
		REQUIRE_FALSE( choice.sRejectReason.empty() );
	}

	SECTION("KMS offers nothing at all") {
		const uint64_t vulkan[] = { kTiledA };

		ScanoutModifierRequest request{ .kmsPrimary = {}, .vulkanPrimary = vulkan };
		ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

		REQUIRE_FALSE( choice.bOk() );
		REQUIRE_FALSE( choice.sRejectReason.empty() );
	}

	SECTION("LINEAR is the only survivor but the caller forbade it") {
		const uint64_t kms[]    = { kTiledA, DRM_FORMAT_MOD_LINEAR };
		const uint64_t vulkan[] = { kTiledB, DRM_FORMAT_MOD_LINEAR };

		ScanoutModifierRequest request{
			.kmsPrimary = kms,
			.vulkanPrimary = vulkan,
			.bAllowLinearFallback = false,
		};
		ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

		REQUIRE_FALSE( choice.bOk() );
	}

	SECTION("overlay rules everything out") {
		const uint64_t kmsPrimary[]    = { kTiledA };
		const uint64_t vulkanPrimary[] = { kTiledA };
		const uint64_t kmsOverlay[]    = { kTiledB };
		const uint64_t vulkanOverlay[] = { kTiledB };

		ScanoutModifierRequest request{
			.kmsPrimary = kmsPrimary,
			.vulkanPrimary = vulkanPrimary,
			.bNeedsOverlay = true,
			.kmsOverlay = kmsOverlay,
			.vulkanOverlay = vulkanOverlay,
		};
		ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

		REQUIRE_FALSE( choice.bOk() );
		REQUIRE_FALSE( choice.sRejectReason.empty() );
	}
}

TEST_CASE("Scanout modifier negotiation rejects when KMS never offers LINEAR", "[scanout]") {
	// A driver that advertises no LINEAR for scanout gets a rejection, not a
	// LINEAR buffer it never said it could flip.
	const uint64_t kms[]    = { kTiledA };
	const uint64_t vulkan[] = { kTiledB, DRM_FORMAT_MOD_LINEAR };

	ScanoutModifierRequest request{ .kmsPrimary = kms, .vulkanPrimary = vulkan };
	ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

	REQUIRE_FALSE( choice.bOk() );
	REQUIRE_FALSE( choice.sRejectReason.empty() );
}

// Regression guard. A healthy Mesa driver advertises a tiled modifier that both
// KMS and Vulkan accept; negotiation must pick it and must NOT quietly drop to
// LINEAR. Silently degrading here would cost AMD/Intel the direct-scanout path
// that is the entire reason for the compositor.
TEST_CASE("Scanout modifier negotiation does not degrade a healthy driver", "[scanout]") {
	const uint64_t kms[]    = { kTiledA, kTiledB, DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	const uint64_t vulkan[] = { kTiledB, kTiledA, DRM_FORMAT_MOD_LINEAR };

	ScanoutModifierRequest request{ .kmsPrimary = kms, .vulkanPrimary = vulkan };
	ScanoutModifierChoice choice = NegotiateScanoutModifiers( request );

	REQUIRE( choice.bOk() );
	REQUIRE_FALSE( choice.bDegradedToLinear );
	REQUIRE( choice.candidates.front() == kTiledA );
	REQUIRE( choice.candidates.back() == DRM_FORMAT_MOD_LINEAR );
}

TEST_CASE("Only nvidia-drm carries the scanout quirks", "[scanout]") {
	const DrmVendorQuirks nvidia = DrmVendorQuirksForDriver( "nvidia-drm" );
	REQUIRE_FALSE( nvidia.bCanDirectScanoutClientBuffers );
	REQUIRE( nvidia.bRequiresGbmScanoutAllocation );

	// Direct scanout is the copy gamescope exists to avoid, and Vulkan-allocated
	// scanout works fine on Mesa. Every other driver keeps both.
	for ( const char *pszDriver : { "amdgpu", "i915", "xe", "nouveau", "msm", "vc4" } )
	{
		const DrmVendorQuirks quirks = DrmVendorQuirksForDriver( pszDriver );
		INFO( "driver: " << pszDriver );
		REQUIRE( quirks.bCanDirectScanoutClientBuffers );
		REQUIRE_FALSE( quirks.bRequiresGbmScanoutAllocation );
	}

	// Unknown drivers get the permissive default, not the NVIDIA workaround.
	REQUIRE( DrmVendorQuirksForDriver( "some-future-driver" ).bCanDirectScanoutClientBuffers );
	REQUIRE( DrmVendorQuirksForDriver( "" ).bCanDirectScanoutClientBuffers );

	// Substring paranoia: "nvidia-drm" must match exactly, or an unrelated
	// driver could silently lose direct scanout.
	REQUIRE( DrmVendorQuirksForDriver( "nvidia" ).bCanDirectScanoutClientBuffers );
	REQUIRE( DrmVendorQuirksForDriver( "nvidia-drm-next" ).bCanDirectScanoutClientBuffers );
}
