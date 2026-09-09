#include "gpu_quad_compositor.h"
#include "graphics_shaders_spv.h"
#include <cstring>
#include <iostream>
#include <algorithm>

using WallpaperEngine::Render::Shaders::fullscreen_vert;
using WallpaperEngine::Render::Shaders::quad_vert;
using WallpaperEngine::Render::Shaders::quad_frag;
using WallpaperEngine::Render::Shaders::gpu_grain_frag;
using WallpaperEngine::Render::Shaders::particle_vert;
using WallpaperEngine::Render::Shaders::particle_frag;

namespace WallpaperEngine::Render {

// 4-corner strip (two triangles via triangle-strip), UV == corner.
static const float CORNER_STRIP[8] = {
    0.0f, 0.0f,
    1.0f, 0.0f,
    0.0f, 1.0f,
    1.0f, 1.0f,
};

static uint32_t alignedUp(uint32_t v, uint32_t a) {
    return (v + a - 1) / a * a;
}

bool GpuQuadCompositor::init(VulkanContext* ctx, uint32_t width, uint32_t height) {
    cleanup();
    if (!ctx || ctx->getDevice() == VK_NULL_HANDLE) {
        m_lastError = "no vulkan device";
        return false;
    }
    m_ctx = ctx;
    m_device = ctx->getDevice();
    m_queue = ctx->getGraphicsQueue();
    m_queueFamily = ctx->getGraphicsQueueFamily();
    m_width = width;
    m_height = height;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamily;
    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        m_lastError = "command pool";
        return false;
    }

    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool = m_commandPool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_device, &cmdInfo, &m_cmd) != VK_SUCCESS) {
        m_lastError = "command buffer";
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
        m_lastError = "fence";
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        m_lastError = "sampler";
        return false;
    }

    if (!createRenderPasses() || !createPipelines()) {
        return false;
    }

    if (!createTargets(width, height)) {
        return false;
    }

    // White 1x1 texture for particle glow quads.
    QImage white(1, 1, QImage::Format_RGBA8888);
    white.fill(QColor(255, 255, 255, 255));
    m_whiteTexture = getOrCreateTexture(white, /*slotId=*/-2);
    if (m_whiteTexture == UINT32_MAX) {
        m_lastError = "white texture";
        return false;
    }

    std::cout << "GpuQuadCompositor: initialized (" << m_width << "x" << m_height << ")" << std::endl;
    return true;
}

void GpuQuadCompositor::cleanup() {
    if (m_device == VK_NULL_HANDLE) {
        m_ctx = nullptr;
        return;
    }
    vkDeviceWaitIdle(m_device);

    clearTextureCache();
    if (m_vertexBuffer) vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
    if (m_vertexMemory) vkFreeMemory(m_device, m_vertexMemory, nullptr);
    if (m_staging) vkDestroyBuffer(m_device, m_staging, nullptr);
    if (m_stagingMemory) vkFreeMemory(m_device, m_stagingMemory, nullptr);
    if (m_readback) vkDestroyBuffer(m_device, m_readback, nullptr);
    if (m_readbackMemory) vkFreeMemory(m_device, m_readbackMemory, nullptr);

    destroyTargets();

    auto destroyPipeline = [&](VkPipeline& p) { if (p) vkDestroyPipeline(m_device, p, nullptr); p = VK_NULL_HANDLE; };
    auto destroyLayout = [&](VkPipelineLayout& l) { if (l) vkDestroyPipelineLayout(m_device, l, nullptr); l = VK_NULL_HANDLE; };
    destroyPipeline(m_quadTranslucent); destroyPipeline(m_quadAdditive); destroyPipeline(m_quadOpaque);
    destroyPipeline(m_particleTranslucent); destroyPipeline(m_particleAdditive); destroyPipeline(m_grainPipeline);
    destroyLayout(m_quadLayout); destroyLayout(m_particleLayout); destroyLayout(m_grainLayout);
    if (m_renderPass) vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    if (m_grainRenderPass) vkDestroyRenderPass(m_device, m_grainRenderPass, nullptr);
    if (m_texturePool) vkDestroyDescriptorPool(m_device, m_texturePool, nullptr);
    if (m_textureSetLayout) vkDestroyDescriptorSetLayout(m_device, m_textureSetLayout, nullptr);
    if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
    if (m_commandPool) vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    if (m_fence) vkDestroyFence(m_device, m_fence, nullptr);

    m_cmd = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

void GpuQuadCompositor::setResolution(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || (width == m_width && height == m_height)) return;
    m_width = width;
    m_height = height;
    if (m_device != VK_NULL_HANDLE) {
        destroyTargets();
        createTargets(width, height);
        clearTextureCache();
    }
}

