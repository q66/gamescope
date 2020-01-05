// Initialize Vulkan and composite stuff with a compute queue

#include <stdio.h>
#include <string.h>

#include "rendervulkan.hpp"
#include "main.hpp"

#include "composite.h"

PFN_vkGetMemoryFdKHR dyn_vkGetMemoryFdKHR;

const VkApplicationInfo appInfo = {
	.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	.pApplicationName = "steamcompmgr",
	.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
	.pEngineName = "just some code",
	.engineVersion = VK_MAKE_VERSION(1, 0, 0),
	.apiVersion = VK_API_VERSION_1_0,
};

std::vector< const char * > g_vecSDLInstanceExts;

VkInstance instance;

#define k_nMaxSets 8 // should only need one or two per output tops

struct VulkanOutput_t
{
	VkSurfaceKHR surface;
	VkSurfaceCapabilitiesKHR surfaceCaps;
	std::vector< VkSurfaceFormatKHR > surfaceFormats;
	std::vector< VkPresentModeKHR > presentModes;
	uint32_t nSwapChainImageIndex;
	
	VkSwapchainKHR swapChain;
	std::vector< VkImage > swapChainImages;
	std::vector< VkImageView > swapChainImageViews;
	
	// If no swapchain, use our own images
	
	int nOutImage; // ping/pong between two RTs
	CVulkanTexture outputImage[2];

	int nCurCmdBuffer;
	VkCommandBuffer commandBuffers[2]; // ping/pong command buffers as well
	
	VkBuffer constantBuffer;
	VkDeviceMemory bufferMemory;
	Composite_t *pCompositeBuffer;
};


VkPhysicalDevice physicalDevice;
uint32_t queueFamilyIndex;
VkQueue queue;
VkShaderModule shaderModule;
VkDevice device;
VkCommandPool commandPool;
VkDescriptorPool descriptorPool;

VkDescriptorSetLayout descriptorSetLayout;
VkPipelineLayout pipelineLayout;
VkDescriptorSet descriptorSet;

VkPipeline pipeline;

VkBuffer uploadBuffer;
VkDeviceMemory uploadBufferMemory;
void *pUploadBuffer;

bool bUploadCmdBufferIdle;
VkCommandBuffer uploadCommandBuffer;

struct VkPhysicalDeviceMemoryProperties memoryProperties;

VulkanOutput_t g_output;

std::unordered_map<VulkanTexture_t, CVulkanTexture *> g_mapVulkanTextures;
std::atomic<VulkanTexture_t> g_nMaxVulkanTexHandle;

struct VulkanSamplerCacheEntry_t
{
	VulkanPipeline_t::LayerBinding_t key;
	VkSampler sampler;
};

std::vector< VulkanSamplerCacheEntry_t > g_vecVulkanSamplerCache;

#define MAX_DEVICE_COUNT 8
#define MAX_QUEUE_COUNT 8

#define VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA (VkStructureType)1000001002
#define VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA (VkStructureType)1000001003

struct wsi_image_create_info {
	VkStructureType sType;
	const void *pNext;
	bool scanout;
	
	uint32_t modifier_count;
	const uint64_t *modifiers;
};

struct wsi_memory_allocate_info {
	VkStructureType sType;
	const void *pNext;
	bool implicit_sync;
};

struct {
	uint32_t DRMFormat;
	VkFormat vkFormat;
	bool bNeedsSwizzle;
	bool bHasAlpha;
} s_DRMVKFormatTable[] = {
	{ DRM_FORMAT_XRGB8888, VK_FORMAT_A8B8G8R8_UNORM_PACK32, true, false },
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_A8B8G8R8_UNORM_PACK32, true, true },
	{ DRM_FORMAT_RGBA8888, VK_FORMAT_R8G8B8A8_UNORM, false, true },
	{ DRM_FORMAT_INVALID, VK_FORMAT_UNDEFINED, false, false },
};

static inline uint32_t VulkanFormatToDRM( VkFormat vkFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].vkFormat == vkFormat )
		{
			return s_DRMVKFormatTable[i].DRMFormat;
		}
	}
	
	return DRM_FORMAT_INVALID;
}

static inline VkFormat DRMFormatToVulkan( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].vkFormat;
		}
	}
	
	return VK_FORMAT_UNDEFINED;
}

static inline bool DRMFormatNeedsSwizzle( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].bNeedsSwizzle;
		}
	}
	
	return false;
}

