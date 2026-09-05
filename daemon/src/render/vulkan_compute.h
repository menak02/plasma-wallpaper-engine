#pragma once

#include "../vulkan/vulkan_context.h"
#include <QImage>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <span>
#include <vulkan/vulkan.h>

namespace WallpaperEngine::Render {

struct ComputeShader {
    VkShaderModule module = VK_NULL_HANDLE;
    std::string entryPoint;

    ComputeShader() : entryPoint("main") {}
};

struct ComputePipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets;
};

struct ComputeImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
};

// Host/device staging buffer for pixel round-trips.
struct ComputeStagingBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

/**
 * Vulkan Compute Pipeline Manager
 * Handles compute shader compilation, pipeline creation, and dispatch for
 * GPU-accelerated image processing effects (blur, water waves, pulse, etc.)
 */
class VulkanCompute {
public:
    VulkanCompute() = default;
    ~VulkanCompute() { cleanup(); }

    bool init(VulkanContext* vulkanCtx);
    void cleanup();

    // Image management
    bool createImage(uint32_t width, uint32_t height, ComputeImage& outImage);
    void destroyImage(ComputeImage& image);
    void transitionImageLayout(ComputeImage& image, VkImageLayout oldLayout, VkImageLayout newLayout);
    void copyBufferToImage(VkBuffer buffer, ComputeImage& image, uint32_t width, uint32_t height);
    void copyImageToBuffer(ComputeImage& image, VkBuffer buffer, uint32_t width, uint32_t height);

    // Pipeline management
    bool createPipeline(const std::string& name, const std::vector<uint32_t>& spvCode,
                        const std::vector<VkDescriptorSetLayoutBinding>& bindings);
    ComputePipeline* getPipeline(const std::string& name);

    // Compute dispatch
    void dispatchCompute(const std::string& pipelineName, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ,
                         const std::vector<VkDescriptorSet>& descriptorSets);

    // Dispatch helpers per effect.
    bool applyBlur(const ComputeImage& input, ComputeImage& output, float radius, bool vertical);
    bool applyWaterWaves(const ComputeImage& input, const ComputeImage& mask, ComputeImage& output,
                         float speed, float scale, float strength, float direction, float time);
    bool applyPulse(const ComputeImage& input, const ComputeImage& mask, ComputeImage& output,
                    float speed, float amount, float power, float time);
    bool applyComposition(const ComputeImage& current, const ComputeImage& background, ComputeImage& output,
                          int blendMode);
    // Full-frame film grain post-process: uploads the QImage, applies the
    // grain compute pass on the GPU and reads the result back into the same
    // QImage. Single submit; returns false when the pipeline/GPU is not
    // available so the caller can fall back to the CPU path.
    bool applyFilmGrain(QImage& frame, float power, float scale, float frameTime);

    // Push constants for dynamic parameters
    struct BlurParams {
        float radius;
        int vertical;
        uint32_t width;
        uint32_t height;
    };

    struct WaveParams {
        float speed;
        float scale;
        float strength;
        float direction;
        float time;
        uint32_t width;
        uint32_t height;
    };

    struct PulseParams {
        float speed;
        float amount;
        float power;
        float time;
        uint32_t width;
        uint32_t height;
    };

    struct CompositionParams {
        int blendMode;
        uint32_t width;
        uint32_t height;
    };

    struct GrainParams {
        float power;
        float scale;
        float frame;
        float pad;
        uint32_t width;
        uint32_t height;
    };

    VulkanContext* m_vulkanCtx = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_computeQueue = VK_NULL_HANDLE;
    uint32_t m_computeQueueFamily = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;

    std::unordered_map<std::string, ComputePipeline> m_pipelines;
    std::unordered_map<std::string, ComputeShader> m_shaders;

    // Reused for the film grain upload/readback round-trip.
    ComputeStagingBuffer m_grainStaging;
    ComputeImage m_grainImage;
    ComputeImage m_grainOutImage;
    bool ensureGrainResources(uint32_t width, uint32_t height);
    void destroyStagingBuffer(ComputeStagingBuffer& staging);

private:
    bool createCommandPool();
    bool createCommandBuffer();
    bool createFence();
    VkShaderModule createShaderModule(const std::vector<uint32_t>& code);
    bool allocateDescriptorSets(ComputePipeline& pipeline);
    void recordAndSubmitCommands(std::function<void(VkCommandBuffer)> recordFunc);
};

} // namespace WallpaperEngine::Render