bool GpuQuadCompositor::createRenderPasses() {
    // Pass 1: clear + quads/particles -> SHADER_READ_ONLY (sampled by grain,
    // readback, and the dmabuf blit).
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Pass 2: load quad target -> grain target, also SHADER_READ_ONLY out.
    VkAttachmentDescription grainColor{};
    grainColor.format = VK_FORMAT_R8G8B8A8_UNORM;
    grainColor.samples = VK_SAMPLE_COUNT_1_BIT;
    grainColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    grainColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    grainColor.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    grainColor.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // The finalLayout SHADER_READ_ONLY transition needs a downstream-aware
    // dependency so the blit/readback never races the pass.
    VkSubpassDependency outDep{};
    outDep.srcSubpass = 0;
    outDep.dstSubpass = VK_SUBPASS_EXTERNAL;
    outDep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    outDep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    outDep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    outDep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = &dep;
    // Two deps: [external->0, 0->external]; pDependencies array order matters.
    VkSubpassDependency deps[2] = {dep, outDep};
    info.pDependencies = deps;

    // The grain render pass differs only in loadOp; reuse the same struct set.
    VkRenderPassCreateInfo grainInfo = info;
    grainInfo.pAttachments = &grainColor;

    if (vkCreateRenderPass(m_device, &info, nullptr, &m_renderPass) != VK_SUCCESS ||
        vkCreateRenderPass(m_device, &grainInfo, nullptr, &m_grainRenderPass) != VK_SUCCESS) {
        m_lastError = "render pass";
        return false;
    }
    return true;
}

static bool createGraphicsPipelineImpl(
    VkDevice device, VkRenderPass renderPass,
    const std::vector<uint32_t>& vertSpv, const std::vector<uint32_t>& fragSpv,
    const std::vector<VkVertexInputBindingDescription>& bindings,
    const std::vector<VkVertexInputAttributeDescription>& attrs,
    VkPipelineLayout layout, bool additive,
    VkPipeline* outPipeline, std::string& err)
{
    auto makeModule = [&](const std::vector<uint32_t>& code, VkShaderModule* out) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size() * sizeof(uint32_t);
        info.pCode = code.data();
        return vkCreateShaderModule(device, &info, nullptr, out) == VK_SUCCESS;
    };

    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    if (!makeModule(vertSpv, &vert) || !makeModule(fragSpv, &frag)) {
        err = "shader module";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1; // both dynamic

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (additive) {
        // Premultiplied additive (QPainter CompositionMode_Plus): since the
        // fragment shader outputs STRAIGHT alpha textures, premultiply first:
        // srcColor used in blending is rgb*a via SRC_ALPHA factor and the
        // +dst via ONE. (SRC_ALPHA, ONE) == src.rgb*a + dst.rgb.
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    } else {
        // Straight-alpha SourceOver (QPainter default):
        // out.rgb = src.rgb*src.a + dst.rgb*(1-src.a)
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &msaa;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = renderPass;
    info.subpass = 0;

    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, outPipeline);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    if (r != VK_SUCCESS) {
        err = "graphics pipeline";
        return false;
    }
    return true;
}