static inline bool DRMFormatHasAlpha( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].bHasAlpha;
		}
	}
	
	return false;
}

int32_t findMemoryType( VkMemoryPropertyFlags properties, uint32_t requiredTypeBits )
{
	for ( uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++ )
	{
		if ( ( ( 1 << i ) & requiredTypeBits ) == 0 )
			continue;
		
		if ( ( properties & memoryProperties.memoryTypes[ i ].propertyFlags ) != properties )
			continue;
		
		return i;
	}
	
	return -1;
}

bool CVulkanTexture::BInit( uint32_t width, uint32_t height, VkFormat format, bool bFlippable, bool bTextureable, wlr_dmabuf_attributes *pDMA /* = nullptr */ )
{
	VkResult res = VK_ERROR_INITIALIZATION_FAILED;

	VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
	VkImageUsageFlags usage = bTextureable ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT;
	
	if ( bTextureable == true && pDMA == nullptr )
	{
		// If we're not importing it, we'll need to copy bits into it later 
		usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}

	VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	
	// Possible extensions for below
	wsi_image_create_info wsiImageCreateInfo = {};
	VkExternalMemoryImageCreateInfo externalImageCreateInfo = {};
	
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = usage;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	
	if ( pDMA != nullptr )
	{
		assert( format == DRMFormatToVulkan( pDMA->format ) );
	}
	
	if ( bFlippable == true || pDMA != nullptr )
	{
		// Either we're scanning out the image, or if we're importing them, they got
		// allocated with scanout in mind by their original WSI.
		wsiImageCreateInfo.sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA;
		wsiImageCreateInfo.scanout = VK_TRUE;
		wsiImageCreateInfo.pNext = imageInfo.pNext;
		
		imageInfo.pNext = &wsiImageCreateInfo;
	}
	
	if ( pDMA != nullptr )
	{
		externalImageCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		externalImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		externalImageCreateInfo.pNext = imageInfo.pNext;
		
		imageInfo.pNext = &externalImageCreateInfo;
	}
	
	if (vkCreateImage(device, &imageInfo, nullptr, &m_vkImage) != VK_SUCCESS) {
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(device, m_vkImage, &memRequirements);
	
	// Possible pNexts
	wsi_memory_allocate_info wsiAllocInfo = {};
	VkImportMemoryFdInfoKHR importMemoryInfo = {};
	VkExportMemoryAllocateInfo memory_export_info = {};
	VkMemoryDedicatedAllocateInfo memory_dedicated_info = {};
	
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(properties, memRequirements.memoryTypeBits );
	
	if ( bFlippable == true || pDMA != nullptr )
	{
		memory_dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		memory_dedicated_info.image = m_vkImage;
		memory_dedicated_info.buffer = VK_NULL_HANDLE;
		memory_dedicated_info.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &memory_dedicated_info;
	}
	
	if ( bFlippable == true )
	{
		wsiAllocInfo.sType = VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA;
		wsiAllocInfo.implicit_sync = true;
		wsiAllocInfo.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &wsiAllocInfo;
		
		memory_export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
		memory_export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		memory_export_info.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &memory_export_info;
	}
	
	if ( pDMA != nullptr )
	{
		// Memory already provided by pDMA
		importMemoryInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
		importMemoryInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		importMemoryInfo.fd = pDMA->fd[0];
		importMemoryInfo.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &importMemoryInfo;
	}
	
	if (vkAllocateMemory(device, &allocInfo, nullptr, &m_vkImageMemory) != VK_SUCCESS) {
		return false;
	}
	
	res = vkBindImageMemory(device, m_vkImage, m_vkImageMemory, 0);
	
	if ( res != VK_SUCCESS )
		return false;
	
	if ( bFlippable == true )
	{
		// We assume we own the memory when doing this right now.
		// We could support the import scenario as well if needed
		assert( bTextureable == false );

		m_DMA.modifier = DRM_FORMAT_MOD_INVALID;
		m_DMA.n_planes = 1;
		m_DMA.width = width;
		m_DMA.height = height;
		m_DMA.format = VulkanFormatToDRM( format );

		const VkMemoryGetFdInfoKHR memory_get_fd_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
			.pNext = NULL,
			.memory = m_vkImageMemory,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
		res = dyn_vkGetMemoryFdKHR(device, &memory_get_fd_info, &m_DMA.fd[0]);
		
		if ( res != VK_SUCCESS )
			return false;
		
		const VkImageSubresource image_subresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.arrayLayer = 0,
		};
		VkSubresourceLayout image_layout;
		vkGetImageSubresourceLayout(device, m_vkImage, &image_subresource, &image_layout);
		

		m_DMA.stride[0] = image_layout.rowPitch;
		
		m_FBID = drm_fbid_from_dmabuf( &g_DRM, &m_DMA );
		
		if ( m_FBID == 0 )
			return false;
	}
	
	
	bool bSwapChannels = pDMA ? DRMFormatNeedsSwizzle( pDMA->format ) : false;
	bool bHasAlpha = pDMA ? DRMFormatHasAlpha( pDMA->format ) : true;
	
	if ( bSwapChannels || !bHasAlpha )
	{
		// Right now this implies no storage bit - check it now as that's incompatible with swizzle
		assert ( bTextureable == true );
	}
	
	VkImageViewCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	createInfo.image = m_vkImage;
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	createInfo.format = format;
	createInfo.components.r = bSwapChannels ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.b = bSwapChannels ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_IDENTITY;
// 	createInfo.components.a = bHasAlpha ? VK_COMPONENT_SWIZZLE_IDENTITY : VK_COMPONENT_SWIZZLE_ONE;
	createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	createInfo.subresourceRange.baseMipLevel = 0;
	createInfo.subresourceRange.levelCount = 1;
	createInfo.subresourceRange.baseArrayLayer = 0;
	createInfo.subresourceRange.layerCount = 1;
	
	res = vkCreateImageView(device, &createInfo, nullptr, &m_vkImageView);
	if ( res != VK_SUCCESS )
		return false;
	
	m_bInitialized = true;
	m_bFlippable = bFlippable;
	
	return true;
}

