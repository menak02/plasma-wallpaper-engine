#pragma once

#include "../vulkan/vulkan_context.h"
#include <QImage>
#include <string>
#include <vector>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace WallpaperEngine::Render {

// Per-layer instance data (matches quad.vert: two vec4s at locations 1/2).
struct QuadInstance {
    float centerX, centerY, width, height;   // screen px
    float rotationRad, opacity;              // rotation radians, 0..1
    float pad0, pad1;
};
static_assert(sizeof(QuadInstance) == 32, "QuadInstance must match the vertex shader stride");

// Per-particle instance (matches particle.vert).
struct ParticleInstance {
    float centerX, centerY, size, rotationRad;
    float r, g, b, a;
};
static_assert(sizeof(ParticleInstance) == 32, "ParticleInstance must match the vertex shader stride");

struct GpuLayer {
    uint32_t textureIndex = 0;
    float centerX = 0, centerY = 0, width = 0, height = 0;
    float rotationRad = 0, opacity = 1;
    // 0 = translucent (srcAlpha blend), 1 = additive (premultiplied Plus),
    // 2 = opaque (no blend)
    uint32_t blendMode = 0;
    // Mesh-deform layer (waterwaves/waterripple/wind/foliagesway): drawn with
    // the deform_quad.vert grid-strip pipeline instead of the plain
    // billboard. deformation math mirrors MeshDeformer::deformVertices;
    // deformStrength == 0 collapses to the plain quad path.
    bool deformed = false;
    float deformSpeed = 0, deformStrength = 0, deformDirection = 0;
};

struct GpuParticle {
    float centerX = 0, centerY = 0, size = 0, rotationRad = 0;
    float r = 1, g = 1, b = 1, a = 1;
    // 0 = translucent, 1 = additive (same enum as GpuLayer::blendMode)
    uint32_t blendMode = 0;
};

struct GpuGrainParams {
    bool enabled = false;
    float power = 0.0f;
    float scale = 4.0f;
    float frame = 0.0f;
};

/**
 * GPU scene compositor. Renders layers as textured billboards (one draw per
 * layer) and particles as glow quads directly on the GPU, with film grain as
 * a second render pass. Replaces the per-frame QPainter composite + staging
 * upload of the CPU path.
 *
 * Frame contract with VulkanContext: renderFrame() leaves the composited
 * image in SHADER_READ_ONLY_OPTIMAL; blitIntoShared() then blits it into the
 * exportable dmabuf target(s) and leaves THEM in GENERAL — the same end state
 * as the legacy staging-upload path — so dmabuf consumers keep working
 * unchanged. readback() pulls the composited frame to a QImage for tests.
 */
class GpuQuadCompositor {
public:
    GpuQuadCompositor() = default;
    ~GpuQuadCompositor() { cleanup(); }
    GpuQuadCompositor(const GpuQuadCompositor&) = delete;
    GpuQuadCompositor& operator=(const GpuQuadCompositor&) = delete;

    bool init(VulkanContext* ctx, uint32_t width, uint32_t height);
    void cleanup();
    bool isInitialized() const { return m_ctx != nullptr && m_quadImage != VK_NULL_HANDLE; }
    // Diagnostic detail for the last init/render failure.
    const std::string& lastError() const { return m_lastError; }

    void setResolution(uint32_t width, uint32_t height);

    // Texture cache, one stable slot per caller-supplied slot id. Static
    // layer images keep their QImage cacheKey across frames, so the GPU copy
    // happens exactly once per layer; video frames (new cacheKey, same
    // dimensions) re-upload in place without churning allocations. Returns
    // UINT32_MAX on failure. Negative slot ids are reserved for internal
    // textures (-2 = particle glow white texture).
    uint32_t getOrCreateTexture(const QImage& image, int slotId);
    void clearTextureCache();

    // Record + execute a full frame. Layers must be sorted back-to-front;
    // particles draw after layers. Synchronous (submits and waits) so the
    // subsequent dmabuf blit and CPU readback see complete data.
    // timeSeconds drives the vertex-stage mesh deformation clock and must be
    // the same engine time value the CPU painter path would use.
    bool renderFrame(const std::vector<GpuLayer>& layers,
                     const std::vector<GpuParticle>& particles,
                     const float clearColor[4],
                     const GpuGrainParams& grain = {},
                     float timeSeconds = 0.0f);