bool GpuQuadCompositor::createPipelines() {
    // Texture set layout: binding 0 = combined image sampler (fragment).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_textureSetLayout) != VK_SUCCESS) {
        m_lastError = "texture set layout";
        return false;
    }

    // Push constant block: vec4 (viewport.xy, parallax.xy) = 16 bytes.
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = 16;

    auto makeLayout = [&](VkPipelineLayout* out) {
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &m_textureSetLayout;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &pc;
        return vkCreatePipelineLayout(m_device, &info, nullptr, out) == VK_SUCCESS;
    };
    if (!makeLayout(&m_quadLayout) || !makeLayout(&m_particleLayout)) {
        m_lastError = "pipeline layout";
        return false;
    }

    // Grain push constants: vec2 viewport + power + scale + frame + pad = 24B.
    VkPushConstantRange grainPc{};
    grainPc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    grainPc.offset = 0;
    grainPc.size = 24;
    VkPipelineLayoutCreateInfo grainLayoutInfo{};
    grainLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    grainLayoutInfo.setLayoutCount = 1;
    grainLayoutInfo.pSetLayouts = &m_textureSetLayout;
    grainLayoutInfo.pushConstantRangeCount = 1;
    grainLayoutInfo.pPushConstantRanges = &grainPc;
    if (vkCreatePipelineLayout(m_device, &grainLayoutInfo, nullptr, &m_grainLayout) != VK_SUCCESS) {
        m_lastError = "grain layout";
        return false;
    }

    std::vector<VkVertexInputBindingDescription> quadBindings(2);
    quadBindings[0] = {0, sizeof(float) * 2, VK_VERTEX_INPUT_RATE_VERTEX};
    quadBindings[1] = {1, sizeof(QuadInstance), VK_VERTEX_INPUT_RATE_INSTANCE};
    std::vector<VkVertexInputAttributeDescription> quadAttrs(3);
    quadAttrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};                     // corner -> loc 0
    quadAttrs[1] = {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0};               // posSize -> loc 1
    quadAttrs[2] = {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16};              // rot/op/pad -> loc 2

    std::vector<VkVertexInputBindingDescription> partBindings(2);
    partBindings[0] = {0, sizeof(float) * 2, VK_VERTEX_INPUT_RATE_VERTEX};
    partBindings[1] = {1, sizeof(ParticleInstance), VK_VERTEX_INPUT_RATE_INSTANCE};
    std::vector<VkVertexInputAttributeDescription> partAttrs(3);
    partAttrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    partAttrs[1] = {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0};               // pos/size/rot
    partAttrs[2] = {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16};              // rgba

    if (!createGraphicsPipelineImpl(m_device, m_renderPass, quad_vert, quad_frag,
                                    quadBindings, quadAttrs, m_quadLayout, false, &m_quadTranslucent, m_lastError))
        return false;
    if (!createGraphicsPipelineImpl(m_device, m_renderPass, quad_vert, quad_frag,
                                    quadBindings, quadAttrs, m_quadLayout, true, &m_quadAdditive, m_lastError))
        return false;
    // Opaque = no blend. Cheapest way: reuse additive structure with blend
    // disabled would need another helper flag; skip opaque pipeline (blendMode
    // 2 maps to translucent pipeline — visually identical because alpha=1).
    if (!createGraphicsPipelineImpl(m_device, m_renderPass, particle_vert, particle_frag,
                                    partBindings, partAttrs, m_particleLayout, false, &m_particleTranslucent, m_lastError))
        return false;
    if (!createGraphicsPipelineImpl(m_device, m_renderPass, particle_vert, particle_frag,
                                    partBindings, partAttrs, m_particleLayout, true, &m_particleAdditive, m_lastError))
        return false;
    if (!createGraphicsPipelineImpl(m_device, m_grainRenderPass, fullscreen_vert, gpu_grain_frag,
                                    {}, {}, m_grainLayout, false, &m_grainPipeline, m_lastError))
        return false;
    return true;
}

bool GpuQuadCompositor::createTargets(uint32_t width, uint32_t height) {
    auto makeImage = [&](VkImage* img, VkDeviceMemory* mem, VkImageView* view) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkCreateImage(m_device, &info, nullptr, img) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, *img, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = m_ctx->findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &alloc, nullptr, mem) != VK_SUCCESS) return false;
        if (vkBindImageMemory(m_device, *img, *mem, 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = *img;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        return vkCreateImageView(m_device, &viewInfo, nullptr, view) == VK_SUCCESS;
    };

    if (!makeImage(&m_quadImage, &m_quadMemory, &m_quadView) ||
        !makeImage(&m_grainImage, &m_grainMemory, &m_grainView)) {
        m_lastError = "render targets";
        destroyTargets();
        return false;
    }

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_quadView;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_quadFramebuffer) != VK_SUCCESS) {
        m_lastError = "quad framebuffer";
        destroyTargets();
        return false;
    }

    VkFramebufferCreateInfo gfbInfo{};
    gfbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    gfbInfo.renderPass = m_grainRenderPass;
    gfbInfo.attachmentCount = 1;
    gfbInfo.pAttachments = &m_grainView;
    gfbInfo.width = width;
    gfbInfo.height = height;
    gfbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &gfbInfo, nullptr, &m_grainFramebuffer) != VK_SUCCESS) {
        m_lastError = "grain framebuffer";
        destroyTargets();
        return false;
    }

    // Grain descriptor set samples the quad target (written in ensureTexturePool).
    if (!ensureTexturePool(1)) {
        m_lastError = "texture pool";
        destroyTargets();
        return false;
    }
    return true;
}