CVulkanTexture::~CVulkanTexture( void )
{
	if ( m_vkImageView != VK_NULL_HANDLE )
	{
		vkDestroyImageView( device, m_vkImageView, nullptr );
		m_vkImageView = VK_NULL_HANDLE;
	}
	
	if ( m_FBID != 0 )
	{
		drm_free_fbid( &g_DRM, m_FBID );
		m_FBID = 0;
	}
	
	
	if ( m_vkImage != VK_NULL_HANDLE )
	{
		vkDestroyImage( device, m_vkImage, nullptr );
		m_vkImage = VK_NULL_HANDLE;
	}
	
	
	if ( m_vkImageMemory != VK_NULL_HANDLE )
	{
		vkFreeMemory( device, m_vkImageMemory, nullptr );	
		m_vkImageMemory = VK_NULL_HANDLE;
	}
	
	m_bInitialized = false;
}


int init_device()
{
	uint32_t physicalDeviceCount;
	VkPhysicalDevice deviceHandles[MAX_DEVICE_COUNT];
	VkQueueFamilyProperties queueFamilyProperties[MAX_QUEUE_COUNT];
	
	vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, 0);
	physicalDeviceCount = physicalDeviceCount > MAX_DEVICE_COUNT ? MAX_DEVICE_COUNT : physicalDeviceCount;
	vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, deviceHandles);
	
	for (uint32_t i = 0; i < physicalDeviceCount; ++i)
	{
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(deviceHandles[i], &queueFamilyCount, NULL);
		queueFamilyCount = queueFamilyCount > MAX_QUEUE_COUNT ? MAX_QUEUE_COUNT : queueFamilyCount;
		vkGetPhysicalDeviceQueueFamilyProperties(deviceHandles[i], &queueFamilyCount, queueFamilyProperties);
		
		for (uint32_t j = 0; j < queueFamilyCount; ++j) {
			if ( queueFamilyProperties[j].queueFlags & VK_QUEUE_COMPUTE_BIT &&
				!(queueFamilyProperties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT ) )
			{
				queueFamilyIndex = j;
				physicalDevice = deviceHandles[i];
			}
		}
		
		if (physicalDevice)
		{
			break;
		}
	}
	
	vkGetPhysicalDeviceMemoryProperties( physicalDevice, &memoryProperties );
	
	float queuePriorities = 1.0f;
	
	VkDeviceQueueCreateInfo queueCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = queueFamilyIndex,
		.queueCount = 1,
		.pQueuePriorities = &queuePriorities
	};
	
	std::vector< const char * > vecEnabledDeviceExtensions;
	
	if ( BIsNested() == true )
	{
		vecEnabledDeviceExtensions.push_back( VK_KHR_SWAPCHAIN_EXTENSION_NAME );
	}
	vecEnabledDeviceExtensions.push_back( VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME );
	vecEnabledDeviceExtensions.push_back( VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME );
	
	VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueCreateInfo,
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = 0,
		.enabledExtensionCount = (uint32_t)vecEnabledDeviceExtensions.size(),
		.ppEnabledExtensionNames = vecEnabledDeviceExtensions.data(),
		.pEnabledFeatures = 0,
	};

	VkResult result = vkCreateDevice(physicalDevice, &deviceCreateInfo, NULL, &device);
	
	if ( result != VK_SUCCESS )
	{
		return false;
	}
	
	vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
	
	if ( queue == VK_NULL_HANDLE )
		return false;
	
	VkShaderModuleCreateInfo shaderModuleCreateInfo = {};
	shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderModuleCreateInfo.codeSize = composite_spv_len;
	shaderModuleCreateInfo.pCode = (const uint32_t*)composite_spv;
	
	VkResult res = vkCreateShaderModule( device, &shaderModuleCreateInfo, nullptr, &shaderModule );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkCommandPoolCreateInfo commandPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.queueFamilyIndex = queueFamilyIndex,
	};

	res = vkCreateCommandPool(device, &commandPoolCreateInfo, 0, &commandPool);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}

	VkDescriptorPoolSize descriptorPoolSize[] = {
		{
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			k_nMaxSets * 1,
		},
		{
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			k_nMaxSets * 1,
		},
		{
			VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			k_nMaxSets * k_nMaxLayers,
		},
		{
			VK_DESCRIPTOR_TYPE_SAMPLER,
			k_nMaxSets * k_nMaxLayers,
		},
	};
	
	VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		nullptr,
		.flags = 0,
		.maxSets = k_nMaxSets,
		.poolSizeCount = 3,
		descriptorPoolSize
	};
	
	res = vkCreateDescriptorPool(device, &descriptorPoolCreateInfo, 0, &descriptorPool);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	std::vector< VkDescriptorSetLayoutBinding > vecLayoutBindings;
	VkDescriptorSetLayoutBinding descriptorSetLayoutBindings =
	{
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	
	vecLayoutBindings.push_back( descriptorSetLayoutBindings ); // first binding is target storage image
	
	descriptorSetLayoutBindings.binding = 1;
	descriptorSetLayoutBindings.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	
	vecLayoutBindings.push_back( descriptorSetLayoutBindings ); // second binding is composite description buffer
	
	for ( uint32_t i = 0; i < k_nMaxLayers; i++ )
	{
		descriptorSetLayoutBindings.binding = 2 + ( i * 2 );
		descriptorSetLayoutBindings.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		
		vecLayoutBindings.push_back( descriptorSetLayoutBindings );
		
		descriptorSetLayoutBindings.binding = 2 + ( i * 2 ) + 1;
		descriptorSetLayoutBindings.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		
		vecLayoutBindings.push_back( descriptorSetLayoutBindings );
	}
	
	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.bindingCount = 2 + ( k_nMaxLayers * 2 ),
		vecLayoutBindings.data()
	};
	
	res = vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCreateInfo, 0, &descriptorSetLayout);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		0,
		0,
		1,
		&descriptorSetLayout,
		0,
		0
	};
	
	res = vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, 0, &pipelineLayout);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkComputePipelineCreateInfo computePipelineCreateInfo = {
		VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		0,
		0,
		{
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			0,
			0,
			VK_SHADER_STAGE_COMPUTE_BIT,
			shaderModule,
			"main",
			0
		},
		pipelineLayout,
		0,
		0
	};
	
	res = vkCreateComputePipelines(device, 0, 1, &computePipelineCreateInfo, 0, &pipeline);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		nullptr,
		descriptorPool,
		1,
		&descriptorSetLayout
	};
	
	res = vkAllocateDescriptorSets(device, &descriptorSetAllocateInfo, &descriptorSet);
	
	if ( res != VK_SUCCESS || descriptorSet == VK_NULL_HANDLE )
	{
		return false;
	}
	
	// Make and map upload buffer
	
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.pNext = nullptr;
	bufferCreateInfo.size = 512 * 512 * 4;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	
	result = vkCreateBuffer( device, &bufferCreateInfo, nullptr, &uploadBuffer );
	
	if ( result != VK_SUCCESS )
	{
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, uploadBuffer, &memRequirements);
	
	int memTypeIndex =  findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	
	if ( memTypeIndex == -1 )
	{
		return false;
	}
	
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = memTypeIndex;
	
	vkAllocateMemory( device, &allocInfo, nullptr, &uploadBufferMemory);
	
	vkBindBufferMemory( device, uploadBuffer, uploadBufferMemory, 0 );
	vkMapMemory( device, uploadBufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pUploadBuffer );
	
	if ( pUploadBuffer == nullptr )
	{
		return false;
	}
	
	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = nullptr,
		.commandPool = commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};
	
	result = vkAllocateCommandBuffers( device, &commandBufferAllocateInfo, &uploadCommandBuffer );
	
	if ( result != VK_SUCCESS )
	{
		return false;
	}
	
	return true;
}

