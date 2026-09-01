#include "vulkan_compute.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <functional>
#include <vulkan/vulkan.h>

namespace WallpaperEngine::Render {

bool VulkanCompute::init(VulkanContext* vulkanCtx) {
    if (!vulkanCtx) return false;
    m_vulkanCtx = vulkanCtx;
    m_device = vulkanCtx->getDevice();

    // In this basic version, we query queue details from context
    m_computeQueueFamily = 0; // assuming graphics/compute queue is 0 (graphics contains compute)
    vkGetDeviceQueue(m_device, m_computeQueueFamily, 0, &m_computeQueue);

    if (!createCommandPool()) return false;
    if (!createCommandBuffer()) return false;
    if (!createFence()) return false;

    return true;
}

void VulkanCompute::cleanup() {
    for (auto& [name, pipeline] : m_pipelines) {
        if (pipeline.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, pipeline.pipeline, nullptr);
        }
        if (pipeline.layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, pipeline.layout, nullptr);
        }
        if (pipeline.descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, pipeline.descriptorSetLayout, nullptr);
        }
        if (pipeline.descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, pipeline.descriptorPool, nullptr);
        }
    }
    m_pipelines.clear();

    for (auto& [name, shader] : m_shaders) {
        if (shader.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, shader.module, nullptr);
        }
    }
    m_shaders.clear();

    if (m_fence != VK_NULL_HANDLE) {
        vkDestroyFence(m_device, m_fence, nullptr);
        m_fence = VK_NULL_HANDLE;
    }
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
}

bool VulkanCompute::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_computeQueueFamily;

    return vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) == VK_SUCCESS;
}

bool VulkanCompute::createCommandBuffer() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    return vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffer) == VK_SUCCESS;
}

bool VulkanCompute::createFence() {
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence) == VK_SUCCESS;
}

VkShaderModule VulkanCompute::createShaderModule(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

bool VulkanCompute::createImage(uint32_t width, uint32_t height, ComputeImage& outImage) {
    outImage.width = width;
    outImage.height = height;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = outImage.format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &outImage.image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_device, outImage.image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = m_vulkanCtx->findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &outImage.memory) != VK_SUCCESS) {
        vkDestroyImage(m_device, outImage.image, nullptr);
        outImage.image = VK_NULL_HANDLE;
        return false;
    }

    vkBindImageMemory(m_device, outImage.image, outImage.memory, 0);

    // Create Image View
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = outImage.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &outImage.view) != VK_SUCCESS) {
        vkFreeMemory(m_device, outImage.memory, nullptr);
        vkDestroyImage(m_device, outImage.image, nullptr);
        outImage.image = VK_NULL_HANDLE;
        outImage.memory = VK_NULL_HANDLE;
        return false;
    }

    // Create Sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &outImage.sampler) != VK_SUCCESS) {
        vkDestroyImageView(m_device, outImage.view, nullptr);
        vkFreeMemory(m_device, outImage.memory, nullptr);
        vkDestroyImage(m_device, outImage.image, nullptr);
        outImage.image = VK_NULL_HANDLE;
        outImage.memory = VK_NULL_HANDLE;
        outImage.view = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void VulkanCompute::destroyImage(ComputeImage& image) {
    if (image.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, image.sampler, nullptr);
        image.sampler = VK_NULL_HANDLE;
    }
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, image.view, nullptr);
        image.view = VK_NULL_HANDLE;
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, image.memory, nullptr);
        image.memory = VK_NULL_HANDLE;
    }
}

void VulkanCompute::transitionImageLayout(ComputeImage& image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    recordAndSubmitCommands([&](VkCommandBuffer cmdBuf) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags sourceStage;
        VkPipelineStageFlags destinationStage;

        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        } else {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = 0;
            sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }

        vkCmdPipelineBarrier(cmdBuf, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
}

void VulkanCompute::recordAndSubmitCommands(std::function<void(VkCommandBuffer)> recordFunc) {
    vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &m_fence);

    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
    recordFunc(m_commandBuffer);
    vkEndCommandBuffer(m_commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;

    vkQueueSubmit(m_computeQueue, 1, &submitInfo, m_fence);
    vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
}

bool VulkanCompute::createPipeline(const std::string& name, const std::vector<uint32_t>& spvCode,
                                  const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    ComputeShader shader;
    shader.module = createShaderModule(spvCode);
    if (shader.module == VK_NULL_HANDLE) {
        return false;
    }
    m_shaders[name] = shader;

    ComputePipeline pipeline;

    // Create Descriptor Set Layout
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &pipeline.descriptorSetLayout) != VK_SUCCESS) {
        return false;
    }

    // Create Pipeline Layout with Push Constant range
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = 128; // enough space for most push constants

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &pipeline.descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &pipeline.layout) != VK_SUCCESS) {
        return false;
    }

    // Create Compute Pipeline
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = pipeline.layout;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader.module;
    pipelineInfo.stage.pName = "main";

    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline.pipeline) != VK_SUCCESS) {
        return false;
    }

    m_pipelines[name] = pipeline;
    return true;
}

