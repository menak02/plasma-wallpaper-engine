#pragma once
#include <vulkan/vulkan.hpp>

class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    bool init();

private:
    vk::Instance m_instance;
    vk::PhysicalDevice m_physicalDevice;
    vk::Device m_device;
};