void fini_device()
{
	vkDestroyDevice(device, 0);
}

void acquire_next_image( void )
{
	vkAcquireNextImageKHR( device, g_output.swapChain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &g_output.nSwapChainImageIndex );
}

void vulkan_present_to_window( void )
{
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	
// 	presentInfo.waitSemaphoreCount = 1;
// 	presentInfo.pWaitSemaphores = signalSemaphores;
	
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &g_output.swapChain;
	
	presentInfo.pImageIndices = &g_output.nSwapChainImageIndex;
	
	vkQueuePresentKHR( queue, &presentInfo );
	
	acquire_next_image();
}

bool vulkan_make_output( VulkanOutput_t *pOutput )
{
	VkResult result;
	
	if ( BIsNested() == true )
	{
		SDL_Vulkan_CreateSurface( window, instance, &pOutput->surface );
		
		if ( pOutput->surface == VK_NULL_HANDLE )
			return false;
		
		result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physicalDevice, pOutput->surface, &pOutput->surfaceCaps );
		
		if ( result != VK_SUCCESS )
			return false;
		
		uint32_t formatCount = 0;
		result = vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, pOutput->surface, &formatCount, nullptr );
		
		if ( result != VK_SUCCESS )
			return false;
		
		if ( formatCount != 0 ) {
			pOutput->surfaceFormats.resize( formatCount );
			vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, pOutput->surface, &formatCount, pOutput->surfaceFormats.data() );
			
			if ( result != VK_SUCCESS )
				return false;
		}
		
		uint32_t presentModeCount = false;
		result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, pOutput->surface, &presentModeCount, nullptr );
		
		if ( result != VK_SUCCESS )
			return false;
		
		if ( presentModeCount != 0 ) {
			pOutput->presentModes.resize(presentModeCount);
			result = vkGetPhysicalDeviceSurfacePresentModesKHR( physicalDevice, pOutput->surface, &presentModeCount, pOutput->presentModes.data() );
			
			if ( result != VK_SUCCESS )
				return false;
		}
		
		uint32_t imageCount = pOutput->surfaceCaps.minImageCount + 1;
		uint32_t surfaceFormat = 0;
		for ( surfaceFormat = 0; surfaceFormat < formatCount; surfaceFormat++ )
		{
			if ( pOutput->surfaceFormats[ surfaceFormat ].format == VK_FORMAT_B8G8R8A8_UNORM )
				break;
		}
		
		if ( surfaceFormat == formatCount )
			return false;
		
		VkSwapchainCreateInfoKHR createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = pOutput->surface;
		
		createInfo.minImageCount = imageCount;
		createInfo.imageFormat = pOutput->surfaceFormats[ surfaceFormat ].format;
		createInfo.imageColorSpace = pOutput->surfaceFormats[surfaceFormat ].colorSpace;
		createInfo.imageExtent = { g_nOutputWidth, g_nOutputHeight };
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		
		createInfo.preTransform = pOutput->surfaceCaps.currentTransform;
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		createInfo.clipped = VK_TRUE;
		
		createInfo.oldSwapchain = VK_NULL_HANDLE;
		
		if (vkCreateSwapchainKHR( device, &createInfo, nullptr, &pOutput->swapChain) != VK_SUCCESS ) {
			return 0;
		}
		
		vkGetSwapchainImagesKHR( device, pOutput->swapChain, &imageCount, nullptr );
		pOutput->swapChainImages.resize( imageCount );
		pOutput->swapChainImageViews.resize( imageCount );
		vkGetSwapchainImagesKHR( device, pOutput->swapChain, &imageCount, pOutput->swapChainImages.data() );
		
		for ( uint32_t i = 0; i < pOutput->swapChainImages.size(); i++ )
		{
			VkImageViewCreateInfo createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			createInfo.image = pOutput->swapChainImages[ i ];
			createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			createInfo.format = pOutput->surfaceFormats[ surfaceFormat ].format;
			createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			createInfo.subresourceRange.baseMipLevel = 0;
			createInfo.subresourceRange.levelCount = 1;
			createInfo.subresourceRange.baseArrayLayer = 0;
			createInfo.subresourceRange.layerCount = 1;
			
			result = vkCreateImageView(device, &createInfo, nullptr, &pOutput->swapChainImageViews[ i ]);
			
			if ( result != VK_SUCCESS )
				return false;
		}
		
		acquire_next_image();
	}
	else
	{
		VkFormat imageFormat = DRMFormatToVulkan( g_nDRMFormat );
		
		if ( imageFormat == VK_FORMAT_UNDEFINED )
		{
			return false;
		}
		
		bool bSuccess = pOutput->outputImage[0].BInit( g_nOutputWidth, g_nOutputHeight, imageFormat, true, false );
		
		if ( bSuccess != true )
			return false;
		
		bSuccess = pOutput->outputImage[1].BInit( g_nOutputWidth, g_nOutputHeight, imageFormat, true, false );
		
		if ( bSuccess != true )
			return false;
	}

	// Make and map constant buffer
	
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.pNext = nullptr;
	bufferCreateInfo.size = sizeof( Composite_t );
	bufferCreateInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	
	result = vkCreateBuffer( device, &bufferCreateInfo, nullptr, &pOutput->constantBuffer );
	
	if ( result != VK_SUCCESS )
	{
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, pOutput->constantBuffer, &memRequirements);
	
	int memTypeIndex =  findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	
	if ( memTypeIndex == -1 )
	{
		return false;
	}
	
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = memTypeIndex;
	
	vkAllocateMemory( device, &allocInfo, nullptr, &pOutput->bufferMemory );
	
	vkBindBufferMemory( device, pOutput->constantBuffer, pOutput->bufferMemory, 0 );
	vkMapMemory( device, pOutput->bufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pOutput->pCompositeBuffer );
	
	if ( pOutput->pCompositeBuffer == nullptr )
	{
		return false;
	}
	
	// Make command buffers
	
	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = nullptr,
		.commandPool = commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 2
	};
	
	result = vkAllocateCommandBuffers(device, &commandBufferAllocateInfo, pOutput->commandBuffers);
	
	if ( result != VK_SUCCESS )
	{
		return false;
	}
	
	pOutput->nCurCmdBuffer = 0;
	
	// Write the constant buffer itno descriptor set
	VkDescriptorBufferInfo bufferInfo = {
		.buffer = g_output.constantBuffer,
		.offset = 0,
		.range = VK_WHOLE_SIZE
	};
	
	VkWriteDescriptorSet writeDescriptorSet = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = nullptr,
		.dstSet = descriptorSet,
		.dstBinding = 1,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pImageInfo = nullptr,
		.pBufferInfo = &bufferInfo,
		.pTexelBufferView = nullptr,
	};
	
	vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
	
	return true;
}

