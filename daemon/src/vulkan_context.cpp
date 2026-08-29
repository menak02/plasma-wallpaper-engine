#include "vulkan_context.h"
#include <iostream>

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() {
    if (m_device) m_device.destroy();
    if (m_instance) m_instance.destroy();
}

bool VulkanContext::init() {
    vk::ApplicationInfo appInfo("PlasmaWallpaperEngineDaemon", 1, "NoEngine", 1, VK_API_VERSION_1_2);
    vk::InstanceCreateInfo createInfo({}, &appInfo);

    try {
        m_instance = vk::createInstance(createInfo);
        std::cout << "Vulkan instance created successfully." << std::endl;
        // Device enumeration and dmabuf extensions setup will be implemented in later phases
    } catch (const std::exception& e) {
        std::cerr << "Vulkan instance creation failed: " << e.what() << std::endl;
        return false;
    }
    return true;
}
