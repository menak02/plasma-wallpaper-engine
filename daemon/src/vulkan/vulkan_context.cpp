#include "vulkan_context.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <cmath>
#include <algorithm>
#include <QImage>

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 0x34325241
#endif

namespace WallpaperEngine::Render {

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() {
    cleanup();
}

bool VulkanContext::init(int preferredGpuIndex) {
    if (!initInstance()) return false;
    if (!selectPhysicalDevice(preferredGpuIndex)) return false;
    if (!createLogicalDevice()) return false;
    if (!createCommandPool()) return false;
    return true;
}

void VulkanContext::destroyExportableBuffer() {
    if (m_currentBuffer.fd >= 0) {
        close(m_currentBuffer.fd);
        m_currentBuffer.fd = -1;
    }

    if (m_sharedImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_sharedImage, nullptr);
        m_sharedImage = VK_NULL_HANDLE;
    }
    if (m_sharedMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_sharedMemory, nullptr);
        m_sharedMemory = VK_NULL_HANDLE;
    }
}

void VulkanContext::destroyOutputTarget(OutputTarget& target) {
    if (target.buffer.fd >= 0) {
        close(target.buffer.fd);
        target.buffer.fd = -1;
    }
    if (target.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, target.image, nullptr);
        target.image = VK_NULL_HANDLE;
    }
    if (target.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, target.memory, nullptr);
        target.memory = VK_NULL_HANDLE;
    }
}

void VulkanContext::cleanup() {
    clearSceneImage();
    destroyExportableBuffer();

    for (auto& [name, target] : m_outputTargets) {
        destroyOutputTarget(target);
    }
    m_outputTargets.clear();

    if (m_fence != VK_NULL_HANDLE) {
        vkDestroyFence(m_device, m_fence, nullptr);
        m_fence = VK_NULL_HANDLE;
    }
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

bool VulkanContext::initInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Plasma Wallpaper Engine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "AGY Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> extensions = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME
    };

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan Instance." << std::endl;
        return false;
    }

    return true;
}

std::vector<GpuDeviceInfo> VulkanContext::getAvailableGpus() const {
    std::vector<GpuDeviceInfo> list;
    if (m_instance == VK_NULL_HANDLE) return list;

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) return list;

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);

        GpuDeviceInfo info;
        info.id = i;
        info.name = props.deviceName;
        info.isDiscrete = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

        // Extension support check.
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &extCount, exts.data());

        for (const auto& ext : exts) {
            if (std::strcmp(ext.extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0 ||
                std::strcmp(ext.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
                info.hasDmaBufSupport = true;
                break;
            }
        }

        list.push_back(std::move(info));
    }

    return list;
}

bool VulkanContext::selectPhysicalDevice(int preferredGpuIndex) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        std::cerr << "No Vulkan physical devices found." << std::endl;
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    if (preferredGpuIndex >= 0 && preferredGpuIndex < static_cast<int>(deviceCount)) {
        m_physicalDevice = devices[preferredGpuIndex];
    } else {
        // Auto-select: discrete GPU first, fallback to integrated.
        for (const auto& dev : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                m_physicalDevice = dev;
                break;
            }
        }

        if (m_physicalDevice == VK_NULL_HANDLE) {
            m_physicalDevice = devices[0];
        }
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    std::cout << "Selected GPU (" << (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "Discrete" : "Integrated")
              << "): " << props.deviceName << " [Driver: " << VK_VERSION_MAJOR(props.driverVersion) << "."
              << VK_VERSION_MINOR(props.driverVersion) << "]" << std::endl;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_graphicsQueueFamily = i;
            return true;
        }
    }

    return false;
}

bool VulkanContext::createLogicalDevice() {
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    std::vector<const char*> deviceExtensions = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME
    };

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan logical device." << std::endl;
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    return true;
}

bool VulkanContext::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffer) != VK_SUCCESS) {
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
        return false;
    }

    return true;
}

uint32_t VulkanContext::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