int vulkan_init(void)
{
	VkResult result = VK_ERROR_INITIALIZATION_FAILED;
	
	std::vector< const char * > vecEnabledInstanceExtensions;
	vecEnabledInstanceExtensions.push_back( VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME );
	
	vecEnabledInstanceExtensions.insert( vecEnabledInstanceExtensions.end(), g_vecSDLInstanceExts.begin(), g_vecSDLInstanceExts.end() );
	
	const VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
		.enabledExtensionCount = (uint32_t)vecEnabledInstanceExtensions.size(),
		.ppEnabledExtensionNames = vecEnabledInstanceExtensions.data(),
	};

	result = vkCreateInstance(&createInfo, 0, &instance);
	
	if ( result != VK_SUCCESS )
		return 0;
	
	if ( init_device() != 1 )
	{
		return 0;
	}
	
	dyn_vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr( device, "vkGetMemoryFdKHR" );
	if ( dyn_vkGetMemoryFdKHR == nullptr )
		return 0;
	
	if ( vulkan_make_output( &g_output ) == false )
	{
		return 0;
	}
	
	return 1;
}

VulkanTexture_t vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA )
{
	VulkanTexture_t ret = 0;

	CVulkanTexture *pTex = new CVulkanTexture();
	
	if ( pTex->BInit( pDMA->width, pDMA->height, DRMFormatToVulkan( pDMA->format ), false, true, pDMA ) == false )
	{
		delete pTex;
		return ret;
	}
	
	ret = ++g_nMaxVulkanTexHandle;
	g_mapVulkanTextures[ ret ] = pTex;
	
	return ret;
}

