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

    // Query queue details from context..
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

    destroyStagingBuffer(m_grainStaging);
    destroyImage(m_grainImage);
    destroyImage(m_grainOutImage);
    m_grainImage = {};
    m_grainOutImage = {};
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

    if (vkBindImageMemory(m_device, outImage.image, outImage.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(m_device, outImage.memory, nullptr);
        vkDestroyImage(m_device, outImage.image, nullptr);
        outImage.image = VK_NULL_HANDLE;
        outImage.memory = VK_NULL_HANDLE;
        return false;
    }

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
    // Update descriptors: 0=input sampler, 1=output storage
    VkDescriptorImageInfo inInfo{ input.sampler, input.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = pl->descriptorSets[0];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &inInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = pl->descriptorSets[0];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &outInfo;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
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
    VkDescriptorImageInfo inInfo{ input.sampler, input.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo maskInfo{ mask.sampler, mask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = pl->descriptorSets[0]; writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].descriptorCount = 1; writes[0].pImageInfo = &inInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = pl->descriptorSets[0]; writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].descriptorCount = 1; writes[1].pImageInfo = &maskInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[2].dstSet = pl->descriptorSets[0]; writes[2].dstBinding = 2; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[2].descriptorCount = 1; writes[2].pImageInfo = &outInfo;
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
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
    VkDescriptorImageInfo inInfo{ input.sampler, input.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo maskInfo{ mask.sampler, mask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = pl->descriptorSets[0]; writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].descriptorCount = 1; writes[0].pImageInfo = &inInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = pl->descriptorSets[0]; writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].descriptorCount = 1; writes[1].pImageInfo = &maskInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[2].dstSet = pl->descriptorSets[0]; writes[2].dstBinding = 2; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[2].descriptorCount = 1; writes[2].pImageInfo = &outInfo;
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
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
    VkDescriptorImageInfo curInfo{ current.sampler, current.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo bgInfo{ background.sampler, background.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = pl->descriptorSets[0]; writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].descriptorCount = 1; writes[0].pImageInfo = &curInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = pl->descriptorSets[0]; writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].descriptorCount = 1; writes[1].pImageInfo = &bgInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[2].dstSet = pl->descriptorSets[0]; writes[2].dstBinding = 2; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[2].descriptorCount = 1; writes[2].pImageInfo = &outInfo;
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
    CompositionParams pc{ blendMode, current.width, current.height };
    recordAndSubmitCommands([&](VkCommandBuffer cmd){
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->layout, 0, static_cast<uint32_t>(pl->descriptorSets.size()), pl->descriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, pl->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd,(current.width+15)/16,(current.height+15)/16,1);
    });
    return true;
}

void VulkanCompute::destroyStagingBuffer(ComputeStagingBuffer& staging) {
    if (staging.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, staging.buffer, nullptr);
    }
    if (staging.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, staging.memory, nullptr);
    }
    staging = {};
}

bool VulkanCompute::ensureGrainResources(uint32_t width, uint32_t height) {
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(width) * height * 4;
    if (m_grainImage.width == width && m_grainImage.height == height &&
        m_grainOutImage.width == width && m_grainOutImage.height == height &&
        m_grainStaging.size >= byteSize) {
        return true;
    }

    destroyStagingBuffer(m_grainStaging);
    destroyImage(m_grainImage);
    destroyImage(m_grainOutImage);
    m_grainImage = {};
    m_grainOutImage = {};

    if (!createImage(width, height, m_grainImage)) return false;
    if (!createImage(width, height, m_grainOutImage)) return false;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = byteSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_grainStaging.buffer) != VK_SUCCESS) {
        destroyStagingBuffer(m_grainStaging);
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, m_grainStaging.buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = m_vulkanCtx->findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &m_grainStaging.memory) != VK_SUCCESS) {
        destroyStagingBuffer(m_grainStaging);
        return false;
    }
    if (vkBindBufferMemory(m_device, m_grainStaging.buffer, m_grainStaging.memory, 0) != VK_SUCCESS) {
        destroyStagingBuffer(m_grainStaging);
        return false;
    }
    m_grainStaging.size = byteSize;
    return true;
}

bool VulkanCompute::applyFilmGrain(QImage& frame, float power, float scale, float frameTime) {
    auto* pl = getPipeline("film_grain");
    if (!pl || frame.isNull() || frame.format() != QImage::Format_RGBA8888) return false;
    const uint32_t w = static_cast<uint32_t>(frame.width());
    const uint32_t h = static_cast<uint32_t>(frame.height());
    if (w == 0 || h == 0 || power <= 0.0f) return false;
    if (!ensureGrainResources(w, h)) return false;
    if (pl->descriptorSets.empty() && !allocateDescriptorSets(*pl)) return false;

    // Input frame (sampler) and grain result (storage). Kept separate so the
    // shader never samples a resource it is also writing.
    VkDescriptorImageInfo inInfo{ m_grainImage.sampler, m_grainImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, m_grainOutImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = pl->descriptorSets[0];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &inInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = pl->descriptorSets[0];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &outInfo;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

    GrainParams pc{ power, scale, frameTime, 0.0f, w, h };

    // Map staging and copy the source frame in.
    void* mapped = nullptr;
    if (vkMapMemory(m_device, m_grainStaging.memory, 0, m_grainStaging.size, 0, &mapped) != VK_SUCCESS) return false;
    std::memcpy(mapped, frame.constBits(), static_cast<size_t>(m_grainStaging.size));
    vkUnmapMemory(m_device, m_grainStaging.memory);

    // One submit: upload -> grain dispatch -> readback. Barriers keep every
    // stage correctly ordered on a single command buffer.
    recordAndSubmitCommands([&](VkCommandBuffer cmd){
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        auto barrierImage = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.image = image;
            vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        };

        // Host buffer -> input image
        barrierImage(m_grainImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { w, h, 1 };
        vkCmdCopyBufferToImage(cmd, m_grainStaging.buffer, m_grainImage.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrierImage(m_grainImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // Output image ready for storage writes
        barrierImage(m_grainOutImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     0, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl->layout, 0,
                                static_cast<uint32_t>(pl->descriptorSets.size()), pl->descriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, pl->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (w + 15) / 16, (h + 15) / 16, 1);

        // Grain result -> host buffer
        barrierImage(m_grainOutImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(cmd, m_grainOutImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_grainStaging.buffer, 1, &region);
    });

    // Fence has signalled: staging now holds the grained frame.
    if (vkMapMemory(m_device, m_grainStaging.memory, 0, m_grainStaging.size, 0, &mapped) != VK_SUCCESS) return false;
    std::memcpy(frame.bits(), mapped, static_cast<size_t>(m_grainStaging.size));
    vkUnmapMemory(m_device, m_grainStaging.memory);
    return true;
}

} // namespace WallpaperEngine::Render