    // Copy the composited frame into the exportable dmabuf image(s).
    bool blitIntoShared();

    // Read the composited frame back into a CPU canvas (RGBA8888, 1920x1080
    // or whatever the current resolution is). Test/diagnostic path.
    bool readback(QImage& outCanvas);

private:
    // Texture cache
    struct GpuTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
    };

    bool createRenderPasses();
    bool createPipelines();
    bool createTargets(uint32_t width, uint32_t height);
    void destroyTargets();

    bool ensureTexturePool(uint32_t neededSets);
    bool createTextureImage(const QImage& image, GpuTexture& tex);
    bool uploadTexturePixels(const QImage& image, uint32_t textureIndex);
    bool ensureVertexCapacity(VkDeviceSize bytes);
    // True after (re)allocation of m_vertexBuffer: bytes 0..31 (corner strip)
    // must be re-initialized before the first draw reads binding 0.
    bool m_needsStripUpload = true;
    bool ensureReadbackCapacity(VkDeviceSize bytes);

    void beginFrame();
    void endFrame();

    VulkanContext* m_ctx = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;

    uint32_t m_width = 0, m_height = 0;
    bool m_hasGrainThisFrame = false;

    // Quad target (layers + particles) and grain target (grain output).
    VkImage m_quadImage = VK_NULL_HANDLE;
    VkDeviceMemory m_quadMemory = VK_NULL_HANDLE;
    VkImageView m_quadView = VK_NULL_HANDLE;
    VkFramebuffer m_quadFramebuffer = VK_NULL_HANDLE;
    VkImage m_grainImage = VK_NULL_HANDLE;
    VkDeviceMemory m_grainMemory = VK_NULL_HANDLE;
    VkImageView m_grainView = VK_NULL_HANDLE;
    VkFramebuffer m_grainFramebuffer = VK_NULL_HANDLE;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;      // clear + quads/particles
    VkRenderPass m_grainRenderPass = VK_NULL_HANDLE; // load + fullscreen grain

    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_texturePool = VK_NULL_HANDLE;
    uint32_t m_poolCapacity = 0;
    std::vector<VkDescriptorSet> m_textureSets;
    VkDescriptorSet m_grainSet = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkPipelineLayout m_quadLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_particleLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_grainLayout = VK_NULL_HANDLE;
    VkPipeline m_quadTranslucent = VK_NULL_HANDLE;
    VkPipeline m_quadAdditive = VK_NULL_HANDLE;
    VkPipeline m_quadOpaque = VK_NULL_HANDLE;
    // Same fragment shader as the plain quad pipelines; vertex stage deforms
    // the grid per-instance (see deform_quad.vert).
    VkPipeline m_deformQuadTranslucent = VK_NULL_HANDLE;
    VkPipeline m_deformQuadAdditive = VK_NULL_HANDLE;
    VkPipeline m_particleTranslucent = VK_NULL_HANDLE;
    VkPipeline m_particleAdditive = VK_NULL_HANDLE;
    VkPipeline m_grainPipeline = VK_NULL_HANDLE;

    // Persistent host-visible buffer: [corner strip][quad insts][deform insts
    // (48B, quad base + deform params)][part insts].
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexMemory = VK_NULL_HANDLE;
    void* m_vertexMapped = nullptr;
    VkDeviceSize m_vertexCapacity = 0;
    VkDeviceSize m_quadInstOffset = 0;
    VkDeviceSize m_deformInstOffset = 0;
    VkDeviceSize m_partInstOffset = 0;

    std::vector<GpuTexture> m_textures;
    // slot id -> texture index; texture index -> QImage cacheKey at upload
    std::unordered_map<int, uint32_t> m_slotTextures;
    std::unordered_map<uint32_t, qint64> m_textureKeys;
    uint32_t m_whiteTexture = UINT32_MAX;

    // Persistent staging (texture uploads) and readback buffers.
    VkBuffer m_staging = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize m_stagingSize = 0;
    void* m_stagingMapped = nullptr;

    VkBuffer m_readback = VK_NULL_HANDLE;
    VkDeviceMemory m_readbackMemory = VK_NULL_HANDLE;
    VkDeviceSize m_readbackSize = 0;
    void* m_readbackMapped = nullptr;

    std::string m_lastError;
};

} // namespace WallpaperEngine::Render
