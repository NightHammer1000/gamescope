#include "frame_generation_config.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace gamescope
{
	static std::mutex s_frameGenerationMutex;
	static FrameGenerationConfig s_frameGenerationConfig;
	static FrameGenerationStatus s_frameGenerationStatus = FrameGenerationStatus::Disabled;
	static std::atomic<uint64_t> s_frameGenerationStateSerial = 1;

	static void UpdateStatusForConfig()
	{
		s_frameGenerationStatus = s_frameGenerationConfig.enabled
			? FrameGenerationStatus::WarmingUp
			: FrameGenerationStatus::Disabled;
	}

	void SetFrameGenerationEnabled( uint32_t enabled )
	{
		std::lock_guard lock{ s_frameGenerationMutex };
		const bool newEnabled = enabled != 0;
		if ( newEnabled == s_frameGenerationConfig.enabled )
			return;

		s_frameGenerationConfig.enabled = newEnabled;
		UpdateStatusForConfig();
		s_frameGenerationStateSerial.fetch_add( 1, std::memory_order_release );
	}

	void SetFrameGenerationFlowScale( uint32_t percent )
	{
		std::lock_guard lock{ s_frameGenerationMutex };
		const uint32_t newPercent = std::clamp(
			percent,
			kFrameGenerationMinFlowScalePercent,
			kFrameGenerationMaxFlowScalePercent );
		if ( newPercent == s_frameGenerationConfig.flowScalePercent )
			return;

		s_frameGenerationConfig.flowScalePercent = newPercent;
		UpdateStatusForConfig();
		s_frameGenerationStateSerial.fetch_add( 1, std::memory_order_release );
	}

	FrameGenerationConfig GetFrameGenerationConfig()
	{
		std::lock_guard lock{ s_frameGenerationMutex };
		return s_frameGenerationConfig;
	}

	void SetFrameGenerationStatus( FrameGenerationStatus status )
	{
		std::lock_guard lock{ s_frameGenerationMutex };
		if ( status == s_frameGenerationStatus )
			return;

		s_frameGenerationStatus = status;
		s_frameGenerationStateSerial.fetch_add( 1, std::memory_order_release );
	}

	FrameGenerationStatus GetFrameGenerationStatus()
	{
		std::lock_guard lock{ s_frameGenerationMutex };
		return s_frameGenerationStatus;
	}

	uint64_t GetFrameGenerationStateSerial()
	{
		return s_frameGenerationStateSerial.load( std::memory_order_acquire );
	}
}
