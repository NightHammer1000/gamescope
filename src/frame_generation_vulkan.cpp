#include "frame_generation_vulkan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "frame_generation_config.hpp"
#include "cs_ffx_opticalflow_prepare_luma_scaled.h"
#include "cs_ffx_frameinterpolation_midpoint.h"
#include "cs_ffx_frameinterpolation_vector_field.h"
#include "ffx_frameinterpolation_compute_inpainting_pyramid_pass.h"
#include "ffx_frameinterpolation_inpainting_pass.h"
#include "ffx_opticalflow_compute_luminance_pyramid_pass.h"
#include "ffx_opticalflow_compute_optical_flow_advanced_pass_v5.h"
#include "ffx_opticalflow_compute_scd_divergence_pass.h"
#include "ffx_opticalflow_filter_optical_flow_pass_v5.h"
#include "ffx_opticalflow_generate_scd_histogram_pass.h"
#include "ffx_opticalflow_prepare_luma_pass.h"
#include "ffx_opticalflow_scale_optical_flow_advanced_pass_v5.h"
#include "convar.h"

gamescope::ConVar<bool> cv_frame_generation_ab(
	"frame_generation_ab", false,
	"Duplicate the current real frame in generated slots for frame-generation A/B testing." );
gamescope::ConVar<bool> cv_frame_generation_debug_flow(
	"frame_generation_debug_flow", false,
	"Visualize FidelityFX optical-flow vectors and field confidence." );

namespace
{
	constexpr uint32_t kPyramidLevels = 7;
	constexpr uint32_t kHistogramWidth = 256 * 3 * 3;

	struct alignas( 16 ) OpticalFlowConstants
	{
		int32_t inputLumaResolution[2];
		uint32_t opticalFlowPyramidLevel;
		uint32_t opticalFlowPyramidLevelCount;
		uint32_t frameIndex;
		uint32_t backbufferTransferFunction;
		float minMaxLuminance[2];
		int32_t sourceResolution[2]; // Read only by the scaled prepare-luma adapter.
	};

	struct alignas( 16 ) OpticalFlowSpdConstants
	{
		uint32_t mips;
		uint32_t numWorkGroups;
		uint32_t workGroupOffset[2];
		uint32_t numWorkGroupsOpticalFlowInputPyramid;
		uint32_t padding[3];
	};

	struct alignas( 16 ) FrameInterpolationConstants
	{
		int32_t renderSize[2];
		int32_t displaySize[2];
		float displaySizeRcp[2];
		float cameraNear;
		float cameraFar;
		int32_t upscalerTargetSize[2];
		int32_t mode;
		int32_t reset;
		float deviceToViewDepth[4];
		float deltaTime;
		int32_t hudLessAttachedFactor;
		int32_t distortionFieldSize[2];
		float opticalFlowScale[2];
		int32_t opticalFlowBlockSize;
		uint32_t dispatchFlags;
		int32_t maxRenderSize[2];
		int32_t opticalFlowHalfResMode;
		int32_t numInstances;
		int32_t interpolationRectBase[2];
		int32_t interpolationRectSize[2];
		float debugBarColor[3];
		uint32_t backBufferTransferFunction;
		float minMaxLuminance[2];
		float tanHalfFov;
		int32_t padding;
		float jitter[2];
		float motionVectorScale[2];
	};
	static_assert( sizeof( FrameInterpolationConstants ) == 176 );

	struct alignas( 16 ) FrameInterpolationSpdConstants
	{
		uint32_t mips;
		uint32_t numWorkGroups;
		uint32_t workGroupOffset[2];
	};

	class RawImage
	{
	public:
		RawImage() = default;
		RawImage( const RawImage & ) = delete;
		RawImage &operator=( const RawImage & ) = delete;
		RawImage( RawImage &&other ) noexcept
		{
			*this = std::move( other );
		}
		RawImage &operator=( RawImage &&other ) noexcept
		{
			if ( this == &other )
				return *this;
			Destroy();
			image = std::exchange( other.image, VK_NULL_HANDLE );
			memory = std::exchange( other.memory, VK_NULL_HANDLE );
			view = std::exchange( other.view, VK_NULL_HANDLE );
			mipViews = std::move( other.mipViews );
			format = other.format;
			width = other.width;
			height = other.height;
			return *this;
		}
		~RawImage() { Destroy(); }

		bool Create( uint32_t newWidth, uint32_t newHeight, VkFormat newFormat, uint32_t newMipLevels = 1 )
		{
			width = std::max( newWidth, 1u );
			height = std::max( newHeight, 1u );
			format = newFormat;
			mipLevels = std::max( newMipLevels, 1u );

			const VkImageCreateInfo imageInfo = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.imageType = VK_IMAGE_TYPE_2D,
				.format = format,
				.extent = { width, height, 1 },
				.mipLevels = mipLevels,
				.arrayLayers = 1,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			if ( g_device.vk.CreateImage( g_device.device(), &imageInfo, nullptr, &image ) != VK_SUCCESS )
				return false;

			VkMemoryRequirements requirements = {};
			g_device.vk.GetImageMemoryRequirements( g_device.device(), image, &requirements );
			const int32_t memoryType = g_device.findMemoryType( VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, requirements.memoryTypeBits );
			if ( memoryType < 0 )
				return false;

			const VkMemoryAllocateInfo memoryInfo = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = requirements.size,
				.memoryTypeIndex = uint32_t( memoryType ),
			};
			if ( g_device.vk.AllocateMemory( g_device.device(), &memoryInfo, nullptr, &memory ) != VK_SUCCESS ||
				 g_device.vk.BindImageMemory( g_device.device(), image, memory, 0 ) != VK_SUCCESS )
				return false;