void GpuQuadCompositor::destroyTargets() {
    auto destroyImage = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view, VkFramebuffer& fb) {
        if (fb) vkDestroyFramebuffer(m_device, fb, nullptr);
        if (view) vkDestroyImageView(m_device, view, nullptr);
        if (img) vkDestroyImage(m_device, img, nullptr);
        if (mem) vkFreeMemory(m_device, mem, nullptr);
        img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE; view = VK_NULL_HANDLE; fb = VK_NULL_HANDLE;
    };
    destroyImage(m_quadImage, m_quadMemory, m_quadView, m_quadFramebuffer);
    destroyImage(m_grainImage, m_grainMemory, m_grainView, m_grainFramebuffer);
    m_grainSet = VK_NULL_HANDLE;
}

bool GpuQuadCompositor::ensureTexturePool(uint32_t neededSets) {
    if (m_texturePool != VK_NULL_HANDLE && m_poolCapacity >= neededSets) return true;

    uint32_t newCap = std::max<uint32_t>(64, alignedUp(neededSets + 8, 64));
    if (m_texturePool) vkDestroyDescriptorPool(m_device, m_texturePool, nullptr);
    m_texturePool = VK_NULL_HANDLE;
    m_textureSets.clear();
    m_grainSet = VK_NULL_HANDLE;

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = newCap;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = newCap;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    if (vkCreateDescriptorPool(m_device, &info, nullptr, &m_texturePool) != VK_SUCCESS) {
        m_lastError = "descriptor pool";
        return false;
    }
    m_poolCapacity = newCap;

    // Sets: one per texture slot + one for the grain pass.
    std::vector<VkDescriptorSetLayout> layouts(newCap, m_textureSetLayout);
    std::vector<VkDescriptorSet> sets(newCap);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_texturePool;
    alloc.descriptorSetCount = newCap;
    alloc.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(m_device, &alloc, sets.data()) != VK_SUCCESS) {
        m_lastError = "descriptor alloc";
        return false;
    }
    m_textureSets.assign(sets.begin(), sets.end() - 1);
    m_grainSet = sets.back();

    // The grain set always samples the quad target; write it once here.
    VkDescriptorImageInfo grainImg{};
    grainImg.imageView = m_quadView;
    grainImg.sampler = m_sampler;
    grainImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet grainWrite{};
    grainWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    grainWrite.dstSet = m_grainSet;
    grainWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    grainWrite.descriptorCount = 1;
    grainWrite.pImageInfo = &grainImg;
    vkUpdateDescriptorSets(m_device, 1, &grainWrite, 0, nullptr);
    return true;
}

uint32_t GpuQuadCompositor::getOrCreateTexture(const QImage& image, int slotId) {
    if (image.isNull()) return UINT32_MAX;
    if (slotId < 0) return UINT32_MAX; // reserved

    // Slot reuse: layers keep one texture slot for their lifetime. A changed
    // QImage cacheKey (video frame advance, web repaint) re-uploads in place
    // — no allocation churn, no descriptor rewrite.
    uint32_t idx = UINT32_MAX;
    auto slotIt = m_slotTextures.find(slotId);
    if (slotIt != m_slotTextures.end()) {
        idx = slotIt->second;
        const qint64 key = image.cacheKey();
        if (m_textureKeys[idx] == key) {
            return idx; // unchanged since last upload
        }
    } else {
        if (!ensureTexturePool(uint32_t(m_textures.size()) + 2)) return UINT32_MAX;

        GpuTexture tex;
        tex.width = uint32_t(image.width());
        tex.height = uint32_t(image.height());
        if (!createTextureImage(image, tex)) return UINT32_MAX;

        idx = uint32_t(m_textures.size());
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageView = tex.view;
        imgInfo.sampler = m_sampler;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_textureSets[idx];
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        m_textures.push_back(tex);
        m_slotTextures[slotId] = idx;
    }

    if (!uploadTexturePixels(image, idx)) {
        return UINT32_MAX;
    }
    m_textureKeys[idx] = image.cacheKey();
    return idx;
}

