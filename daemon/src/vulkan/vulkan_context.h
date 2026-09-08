#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <map>
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

    // Multi-output support: one exportable DmaBuf per logical output name.
    // The "default" output aliases the legacy single-buffer path so existing
    // consumers (viewer, compositor target resolution) keep working.
    bool setResolutionForOutput(const std::string& outputName, uint32_t width, uint32_t height, DmaBufBuffer& outBuffer);
    const DmaBufBuffer* getBufferForOutput(const std::string& outputName) const;
    bool removeOutput(const std::string& outputName);
    std::vector<std::string> getOutputNames() const;

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

    // Active shared render target (legacy single-output path, aliases "default" output)
    VkImage m_sharedImage = VK_NULL_HANDLE;
    VkDeviceMemory m_sharedMemory = VK_NULL_HANDLE;
    DmaBufBuffer m_currentBuffer;

    // Per-output render targets for multi-monitor (wlr-layer-shell style)
    struct OutputTarget {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        DmaBufBuffer buffer;
    };
    std::map<std::string, OutputTarget> m_outputTargets;

    // Uploaded wallpaper texture staging buffer. Persistently mapped and
    // reused across frames: recreating buffer + allocation + map cycle 60x
    // per second was a major CPU/GPU-driver cost in the frame loop.
    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    void* m_stagingMapped = nullptr;
    VkDeviceSize m_stagingSize = 0;
    uint32_t m_texWidth = 0;
    uint32_t m_texHeight = 0;
    bool m_hasSceneImage = false;

    bool ensureStagingBuffer(VkDeviceSize size);

    bool initInstance();
    bool selectPhysicalDevice(int preferredGpuIndex);
    bool createLogicalDevice();
    bool createCommandPool();

    public:
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void destroyExportableBuffer();
    void destroyOutputTarget(OutputTarget& target);
    bool createExportableImage(uint32_t width, uint32_t height, VkImage& outImage,
                               VkDeviceMemory& outMemory, DmaBufBuffer& outBuffer);
};

} // namespace WallpaperEngine::Render
