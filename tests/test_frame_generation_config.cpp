#include "frame_generation_config.hpp"
#include "frame_generation_pacing.hpp"

#include <cassert>

int main()
{
	using namespace gamescope;
	static_assert( static_cast<uint32_t>( FrameGenerationStatus::Disabled ) == 0 );
	static_assert( static_cast<uint32_t>( FrameGenerationStatus::WarmingUp ) == 1 );
	static_assert( static_cast<uint32_t>( FrameGenerationStatus::Active ) == 2 );
	static_assert( static_cast<uint32_t>( FrameGenerationStatus::UnsupportedFormat ) == 3 );
	static_assert( static_cast<uint32_t>( FrameGenerationStatus::UnsupportedGPU ) == 4 );
	static_assert( static_cast<uint32_t>( FrameGenerationStatus::DeadlineMiss ) == 5 );

	SetFrameGenerationEnabled( 0 );
	SetFrameGenerationFlowScale( 100 );
	assert( GetFrameGenerationConfig() == FrameGenerationConfig{} );
	assert( GetFrameGenerationStatus() == FrameGenerationStatus::Disabled );

	SetFrameGenerationEnabled( 1 );
	assert( GetFrameGenerationConfig().enabled );
	assert( GetFrameGenerationStatus() == FrameGenerationStatus::WarmingUp );
	const uint64_t enabledConfigSerial = GetFrameGenerationConfigSerial();
	const uint64_t enabledSerial = GetFrameGenerationStateSerial();
	SetFrameGenerationEnabled( 1 );
	assert( GetFrameGenerationConfigSerial() == enabledConfigSerial );
	assert( GetFrameGenerationStateSerial() == enabledSerial );
	SetFrameGenerationStatus( FrameGenerationStatus::Active );
	assert( GetFrameGenerationConfigSerial() == enabledConfigSerial );

	SetFrameGenerationFlowScale( 75 );
	assert( GetFrameGenerationConfig().flowScalePercent == 75 );
	assert( GetFrameGenerationStatus() == FrameGenerationStatus::WarmingUp );
	assert( GetFrameGenerationConfigSerial() > enabledConfigSerial );
	assert( GetFrameGenerationStateSerial() > enabledSerial );

	SetFrameGenerationFlowScale( 0 );
	assert( GetFrameGenerationConfig().flowScalePercent == 10 );
	SetFrameGenerationFlowScale( 10 );
	assert( GetFrameGenerationConfig().flowScalePercent == 10 );
	SetFrameGenerationFlowScale( 101 );
	assert( GetFrameGenerationConfig().flowScalePercent == 100 );

	SetFrameGenerationEnabled( 0 );
	assert( GetFrameGenerationStatus() == FrameGenerationStatus::Disabled );

	uint32_t sourceSlots = 0;
	uint32_t outputSlots = 0;
	for ( uint64_t vblank = 1; vblank <= 120; ++vblank )
	{
		sourceSlots += FrameGenerationSourceSlotDue( vblank, 120, 120 );
		outputSlots += FrameGenerationOutputSlotDue( vblank, 120, 120 );
	}
	assert( sourceSlots == 60 );
	assert( outputSlots == 120 );

	// Rational scheduling must not round 90 Hz total output on a 120 Hz mode.
	sourceSlots = 0;
	outputSlots = 0;
	for ( uint64_t vblank = 1; vblank <= 120; ++vblank )
	{
		sourceSlots += FrameGenerationSourceSlotDue( vblank, 90, 120 );
		outputSlots += FrameGenerationOutputSlotDue( vblank, 90, 120 );
	}
	assert( sourceSlots == 45 );
	assert( outputSlots == 90 );

	// Let the client begin its next frame while the current real frame is queued,
	// but not while the generated midpoint still needs to be presented.
	assert( FrameGenerationCanRequestSourceFrame( 0, false ) );
	assert( !FrameGenerationCanRequestSourceFrame( 2, true ) );
	assert( !FrameGenerationCanRequestSourceFrame( 1, true ) );
	assert( FrameGenerationCanRequestSourceFrame( 1, false ) );

	constexpr uint64_t outputInterval = 8'333'333;
	const uint64_t firstDeadline = FrameGenerationNextOutputDeadline(
		0, 100'000'000, outputInterval );
	assert( firstDeadline == 108'333'333 );
	assert( !FrameGenerationOutputDeadlineMissed(
		firstDeadline + outputInterval - 1, firstDeadline, outputInterval ) );
	assert( FrameGenerationOutputDeadlineMissed(
		firstDeadline + outputInterval, firstDeadline, outputInterval ) );
	assert( FrameGenerationNextOutputDeadline(
		firstDeadline, firstDeadline + 100'000, outputInterval ) ==
		firstDeadline + outputInterval );
	assert( FrameGenerationNextOutputDeadline(
		firstDeadline, firstDeadline + outputInterval, outputInterval ) ==
		firstDeadline + outputInterval * 2u );
	return 0;
}
