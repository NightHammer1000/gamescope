#pragma once

#include <cstdint>

namespace gamescope
{
	enum class FrameGenerationStatus : uint32_t
	{
		Disabled = 0,
		WarmingUp,
		Active,
		UnsupportedFormat,
		UnsupportedGPU,
		DeadlineMiss,
	};

	inline constexpr uint32_t kFrameGenerationMinFlowScalePercent = 10;
	inline constexpr uint32_t kFrameGenerationMaxFlowScalePercent = 100;

	struct FrameGenerationConfig
	{
		bool enabled = false;
		uint32_t flowScalePercent = 100;
		bool operator==( const FrameGenerationConfig &other ) const = default;
	};

	void SetFrameGenerationEnabled( uint32_t enabled );
	void SetFrameGenerationFlowScale( uint32_t percent );
	FrameGenerationConfig GetFrameGenerationConfig();
	void SetFrameGenerationStatus( FrameGenerationStatus status );
	FrameGenerationStatus GetFrameGenerationStatus();
	uint64_t GetFrameGenerationStateSerial();
}