ComputePipeline* VulkanCompute::getPipeline(const std::string& name) {
    auto it = m_pipelines.find(name);
    if (it != m_pipelines.end()) {
        return &it->second;
    }
    return nullptr;
}

void VulkanCompute::copyBufferToImage(VkBuffer buffer, ComputeImage& image, uint32_t width, uint32_t height) {
    recordAndSubmitCommands([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0,0,0};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    });
}

void VulkanCompute::copyImageToBuffer(ComputeImage& image, VkBuffer buffer, uint32_t width, uint32_t height) {
    recordAndSubmitCommands([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0,0,0};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    });
}

void VulkanCompute::dispatchCompute(const std::string& pipelineName, uint32_t gx, uint32_t gy, uint32_t gz,
                                    const std::vector<VkDescriptorSet>& sets) {
    auto* pl = getPipeline(pipelineName);
    if (!pl) return;
    recordAndSubmitCommands([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->pipeline);
        if (!sets.empty())
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->layout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdDispatch(cmd, gx, gy, gz);
    });
}

bool VulkanCompute::allocateDescriptorSets(ComputePipeline& pipeline) {
    if (pipeline.descriptorSetLayout == VK_NULL_HANDLE) return false;
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 4;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 4;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &pipeline.descriptorPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pipeline.descriptorPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &pipeline.descriptorSetLayout;
    pipeline.descriptorSets.resize(1);
    if (vkAllocateDescriptorSets(m_device, &alloc, pipeline.descriptorSets.data()) != VK_SUCCESS) return false;
    return true;
}

bool VulkanCompute::applyBlur(const ComputeImage& input, ComputeImage& output, float radius, bool vertical) {
    auto* pl = getPipeline("blur");
    if (!pl) return false;
    if (pl->descriptorSets.empty() && !allocateDescriptorSets(*pl)) return false;
    BlurParams pc{ radius, vertical?1:0, input.width, input.height };
    recordAndSubmitCommands([&](VkCommandBuffer cmd){
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->layout, 0, static_cast<uint32_t>(pl->descriptorSets.size()), pl->descriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, pl->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        uint32_t gx=(input.width+15)/16, gy=(input.height+15)/16;
        vkCmdDispatch(cmd,gx,gy,1);
    });
    return true;
}
bool VulkanCompute::applyWaterWaves(const ComputeImage& input, const ComputeImage& mask, ComputeImage& output, float speed, float scale, float strength, float direction, float time) {
    auto* pl = getPipeline("water_waves");
    if (!pl) pl=getPipeline("waterwaves");
    if (!pl) return false;
    if (pl->descriptorSets.empty() && !allocateDescriptorSets(*pl)) return false;
    WaveParams pc{ speed, scale, strength, direction, time, input.width, input.height };
    recordAndSubmitCommands([&](VkCommandBuffer cmd){
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->layout, 0, static_cast<uint32_t>(pl->descriptorSets.size()), pl->descriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, pl->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd,(input.width+15)/16,(input.height+15)/16,1);
    });
    return true;
}
bool VulkanCompute::applyPulse(const ComputeImage& input, const ComputeImage& mask, ComputeImage& output, float speed, float amount, float power, float time) {
    auto* pl = getPipeline("pulse");
    if (!pl) return false;
    if (pl->descriptorSets.empty() && !allocateDescriptorSets(*pl)) return false;
    PulseParams pc{ speed, amount, power, time, input.width, input.height };
    recordAndSubmitCommands([&](VkCommandBuffer cmd){
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->layout, 0, static_cast<uint32_t>(pl->descriptorSets.size()), pl->descriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, pl->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd,(input.width+15)/16,(input.height+15)/16,1);
    });
    return true;
}
bool VulkanCompute::applyComposition(const ComputeImage& current, const ComputeImage& background, ComputeImage& output, int blendMode) {
    auto* pl = getPipeline("composition");
    if (!pl) return false;
    if (pl->descriptorSets.empty() && !allocateDescriptorSets(*pl)) return false;
    CompositionParams pc{ blendMode, current.width, current.height };
    recordAndSubmitCommands([&](VkCommandBuffer cmd){
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->layout, 0, static_cast<uint32_t>(pl->descriptorSets.size()), pl->descriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, pl->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd,(current.width+15)/16,(current.height+15)/16,1);
    });
    return true;
}

} // namespace WallpaperEngine::Render