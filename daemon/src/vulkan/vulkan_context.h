#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>
#include <span>

namespace WallpaperEngine::Render {

struct DmaBufBuffer {
    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0; // DRM fourcc
    uint64_t modifier = 0;
    size_t size = 0;
};

struct GpuDeviceInfo {
    uint32_t id = 0;
    std::string name;
    std::string driverInfo;
    bool isDiscrete = false;
    bool hasDmaBufSupport = false;
};

class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    bool init(int preferredGpuIndex = -1);
    void cleanup();

    // Reallocates DmaBuf buffer dynamically for any resolution/aspect ratio
    bool setResolution(uint32_t width, uint32_t height, DmaBufBuffer& outBuffer);
    void renderFrame(float time);

    // Upload wallpaper image texture (supports any resolution/aspect ratio)
    bool uploadSceneImage(uint32_t width, uint32_t height, std::span<const uint8_t> rgbaPixels);
    void clearSceneImage();

    std::vector<GpuDeviceInfo> getAvailableGpus() const;
    VkDevice getDevice() const { return m_device; }
    VkInstance getInstance() const { return m_instance; }
    const DmaBufBuffer& getCurrentBuffer() const { return m_currentBuffer; }

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;

    // Active shared render target
    VkImage m_sharedImage = VK_NULL_HANDLE;
    VkDeviceMemory m_sharedMemory = VK_NULL_HANDLE;
    DmaBufBuffer m_currentBuffer;

    // Uploaded wallpaper texture staging buffer
    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    uint32_t m_texWidth = 0;
    uint32_t m_texHeight = 0;
    bool m_hasSceneImage = false;

    bool initInstance();
    bool selectPhysicalDevice(int preferredGpuIndex);
    bool createLogicalDevice();
    bool createCommandPool();

    public:
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void destroyExportableBuffer();
};

} // namespace WallpaperEngine::Render