bool GpuQuadCompositor::createTextureImage(const QImage& image, GpuTexture& tex) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {tex.width, tex.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(m_device, &info, nullptr, &tex.image) != VK_SUCCESS) {
        m_lastError = "texture image";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, tex.image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = m_ctx->findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &tex.memory) != VK_SUCCESS) {
        vkDestroyImage(m_device, tex.image, nullptr);
        m_lastError = "texture memory";
        return false;
    }
    vkBindImageMemory(m_device, tex.image, tex.memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &tex.view) != VK_SUCCESS) {
        vkFreeMemory(m_device, tex.memory, nullptr);
        vkDestroyImage(m_device, tex.image, nullptr);
        m_lastError = "texture view";
        return false;
    }
    return true;
}

bool GpuQuadCompositor::uploadTexturePixels(const QImage& image, uint32_t textureIndex) {
    const VkDeviceSize size = VkDeviceSize(image.width()) * image.height() * 4;
    if (!m_staging || m_stagingSize < size) {
        if (m_staging) vkDestroyBuffer(m_device, m_staging, nullptr);
        if (m_stagingMemory) vkFreeMemory(m_device, m_stagingMemory, nullptr);
        m_staging = VK_NULL_HANDLE; m_stagingMemory = VK_NULL_HANDLE; m_stagingMapped = nullptr;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(m_device, &info, nullptr, &m_staging) != VK_SUCCESS) {
            m_lastError = "staging buffer";
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, m_staging, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = m_ctx->findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &alloc, nullptr, &m_stagingMemory) != VK_SUCCESS) {
            m_lastError = "staging memory";
            return false;
        }
        vkBindBufferMemory(m_device, m_staging, m_stagingMemory, 0);
        if (vkMapMemory(m_device, m_stagingMemory, 0, size, 0, &m_stagingMapped) != VK_SUCCESS) {
            m_lastError = "staging map";
            return false;
        }
        m_stagingSize = size;
    }

    const GpuTexture& tex = m_textures[textureIndex];
    std::memcpy(m_stagingMapped, image.constBits(), size);

    beginFrame();
    // UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ_ONLY. One submit.
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = tex.image;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.layerCount = 1;
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(m_cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {tex.width, tex.height, 1};
    vkCmdCopyBufferToImage(m_cmd, m_staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShaderRead{};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.image = tex.image;
    toShaderRead.subresourceRange = toDst.subresourceRange;
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(m_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShaderRead);
    endFrame();
    return true;
}

bool GpuQuadCompositor::ensureVertexCapacity(VkDeviceSize bytes) {
    if (m_vertexBuffer != VK_NULL_HANDLE && m_vertexCapacity >= bytes) return true;
    if (m_vertexBuffer) vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
    if (m_vertexMemory) vkFreeMemory(m_device, m_vertexMemory, nullptr);
    m_vertexBuffer = VK_NULL_HANDLE; m_vertexMemory = VK_NULL_HANDLE; m_vertexMapped = nullptr;
    m_vertexCapacity = VkDeviceSize(alignedUp(uint32_t(bytes) * 2, 4096));

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = m_vertexCapacity;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(m_device, &info, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
        m_lastError = "vertex buffer";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = m_ctx->findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &m_vertexMemory) != VK_SUCCESS) {
        m_lastError = "vertex memory";
        return false;
    }
    vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexMemory, 0);
    if (vkMapMemory(m_device, m_vertexMemory, 0, m_vertexCapacity, 0, &m_vertexMapped) != VK_SUCCESS) {
        m_lastError = "vertex map";
        return false;
    }
    return true;
}