VulkanTexture_t vulkan_create_texture_from_bits( uint32_t width, uint32_t height, VkFormat format, void *bits )
{
	VulkanTexture_t ret = 0;
	
	CVulkanTexture *pTex = new CVulkanTexture();
	
	if ( pTex->BInit( width, height, format, false, true, nullptr ) == false )
	{
		delete pTex;
		return ret;
	}
	
	memcpy( pUploadBuffer, bits, width * height * 4 );
	
	VkCommandBufferBeginInfo commandBufferBeginInfo = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		0,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		0
	};
	
	assert( bUploadCmdBufferIdle == true );
	
	VkResult res = vkResetCommandBuffer( uploadCommandBuffer, 0 );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	res = vkBeginCommandBuffer( uploadCommandBuffer, &commandBufferBeginInfo);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkBufferImageCopy region = {};
	
	region.imageSubresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.layerCount = 1
	};
	
	region.imageExtent = {
		.width = width,
		.height = height,
		.depth = 1
	};
	
	vkCmdCopyBufferToImage( uploadCommandBuffer, uploadBuffer, pTex->m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region );
	
	res = vkEndCommandBuffer( uploadCommandBuffer );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkSubmitInfo submitInfo = {
		VK_STRUCTURE_TYPE_SUBMIT_INFO,
		0,
		0,
		0,
		0,
		1,
		&uploadCommandBuffer,
		0,
		0
	};
	
	res = vkQueueSubmit(queue, 1, &submitInfo, 0);
	
	bUploadCmdBufferIdle = false;
	
	ret = ++g_nMaxVulkanTexHandle;
	g_mapVulkanTextures[ ret ] = pTex;
	
	return ret;
}