			const VkImageViewCreateInfo viewInfo = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = image,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = format,
				.subresourceRange = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = mipLevels,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
			};
			if ( g_device.vk.CreateImageView( g_device.device(), &viewInfo, nullptr, &view ) != VK_SUCCESS )
				return false;

			mipViews.resize( mipLevels );
			for ( uint32_t level = 0; level < mipLevels; ++level )
			{
				VkImageViewCreateInfo mipViewInfo = viewInfo;
				mipViewInfo.subresourceRange.baseMipLevel = level;
				mipViewInfo.subresourceRange.levelCount = 1;
				if ( g_device.vk.CreateImageView( g_device.device(), &mipViewInfo, nullptr, &mipViews[level] ) != VK_SUCCESS )
					return false;
			}
			return true;
		}

		void Destroy()
		{
			for ( VkImageView mipView : mipViews )
				if ( mipView ) g_device.vk.DestroyImageView( g_device.device(), mipView, nullptr );
			mipViews.clear();
			if ( view )
				g_device.vk.DestroyImageView( g_device.device(), view, nullptr );
			if ( image )
				g_device.vk.DestroyImage( g_device.device(), image, nullptr );
			if ( memory )
				g_device.vk.FreeMemory( g_device.device(), memory, nullptr );
			view = VK_NULL_HANDLE;
			image = VK_NULL_HANDLE;
			memory = VK_NULL_HANDLE;
		}

		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		std::vector<VkImageView> mipViews;
		VkFormat format = VK_FORMAT_UNDEFINED;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t mipLevels = 1;
	};

	class RawBuffer
	{
	public:
		~RawBuffer()
		{
			if ( mapped ) g_device.vk.UnmapMemory( g_device.device(), memory );
			if ( buffer ) g_device.vk.DestroyBuffer( g_device.device(), buffer, nullptr );
			if ( memory ) g_device.vk.FreeMemory( g_device.device(), memory, nullptr );
		}

		bool Create( VkDeviceSize newSize )
		{
			size = newSize;
			const VkBufferCreateInfo bufferInfo = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = size,
				.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};
			if ( g_device.vk.CreateBuffer( g_device.device(), &bufferInfo, nullptr, &buffer ) != VK_SUCCESS )
				return false;
			VkMemoryRequirements requirements = {};
			g_device.vk.GetBufferMemoryRequirements( g_device.device(), buffer, &requirements );
			const int32_t memoryType = g_device.findMemoryType(
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, requirements.memoryTypeBits );
			if ( memoryType < 0 )
				return false;
			const VkMemoryAllocateInfo allocation = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = requirements.size,
				.memoryTypeIndex = uint32_t( memoryType ),
			};
			return g_device.vk.AllocateMemory( g_device.device(), &allocation, nullptr, &memory ) == VK_SUCCESS &&
				g_device.vk.BindBufferMemory( g_device.device(), buffer, memory, 0 ) == VK_SUCCESS &&
				g_device.vk.MapMemory( g_device.device(), memory, 0, size, 0, &mapped ) == VK_SUCCESS;
		}

		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		void *mapped = nullptr;
	};

	struct Pass
	{
		VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkShaderModule shaderModule = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		std::vector<VkDescriptorSetLayoutBinding> bindings;

		void Destroy()
		{
			if ( pipeline )
				g_device.vk.DestroyPipeline( g_device.device(), pipeline, nullptr );
			if ( shaderModule )
				g_device.vk.DestroyShaderModule( g_device.device(), shaderModule, nullptr );
			if ( pipelineLayout )
				g_device.vk.DestroyPipelineLayout( g_device.device(), pipelineLayout, nullptr );
			if ( descriptorLayout )
				g_device.vk.DestroyDescriptorSetLayout( g_device.device(), descriptorLayout, nullptr );
			*this = {};
		}
	};

	struct Descriptor
	{
		VkDescriptorType type;
		VkImageView imageView = VK_NULL_HANDLE;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		VkDeviceSize range = 0;
		VkSampler sampler = VK_NULL_HANDLE;
	};

	class OpticalFlowResources
	{
	public:
		~OpticalFlowResources()
		{
			for ( Pass *pass : AllPasses() )
				pass->Destroy();
			if ( descriptorPool )
				g_device.vk.DestroyDescriptorPool( g_device.device(), descriptorPool, nullptr );
			if ( linearSampler )
				g_device.vk.DestroySampler( g_device.device(), linearSampler, nullptr );
		}

		bool Create( uint32_t sourceWidth, uint32_t sourceHeight, uint32_t scalePercent )
		{
			sourceExtent = { sourceWidth, sourceHeight };
			flowScalePercent = std::clamp(
				scalePercent,
				gamescope::kFrameGenerationMinFlowScalePercent,
				gamescope::kFrameGenerationMaxFlowScalePercent );
			lumaExtent = {
				std::max( 1u, uint32_t( ( uint64_t( sourceWidth ) * flowScalePercent + 99 ) / 100 ) ),
				std::max( 1u, uint32_t( ( uint64_t( sourceHeight ) * flowScalePercent + 99 ) / 100 ) ),
			};

			if ( !CheckCapabilities() || !CreatePool() || !CreatePasses() )
				return false;

			for ( uint32_t ping = 0; ping < 2; ++ping )
			{
				for ( uint32_t level = 0; level < kPyramidLevels; ++level )
				{
					const uint32_t lumaWidth = std::max( lumaExtent.width >> level, 1u );
					const uint32_t lumaHeight = std::max( lumaExtent.height >> level, 1u );
					if ( !luma[ping][level].Create( lumaWidth, lumaHeight, VK_FORMAT_R8_UINT ) )
						return false;
				}
			}

			uint32_t flowWidth = ( lumaExtent.width + 7 ) / 8;
			uint32_t flowHeight = ( lumaExtent.height + 7 ) / 8;
			for ( uint32_t level = 0; level < kPyramidLevels; ++level )
			{
				for ( uint32_t ping = 0; ping < 2; ++ping )
				{
					if ( !flow[ping][level].Create( flowWidth, flowHeight, VK_FORMAT_R16G16_SINT ) )
						return false;
				}
				flowExtent[level] = { flowWidth, flowHeight };
				flowWidth = std::max( ( flowWidth + 1 ) / 2, 1u );
				flowHeight = std::max( ( flowHeight + 1 ) / 2, 1u );
			}

			if ( !outputFlow.Create( flowExtent[0].width, flowExtent[0].height, VK_FORMAT_R16G16_SINT ) ||
				 !scdHistogram.Create( kHistogramWidth, 1, VK_FORMAT_R32_UINT ) )
				return false;
			if ( !scdPreviousHistogram.Create( kHistogramWidth, 1, VK_FORMAT_R32_SFLOAT ) ||
				 !scdTemp.Create( 3, 1, VK_FORMAT_R32_UINT ) || !scdOutput.Create( 3, 1, VK_FORMAT_R32_UINT ) ||
				 !vectorFieldX.Create( flowExtent[0].width, flowExtent[0].height, VK_FORMAT_R32_UINT ) ||
				 !vectorFieldY.Create( flowExtent[0].width, flowExtent[0].height, VK_FORMAT_R32_UINT ) )
				return false;

			const uint32_t pyramidWidth = std::max( sourceWidth / 2, 1u );
			const uint32_t pyramidHeight = std::max( sourceHeight / 2, 1u );
			const uint32_t pyramidMips = std::min( 13u,
				uint32_t( std::floor( std::log2( std::max( pyramidWidth, pyramidHeight ) ) ) ) + 1u );
			if ( !inpaintingPyramid.Create( pyramidWidth, pyramidHeight, VK_FORMAT_R16G16B16A16_SFLOAT, pyramidMips ) ||
				 !counters.Create( 8 ) )
				return false;

			CVulkanTexture::createFlags midpointFlags;
			midpointFlags.bSampled = true;
			midpointFlags.bStorage = true;
			midpoint = new CVulkanTexture;
			if ( !midpoint->BInit( sourceWidth, sourceHeight, 1, DRM_FORMAT_ABGR16161616F, midpointFlags ) )
				return false;

			const VkSamplerCreateInfo samplerInfo = {
				.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				.magFilter = VK_FILTER_LINEAR,
				.minFilter = VK_FILTER_LINEAR,
				.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
				.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				.maxLod = float( pyramidMips ),
			};
			return g_device.vk.CreateSampler( g_device.device(), &samplerInfo, nullptr, &linearSampler ) == VK_SUCCESS;
		}

		bool ResetDescriptors()
		{
			return g_device.vk.ResetDescriptorPool( g_device.device(), descriptorPool, 0 ) == VK_SUCCESS;
		}

		std::array<Pass *, 12> AllPasses()
		{
			return { &prepare, &prepareScaled, &pyramid, &histogram, &divergence, &search, &filter, &scale,
				&vectorField, &midpointPass, &inpaintingPyramidPass, &inpaintingPass };
		}

		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		Pass prepare;
		Pass prepareScaled;
		Pass pyramid;
		Pass histogram;
		Pass divergence;
		Pass search;
		Pass filter;
		Pass scale;
		Pass vectorField;
		Pass midpointPass;
		Pass inpaintingPyramidPass;
		Pass inpaintingPass;
		std::array<std::array<RawImage, kPyramidLevels>, 2> luma;
		std::array<std::array<RawImage, kPyramidLevels>, 2> flow;
		std::array<VkExtent2D, kPyramidLevels> flowExtent = {};
		RawImage outputFlow;
		RawImage scdHistogram;
		RawImage scdPreviousHistogram;
		RawImage scdTemp;
		RawImage scdOutput;
		RawImage vectorFieldX;
		RawImage vectorFieldY;
		RawImage inpaintingPyramid;
		RawBuffer counters;
		gamescope::OwningRc<CVulkanTexture> midpoint;
		VkSampler linearSampler = VK_NULL_HANDLE;
		VkExtent2D sourceExtent = {};
		VkExtent2D lumaExtent = {};
		uint32_t flowScalePercent = 100;
		bool initializedLayouts = false;
		uint64_t sequence = 0;

	private:
		bool CheckCapabilities()
		{
			for ( VkFormat format : { VK_FORMAT_R8_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32_UINT,
				VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT } )
			{
				VkFormatProperties properties = {};
				g_device.vk.GetPhysicalDeviceFormatProperties( g_device.physDev(), format, &properties );
				const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
				if ( ( properties.optimalTilingFeatures & required ) != required )
					return false;
			}

			VkPhysicalDeviceSubgroupProperties subgroup = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
			};
			VkPhysicalDeviceProperties2 properties = {
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
				.pNext = &subgroup,
			};
			g_device.vk.GetPhysicalDeviceProperties2( g_device.physDev(), &properties );
			return g_device.supportsStorageImageReadWithoutFormat() &&
				g_device.supportsStorageImageWriteWithoutFormat() &&
				( subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT ) &&
				( subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT ) &&
				( subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT );
		}

		bool CreatePool()
		{
			const std::array<VkDescriptorPoolSize, 5> sizes = {{
				{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128 },
				{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256 },
				{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 128 },
				{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32 },
				{ VK_DESCRIPTOR_TYPE_SAMPLER, 32 },
			}};
			const VkDescriptorPoolCreateInfo info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = 128,
				.poolSizeCount = uint32_t( sizes.size() ),
				.pPoolSizes = sizes.data(),
			};
			return g_device.vk.CreateDescriptorPool( g_device.device(), &info, nullptr, &descriptorPool ) == VK_SUCCESS;
		}

		bool CreatePassBindings( Pass &pass, const uint32_t *spirv, size_t size,
			std::initializer_list<VkDescriptorSetLayoutBinding> bindings )
		{
			pass.bindings.assign( bindings );
			const VkDescriptorSetLayoutCreateInfo descriptorInfo = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = uint32_t( pass.bindings.size() ),
				.pBindings = pass.bindings.data(),
			};
			if ( g_device.vk.CreateDescriptorSetLayout( g_device.device(), &descriptorInfo, nullptr, &pass.descriptorLayout ) != VK_SUCCESS )
				return false;

			const VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &pass.descriptorLayout,
			};
			if ( g_device.vk.CreatePipelineLayout( g_device.device(), &pipelineLayoutInfo, nullptr, &pass.pipelineLayout ) != VK_SUCCESS )
				return false;

			const VkShaderModuleCreateInfo moduleInfo = {
				.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
				.codeSize = size,
				.pCode = spirv,
			};
			if ( g_device.vk.CreateShaderModule( g_device.device(), &moduleInfo, nullptr, &pass.shaderModule ) != VK_SUCCESS )
				return false;

			const VkPipelineShaderStageCreateInfo stage = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = pass.shaderModule,
				.pName = "main",
			};
			const VkComputePipelineCreateInfo pipelineInfo = {
				.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
				.stage = stage,
				.layout = pass.pipelineLayout,
			};
			return g_device.vk.CreateComputePipelines( g_device.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pass.pipeline ) == VK_SUCCESS;
		}

		bool CreatePass( Pass &pass, const uint32_t *spirv, size_t size, std::initializer_list<VkDescriptorType> types )
		{
			std::vector<VkDescriptorSetLayoutBinding> bindings;
			uint32_t binding = 0;
			for ( VkDescriptorType type : types )
				bindings.push_back( { binding++, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } );
			pass.bindings = std::move( bindings );
			const VkDescriptorSetLayoutCreateInfo descriptorInfo = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = uint32_t( pass.bindings.size() ),
				.pBindings = pass.bindings.data(),
			};
			if ( g_device.vk.CreateDescriptorSetLayout( g_device.device(), &descriptorInfo, nullptr, &pass.descriptorLayout ) != VK_SUCCESS )
				return false;
			const VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &pass.descriptorLayout, 0, nullptr };
			if ( g_device.vk.CreatePipelineLayout( g_device.device(), &layoutInfo, nullptr, &pass.pipelineLayout ) != VK_SUCCESS ) return false;
			const VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, size, spirv };
			if ( g_device.vk.CreateShaderModule( g_device.device(), &moduleInfo, nullptr, &pass.shaderModule ) != VK_SUCCESS ) return false;
			const VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, pass.shaderModule, "main", nullptr };
			const VkComputePipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0, stage, pass.pipelineLayout, VK_NULL_HANDLE, 0 };
			return g_device.vk.CreateComputePipelines( g_device.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pass.pipeline ) == VK_SUCCESS;
		}

		bool CreatePasses()
		{
			using D = VkDescriptorType;
			const auto Binding = []( uint32_t binding, D type ) {
				return VkDescriptorSetLayoutBinding{ binding, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
			};
			return CreatePass( prepare, ffx_opticalflow_prepare_luma_pass, sizeof( ffx_opticalflow_prepare_luma_pass ),
				{ D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER } ) &&
				CreatePass( prepareScaled, cs_ffx_opticalflow_prepare_luma_scaled, sizeof( cs_ffx_opticalflow_prepare_luma_scaled ),
					{ D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER } ) &&
				CreatePass( pyramid, ffx_opticalflow_compute_luminance_pyramid_pass, sizeof( ffx_opticalflow_compute_luminance_pyramid_pass ),
					{ D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
					  D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
					  D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER } ) &&
				CreatePass( histogram, ffx_opticalflow_generate_scd_histogram_pass, sizeof( ffx_opticalflow_generate_scd_histogram_pass ),
					{ D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER } ) &&
				CreatePass( divergence, ffx_opticalflow_compute_scd_divergence_pass, sizeof( ffx_opticalflow_compute_scd_divergence_pass ),
					{ D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
					  D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER } ) &&
				CreatePass( search, ffx_opticalflow_compute_optical_flow_advanced_pass_v5, sizeof( ffx_opticalflow_compute_optical_flow_advanced_pass_v5 ),
					{ D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
					  D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER } ) &&
				CreatePass( filter, ffx_opticalflow_filter_optical_flow_pass_v5, sizeof( ffx_opticalflow_filter_optical_flow_pass_v5 ),
					{ D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER } ) &&
				CreatePass( scale, ffx_opticalflow_scale_optical_flow_advanced_pass_v5, sizeof( ffx_opticalflow_scale_optical_flow_advanced_pass_v5 ),
					{ D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
					  D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER } ) &&
				CreatePassBindings( vectorField, cs_ffx_frameinterpolation_vector_field, sizeof( cs_ffx_frameinterpolation_vector_field ), {
					Binding( 0, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ), Binding( 1, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ),
					Binding( 2, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ), Binding( 3, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ),
					Binding( 4, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ), Binding( 5, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ),
					Binding( 1000, D::VK_DESCRIPTOR_TYPE_SAMPLER ),
				} ) &&
				CreatePassBindings( midpointPass, cs_ffx_frameinterpolation_midpoint, sizeof( cs_ffx_frameinterpolation_midpoint ), {
					Binding( 0, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ), Binding( 1, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ),
					Binding( 2, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ), Binding( 3, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ),
					Binding( 4, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ), Binding( 5, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ),
					Binding( 6, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ), Binding( 1000, D::VK_DESCRIPTOR_TYPE_SAMPLER ),
				} ) &&
				CreatePassBindings( inpaintingPyramidPass, ffx_frameinterpolation_compute_inpainting_pyramid_pass,
					sizeof( ffx_frameinterpolation_compute_inpainting_pyramid_pass ), {
					Binding( 0, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ), Binding( 1, D::VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ),
					Binding( 2, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ), Binding( 3, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ),
					Binding( 4, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ), Binding( 5, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ),
					Binding( 6, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ), Binding( 7, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ),
					Binding( 8, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ), Binding( 9, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ),
					Binding( 10, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ), Binding( 11, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ),
					Binding( 12, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ), Binding( 13, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ),
					Binding( 14, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ), Binding( 15, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ),
					Binding( 16, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ), Binding( 1000, D::VK_DESCRIPTOR_TYPE_SAMPLER ),
				} ) &&
				CreatePassBindings( inpaintingPass, ffx_frameinterpolation_inpainting_pass,
					sizeof( ffx_frameinterpolation_inpainting_pass ), {
					Binding( 0, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ), Binding( 1, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ),
					Binding( 2, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ), Binding( 3, D::VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ),
					Binding( 7, D::VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ), Binding( 8, D::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ),
					Binding( 1000, D::VK_DESCRIPTOR_TYPE_SAMPLER ),
				} );
		}
	};

	class OpticalFlowContext
	{
	public:
		bool Record( CVulkanCmdBuffer *cmdBuffer, gamescope::Rc<CVulkanTexture> source, uint32_t scalePercent, bool reset )
		{
			if ( !cmdBuffer || !source || source->isYcbcr() )
				return false;
			CollectRetired();
			const uint32_t clampedScale = std::clamp(
				scalePercent,
				gamescope::kFrameGenerationMinFlowScalePercent,
				gamescope::kFrameGenerationMaxFlowScalePercent );
			if ( !resources || resources->sourceExtent.width != source->width() ||
				 resources->sourceExtent.height != source->height() || resources->flowScalePercent != clampedScale )
			{
				if ( resources && !g_device.isComplete( resources->sequence ) )
					return false;
				resources.reset();
				resources = std::make_shared<OpticalFlowResources>();
				if ( !resources->Create( source->width(), source->height(), clampedScale ) )
				{
					resources.reset();
					fprintf( stderr, "FidelityFX Optical Flow is unsupported for this GPU or extent\n" );
					return false;
				}
				reset = true;
			}

			if ( !g_device.isComplete( resources->sequence ) || !resources->ResetDescriptors() )
				return false;

			cmdBuffer->bindTexture( 0, source );
			cmdBuffer->prepareSrcImage( source.get() );
			cmdBuffer->insertBarrier();
			if ( !resources->initializedLayouts )
			{
				TransitionAll( cmdBuffer->rawBuffer() );
				resources->initializedLayouts = true;
				reset = true;
			}
			if ( reset || firstExecution )
			{
				ClearAll( cmdBuffer->rawBuffer() );
				frameIndex = 0;
			}
			else
			{
				++frameIndex;
			}

			const uint32_t current = resourceFrameIndex & 1u;
			const uint32_t previous = current ^ 1u;
			OpticalFlowConstants constants = {
				.inputLumaResolution = { int32_t( resources->lumaExtent.width ), int32_t( resources->lumaExtent.height ) },
				.opticalFlowPyramidLevel = 0,
				.opticalFlowPyramidLevelCount = kPyramidLevels,
				.frameIndex = frameIndex,
				.backbufferTransferFunction = 0,
				.minMaxLuminance = { 0.0f, 1000.0f },
				.sourceResolution = { int32_t( source->width() ), int32_t( source->height() ) },
			};

			const auto common = Upload( constants );
			if ( !Dispatch( cmdBuffer->rawBuffer(), clampedScale == 100 ? resources->prepare : resources->prepareScaled,
				{
					Sampled( source->srgbView() ), Storage( resources->luma[current][0] ), common,
				}, ( resources->lumaExtent.width + 31 ) / 32, ( resources->lumaExtent.height + 31 ) / 32 ) )
				return false;
			Barrier( cmdBuffer->rawBuffer() );

			const uint32_t pyramidX = ( resources->lumaExtent.width + 63 ) / 64;
			const uint32_t pyramidY = ( resources->lumaExtent.height + 63 ) / 64;
			const OpticalFlowSpdConstants spd = {
				.mips = 4,
				.numWorkGroups = pyramidX * pyramidY,
				.workGroupOffset = { 0, 0 },
				.numWorkGroupsOpticalFlowInputPyramid = pyramidX * pyramidY,
			};
			const auto spdBuffer = Upload( spd );
			std::vector<Descriptor> pyramidBindings;
			for ( RawImage &image : resources->luma[current] )
				pyramidBindings.push_back( Storage( image ) );
			pyramidBindings.push_back( common );
			pyramidBindings.push_back( spdBuffer );
			if ( !Dispatch( cmdBuffer->rawBuffer(), resources->pyramid, pyramidBindings, pyramidX, pyramidY ) )
				return false;
			Barrier( cmdBuffer->rawBuffer() );

			const uint32_t strataWidth = std::max( ( resources->lumaExtent.width / 4 ) / 3, 1u );
			if ( !Dispatch( cmdBuffer->rawBuffer(), resources->histogram,
				{ Sampled( resources->luma[current][0] ), Storage( resources->scdHistogram ), common },
				( strataWidth + 31 ) / 32, 16, 9 ) )
				return false;
			Barrier( cmdBuffer->rawBuffer() );
			if ( !Dispatch( cmdBuffer->rawBuffer(), resources->divergence,
				{ Storage( resources->scdHistogram ), Storage( resources->scdPreviousHistogram ), Storage( resources->scdTemp ),
				  Storage( resources->scdOutput ), common }, 9, 3 ) )
				return false;
			Barrier( cmdBuffer->rawBuffer() );

			for ( int32_t level = int32_t( kPyramidLevels ) - 1; level >= 0; --level )
			{
				constants.opticalFlowPyramidLevel = uint32_t( level );
				const auto levelConstants = Upload( constants );
				const bool oddLevel = ( level & 1 ) != 0;
				const uint32_t a = ( bool( current ) != oddLevel ) ? 1u : 0u;
				const uint32_t b = a ^ 1u;
				const uint32_t lumaWidth = std::max( resources->lumaExtent.width >> level, 1u );
				const uint32_t lumaHeight = std::max( resources->lumaExtent.height >> level, 1u );
				const uint32_t searchX = ( ( ( lumaWidth + 3 ) / 4 ) * 16 + 63 ) / 64;
				const uint32_t searchY = ( lumaHeight + 15 ) / 16;
				if ( !Dispatch( cmdBuffer->rawBuffer(), resources->search,
					{ Sampled( resources->luma[current][level] ), Sampled( resources->luma[previous][level] ),
					  Storage( resources->flow[a][level] ), Storage( resources->scdOutput ), levelConstants }, searchX, searchY ) )
					return false;
				Barrier( cmdBuffer->rawBuffer() );

				RawImage &filterOutput = level == 0 ? resources->outputFlow : resources->flow[b][level];
				if ( !Dispatch( cmdBuffer->rawBuffer(), resources->filter,
					{ Sampled( resources->flow[a][level] ), Storage( filterOutput ), levelConstants },
					( resources->flowExtent[level].width + 15 ) / 16,
					( resources->flowExtent[level].height + 3 ) / 4 ) )
					return false;
				Barrier( cmdBuffer->rawBuffer() );

				if ( level > 0 )
				{
					if ( !Dispatch( cmdBuffer->rawBuffer(), resources->scale,
						{ Sampled( resources->luma[current][level - 1] ), Sampled( resources->luma[previous][level - 1] ),
						  Sampled( resources->flow[b][level] ), Storage( resources->flow[b][level - 1] ),
						  Storage( resources->scdOutput ), levelConstants },
						( resources->flowExtent[level - 1].width + 3 ) / 4,
						( resources->flowExtent[level - 1].height + 3 ) / 4 ) )
						return false;
					Barrier( cmdBuffer->rawBuffer() );
				}
			}

			resourceFrameIndex = ( resourceFrameIndex + 1 ) & 1u;
			firstExecution = false;
			recordedResources = resources;
			return true;
		}

		gamescope::Rc<CVulkanTexture> RecordInterpolation( CVulkanCmdBuffer *cmdBuffer,
			gamescope::Rc<CVulkanTexture> previous, gamescope::Rc<CVulkanTexture> current, bool reset )
		{
			if ( !cmdBuffer || !resources || recordedResources != resources || !previous || !current ||
				 previous->isYcbcr() || current->isYcbcr() ||
				 previous->width() != resources->sourceExtent.width || previous->height() != resources->sourceExtent.height ||
				 current->width() != resources->sourceExtent.width || current->height() != resources->sourceExtent.height )
				return nullptr;

			cmdBuffer->bindTexture( 1, previous );
			cmdBuffer->bindTexture( 2, current );
			cmdBuffer->prepareSrcImage( previous.get() );
			cmdBuffer->prepareSrcImage( current.get() );
			cmdBuffer->bindTarget( resources->midpoint );
			cmdBuffer->prepareDestImage( resources->midpoint.get() );
			cmdBuffer->insertBarrier();

			const VkClearColorValue zero = {};
			const VkImageSubresourceRange oneLevel = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			g_device.vk.CmdClearColorImage( cmdBuffer->rawBuffer(), resources->vectorFieldX.image,
				VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &oneLevel );
			g_device.vk.CmdClearColorImage( cmdBuffer->rawBuffer(), resources->vectorFieldY.image,
				VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &oneLevel );
			Barrier( cmdBuffer->rawBuffer() );

			FrameInterpolationConstants constants = {};
			constants.renderSize[0] = int32_t( resources->sourceExtent.width );
			constants.renderSize[1] = int32_t( resources->sourceExtent.height );
			constants.displaySize[0] = constants.renderSize[0];
			constants.displaySize[1] = constants.renderSize[1];
			constants.displaySizeRcp[0] = 1.0f / float( constants.displaySize[0] );
			constants.displaySizeRcp[1] = 1.0f / float( constants.displaySize[1] );
			constants.upscalerTargetSize[0] = constants.displaySize[0];
			constants.upscalerTargetSize[1] = constants.displaySize[1];
			constants.reset = reset || frameIndex == 0;
			constants.opticalFlowScale[0] = 1.0f / float( resources->lumaExtent.width );
			constants.opticalFlowScale[1] = 1.0f / float( resources->lumaExtent.height );
			constants.opticalFlowBlockSize = 8;
			constants.maxRenderSize[0] = constants.displaySize[0];
			constants.maxRenderSize[1] = constants.displaySize[1];
			constants.interpolationRectSize[0] = constants.displaySize[0];
			constants.interpolationRectSize[1] = constants.displaySize[1];
			constants.dispatchFlags = cv_frame_generation_debug_flow ? 0x80000000u : 0u;
			constants.minMaxLuminance[1] = 1000.0f;
			const Descriptor fiConstants = Upload( constants );

			if ( !Dispatch( cmdBuffer->rawBuffer(), resources->vectorField, {
				Sampled( resources->outputFlow ), Sampled( previous->srgbView() ), Sampled( current->srgbView() ),
				Storage( resources->vectorFieldX ), Storage( resources->vectorFieldY ), fiConstants,
				Sampler( resources->linearSampler ),
			}, ( resources->flowExtent[0].width + 7 ) / 8, ( resources->flowExtent[0].height + 7 ) / 8 ) )
				return nullptr;
			Barrier( cmdBuffer->rawBuffer() );

			if ( !Dispatch( cmdBuffer->rawBuffer(), resources->midpointPass, {
				Sampled( resources->vectorFieldX ), Sampled( resources->vectorFieldY ), Sampled( previous->srgbView() ),
				Sampled( current->srgbView() ), Sampled( resources->scdOutput ), StorageView( resources->midpoint->srgbView() ),
				fiConstants, Sampler( resources->linearSampler ),
			}, ( resources->sourceExtent.width + 7 ) / 8, ( resources->sourceExtent.height + 7 ) / 8 ) )
				return nullptr;
			Barrier( cmdBuffer->rawBuffer() );

			memset( resources->counters.mapped, 0, size_t( resources->counters.size ) );
			const uint32_t spdX = ( resources->sourceExtent.width + 63 ) / 64;
			const uint32_t spdY = ( resources->sourceExtent.height + 63 ) / 64;
			const FrameInterpolationSpdConstants spdConstants = {
				.mips = std::min( 12u, uint32_t( std::floor( std::log2( std::max(
					resources->sourceExtent.width, resources->sourceExtent.height ) ) ) ) ),
				.numWorkGroups = spdX * spdY,
				.workGroupOffset = { 0, 0 },
			};
			const Descriptor fiSpdConstants = Upload( spdConstants );
			std::vector<Descriptor> inpaintingPyramidBindings = {
				Sampled( resources->midpoint->srgbView() ), StorageBuffer( resources->counters ),
			};
			for ( uint32_t level = 0; level < 13; ++level )
				inpaintingPyramidBindings.push_back( StorageView( resources->inpaintingPyramid.mipViews[
					std::min( level, resources->inpaintingPyramid.mipLevels - 1 )] ) );
			inpaintingPyramidBindings.push_back( fiConstants );
			inpaintingPyramidBindings.push_back( fiSpdConstants );
			inpaintingPyramidBindings.push_back( Sampler( resources->linearSampler ) );
			if ( !Dispatch( cmdBuffer->rawBuffer(), resources->inpaintingPyramidPass,
				inpaintingPyramidBindings, spdX, spdY ) )
				return nullptr;
			Barrier( cmdBuffer->rawBuffer() );

			if ( !Dispatch( cmdBuffer->rawBuffer(), resources->inpaintingPass, {
				Sampled( resources->scdOutput ), Sampled( resources->inpaintingPyramid ), Sampled( current->srgbView() ),
				Sampled( current->srgbView() ), StorageView( resources->midpoint->srgbView() ), fiConstants,
				Sampler( resources->linearSampler ),
			}, ( resources->sourceExtent.width + 7 ) / 8, ( resources->sourceExtent.height + 7 ) / 8 ) )
				return nullptr;

			cmdBuffer->markDirty( resources->midpoint.get() );
			return resources->midpoint;
		}

		void NotifySubmit( uint64_t sequence )
		{
			if ( recordedResources )
				recordedResources->sequence = sequence;
			recordedResources.reset();
		}

		bool Complete() const
		{
			return !resources || g_device.isComplete( resources->sequence );
		}

		void Reset()
		{
			firstExecution = true;
			frameIndex = 0;
			resourceFrameIndex = 0;
		}

	private:
		Descriptor UploadBytes( const void *data, size_t size )
		{
			VkPhysicalDeviceProperties properties = {};
			g_device.vk.GetPhysicalDeviceProperties( g_device.physDev(), &properties );
			const uint32_t alignment = uint32_t( properties.limits.minUniformBufferOffsetAlignment );
			auto [target, offset] = g_device.uploadBufferData( uint32_t( size ), std::max( alignment, 16u ) );
			memcpy( target, data, size );
			return { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_NULL_HANDLE, g_device.uploadBuffer(), offset, size };
		}

		template<typename T>
		Descriptor Upload( const T &value )
		{
			return UploadBytes( &value, sizeof( value ) );
		}

		static Descriptor Sampled( const RawImage &image )
		{
			return { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, image.view };
		}
		static Descriptor Sampled( VkImageView view )
		{
			return { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view };
		}
		static Descriptor Storage( const RawImage &image )
		{
			return { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, image.view };
		}
		static Descriptor StorageView( VkImageView view )
		{
			return { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view };
		}
		static Descriptor StorageBuffer( const RawBuffer &buffer )
		{
			return { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_NULL_HANDLE, buffer.buffer, 0, buffer.size };
		}
		static Descriptor Sampler( VkSampler sampler )
		{
			Descriptor result = { VK_DESCRIPTOR_TYPE_SAMPLER };
			result.sampler = sampler;
			return result;
		}

		bool Dispatch( VkCommandBuffer commandBuffer, Pass &pass, const std::vector<Descriptor> &descriptors,
			uint32_t x, uint32_t y, uint32_t z = 1 )
		{
			if ( descriptors.size() != pass.bindings.size() )
				return false;
			VkDescriptorSet set = VK_NULL_HANDLE;
			const VkDescriptorSetAllocateInfo allocateInfo = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = resources->descriptorPool,
				.descriptorSetCount = 1,
				.pSetLayouts = &pass.descriptorLayout,
			};
			if ( g_device.vk.AllocateDescriptorSets( g_device.device(), &allocateInfo, &set ) != VK_SUCCESS )
				return false;

			std::vector<VkDescriptorImageInfo> imageInfos( descriptors.size() );
			std::vector<VkDescriptorBufferInfo> bufferInfos( descriptors.size() );
			std::vector<VkWriteDescriptorSet> writes( descriptors.size() );
			for ( uint32_t i = 0; i < descriptors.size(); ++i )
			{
				const Descriptor &descriptor = descriptors[i];
				imageInfos[i] = { descriptor.sampler, descriptor.imageView, VK_IMAGE_LAYOUT_GENERAL };
				bufferInfos[i] = { descriptor.buffer, descriptor.offset, descriptor.range };
				writes[i] = {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = set,
					.dstBinding = pass.bindings[i].binding,
					.descriptorCount = 1,
					.descriptorType = descriptor.type,
					.pImageInfo = descriptor.imageView || descriptor.sampler ? &imageInfos[i] : nullptr,
					.pBufferInfo = descriptor.buffer ? &bufferInfos[i] : nullptr,
				};
			}
			g_device.vk.UpdateDescriptorSets( g_device.device(), uint32_t( writes.size() ), writes.data(), 0, nullptr );
			g_device.vk.CmdBindPipeline( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline );
			g_device.vk.CmdBindDescriptorSets( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelineLayout,
				0, 1, &set, 0, nullptr );
			g_device.vk.CmdDispatch( commandBuffer, x, y, z );
			return true;
		}

		static void Barrier( VkCommandBuffer commandBuffer )
		{
			const VkMemoryBarrier barrier = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
			};
			g_device.vk.CmdPipelineBarrier( commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr );
		}

		std::vector<RawImage *> Images()
		{
			std::vector<RawImage *> result;
			for ( auto &ping : resources->luma )
				for ( auto &image : ping ) result.push_back( &image );
			for ( auto &ping : resources->flow )
				for ( auto &image : ping ) result.push_back( &image );
			result.push_back( &resources->outputFlow );
			result.push_back( &resources->scdHistogram );
			result.push_back( &resources->scdPreviousHistogram );
			result.push_back( &resources->scdTemp );
			result.push_back( &resources->scdOutput );
			result.push_back( &resources->vectorFieldX );
			result.push_back( &resources->vectorFieldY );
			result.push_back( &resources->inpaintingPyramid );
			return result;
		}

		void TransitionAll( VkCommandBuffer commandBuffer )
		{
			const auto images = Images();
			std::vector<VkImageMemoryBarrier> barriers;
			barriers.reserve( images.size() );
			for ( const RawImage *image : images )
			{
				barriers.push_back( {
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.srcAccessMask = 0,
					.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout = VK_IMAGE_LAYOUT_GENERAL,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = image->image,
					.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, image->mipLevels, 0, 1 },
				} );
			}
			g_device.vk.CmdPipelineBarrier( commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
				0, nullptr, 0, nullptr, uint32_t( barriers.size() ), barriers.data() );
		}

		void ClearAll( VkCommandBuffer commandBuffer )
		{
			const VkClearColorValue zero = {};
			for ( const RawImage *image : Images() )
			{
				const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, image->mipLevels, 0, 1 };
				g_device.vk.CmdClearColorImage( commandBuffer, image->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range );
			}
			Barrier( commandBuffer );
		}

		void CollectRetired()
		{
			std::erase_if( retired, []( const std::shared_ptr<OpticalFlowResources> &entry ) {
				return g_device.isComplete( entry->sequence );
			} );
		}

		std::shared_ptr<OpticalFlowResources> resources;
		std::shared_ptr<OpticalFlowResources> recordedResources;
		std::vector<std::shared_ptr<OpticalFlowResources>> retired;
		uint32_t resourceFrameIndex = 0;
		uint32_t frameIndex = 0;
		bool firstExecution = true;
	};

	OpticalFlowContext &GetOpticalFlowContext()
	{
		// Intentionally process-lifetime. Vulkan teardown owns device ordering and
		// waits idle; runtime reallocations still destroy completed generations.
		static OpticalFlowContext *context = new OpticalFlowContext;
		return *context;
	}
}

bool vulkan_frame_generation_record_optical_flow(
	CVulkanCmdBuffer *cmdBuffer,
	gamescope::Rc<CVulkanTexture> source,
	uint32_t flowScalePercent,
	bool reset )
{
	return GetOpticalFlowContext().Record( cmdBuffer, std::move( source ), flowScalePercent, reset );
}

gamescope::Rc<CVulkanTexture> vulkan_frame_generation_record_interpolation(
	CVulkanCmdBuffer *cmdBuffer,
	gamescope::Rc<CVulkanTexture> previous,
	gamescope::Rc<CVulkanTexture> current,
	bool reset )
{
	return GetOpticalFlowContext().RecordInterpolation(
		cmdBuffer, std::move( previous ), std::move( current ), reset );
}

void vulkan_frame_generation_notify_submit( uint64_t sequence )
{
	GetOpticalFlowContext().NotifySubmit( sequence );
}

bool vulkan_frame_generation_work_complete()
{
	return GetOpticalFlowContext().Complete();
}

void vulkan_frame_generation_reset_optical_flow()
{
	GetOpticalFlowContext().Reset();
}

bool vulkan_frame_generation_ab_enabled()
{
	return cv_frame_generation_ab;
}