bool VulkanContext::createExportableImage(uint32_t width, uint32_t height, VkImage& outImage,
                                          VkDeviceMemory& outMemory, DmaBufBuffer& outBuffer) {
    VkExternalMemoryImageCreateInfo externalImageCreateInfo{};
    externalImageCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalImageCreateInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &outImage) != VK_SUCCESS) {
        std::cerr << "Failed to create exportable Vulkan image for resolution " << width << "x" << height << std::endl;
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_device, outImage, &memRequirements);

    VkExportMemoryAllocateInfo exportAllocInfo{};
    exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &exportAllocInfo;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate Vulkan memory for export." << std::endl;
        return false;
    }

    vkBindImageMemory(m_device, outImage, outMemory, 0);

    auto fpGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(m_device, "vkGetMemoryFdKHR"));
    if (!fpGetMemoryFdKHR) {
        std::cerr << "vkGetMemoryFdKHR not available." << std::endl;
        return false;
    }

    VkMemoryGetFdInfoKHR getFdInfo{};
    getFdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    getFdInfo.memory = outMemory;
    getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    int fd = -1;
    if (fpGetMemoryFdKHR(m_device, &getFdInfo, &fd) != VK_SUCCESS) {
        std::cerr << "Failed to export memory FD." << std::endl;
        return false;
    }

    VkImageSubresource subResource{};
    subResource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(m_device, outImage, &subResource, &layout);

    outBuffer.fd = fd;
    outBuffer.width = width;
    outBuffer.height = height;
    outBuffer.stride = static_cast<uint32_t>(layout.rowPitch);
    outBuffer.format = DRM_FORMAT_ARGB8888;
    outBuffer.size = memRequirements.size;
    return true;
}

bool VulkanContext::setResolution(uint32_t width, uint32_t height, DmaBufBuffer& outBuffer) {
    if (width == 0 || height == 0) return false;
    if (m_currentBuffer.width == width && m_currentBuffer.height == height && m_sharedImage != VK_NULL_HANDLE) {
        outBuffer = m_currentBuffer;
        return true;
    }

    destroyExportableBuffer();

    if (!createExportableImage(width, height, m_sharedImage, m_sharedMemory, outBuffer)) {
        return false;
    }

    m_currentBuffer = outBuffer;
    std::cout << "Dynamic DmaBuf Reallocated: " << width << "x" << height << " (FD: " << outBuffer.fd
              << ", Stride: " << outBuffer.stride << " bytes, Size: " << outBuffer.size << " bytes)" << std::endl;
    return true;
}

void VulkanContext::clearSceneImage() {
    if (m_stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
        m_stagingBuffer = VK_NULL_HANDLE;
    }
    if (m_stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_stagingMemory = VK_NULL_HANDLE;
    }
    m_hasSceneImage = false;
    m_texWidth = 0;
    m_texHeight = 0;
}

bool VulkanContext::uploadSceneImage(uint32_t width, uint32_t height, std::span<const uint8_t> rgbaPixels) {
    if (width == 0 || height == 0 || rgbaPixels.empty()) {
        return false;
    }

    clearSceneImage();

    // Scale + center-crop to target framebuffer dimensions.
    // The exportable DmaBuf image is VK_FORMAT_B8G8R8A8_UNORM
    // (DRM_FORMAT_ARGB8888): memory order B,G,R,X. Converting the RGBA
    // canvas to Format_ARGB32 puts bytes in that exact order on
    // little-endian (premultiplied 0xAARRGGBB -> B,G,R,A storage), so the
    // staging memcpy below needs no per-pixel channel swizzle. Uploading
    // the RGBA bytes directly would swap red and blue for every consumer
    // of the buffer.
    QImage srcImg(rgbaPixels.data(), width, height, width * 4, QImage::Format_RGBA8888);
    QImage scaledImg = srcImg.scaled(m_currentBuffer.width, m_currentBuffer.height, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32);

    int cropX = std::max(0, (scaledImg.width() - static_cast<int>(m_currentBuffer.width)) / 2);
    int cropY = std::max(0, (scaledImg.height() - static_cast<int>(m_currentBuffer.height)) / 2);
    QImage finalImg = scaledImg.copy(cropX, cropY, m_currentBuffer.width, m_currentBuffer.height);

    uint32_t finalWidth = finalImg.width();
    uint32_t finalHeight = finalImg.height();
    VkDeviceSize imageSize = finalWidth * finalHeight * 4;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_stagingBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, m_stagingBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_stagingMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_stagingBuffer, m_stagingMemory, 0);

    void* data = nullptr;
    vkMapMemory(m_device, m_stagingMemory, 0, imageSize, 0, &data);
    std::memcpy(data, finalImg.constBits(), imageSize);
    vkUnmapMemory(m_device, m_stagingMemory);

    m_texWidth = finalWidth;
    m_texHeight = finalHeight;
    m_hasSceneImage = true;

    std::cout << "Uploaded & Scaled Scene Texture: " << width << "x" << height << " -> " 
              << finalWidth << "x" << finalHeight << " (" << imageSize << " bytes)" << std::endl;
    return true;
}