bool GpuQuadCompositor::ensureReadbackCapacity(VkDeviceSize bytes) {
    if (m_readback != VK_NULL_HANDLE && m_readbackSize >= bytes) return true;
    if (m_readback) vkDestroyBuffer(m_device, m_readback, nullptr);
    if (m_readbackMemory) vkFreeMemory(m_device, m_readbackMemory, nullptr);
    m_readback = VK_NULL_HANDLE; m_readbackMemory = VK_NULL_HANDLE; m_readbackMapped = nullptr;
    m_readbackSize = bytes;

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(m_device, &info, nullptr, &m_readback) != VK_SUCCESS) {
        m_lastError = "readback buffer";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, m_readback, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = m_ctx->findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &m_readbackMemory) != VK_SUCCESS) {
        m_lastError = "readback memory";
        return false;
    }
    vkBindBufferMemory(m_device, m_readback, m_readbackMemory, 0);
    if (vkMapMemory(m_device, m_readbackMemory, 0, bytes, 0, &m_readbackMapped) != VK_SUCCESS) {
        m_lastError = "readback map";
        return false;
    }
    return true;
}

void GpuQuadCompositor::beginFrame() {
    vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &m_fence);
    vkResetCommandBuffer(m_cmd, 0);
    VkCommandBufferBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(m_cmd, &info);
}

void GpuQuadCompositor::endFrame() {
    vkEndCommandBuffer(m_cmd);
    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    info.commandBufferCount = 1;
    info.pCommandBuffers = &m_cmd;
    vkQueueSubmit(m_queue, 1, &info, m_fence);
    vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
}

bool GpuQuadCompositor::renderFrame(const std::vector<GpuLayer>& layers,
                                    const std::vector<GpuParticle>& particles,
                                    const float clearColor[4],
                                    const GpuGrainParams& grain) {
    if (!isInitialized()) return false;

    m_hasGrainThisFrame = false;

    // ---- Vertex payload: [strip][quad instances][particle instances] ----
    const VkDeviceSize stripBytes = sizeof(CORNER_STRIP);
    m_quadInstOffset = alignedUp(uint32_t(stripBytes), 16);
    m_partInstOffset = m_quadInstOffset + VkDeviceSize(layers.size()) * sizeof(QuadInstance);
    const VkDeviceSize totalBytes = m_partInstOffset + VkDeviceSize(particles.size()) * sizeof(ParticleInstance);
    if (!ensureVertexCapacity(totalBytes)) return false;

    auto* quadInst = reinterpret_cast<QuadInstance*>(static_cast<uint8_t*>(m_vertexMapped) + m_quadInstOffset);
    for (size_t i = 0; i < layers.size(); ++i) {
        quadInst[i] = {layers[i].centerX, layers[i].centerY, layers[i].width, layers[i].height,
                       layers[i].rotationRad, layers[i].opacity, 0.f, 0.f};
    }
    auto* partInst = reinterpret_cast<ParticleInstance*>(static_cast<uint8_t*>(m_vertexMapped) + m_partInstOffset);
    for (size_t i = 0; i < particles.size(); ++i) {
        partInst[i] = {particles[i].centerX, particles[i].centerY, particles[i].size, particles[i].rotationRad,
                       particles[i].r, particles[i].g, particles[i].b, particles[i].a};
    }

    beginFrame();

    // ---- Pass 1: quads + particles -> quad target ----
    VkClearValue clear{};
    clear.color = {{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_renderPass;
    rp.framebuffer = m_quadFramebuffer;
    rp.renderArea.extent = {m_width, m_height};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(m_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0, 0, float(m_width), float(m_height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    vkCmdSetViewport(m_cmd, 0, 1, &viewport);
    vkCmdSetScissor(m_cmd, 0, 1, &scissor);

    struct QuadPC { float vx, vy, px, py; } pc{float(m_width), float(m_height), 0.f, 0.f};

    VkBuffer vbuf = m_vertexBuffer;
    VkDeviceSize quadOffsets[2] = {0, m_quadInstOffset};
    vkCmdBindVertexBuffers(m_cmd, 0, 2, &vbuf, quadOffsets);

    VkPipeline currentPipeline = VK_NULL_HANDLE;
    uint32_t currentTex = UINT32_MAX;
    for (size_t i = 0; i < layers.size(); ++i) {
        const GpuLayer& L = layers[i];
        if (L.textureIndex >= m_textures.size()) continue;
        VkPipeline wanted = (L.blendMode == 1) ? m_quadAdditive
                          : (L.blendMode == 2) ? m_quadTranslucent // alpha=1: identical to opaque
                          : m_quadTranslucent;
        if (wanted != currentPipeline) {
            vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
            currentPipeline = wanted;
        }
        if (L.textureIndex != currentTex) {
            vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_quadLayout,
                                    0, 1, &m_textureSets[L.textureIndex], 0, nullptr);
            currentTex = L.textureIndex;
        }
        vkCmdPushConstants(m_cmd, m_quadLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(QuadPC), &pc);
        vkCmdDraw(m_cmd, 4, 1, 0, uint32_t(i));
    }

    // ---- Particles (one draw per particle; pipeline switches on blend runs) ----
    if (!particles.empty()) {
        VkDeviceSize partOffsets[2] = {0, m_partInstOffset};
        vkCmdBindVertexBuffers(m_cmd, 0, 2, &vbuf, partOffsets);
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_particleLayout,
                                0, 1, &m_textureSets[m_whiteTexture], 0, nullptr);
        currentPipeline = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < uint32_t(particles.size()); ++i) {
            VkPipeline wanted = particles[i].blendMode == 1 ? m_particleAdditive : m_particleTranslucent;
            if (wanted != currentPipeline) {
                vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
                currentPipeline = wanted;
            }
            vkCmdPushConstants(m_cmd, m_particleLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(QuadPC), &pc);
            vkCmdDraw(m_cmd, 4, 1, 0, i);
        }
    }

    vkCmdEndRenderPass(m_cmd);

    // ---- Pass 2: film grain (quad target -> grain target) ----
    if (grain.enabled && grain.power > 0.0f) {
        VkRenderPassBeginInfo grp{};
        grp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        grp.renderPass = m_grainRenderPass;
        grp.framebuffer = m_grainFramebuffer;
        grp.renderArea.extent = {m_width, m_height};
        vkCmdBeginRenderPass(m_cmd, &grp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(m_cmd, 0, 1, &viewport);
        vkCmdSetScissor(m_cmd, 0, 1, &scissor);
        vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_grainPipeline);
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_grainLayout,
                                0, 1, &m_grainSet, 0, nullptr);
        struct GrainPC { float vx, vy, power, scale, frame, pad; }
            gpc{float(m_width), float(m_height), grain.power, grain.scale, grain.frame, 0.f};
        vkCmdPushConstants(m_cmd, m_grainLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GrainPC), &gpc);
        vkCmdDraw(m_cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(m_cmd);
        m_hasGrainThisFrame = true;
    }

    endFrame();
    return true;
}