void vulkan_free_texture( VulkanTexture_t vulkanTex )
{
	if ( vulkanTex == 0 )
		return;

	assert( g_mapVulkanTextures[ vulkanTex ] != nullptr );
	
	// we'll just free it here for now because we WaitIdle immediately after drawing
	// TODO move this into deferred free at some point
	delete g_mapVulkanTextures[ vulkanTex ];
	g_mapVulkanTextures[ vulkanTex ] = nullptr;
}

bool operator==(const struct VulkanPipeline_t::LayerBinding_t& lhs, struct VulkanPipeline_t::LayerBinding_t& rhs)
{
	if ( lhs.bFilter != rhs.bFilter )
		return false;

	if ( lhs.bBlackBorder != rhs.bBlackBorder )
		return false;

	return true;
}

VkSampler vulkan_make_sampler( struct VulkanPipeline_t::LayerBinding_t *pBinding )
{
	VkSampler ret = VK_NULL_HANDLE;

	VkSamplerCreateInfo samplerCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = nullptr,
		.magFilter = pBinding->bFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
		.minFilter = pBinding->bFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.borderColor = pBinding->bBlackBorder ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_INT_TRANSPARENT_BLACK,
		.unnormalizedCoordinates = VK_TRUE,
	};
	
	vkCreateSampler( device, &samplerCreateInfo, nullptr, &ret );
	
	return ret;
}