void VulkanContext::renderFrame(float time) {
    if (m_sharedImage == VK_NULL_HANDLE) return;

    vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &m_fence);

    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(m_commandBuffer, &beginInfo);

    // Primary image first, then every registered per-output target.
    std::vector<VkImage> targets;
    targets.reserve(1 + m_outputTargets.size());
    targets.push_back(m_sharedImage);
    for (auto& [name, target] : m_outputTargets) {
        if (target.image != VK_NULL_HANDLE && target.image != m_sharedImage) {
            targets.push_back(target.image);
        }
    }

    for (VkImage image : targets) {
    // Transition to TRANSFER_DST layout.
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(m_commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    if (m_hasSceneImage && m_stagingBuffer != VK_NULL_HANDLE) {
        // Copy wallpaper staging buffer into shared image
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {
            std::min(m_texWidth, m_currentBuffer.width),
            std::min(m_texHeight, m_currentBuffer.height),
            1
        };

        vkCmdCopyBufferToImage(m_commandBuffer, m_stagingBuffer, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
        // Diagnostic pulsating color when no scene image is available.
        VkClearColorValue clearColor{};
        clearColor.float32[0] = std::sin(time) * 0.5f + 0.5f;
        clearColor.float32[1] = std::sin(time + 2.0f) * 0.5f + 0.5f;
        clearColor.float32[2] = std::sin(time + 4.0f) * 0.5f + 0.5f;
        clearColor.float32[3] = 1.0f;

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        vkCmdClearColorImage(m_commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);
    }

    // Transition to GENERAL for zero-copy reads.
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.image = image;

    vkCmdPipelineBarrier(m_commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    vkEndCommandBuffer(m_commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_fence);
}

bool VulkanContext::setResolutionForOutput(const std::string& outputName, uint32_t width, uint32_t height, DmaBufBuffer& outBuffer) {
    if (outputName.empty() || width == 0 || height == 0) return false;

    auto it = m_outputTargets.find(outputName);
    if (it != m_outputTargets.end()) {
        const auto& buf = it->second.buffer;
        if (buf.width == width && buf.height == height && it->second.image != VK_NULL_HANDLE) {
            outBuffer = buf;
            return true; // already correct size
        }
        destroyOutputTarget(it->second);
    }

    OutputTarget target;
    if (!createExportableImage(width, height, target.image, target.memory, target.buffer)) {
        // Make sure a failed create does not leave a stale entry behind
        m_outputTargets.erase(outputName);
        return false;
    }

    outBuffer = target.buffer;
    m_outputTargets[outputName] = std::move(target);
    std::cout << "Per-output DmaBuf registered: '" << outputName << "' " << width << "x" << height
              << " (FD: " << outBuffer.fd << ", Stride: " << outBuffer.stride << ")" << std::endl;
    return true;
}

const DmaBufBuffer* VulkanContext::getBufferForOutput(const std::string& outputName) const {
    auto it = m_outputTargets.find(outputName);
    return it != m_outputTargets.end() ? &it->second.buffer : nullptr;
}

bool VulkanContext::removeOutput(const std::string& outputName) {
    auto it = m_outputTargets.find(outputName);
    if (it == m_outputTargets.end()) return false;
    destroyOutputTarget(it->second);
    m_outputTargets.erase(it);
    std::cout << "Per-output DmaBuf removed: '" << outputName << "'" << std::endl;
    return true;
}

std::vector<std::string> VulkanContext::getOutputNames() const {
    std::vector<std::string> names;
    names.reserve(m_outputTargets.size());
    for (const auto& [name, target] : m_outputTargets) {
        names.push_back(name);
    }
    return names;
}

} // namespace WallpaperEngine::Render