bool GpuQuadCompositor::blitIntoShared() {
    if (!m_ctx || m_quadImage == VK_NULL_HANDLE) return false;
    const VkImage src = m_hasGrainThisFrame ? m_grainImage : m_quadImage;
    return m_ctx->blitIntoSharedImage(src, m_width, m_height);
}

bool GpuQuadCompositor::readback(QImage& outCanvas) {
    if (!isInitialized()) return false;
    const VkImage src = m_hasGrainThisFrame ? m_grainImage : m_quadImage;
    const VkDeviceSize size = VkDeviceSize(m_width) * m_height * 4;
    if (!ensureReadbackCapacity(size)) return false;

    beginFrame();
    // SHADER_READ_ONLY -> TRANSFER_SRC -> copy -> back to SHADER_READ_ONLY.
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = src;
    toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toSrc.subresourceRange.levelCount = 1;
    toSrc.subresourceRange.layerCount = 1;
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(m_cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {m_width, m_height, 1};
    vkCmdCopyImageToBuffer(m_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readback, 1, &region);

    VkImageMemoryBarrier toShaderRead{};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.image = src;
    toShaderRead.subresourceRange = toSrc.subresourceRange;
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(m_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShaderRead);
    endFrame();

    outCanvas = QImage(int(m_width), int(m_height), QImage::Format_RGBA8888);
    std::memcpy(outCanvas.bits(), m_readbackMapped, size);
    return true;
}

void GpuQuadCompositor::clearTextureCache() {
    for (auto& tex : m_textures) {
        if (tex.view) vkDestroyImageView(m_device, tex.view, nullptr);
        if (tex.image) vkDestroyImage(m_device, tex.image, nullptr);
        if (tex.memory) vkFreeMemory(m_device, tex.memory, nullptr);
    }
    m_textures.clear();
    m_slotTextures.clear();
    m_textureKeys.clear();
    m_whiteTexture = UINT32_MAX;
}

} // namespace WallpaperEngine::Render