void vulkan_update_descriptor( struct VulkanPipeline_t *pPipeline )
{
	CVulkanTexture *pTex[ k_nMaxLayers ] = {};
	uint32_t texLayerIDs[ k_nMaxLayers ] = {};
	
	uint32_t nTexCount = 0;
	
	for ( uint32_t i = 0; i < k_nMaxLayers; i ++ )
	{
		if ( pPipeline->layerBindings[ i ].tex != 0 )
		{
			pTex[ nTexCount ] = g_mapVulkanTextures[ pPipeline->layerBindings[ i ].tex ];
			assert( pTex[ nTexCount ] );
			
			texLayerIDs[ nTexCount ] = i;
			
			nTexCount++;
		}
	}
	
	{
		VkImageView targetImageView;
		
		if ( BIsNested() == true )
		{
			targetImageView = g_output.swapChainImageViews[ g_output.nSwapChainImageIndex ];
		}
		else
		{
			targetImageView = g_output.outputImage[ g_output.nOutImage ].m_vkImageView;
		}

		VkDescriptorImageInfo imageInfo = {
			.imageView = targetImageView,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL
		};
		
		VkWriteDescriptorSet writeDescriptorSet = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = descriptorSet,
			.dstBinding = 0,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo = &imageInfo,
			.pBufferInfo = nullptr,
			.pTexelBufferView = nullptr,
		};
		
		vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
	}
	
	for ( uint32_t i = 0; i < nTexCount; i++ )
	{
		VkSampler sampler = VK_NULL_HANDLE;
		
		// First try to look up the sampler in the cache.
		for ( uint32_t j = 0; j < g_vecVulkanSamplerCache.size(); j++ )
		{
			if ( g_vecVulkanSamplerCache[ j ].key == pPipeline->layerBindings[ texLayerIDs[ i ] ] )
			{
				sampler = g_vecVulkanSamplerCache[ j ].sampler;
				break;
			}
		}
		
		if ( sampler == VK_NULL_HANDLE )
		{
			sampler = vulkan_make_sampler( &pPipeline->layerBindings[ texLayerIDs[ i ] ] );
			
			assert( sampler != VK_NULL_HANDLE );
			
			VulkanSamplerCacheEntry_t entry = { pPipeline->layerBindings[ texLayerIDs[ i ] ], sampler };
			g_vecVulkanSamplerCache.push_back( entry );
		}

		{
			VkDescriptorImageInfo imageInfo = {
				.imageView = pTex[ i ]->m_vkImageView,
				// TODO figure out what it is exactly for the wayland surfaces
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			};
			
			VkWriteDescriptorSet writeDescriptorSet = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.pNext = nullptr,
				.dstSet = descriptorSet,
				.dstBinding = 2 + (i * 2),
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				.pImageInfo = &imageInfo,
				.pBufferInfo = nullptr,
				.pTexelBufferView = nullptr,
			};
			
			vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
		}
		
		{
			VkDescriptorImageInfo imageInfo = {
				.sampler = sampler,
			};
			
			VkWriteDescriptorSet writeDescriptorSet = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.pNext = nullptr,
				.dstSet = descriptorSet,
				.dstBinding = 2 + (i * 2) + 1,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
				.pImageInfo = &imageInfo,
				.pBufferInfo = nullptr,
				.pTexelBufferView = nullptr,
			};
			
			vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
		}
	}
}

bool vulkan_composite( struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline )
{
	if ( BIsNested() == false && DRMFormatNeedsSwizzle( g_nDRMFormat ) )
	{
		pComposite->flSwapChannels = 1.0;
	}

	*g_output.pCompositeBuffer = *pComposite;
	// XXX maybe flush something?
	
	vulkan_update_descriptor( pPipeline );
	
	VkCommandBuffer curCommandBuffer = g_output.commandBuffers[ g_output.nCurCmdBuffer ];
	
	VkCommandBufferBeginInfo commandBufferBeginInfo = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		0,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		0
	};
	
	VkResult res = vkResetCommandBuffer( curCommandBuffer, 0 );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	res = vkBeginCommandBuffer( curCommandBuffer, &commandBufferBeginInfo);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	
	vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
							pipelineLayout, 0, 1, &descriptorSet, 0, 0);
	
	vkCmdDispatch(curCommandBuffer, g_nOutputWidth / 16, g_nOutputHeight / 16, 1);
	
	res = vkEndCommandBuffer(curCommandBuffer);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkSubmitInfo submitInfo = {
		VK_STRUCTURE_TYPE_SUBMIT_INFO,
		0,
		0,
		0,
		0,
		1,
		&curCommandBuffer,
		0,
		0
	};
	
	res = vkQueueSubmit(queue, 1, &submitInfo, 0);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	vkQueueWaitIdle( queue );
	
	bUploadCmdBufferIdle = true;
	
	if ( BIsNested() == false )
	{
		g_output.nOutImage = !g_output.nOutImage;
	}
	
	g_output.nCurCmdBuffer = !g_output.nCurCmdBuffer;
	
	return true;
}

uint32_t vulkan_get_last_composite_fbid( void )
{
	return g_output.outputImage[ !g_output.nOutImage ].m_FBID;
}













































































