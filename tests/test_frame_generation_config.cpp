#include "frame_generation_config.hpp"

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
	const uint64_t enabledSerial = GetFrameGenerationStateSerial();
	SetFrameGenerationEnabled( 1 );
	assert( GetFrameGenerationStateSerial() == enabledSerial );

	SetFrameGenerationFlowScale( 75 );
	assert( GetFrameGenerationConfig().flowScalePercent == 75 );
	assert( GetFrameGenerationStatus() == FrameGenerationStatus::WarmingUp );
	assert( GetFrameGenerationStateSerial() > enabledSerial );

	SetFrameGenerationFlowScale( 0 );
	assert( GetFrameGenerationConfig().flowScalePercent == 10 );
	SetFrameGenerationFlowScale( 10 );
	assert( GetFrameGenerationConfig().flowScalePercent == 10 );
	SetFrameGenerationFlowScale( 101 );
	assert( GetFrameGenerationConfig().flowScalePercent == 100 );

	SetFrameGenerationEnabled( 0 );
	assert( GetFrameGenerationStatus() == FrameGenerationStatus::Disabled );
	return 0;
}
