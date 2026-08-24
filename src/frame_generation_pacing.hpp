#pragma once

#include <cstdint>

namespace gamescope
{
	constexpr bool FrameGenerationRationalSlotDue(
		uint64_t index, uint64_t rate, uint64_t clockRate )
	{
		if ( rate == 0 || clockRate == 0 )
			return false;
		const uint64_t slot = index * rate / clockRate;
		const uint64_t previous = index > 0
			? ( index - 1 ) * rate / clockRate : uint64_t( -1 );
		return slot != previous;
	}

	constexpr bool FrameGenerationSourceSlotDue(
		uint64_t vblankIndex, uint64_t totalFps, uint64_t refreshHz )
	{
		return FrameGenerationRationalSlotDue(
			vblankIndex, totalFps, refreshHz * 2u );
	}

	constexpr bool FrameGenerationOutputSlotDue(
		uint64_t vblankIndex, uint64_t totalFps, uint64_t refreshHz )
	{
		return FrameGenerationRationalSlotDue(
			vblankIndex, totalFps, refreshHz );
	}

	constexpr bool FrameGenerationCanRequestSourceFrame(
		uint32_t queuedFrames, bool frontFrameGenerated )
	{
		return queuedFrames == 0u ||
			( queuedFrames == 1u && !frontFrameGenerated );
	}

	constexpr bool FrameGenerationOutputDeadlineMissed(
		uint64_t now, uint64_t deadline, uint64_t interval )
	{
		return deadline != 0u && interval != 0u && now >= deadline &&
			now - deadline >= interval;
	}

	constexpr uint64_t FrameGenerationNextOutputDeadline(
		uint64_t previousDeadline, uint64_t now, uint64_t interval )
	{
		if ( interval == 0u )
			return 0u;
		if ( previousDeadline != 0u && now >= previousDeadline &&
			now - previousDeadline < interval )
			return previousDeadline + interval;
		return now + interval;
	}
}
