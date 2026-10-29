// Dear ImGui: standalone example application for Glfw + Vulkan

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

// Important note to the reader who wish to integrate imgui_impl_vulkan.cpp/.h in their own engine/app.
// - Common ImGui_ImplVulkan_XXX functions and structures are used to interface with imgui_impl_vulkan.cpp/.h.
//   You will use those if you want to use this rendering backend in your engine/app.
// - Helper ImGui_ImplVulkanH_XXX functions and structures are only used by this example (main.cpp) and by
//   the backend itself (imgui_impl_vulkan.cpp), but should PROBABLY NOT be used by your own engine/app code.
// Read comments in imgui_impl_vulkan.h.

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "app.h"
#include "decoder.h"
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// Volk headers
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include <volk.h>
#endif

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and compatibility with old VS compilers.
// To link with VS2010-era libraries, VS2015+ requires linking with legacy_stdio_definitions.lib, which we do using this pragma.
// Your own project should not be affected, as you are likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

//#define APP_USE_UNLIMITED_FRAME_RATE
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
#endif

// Data
static VkAllocationCallbacks*   g_Allocator = nullptr;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t                 g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
static void check_vk_result(VkResult err)
{
    if (err == VK_SUCCESS)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData)
{
    (void)flags; (void)object; (void)location; (void)messageCode; (void)pUserData; (void)pLayerPrefix; // Unused arguments
    fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif // APP_USE_VULKAN_DEBUG_REPORT

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension)
{
    for (const VkExtensionProperties& p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

static void SetupVulkan(ImVector<const char*> instance_extensions)
{
    VkResult err;
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
    volkInitialize();
#endif

    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        // Enumerate available extensions
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);

        // Enable required extensions
        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        // Enabling validation layers
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        // Create Vulkan Instance
        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(g_Instance);
#endif

        // Setup the debug report callback
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report;
        debug_report_ci.pUserData = nullptr;
        err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
        check_vk_result(err);
#endif
    }

    // Select Physical Device (GPU)
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    // Select graphics queue family
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    // Create Logical Device (with 1 queue)
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // Enumerate physical device extension
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    // Create Descriptor Pool
    // If you wish to load e.g. additional textures you may need to alter pools sizes and maxSets.
    {
        VkDescriptorPoolSize pool_sizes[] =
        {
            // One per texture ImGui draws: the font, the live picture, the
            // enhanced picture, the viewer and one per gallery thumbnail.
            // 8 ran out once the gallery held a few files (abort on open).
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

// All the ImGui_ImplVulkanH_XXX structures/functions are optional helpers used by the demo.
// Your real engine/app may not use them.
static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
{
    wd->Surface = surface;

    // Check for WSI support
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE)
    {
        fprintf(stderr, "Error no WSI support on physical device 0\n");
        exit(-1);
    }

    // Select Surface Format
    const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    // Select Present Mode
#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));
    //printf("[vulkan] Selected PresentMode = %d\n", wd->PresentMode);

    // Create SwapChain, RenderPass, Framebuffer, etc.
    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
}

static void CleanupVulkan()
{
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    // Remove the debug report callback
    auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG_REPORT

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow()
{
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data)
{
    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);    // wait indefinitely instead of periodically checking
        check_vk_result(err);

        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    // Record dear imgui primitives into command buffer
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    // Submit command buffer
    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd)
{
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount; // Now we can use the next set of semaphores
}

// ---------------------------------------------------------------------------
// Glint desktop app
// ---------------------------------------------------------------------------

#include "app.h"
#include "decoder.h"
#include "player.h"
#include "destinations.h"
#include "tls_relay.h"
#include "oauth.h"
#include "log.h"
#include "settings.h"
#include "glint/version.h"

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <future>
#include <map>

static VkCommandPool   g_UploadPool = VK_NULL_HANDLE;
static VkCommandBuffer g_UploadCmd  = VK_NULL_HANDLE;
static VkFence         g_UploadFence = VK_NULL_HANDLE;   // signalled when the last upload finished

struct VideoTexture {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
    VkSampler      sampler = VK_NULL_HANDLE;
    VkBuffer       staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    size_t         staging_size = 0;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    ImTextureID    im_id() const { return (ImTextureID)(intptr_t)descriptor; }
    int            width = 0;
    int            height = 0;
    bool           uploaded = false;
    bool           live = false;   // the console picture: gets the enhancement passes
};

// Picture enhancement passes, recorded after each upload (defined below).
static void enhance_record(VkCommandBuffer cmd, VideoTexture& tex);

// Bumped whenever a picture image is recreated. Descriptor caches compare
// view handles, and a new view can reuse a freed one's value, so they also
// rebind when this changes.
static uint32_t g_view_generation = 0;

static uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    return 0;
}

static void create_upload_resources()
{
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = g_QueueFamily;
    check_vk_result(vkCreateCommandPool(g_Device, &pool_info, g_Allocator, &g_UploadPool));

    VkCommandBufferAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = g_UploadPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    check_vk_result(vkAllocateCommandBuffers(g_Device, &alloc, &g_UploadCmd));

    VkFenceCreateInfo fence = {};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check_vk_result(vkCreateFence(g_Device, &fence, g_Allocator, &g_UploadFence));
}

// Uploads are submitted without waiting (vkQueueWaitIdle busy-waits on some
// drivers and kept the UI thread at 100%). Anything that reuses the staging
// memory or the command buffer waits here first; by then the GPU is long done.
static void upload_wait()
{
    if (g_UploadFence) check_vk_result(vkWaitForFences(g_Device, 1, &g_UploadFence, VK_TRUE, UINT64_MAX));
}

static VkCommandBuffer begin_once()
{
    upload_wait();
    check_vk_result(vkResetFences(g_Device, 1, &g_UploadFence));
    check_vk_result(vkResetCommandPool(g_Device, g_UploadPool, 0));
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(g_UploadCmd, &begin));
    return g_UploadCmd;
}

static void end_once()
{
    check_vk_result(vkEndCommandBuffer(g_UploadCmd));
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_UploadCmd;
    // Same queue as the UI's draw, so the draw sees the upload (the barriers
    // recorded here order it); no CPU wait needed.
    check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, g_UploadFence));
}

static void image_barrier(VkCommandBuffer cmd, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

static void destroy_texture(VideoTexture& tex)
{
    if (g_Device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(g_Device);
    if (tex.descriptor) {
        ImGui_ImplVulkan_RemoveTexture(tex.descriptor);
        tex.descriptor = VK_NULL_HANDLE;
    }
    if (tex.view) vkDestroyImageView(g_Device, tex.view, g_Allocator);
    if (tex.image) vkDestroyImage(g_Device, tex.image, g_Allocator);
    if (tex.memory) vkFreeMemory(g_Device, tex.memory, g_Allocator);
    if (tex.sampler) vkDestroySampler(g_Device, tex.sampler, g_Allocator);
    if (tex.staging) vkDestroyBuffer(g_Device, tex.staging, g_Allocator);
    if (tex.staging_memory) vkFreeMemory(g_Device, tex.staging_memory, g_Allocator);
    bool live = tex.live;
    tex = VideoTexture{};
    tex.live = live;
}

static void ensure_image(VideoTexture& tex, int width, int height)
{
    if (tex.image && tex.width == width && tex.height == height) return;
    if (tex.image) vkDeviceWaitIdle(g_Device);   // the old image may still be in flight
    if (tex.descriptor) {
        ImGui_ImplVulkan_RemoveTexture(tex.descriptor);
        tex.descriptor = VK_NULL_HANDLE;
    }
    if (tex.view) { vkDestroyImageView(g_Device, tex.view, g_Allocator); tex.view = VK_NULL_HANDLE; }
    if (tex.image) { vkDestroyImage(g_Device, tex.image, g_Allocator); tex.image = VK_NULL_HANDLE; }
    if (tex.memory) { vkFreeMemory(g_Device, tex.memory, g_Allocator); tex.memory = VK_NULL_HANDLE; }

    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent.width = (uint32_t)width;
    info.extent.height = (uint32_t)height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT;   // storage: written by the YUV compute shader
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check_vk_result(vkCreateImage(g_Device, &info, g_Allocator, &tex.image));

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(g_Device, tex.image, &requirements);
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &tex.memory));
    check_vk_result(vkBindImageMemory(g_Device, tex.image, tex.memory, 0));

    VkImageViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = tex.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    check_vk_result(vkCreateImageView(g_Device, &view, g_Allocator, &tex.view));

    if (!tex.sampler) {
        VkSamplerCreateInfo sampler = {};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check_vk_result(vkCreateSampler(g_Device, &sampler, g_Allocator, &tex.sampler));
    }

    tex.width = width;
    tex.height = height;
    tex.uploaded = false;
    g_view_generation++;   // a new view can reuse the old handle value: make the converter rebind
}

static void upload_frame(VideoTexture& tex, const std::vector<uint8_t>& rgba, int width, int height)
{
    ensure_image(tex, width, height);

    size_t bytes = (size_t)width * height * 4;
    if (tex.staging_size < bytes) {
        upload_wait();
        if (tex.staging) vkDestroyBuffer(g_Device, tex.staging, g_Allocator);
        if (tex.staging_memory) vkFreeMemory(g_Device, tex.staging_memory, g_Allocator);
        VkBufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check_vk_result(vkCreateBuffer(g_Device, &info, g_Allocator, &tex.staging));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(g_Device, tex.staging, &requirements);
        VkMemoryAllocateInfo alloc = {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &tex.staging_memory));
        check_vk_result(vkBindBufferMemory(g_Device, tex.staging, tex.staging_memory, 0));
        tex.staging_size = bytes;
    }

    upload_wait();   // the previous upload may still be reading the staging buffer
    void* mapped = nullptr;
    check_vk_result(vkMapMemory(g_Device, tex.staging_memory, 0, bytes, 0, &mapped));
    std::memcpy(mapped, rgba.data(), bytes);
    vkUnmapMemory(g_Device, tex.staging_memory);

    VkCommandBuffer cmd = begin_once();
    image_barrier(cmd, tex.image,
                  tex.uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                               : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)width;
    region.imageExtent.height = (uint32_t)height;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, tex.staging, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    enhance_record(cmd, tex);
    end_once();

    tex.uploaded = true;
    if (!tex.descriptor)
        tex.descriptor = ImGui_ImplVulkan_AddTexture(tex.sampler, tex.view,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ---------------------------------------------------------------------------
// GPU colour conversion: YUV planes -> RGBA display image (Vulkan compute).
// The decoder hands out NV12 (D3D11VA) or I420 (CPU) planes; they are copied
// to small textures and shaders/yuv_to_rgba.comp writes the display image.
// ---------------------------------------------------------------------------

static const uint32_t kYuvToRgbaSpv[] =
#include "shaders/yuv_to_rgba.spv.inc"
;

struct YuvPlane {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool initialised = false;   // has a defined layout (SHADER_READ_ONLY)
};

struct YuvConverter {
    bool tried = false;
    bool available = false;
    std::string error;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    YuvPlane planes[3];
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    size_t staging_size = 0;
    void* mapped = nullptr;
    VkImageView bound[4] = {};
};
static YuvConverter g_yuv;

static void layout_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags src_access, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
}

static bool yuv_converter_init()
{
    if (g_yuv.tried) return g_yuv.available;
    g_yuv.tried = true;
    // The graphics queue must also do compute (true on desktop GPUs, but check).
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, families.data());
    if (g_QueueFamily >= count || !(families[g_QueueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        g_yuv.error = "the graphics queue has no compute support";
        return false;
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        g_yuv.error = "RGBA storage images are not supported";
        return false;
    }

    VkSamplerCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(g_Device, &si, g_Allocator, &g_yuv.nearest) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(g_Device, &si, g_Allocator, &g_yuv.linear) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding bindings[4] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    for (int i = 1; i < 4; i++) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl = {};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 4;
    dl.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_Device, &dl, g_Allocator, &g_yuv.set_layout) != VK_SUCCESS) return false;

    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 5 * sizeof(int32_t)};
    VkPipelineLayoutCreateInfo pl = {};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_yuv.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(g_Device, &pl, g_Allocator, &g_yuv.pipeline_layout) != VK_SUCCESS) return false;

    VkShaderModuleCreateInfo sm = {};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = sizeof(kYuvToRgbaSpv);
    sm.pCode = kYuvToRgbaSpv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &sm, g_Allocator, &module) != VK_SUCCESS) {
        g_yuv.error = "cannot load the colour shader";
        return false;
    }
    VkComputePipelineCreateInfo cp = {};
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = module;
    cp.stage.pName = "main";
    cp.layout = g_yuv.pipeline_layout;
    VkResult r = vkCreateComputePipelines(g_Device, g_PipelineCache, 1, &cp, g_Allocator, &g_yuv.pipeline);
    vkDestroyShaderModule(g_Device, module, g_Allocator);
    if (r != VK_SUCCESS) {
        g_yuv.error = "cannot create the colour pipeline";
        return false;
    }

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}};
    VkDescriptorPoolCreateInfo dp = {};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 1;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(g_Device, &dp, g_Allocator, &g_yuv.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo da = {};
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = g_yuv.pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &g_yuv.set_layout;
    if (vkAllocateDescriptorSets(g_Device, &da, &g_yuv.set) != VK_SUCCESS) return false;
    g_yuv.available = true;
    return true;
}

static void yuv_destroy_plane(YuvPlane& p)
{
    if (p.view) vkDestroyImageView(g_Device, p.view, g_Allocator);
    if (p.image) vkDestroyImage(g_Device, p.image, g_Allocator);
    if (p.memory) vkFreeMemory(g_Device, p.memory, g_Allocator);
    p = YuvPlane{};
}

static void yuv_converter_destroy()
{
    if (g_Device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(g_Device);
    for (auto& p : g_yuv.planes) yuv_destroy_plane(p);
    if (g_yuv.staging_memory) {
        vkUnmapMemory(g_Device, g_yuv.staging_memory);
        vkFreeMemory(g_Device, g_yuv.staging_memory, g_Allocator);
    }
    if (g_yuv.staging) vkDestroyBuffer(g_Device, g_yuv.staging, g_Allocator);
    if (g_yuv.pool) vkDestroyDescriptorPool(g_Device, g_yuv.pool, g_Allocator);
    if (g_yuv.pipeline) vkDestroyPipeline(g_Device, g_yuv.pipeline, g_Allocator);
    if (g_yuv.pipeline_layout) vkDestroyPipelineLayout(g_Device, g_yuv.pipeline_layout, g_Allocator);
    if (g_yuv.set_layout) vkDestroyDescriptorSetLayout(g_Device, g_yuv.set_layout, g_Allocator);
    if (g_yuv.nearest) vkDestroySampler(g_Device, g_yuv.nearest, g_Allocator);
    if (g_yuv.linear) vkDestroySampler(g_Device, g_yuv.linear, g_Allocator);
    g_yuv = YuvConverter{};
}

static void yuv_ensure_plane(YuvPlane& p, int width, int height, VkFormat format)
{
    if (p.image && p.width == width && p.height == height && p.format == format) return;
    vkDeviceWaitIdle(g_Device);
    yuv_destroy_plane(p);
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {(uint32_t)width, (uint32_t)height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check_vk_result(vkCreateImage(g_Device, &info, g_Allocator, &p.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_Device, p.image, &req);
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &p.memory));
    check_vk_result(vkBindImageMemory(g_Device, p.image, p.memory, 0));
    VkImageViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = p.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    check_vk_result(vkCreateImageView(g_Device, &view, g_Allocator, &p.view));
    p.width = width;
    p.height = height;
    p.format = format;
    p.initialised = false;
    g_view_generation++;   // the new view may reuse an old handle value
}

// Converts `pic` (NV12 / I420) into `tex` on the GPU. Returns false if the
// converter is unavailable (the caller then converts on the CPU).
static bool upload_yuv(VideoTexture& tex, const glint::Picture& pic)
{
    if (!yuv_converter_init()) return false;
    const int w = pic.width, h = pic.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    const bool nv12 = pic.format == glint::Picture::Format::Nv12;
    ensure_image(tex, w, h);
    yuv_ensure_plane(g_yuv.planes[0], w, h, VK_FORMAT_R8_UNORM);
    yuv_ensure_plane(g_yuv.planes[1], cw, ch, nv12 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM);
    if (nv12) yuv_ensure_plane(g_yuv.planes[2], 1, 1, VK_FORMAT_R8_UNORM);   // unused, must be bound
    else yuv_ensure_plane(g_yuv.planes[2], cw, ch, VK_FORMAT_R8_UNORM);

    if (g_yuv.staging_size < pic.data.size()) {
        upload_wait();
        if (g_yuv.staging_memory) {
            vkUnmapMemory(g_Device, g_yuv.staging_memory);
            vkFreeMemory(g_Device, g_yuv.staging_memory, g_Allocator);
        }
        if (g_yuv.staging) vkDestroyBuffer(g_Device, g_yuv.staging, g_Allocator);
        VkBufferCreateInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = pic.data.size();
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check_vk_result(vkCreateBuffer(g_Device, &bi, g_Allocator, &g_yuv.staging));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_Device, g_yuv.staging, &req);
        VkMemoryAllocateInfo alloc = {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &g_yuv.staging_memory));
        check_vk_result(vkBindBufferMemory(g_Device, g_yuv.staging, g_yuv.staging_memory, 0));
        check_vk_result(vkMapMemory(g_Device, g_yuv.staging_memory, 0, VK_WHOLE_SIZE, 0, &g_yuv.mapped));
        g_yuv.staging_size = pic.data.size();
    }
    upload_wait();   // the previous upload may still be reading the staging buffer
    std::memcpy(g_yuv.mapped, pic.data.data(), pic.data.size());

    VkImageView want[4] = {tex.view, g_yuv.planes[0].view, g_yuv.planes[1].view, g_yuv.planes[2].view};
    static uint32_t bound_generation = 0;
    if (std::memcmp(want, g_yuv.bound, sizeof(want)) != 0 || bound_generation != g_view_generation) {
        bound_generation = g_view_generation;
        VkDescriptorImageInfo images[4] = {};
        images[0] = {VK_NULL_HANDLE, tex.view, VK_IMAGE_LAYOUT_GENERAL};
        images[1] = {g_yuv.nearest, g_yuv.planes[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[2] = {g_yuv.linear, g_yuv.planes[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[3] = {g_yuv.linear, g_yuv.planes[2].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[4] = {};
        for (int i = 0; i < 4; i++) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = g_yuv.set;
            writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(g_Device, 4, writes, 0, nullptr);
        std::memcpy(g_yuv.bound, want, sizeof(want));
    }

    VkCommandBuffer cmd = begin_once();
    struct Copy { int plane; size_t offset; };
    std::vector<Copy> copies = {{0, 0}, {1, (size_t)w * h}};
    if (!nv12) copies.push_back({2, (size_t)w * h + (size_t)cw * ch});
    for (int i = 0; i < 3; i++) {
        YuvPlane& p = g_yuv.planes[i];
        bool copied = i < 2 || !nv12;
        if (!copied && p.initialised) continue;
        layout_barrier(cmd, p.image, p.initialised ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       copied ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_SHADER_READ_BIT, copied ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT);
        if (!copied) p.initialised = true;
    }
    for (const Copy& c : copies) {
        YuvPlane& p = g_yuv.planes[c.plane];
        VkBufferImageCopy region = {};
        region.bufferOffset = c.offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {(uint32_t)p.width, (uint32_t)p.height, 1};
        vkCmdCopyBufferToImage(cmd, g_yuv.staging, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        layout_barrier(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        p.initialised = true;
    }
    layout_barrier(cmd, tex.image, tex.uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_yuv.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_yuv.pipeline_layout, 0, 1, &g_yuv.set, 0, nullptr);
    int32_t params[5] = {w, h, nv12 ? 1 : 0, pic.full_range ? 1 : 0, pic.bt709 ? 1 : 0};
    vkCmdPushConstants(cmd, g_yuv.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), params);
    vkCmdDispatch(cmd, (uint32_t)(w + 15) / 16, (uint32_t)(h + 15) / 16, 1);
    layout_barrier(cmd, tex.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    enhance_record(cmd, tex);
    end_once();

    tex.uploaded = true;
    if (!tex.descriptor)
        tex.descriptor = ImGui_ImplVulkan_AddTexture(tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Reads the display image back (tests only).
static bool read_texture(VideoTexture& tex, std::vector<uint8_t>& rgba)
{
    size_t bytes = (size_t)tex.width * tex.height * 4;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check_vk_result(vkCreateBuffer(g_Device, &bi, g_Allocator, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_Device, buffer, &req);
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &memory));
    check_vk_result(vkBindBufferMemory(g_Device, buffer, memory, 0));
    VkCommandBuffer cmd = begin_once();
    layout_barrier(cmd, tex.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {(uint32_t)tex.width, (uint32_t)tex.height, 1};
    vkCmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    layout_barrier(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    end_once();
    upload_wait();
    void* mapped = nullptr;
    vkMapMemory(g_Device, memory, 0, bytes, 0, &mapped);
    rgba.assign((const uint8_t*)mapped, (const uint8_t*)mapped + bytes);
    vkUnmapMemory(g_Device, memory);
    vkDestroyBuffer(g_Device, buffer, g_Allocator);
    vkFreeMemory(g_Device, memory, g_Allocator);
    return true;
}

// ---------------------------------------------------------------------------
// Picture enhancement (Settings -> Picture): compute passes between the
// converted picture and the screen, recorded into the same command buffer as
// the upload, so they add no CPU wait.
//   pre    cleanup + colour at the stream's size   (live tex -> a)
//   scale  upscale to the window's pixel size       (a -> b), only when larger
//   final  sharpen, compare split, dither           (b or a -> out)
// Off draws the converted picture as before: no passes, no cost.
// ---------------------------------------------------------------------------

static const uint32_t kEnhancePreSpv[] =
#include "shaders/enhance_pre.spv.inc"
;
static const uint32_t kEnhanceScaleSpv[] =
#include "shaders/enhance_scale.spv.inc"
;
static const uint32_t kEnhanceFinalSpv[] =
#include "shaders/enhance_final.spv.inc"
;
static const uint32_t kEnhanceEasuSpv[] =
#include "shaders/enhance_easu.spv.inc"
;
static const uint32_t kFsrcnnxLumaSpv[] =
#include "shaders/fsrcnnx_luma.spv.inc"
;
static const uint32_t kFsrcnnxOutSpv[] =
#include "shaders/fsrcnnx_out.spv.inc"
;

// One FSRCNNX layer (tools/gen_fsrcnnx.js): reads up to three named images at
// the stream's size, writes one.
struct FsrcnnxLayer {
    const char* desc;
    const uint32_t* spv;
    size_t bytes;
    const char* save;
    const char* binds[3];
};
#include "shaders/fsrcnnx/passes.inc"
constexpr int kFsrcnnxLayerCount = (int)(sizeof(kFsrcnnxLayers) / sizeof(kFsrcnnxLayers[0]));
// The images the layers read and write (mpv's names), all RGBA16F.
static const char* const kFsrcnnxImages[] = {"LUMA",   "FEATURE1", "FEATURE2", "MODEL21", "MODEL22",
                                             "MODEL1", "MODEL2",   "RES1",     "RES2",    "SUBCONV1"};
constexpr int kFsrcnnxImageCount = (int)(sizeof(kFsrcnnxImages) / sizeof(kFsrcnnxImages[0]));

enum Upscaler { UpscaleBilinear, UpscaleLanczos, UpscaleFsr, UpscaleAi };
static const char* const kUpscalerNames[] = {"Bilinear", "Lanczos", "FSR 1", "AI"};

// What the passes do this frame, resolved from the profile (or Custom).
struct EnhanceParams {
    bool active = false;
    int upscaler = 0;   // Upscaler
    float sharpness = 0, denoise = 0, deband = 0, dither = 0;
    float exposure = 0, saturation = 1, shadows = 0;
};

struct EnhanceImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool initialised = false;   // has been written once (layout SHADER_READ_ONLY)
};

// Pipelines; the FSRCNNX layers follow EnhanceNet.
enum { EnhancePre, EnhanceScale, EnhanceFinal, EnhanceEasu, EnhanceAiLuma, EnhanceAiOut, EnhanceNet };
constexpr int kEnhancePipelines = EnhanceNet + kFsrcnnxLayerCount;

struct Enhancer {
    bool tried = false, available = false;
    std::string error;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipelines[kEnhancePipelines] = {};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[kEnhancePipelines] = {};   // one per pipeline: each runs once a frame
    VkImageView bound[kEnhancePipelines][4] = {};
    VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    EnhanceImage a, b, out, ai2x;
    EnhanceImage net[kFsrcnnxImageCount];
    VkDescriptorSet out_descriptor = VK_NULL_HANDLE;   // `out` for ImGui
    // GPU time of the passes, from timestamps read one frame later.
    VkQueryPool queries = VK_NULL_HANDLE;
    float timestamp_ns = 0.0f;
    bool query_pending = false;
    float gpu_ms = 0.0f;          // smoothed
    // This frame's settings and the window's pixel size for the picture
    // (set by the UI, used by the next upload).
    EnhanceParams params;
    int want_w = 0, want_h = 0;
    uint32_t frame = 0;
    bool shown = false;           // `out` holds the current frame: draw it instead of the live texture
    int used_upscaler = -1;       // what last frame's upscale really was (-1: none, window not larger)
    float split = 0.0f;           // compare split, 0 = off
};
static Enhancer g_enh;

// Side-by-side compare (Settings -> Picture, or C): the same spot of the
// original and the enhanced picture next to each other, zoomed and panned
// together.
struct Compare {
    bool on = false;
    float zoom = 1.0f;              // 1 = the whole picture fits a panel
    ImVec2 center = {0.5f, 0.5f};   // middle of the view, in picture UV
    bool dragging = false;
};
static Compare g_compare;
static bool g_tweaks_open = false;   // side by side's control panel (X)

static bool enhance_init()
{
    if (g_enh.tried) return g_enh.available;
    g_enh.tried = true;
    if (!yuv_converter_init()) {   // same requirements: compute on the graphics queue, RGBA storage images
        g_enh.error = g_yuv.error;
        return false;
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &fp);
    bool half_storage = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;

    VkSamplerCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(g_Device, &si, g_Allocator, &g_enh.nearest) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(g_Device, &si, g_Allocator, &g_enh.linear) != VK_SUCCESS) return false;

    // One layout for every pass: the output, then up to three inputs.
    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (int i = 0; i < 4; i++) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl = {};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 4;
    dl.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_Device, &dl, g_Allocator, &g_enh.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pl = {};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_enh.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(g_Device, &pl, g_Allocator, &g_enh.pipeline_layout) != VK_SUCCESS) return false;

    struct Code { const uint32_t* words; size_t bytes; };
    Code code[kEnhancePipelines] = {{kEnhancePreSpv, sizeof(kEnhancePreSpv)},   {kEnhanceScaleSpv, sizeof(kEnhanceScaleSpv)},
                                    {kEnhanceFinalSpv, sizeof(kEnhanceFinalSpv)}, {kEnhanceEasuSpv, sizeof(kEnhanceEasuSpv)},
                                    {kFsrcnnxLumaSpv, sizeof(kFsrcnnxLumaSpv)},   {kFsrcnnxOutSpv, sizeof(kFsrcnnxOutSpv)}};
    for (int i = 0; i < kFsrcnnxLayerCount; i++) code[EnhanceNet + i] = {kFsrcnnxLayers[i].spv, kFsrcnnxLayers[i].bytes};
    for (int i = 0; i < kEnhancePipelines; i++) {
        if (!half_storage && (i == EnhanceAiLuma || i >= EnhanceNet)) continue;   // AI needs RGBA16F storage
        VkShaderModuleCreateInfo sm = {};
        sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize = code[i].bytes;
        sm.pCode = code[i].words;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(g_Device, &sm, g_Allocator, &module) != VK_SUCCESS) {
            g_enh.error = "cannot load the picture shaders";
            return false;
        }
        VkComputePipelineCreateInfo cp = {};
        cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = module;
        cp.stage.pName = "main";
        cp.layout = g_enh.pipeline_layout;
        VkResult r = vkCreateComputePipelines(g_Device, g_PipelineCache, 1, &cp, g_Allocator, &g_enh.pipelines[i]);
        vkDestroyShaderModule(g_Device, module, g_Allocator);
        if (r != VK_SUCCESS) {
            g_enh.error = "cannot create the picture pipelines";
            return false;
        }
    }

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)kEnhancePipelines},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3u * kEnhancePipelines}};
    VkDescriptorPoolCreateInfo dp = {};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = kEnhancePipelines;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(g_Device, &dp, g_Allocator, &g_enh.pool) != VK_SUCCESS) return false;
    std::vector<VkDescriptorSetLayout> layouts(kEnhancePipelines, g_enh.set_layout);
    VkDescriptorSetAllocateInfo da = {};
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = g_enh.pool;
    da.descriptorSetCount = kEnhancePipelines;
    da.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(g_Device, &da, g_enh.sets) != VK_SUCCESS) return false;

    // GPU timing, where the queue supports timestamps.
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, families.data());
    if (g_QueueFamily < count && families[g_QueueFamily].timestampValidBits > 0 && props.limits.timestampPeriod > 0) {
        VkQueryPoolCreateInfo qi = {};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        if (vkCreateQueryPool(g_Device, &qi, g_Allocator, &g_enh.queries) == VK_SUCCESS)
            g_enh.timestamp_ns = props.limits.timestampPeriod;
    }
    g_enh.available = true;
    return true;
}

// The AI upscaler can run here (RGBA16F storage images are supported).
static bool enhance_ai_available()
{
    return enhance_init() && g_enh.pipelines[EnhanceNet] != VK_NULL_HANDLE;
}

static void enhance_destroy_image(EnhanceImage& im)
{
    if (im.view) vkDestroyImageView(g_Device, im.view, g_Allocator);
    if (im.image) vkDestroyImage(g_Device, im.image, g_Allocator);
    if (im.memory) vkFreeMemory(g_Device, im.memory, g_Allocator);
    im = EnhanceImage{};
}

static void enhance_destroy()
{
    if (g_Device == VK_NULL_HANDLE || !g_enh.tried) return;
    vkDeviceWaitIdle(g_Device);
    if (g_enh.out_descriptor) ImGui_ImplVulkan_RemoveTexture(g_enh.out_descriptor);
    for (EnhanceImage* im : {&g_enh.a, &g_enh.b, &g_enh.out, &g_enh.ai2x}) enhance_destroy_image(*im);
    for (EnhanceImage& im : g_enh.net) enhance_destroy_image(im);
    if (g_enh.queries) vkDestroyQueryPool(g_Device, g_enh.queries, g_Allocator);
    if (g_enh.pool) vkDestroyDescriptorPool(g_Device, g_enh.pool, g_Allocator);
    for (VkPipeline p : g_enh.pipelines)
        if (p) vkDestroyPipeline(g_Device, p, g_Allocator);
    if (g_enh.pipeline_layout) vkDestroyPipelineLayout(g_Device, g_enh.pipeline_layout, g_Allocator);
    if (g_enh.set_layout) vkDestroyDescriptorSetLayout(g_Device, g_enh.set_layout, g_Allocator);
    if (g_enh.nearest) vkDestroySampler(g_Device, g_enh.nearest, g_Allocator);
    if (g_enh.linear) vkDestroySampler(g_Device, g_enh.linear, g_Allocator);
    g_enh = Enhancer{};
}

// (Re)creates `im` at the size and format. Returns true if it changed.
static bool enhance_ensure_image(EnhanceImage& im, int width, int height,
                                 VkFormat format = VK_FORMAT_R8G8B8A8_UNORM)
{
    if (im.image && im.width == width && im.height == height && im.format == format) return false;
    // A new view can get the old one's handle value: forget what every set was
    // bound to, or enhance_bind would skip the update and leave the set
    // pointing at freed memory (device lost when zooming fast).
    std::memset(g_enh.bound, 0, sizeof(g_enh.bound));
    g_view_generation++;
    if (im.image) {
        vkDeviceWaitIdle(g_Device);   // the old image may still be in flight
        if (&im == &g_enh.out && g_enh.out_descriptor) {
            ImGui_ImplVulkan_RemoveTexture(g_enh.out_descriptor);
            g_enh.out_descriptor = VK_NULL_HANDLE;
        }
    }
    enhance_destroy_image(im);
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {(uint32_t)width, (uint32_t)height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // transfer: GLINT_PICTURE_DUMP reads it back
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check_vk_result(vkCreateImage(g_Device, &info, g_Allocator, &im.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_Device, im.image, &req);
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &im.memory));
    check_vk_result(vkBindImageMemory(g_Device, im.image, im.memory, 0));
    VkImageViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = im.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    check_vk_result(vkCreateImageView(g_Device, &view, g_Allocator, &im.view));
    im.width = width;
    im.height = height;
    im.format = format;
    return true;
}

// An input of a pass: an image read through a sampler.
struct EnhanceInput { VkImageView view; VkSampler sampler; };

// Points pipeline `pass` at its output and inputs (only when they changed).
// Unused inputs repeat the first, so every binding is valid.
static void enhance_bind(int pass, VkImageView out, EnhanceInput in0, EnhanceInput in1 = {}, EnhanceInput in2 = {})
{
    if (!in1.view) in1 = in0;
    if (!in2.view) in2 = in0;
    VkImageView want[4] = {out, in0.view, in1.view, in2.view};
    if (std::memcmp(want, g_enh.bound[pass], sizeof(want)) == 0) return;
    vkDeviceWaitIdle(g_Device);   // the set may be in use by a frame in flight
    VkDescriptorImageInfo images[4] = {{VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL},
                                       {in0.sampler, in0.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {in1.sampler, in1.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {in2.sampler, in2.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_enh.sets[pass];
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(g_Device, 4, writes, 0, nullptr);
    std::memcpy(g_enh.bound[pass], want, sizeof(want));
}

static void enhance_dispatch(VkCommandBuffer cmd, int pass, EnhanceImage& out, const void* params, uint32_t bytes)
{
    layout_barrier(cmd, out.image, out.initialised ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_enh.pipelines[pass]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_enh.pipeline_layout, 0, 1, &g_enh.sets[pass], 0,
                            nullptr);
    vkCmdPushConstants(cmd, g_enh.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, params);
    vkCmdDispatch(cmd, (uint32_t)(out.width + 15) / 16, (uint32_t)(out.height + 15) / 16, 1);
    layout_barrier(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    out.initialised = true;
}

static EnhanceImage& enhance_net_image(const char* name)
{
    for (int i = 0; i < kFsrcnnxImageCount; i++)
        if (std::strcmp(kFsrcnnxImages[i], name) == 0) return g_enh.net[i];
    return g_enh.net[0];   // not reached: gen_fsrcnnx.js only emits these names
}

// FSRCNNX: luma -> 13 network layers -> 2x picture in `ai2x`.
static void enhance_record_ai(VkCommandBuffer cmd, int sw, int sh)
{
    for (EnhanceImage& im : g_enh.net) enhance_ensure_image(im, sw, sh, VK_FORMAT_R16G16B16A16_SFLOAT);
    enhance_ensure_image(g_enh.ai2x, sw * 2, sh * 2);
    struct { int32_t w, h; } size = {sw, sh};
    enhance_bind(EnhanceAiLuma, enhance_net_image("LUMA").view, {g_enh.a.view, g_enh.nearest});
    enhance_dispatch(cmd, EnhanceAiLuma, enhance_net_image("LUMA"), &size, sizeof(size));
    for (int i = 0; i < kFsrcnnxLayerCount; i++) {
        const FsrcnnxLayer& layer = kFsrcnnxLayers[i];
        EnhanceInput in[3] = {};
        for (int k = 0; k < 3; k++)
            if (layer.binds[k]) in[k] = {enhance_net_image(layer.binds[k]).view, g_enh.nearest};
        EnhanceImage& out = enhance_net_image(layer.save);
        enhance_bind(EnhanceNet + i, out.view, in[0], in[1], in[2]);
        enhance_dispatch(cmd, EnhanceNet + i, out, &size, sizeof(size));
    }
    enhance_bind(EnhanceAiOut, g_enh.ai2x.view, {enhance_net_image("SUBCONV1").view, g_enh.nearest},
                 {g_enh.a.view, g_enh.linear});
    enhance_dispatch(cmd, EnhanceAiOut, g_enh.ai2x, &size, sizeof(size));
}

static void enhance_record(VkCommandBuffer cmd, VideoTexture& tex)
{
    const EnhanceParams& p = g_enh.params;
    if (!tex.live || !p.active || tex.width <= 0 || !enhance_init()) {
        g_enh.shown = false;
        return;
    }
    // Last frame's GPU time (its command buffer has finished: begin_once waited for it).
    if (g_enh.query_pending) {
        uint64_t t[2] = {};
        if (vkGetQueryPoolResults(g_Device, g_enh.queries, 0, 2, sizeof(t), t, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
            float ms = (float)((double)(t[1] - t[0]) * g_enh.timestamp_ns / 1e6);
            g_enh.gpu_ms = g_enh.gpu_ms <= 0.0f ? ms : g_enh.gpu_ms * 0.9f + ms * 0.1f;
        }
        g_enh.query_pending = false;
    }

    const int sw = tex.width, sh = tex.height;
    // Testing the upscalers on a screen no larger than the stream:
    // GLINT_PICTURE_TARGET=3840x2160 forces the output size.
    static int force_w = -1, force_h = 0;
    if (force_w < 0) {
        force_w = 0;
        if (const char* e = std::getenv("GLINT_PICTURE_TARGET")) std::sscanf(e, "%dx%d", &force_w, &force_h);
    }
    // Upscale only when the window shows the picture larger than the stream
    // (capped at 4K); otherwise the passes run at the stream's size.
    int want_w = force_w > 0 ? force_w : g_enh.want_w, want_h = force_h > 0 ? force_h : g_enh.want_h;
    // Resizing recreates the images (and waits for the GPU), so follow a
    // changing size (zooming, dragging the window) only in 32-pixel steps and
    // once it has held still for 0.15 s; meanwhile the last size is scaled.
    {
        static int held_w = 0, held_h = 0, pending_w = 0, pending_h = 0;
        static double pending_since = 0.0;
        int qw = (want_w + 16) / 32 * 32, qh = (want_h + 16) / 32 * 32;
        double now = glfwGetTime();
        if (held_w == 0 || !g_enh.out.image) {
            held_w = qw; held_h = qh;
        } else if (qw != held_w || qh != held_h) {
            if (qw != pending_w || qh != pending_h) { pending_w = qw; pending_h = qh; pending_since = now; }
            else if (now - pending_since > 0.15) { held_w = qw; held_h = qh; }
        }
        want_w = held_w;
        want_h = held_h;
    }
    int tw = std::min(std::max(want_w, sw), 3840), th = std::min(std::max(want_h, sh), 2160);
    bool scale = tw > sw + 1 || th > sh + 1;
    if (!scale) { tw = sw; th = sh; }
    // The AI upscaler doubles; like mpv's FSRCNNX it is only worth it from 1.3x
    // on, below that Lanczos does as well.
    int upscaler = p.upscaler;
    if (upscaler == UpscaleAi && (!enhance_ai_available() || tw < sw * 1.3f || th < sh * 1.3f))
        upscaler = UpscaleLanczos;
    g_enh.used_upscaler = scale ? upscaler : -1;

    enhance_ensure_image(g_enh.a, sw, sh);
    if (enhance_ensure_image(g_enh.out, tw, th) || !g_enh.out_descriptor)
        g_enh.out_descriptor = ImGui_ImplVulkan_AddTexture(g_enh.linear, g_enh.out.view,
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (g_enh.queries) {
        vkCmdResetQueryPool(cmd, g_enh.queries, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_enh.queries, 0);
    }
    g_enh.frame++;

    enhance_bind(EnhancePre, g_enh.a.view, {tex.view, g_enh.nearest});
    struct { int32_t w, h; float denoise, deband, exposure, saturation, shadows; uint32_t frame; } pre = {
        sw, sh, p.denoise, p.deband, p.exposure, p.saturation, p.shadows, g_enh.frame};
    enhance_dispatch(cmd, EnhancePre, g_enh.a, &pre, sizeof(pre));

    EnhanceImage* processed = &g_enh.a;
    if (scale) {
        EnhanceImage* from = &g_enh.a;
        int fw = sw, fh = sh;
        if (upscaler == UpscaleAi) {
            enhance_record_ai(cmd, sw, sh);
            from = &g_enh.ai2x;
            fw = sw * 2;
            fh = sh * 2;
        }
        if (from->width == tw && from->height == th) {   // the AI's 2x is exactly the window
            processed = from;
        } else {
            enhance_ensure_image(g_enh.b, tw, th);
            struct { int32_t sw, sh, dw, dh, mode; } sc = {fw, fh, tw, th, upscaler == UpscaleBilinear ? 0 : 1};
            if (upscaler == UpscaleFsr) {
                enhance_bind(EnhanceEasu, g_enh.b.view, {from->view, g_enh.linear});
                enhance_dispatch(cmd, EnhanceEasu, g_enh.b, &sc, 16);
            } else {
                enhance_bind(EnhanceScale, g_enh.b.view, {from->view, g_enh.linear});
                enhance_dispatch(cmd, EnhanceScale, g_enh.b, &sc, sizeof(sc));
            }
            processed = &g_enh.b;
        }
    }

    enhance_bind(EnhanceFinal, g_enh.out.view, {processed->view, g_enh.nearest}, {tex.view, g_enh.linear});
    struct { int32_t w, h; float sharpness, dither, split; uint32_t frame; } fin = {
        tw, th, p.sharpness, p.dither, g_compare.on ? 0.0f : g_enh.split, g_enh.frame};
    enhance_dispatch(cmd, EnhanceFinal, g_enh.out, &fin, sizeof(fin));

    if (g_enh.queries) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_enh.queries, 1);
        g_enh.query_pending = true;
    }
    g_enh.shown = true;
}

// Testing: GLINT_PICTURE_DUMP=<folder> saves the enhanced picture as a PNG
// every 300 frames (about 5 s), to inspect the upscalers pixel for pixel.
static void enhance_dump_maybe()
{
    static const char* dir = std::getenv("GLINT_PICTURE_DUMP");
    static uint32_t last = 0;
    if (!dir || !g_enh.shown || !g_enh.out.image || g_enh.frame - last < 300) return;
    last = g_enh.frame;
    VideoTexture view;   // read_texture only needs the image and its size
    view.image = g_enh.out.image;
    view.width = g_enh.out.width;
    view.height = g_enh.out.height;
    std::vector<uint8_t> rgba;
    std::string path, error;
    if (read_texture(view, rgba) && glint::save_screenshot(dir, rgba, view.width, view.height, path, error))
        glint::logf("picture dump: %s (%s, %.2f ms GPU)", path.c_str(),
                    g_enh.used_upscaler >= 0 ? kUpscalerNames[g_enh.used_upscaler] : "no upscaling", g_enh.gpu_ms);
}

// Shows a decoded picture: GPU conversion when possible, else the CPU.
static void present_picture(VideoTexture& tex, const glint::Picture& pic, glint::App& app)
{
    if (pic.format == glint::Picture::Format::Rgba) {
        upload_frame(tex, pic.data, pic.width, pic.height);
        return;
    }
    if (upload_yuv(tex, pic)) return;
    static bool logged = false;
    if (!logged) {
        logged = true;
        glint::logf("colour conversion: GPU unavailable (%s); using the CPU", g_yuv.error.c_str());
    }
    app.set_gpu_convert(false);   // decoder converts from the next frame on
    std::vector<uint8_t> rgba;
    if (glint::picture_to_rgba(pic, rgba)) upload_frame(tex, rgba, pic.width, pic.height);
}

// ---------------------------------------------------------------------------
// UI
//
// Layout: the console picture fills the window; status, the command bar and
// toasts float over it as glass and fade out when the mouse rests. Settings is
// a sheet that slides in from the right, the gallery a full-window overlay.
// All widgets are drawn by hand on top of ImGui's item system so the look stays
// consistent: zinc neutrals, hairline borders, white (dark theme) or near-black
// (light theme) for the primary action, red only for recording and live, amber
// only for frames that arrived late. Dark is the default theme.
// ---------------------------------------------------------------------------

// Live streaming to Twitch / YouTube / TikTok / custom RTMP is switched OFF in
// this build: the Streaming settings tab, Go live in the command bar, the live
// badge, account sign-in and the --broadcast-test / --golive-test /
// --oauth-test / --tls-probe / --store-test modes are compiled out. The engine stays in the
// source (src/stream/: broadcast, destinations, oauth, tls_relay; App's
// start_broadcast) so it can come back by building with -DSL_LIVE_STREAMING=1.
#ifndef SL_LIVE_STREAMING
#define SL_LIVE_STREAMING 0
#endif

static GLFWwindow* g_window = nullptr;

static inline ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x + b.x, a.y + b.y); }
static inline ImVec2 operator-(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x - b.x, a.y - b.y); }
static inline ImVec2 operator*(const ImVec2& a, float s) { return ImVec2(a.x * s, a.y * s); }

namespace ui {

static inline ImU32 rgba(int r, int g, int b, float a = 1.0f)
{
    return IM_COL32(r, g, b, (int)(a * 255.0f + 0.5f));
}

// Every field is an ImU32 so two palettes can be blended field by field.
struct Palette {
    ImU32 scrim;          // dims the picture behind the settings sheet
    ImU32 glass;          // chrome over the picture
    ImU32 glass_hover;
    ImU32 glass_border;
    ImU32 panel;          // sheet, gallery and connect backgrounds
    ImU32 nav;            // raised: tab and segment tracks, toasts
    ImU32 nav_active;     // the selected tab
    ImU32 card;
    ImU32 card_hover;
    ImU32 input;
    ImU32 border;
    ImU32 border_hi;
    ImU32 text;
    ImU32 muted;
    ImU32 faint;
    ImU32 accent;         // primary action: white on dark, near-black on light
    ImU32 accent_hover;
    ImU32 accent_press;
    ImU32 accent_soft;
    ImU32 on_accent;
    ImU32 success;        // monochrome: "fine" is not a colour here
    ImU32 success_soft;
    ImU32 danger;         // red text
    ImU32 danger_fill;    // recording, live, destructive buttons
    ImU32 danger_soft;
    ImU32 warn;           // late frames
    ImU32 warn_soft;
    ImU32 shadow;
};

static const Palette kDark = {
    rgba(9, 9, 11, 0.45f),      rgba(9, 9, 11, 0.80f),      rgba(39, 39, 42, 0.92f),   rgba(255, 255, 255, 0.08f),
    rgba(9, 9, 11),             rgba(24, 24, 27),           rgba(39, 39, 42),          rgba(9, 9, 11),
    rgba(24, 24, 27),           rgba(9, 9, 11),             rgba(39, 39, 42),          rgba(63, 63, 70),
    rgba(250, 250, 250),        rgba(161, 161, 170),        rgba(113, 113, 122),       rgba(250, 250, 250),
    rgba(228, 228, 231),        rgba(212, 212, 216),        rgba(250, 250, 250, 0.10f), rgba(9, 9, 11),
    rgba(250, 250, 250),        rgba(250, 250, 250, 0.10f), rgba(252, 165, 165),       rgba(240, 68, 56),
    rgba(240, 68, 56, 0.16f),   rgba(245, 165, 36),         rgba(245, 165, 36, 0.14f), rgba(0, 0, 0, 0.45f),
};

static const Palette kLight = {
    rgba(9, 9, 11, 0.28f),      rgba(255, 255, 255, 0.86f), rgba(244, 244, 245, 0.95f), rgba(9, 9, 11, 0.08f),
    rgba(255, 255, 255),        rgba(244, 244, 245),        rgba(255, 255, 255),        rgba(255, 255, 255),
    rgba(244, 244, 245),        rgba(255, 255, 255),        rgba(228, 228, 231),        rgba(212, 212, 216),
    rgba(9, 9, 11),             rgba(113, 113, 122),        rgba(161, 161, 170),        rgba(24, 24, 27),
    rgba(39, 39, 42),           rgba(63, 63, 70),           rgba(24, 24, 27, 0.08f),    rgba(250, 250, 250),
    rgba(9, 9, 11),             rgba(9, 9, 11, 0.06f),      rgba(220, 38, 38),          rgba(220, 38, 38),
    rgba(220, 38, 38, 0.10f),   rgba(217, 119, 6),          rgba(217, 119, 6, 0.12f),   rgba(0, 0, 0, 0.12f),
};

static Palette C = kDark;

// Theme: 0 dark (default), 1 light. Switching cross-fades the palette.
static int g_theme = 0;
static float g_theme_t = 0.0f;   // 0 dark .. 1 light, eased towards g_theme

static void sync_style_colors();

static void update_theme(float dt)
{
    float target = (float)g_theme;
    if (g_theme_t == target) return;
    float step = dt / 0.25f;
    g_theme_t = target > g_theme_t ? std::min(target, g_theme_t + step) : std::max(target, g_theme_t - step);
    float t = g_theme_t * g_theme_t * (3.0f - 2.0f * g_theme_t);   // smoothstep
    const ImU32* a = reinterpret_cast<const ImU32*>(&kDark);
    const ImU32* b = reinterpret_cast<const ImU32*>(&kLight);
    ImU32* out = reinterpret_cast<ImU32*>(&C);
    for (size_t i = 0; i < sizeof(Palette) / sizeof(ImU32); i++) {
        ImVec4 x = ImGui::ColorConvertU32ToFloat4(a[i]), y = ImGui::ColorConvertU32ToFloat4(b[i]);
        out[i] = ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                       x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
    }
    sync_style_colors();
}

// `instant`: at start-up, without the cross-fade.
static void set_theme(int theme, bool instant = false)
{
    g_theme = theme == 1 ? 1 : 0;
    if (!instant) return;
    g_theme_t = (float)g_theme;
    C = g_theme ? kLight : kDark;
    sync_style_colors();
}

struct Fonts {
    ImFont* body = nullptr;      // Geist 15
    ImFont* small = nullptr;     // Geist 13.5: descriptions, notes
    ImFont* label = nullptr;     // Geist Medium 13.5: tabs, segments, section headings, badges
    ImFont* strong = nullptr;    // Geist Medium 15: row titles, button labels
    ImFont* title = nullptr;     // Geist SemiBold 20
    ImFont* display = nullptr;   // Geist SemiBold 30
    ImFont* mono = nullptr;      // Geist Mono 13: only numbers that change while you watch
    bool icons = false;
};
static Fonts F;
static float S = 1.0f;   // content scale (monitor DPI)
static inline float px(float v) { return v * S; }

// Segoe Fluent Icons code points (Windows 11; checked against SegoeIcons.ttf).
namespace icon {
static const char* volume   = u8"\uE767";
static const char* mute     = u8"\uE74F";
static const char* camera   = u8"\uE722";
static const char* record   = u8"\uE7C8";
static const char* stop     = u8"\uE71A";
static const char* settings = u8"\uE713";
static const char* close    = u8"\uE711";
static const char* add      = u8"\uE710";
static const char* folder   = u8"\uE838";
static const char* refresh  = u8"\uE72C";
static const char* play     = u8"\uE768";
static const char* trash    = u8"\uE74D";
static const char* monitor  = u8"\uE7F4";
static const char* video    = u8"\uE714";
static const char* audio    = u8"\uE8D6";
static const char* photo    = u8"\uE91B";
static const char* stream   = u8"\uE93E";
static const char* help     = u8"\uE897";
static const char* hide     = u8"\uED1A";
static const char* full     = u8"\uE740";
static const char* window   = u8"\uE73F";
static const char* connect  = u8"\uE703";
static const char* keyboard = u8"\uE765";
static const char* info     = u8"\uE946";
static const char* warning  = u8"\uE7BA";
static const char* share    = u8"\uE72D";
static const char* sync     = u8"\uE895";
static const char* check    = u8"\uE73E";
static const char* capture  = u8"\uE8B9";
static const char* library  = u8"\uE8F1";
static const char* power    = u8"\uE7E8";
static const char* pause    = u8"\uE769";
static const char* prev     = u8"\uE76B";
static const char* next     = u8"\uE76C";
static const char* open_ext = u8"\uE8A7";
}

static const char* ic(const char* glyph) { return F.icons ? glyph : ""; }

static ImU32 ink(ImU32 c) { return ImGui::GetColorU32(c); }   // applies style alpha

// The Glint bolt (tools/make_brand.py has the same points), `height` tall with
// its top-left corner at `at`. Returns its width.
static float draw_bolt(ImDrawList* dl, ImVec2 at, float height, ImU32 col)
{
    static const ImVec2 kBolt[] = {{24.26f, 0.0f}, {56.08f, 0.0f}, {34.17f, 33.28f}, {62.74f, 33.28f},
                                   {7.15f, 100.0f}, {24.52f, 52.15f}, {0.0f, 52.15f}};
    float s = height / 100.0f;
    ImVec2 pts[7];
    for (int i = 0; i < 7; i++) pts[i] = ImVec2(at.x + kBolt[i].x * s, at.y + kBolt[i].y * s);
    dl->AddConcavePolyFilled(pts, 7, col);
    return 62.74f * s;
}

static ImU32 mix(ImU32 a, ImU32 b, float t)
{
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

// Per-item eased value kept in ImGui's state storage.
static float anim(ImGuiID id, float target, float speed = 14.0f)
{
    ImGuiStorage* st = ImGui::GetStateStorage();
    float v = st->GetFloat(id, target);
    v += (target - v) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
    st->SetFloat(id, v);
    return v;
}

static float ease_out(float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

// Moves `t` towards 1 (open) or 0 (closed) over about `seconds` (overlays).
static void step_towards(float& t, bool open, float seconds)
{
    float step = ImGui::GetIO().DeltaTime / seconds;
    t = open ? std::min(1.0f, t + step) : std::max(0.0f, t - step);
}

// Eased wheel scrolling for the current child window. Begin the child with
// NoScrollbar | NoScrollWithMouse: the wheel moves a target and the view glides
// to it. `reset` jumps back to the top (a new page).
static void smooth_scroll(bool reset = false)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID id = ImGui::GetID("##smooth_scroll");
    float target = reset ? 0.0f : st->GetFloat(id, ImGui::GetScrollY());
    if (!reset && io.MouseWheel != 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        target -= io.MouseWheel * px(110);
    target = std::clamp(target, 0.0f, ImGui::GetScrollMaxY());
    float y = reset ? 0.0f : ImGui::GetScrollY();
    y += (target - y) * std::min(1.0f, io.DeltaTime * 14.0f);
    if (std::fabs(target - y) < 0.5f) y = target;
    ImGui::SetScrollY(y);
    st->SetFloat(id, target);
}

static bool file_exists(const char* path)
{
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

#include "fonts.inc"   // Geist, embedded (tools/embed_fonts.js)

static void load_fonts(float scale)
{
    ImGuiIO& io = ImGui::GetIO();
    const char* icons = file_exists("C:\\Windows\\Fonts\\SegoeIcons.ttf")
                            ? "C:\\Windows\\Fonts\\SegoeIcons.ttf"
                            : (file_exists("C:\\Windows\\Fonts\\segmdl2.ttf") ? "C:\\Windows\\Fonts\\segmdl2.ttf" : nullptr);
    static const ImWchar icon_ranges[] = {0xE700, 0xF8FF, 0};
    F.icons = icons != nullptr;

    auto add = [&](const unsigned char* data, size_t bytes, float size) -> ImFont* {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;   // static data in the exe
        cfg.OversampleH = 3;
        cfg.OversampleV = 2;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), (int)bytes, size * scale, &cfg);
        if (icons) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.PixelSnapH = true;
            merge.GlyphMinAdvanceX = size * scale;
            merge.GlyphOffset = ImVec2(0, size * scale * 0.12f);
            io.Fonts->AddFontFromFileTTF(icons, size * scale * 0.92f, &merge, icon_ranges);
        }
        return font;
    };
    F.body = add(kGeistRegular, sizeof(kGeistRegular), 15.0f);   // first font = default
    F.small = add(kGeistRegular, sizeof(kGeistRegular), 13.5f);
    F.label = add(kGeistMedium, sizeof(kGeistMedium), 13.5f);
    F.strong = add(kGeistMedium, sizeof(kGeistMedium), 15.0f);
    F.title = add(kGeistSemiBold, sizeof(kGeistSemiBold), 20.0f);
    F.display = add(kGeistSemiBold, sizeof(kGeistSemiBold), 30.0f);
    F.mono = add(kGeistMonoRegular, sizeof(kGeistMonoRegular), 13.0f);
}

// ImGui's own widgets (text fields, scrollbars, tooltips) follow the palette.
static void sync_style_colors()
{
    ImVec4* c = ImGui::GetStyle().Colors;
    auto v = [](ImU32 col) { return ImGui::ColorConvertU32ToFloat4(col); };
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v(C.nav);
    c[ImGuiCol_Border] = v(C.border);
    c[ImGuiCol_FrameBg] = v(C.input);
    c[ImGuiCol_FrameBgHovered] = v(C.input);
    c[ImGuiCol_FrameBgActive] = v(C.input);
    c[ImGuiCol_Text] = v(C.text);
    c[ImGuiCol_TextDisabled] = v(C.faint);
    c[ImGuiCol_TextSelectedBg] = v(C.accent_soft);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v(C.border);
    c[ImGuiCol_ScrollbarGrabHovered] = v(C.border_hi);
    c[ImGuiCol_ScrollbarGrabActive] = v(C.border_hi);
    c[ImGuiCol_CheckMark] = v(C.accent);
    c[ImGuiCol_SliderGrab] = v(C.accent);
    c[ImGuiCol_SliderGrabActive] = v(C.accent_hover);
    c[ImGuiCol_Button] = v(C.card);
    c[ImGuiCol_ButtonHovered] = v(C.card_hover);
    c[ImGuiCol_ButtonActive] = v(C.border);
    c[ImGuiCol_Header] = v(C.accent_soft);
    c[ImGuiCol_HeaderHovered] = v(C.card_hover);
    c[ImGuiCol_HeaderActive] = v(C.accent_soft);
    c[ImGuiCol_Separator] = v(C.border);
    c[ImGuiCol_NavCursor] = v(C.text);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0);   // the sheet draws its own animated scrim
}

static void apply_style(float scale)
{
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 12.0f;
    s.ChildRounding = 0.0f;    // the settings sheet and gallery are square panels
    s.FrameRounding = 8.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 8.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 10);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.ScrollbarSize = 10.0f;
    s.WindowMinSize = ImVec2(1, 1);

    sync_style_colors();
    s.ScaleAllSizes(scale);
}

// --- Widgets ---------------------------------------------------------------

// Primary: the one main action. Secondary: outlined. Ghost: text only.
// Danger: red-tinted (stop recording, end live). DangerSolid: confirms a
// destructive action. Glass: round chrome over the picture; GlassOn: selected.
enum class Kind { Primary, Secondary, Ghost, Danger, DangerSolid, Glass, GlassOn };

static std::string compose(const char* glyph, const char* text)
{
    std::string label;
    if (glyph && *ic(glyph)) label = glyph;
    if (text && *text) {
        if (!label.empty()) label += "  ";
        label += text;
    }
    return label;
}

static float button_width(const char* glyph, const char* text, float pad_x = 14.0f)
{
    std::string label = compose(glyph, text);
    ImGui::PushFont(F.label);   // button labels are medium weight
    float w = ImGui::CalcTextSize(label.c_str()).x + px(pad_x) * 2.0f;
    ImGui::PopFont();
    return w;
}

static bool button(const char* id, const char* glyph, const char* text, Kind kind,
                   ImVec2 size = ImVec2(0, 0), bool enabled = true)
{
    std::string label = compose(glyph, text);
    ImGui::PushFont(F.label);
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    if (size.x <= 0) size.x = ts.x + px(14) * 2.0f;
    if (size.y <= 0) size.y = px(36);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, size) && enabled;
    bool hovered = enabled && ImGui::IsItemHovered();
    bool held = enabled && ImGui::IsItemActive();
    ImGuiID iid = ImGui::GetItemID();
    float h = anim(iid, hovered ? 1.0f : 0.0f);
    float press = anim(iid + 1, held ? 1.0f : 0.0f, 30.0f);

    ImU32 clear = C.accent_soft & ~IM_COL32_A_MASK;   // transparent, same hue: fades cleanly
    ImU32 fill = 0, line = 0, fg = C.text;
    switch (kind) {
    case Kind::Primary:
        fill = mix(mix(C.accent, C.accent_hover, h), C.accent_press, press);
        fg = C.on_accent;
        break;
    case Kind::Secondary:
        fill = mix(clear, C.card_hover, h);
        line = mix(C.border, C.border_hi, h);
        break;
    case Kind::Ghost:
        fill = mix(clear, C.accent_soft, h);
        fg = mix(C.muted, C.text, h);
        break;
    case Kind::Danger:
        fill = mix(C.danger_soft, C.danger_fill & 0x40FFFFFF, h);
        fg = C.danger;
        break;
    case Kind::DangerSolid:
        fill = mix(C.danger_fill, mix(C.danger_fill, rgba(0, 0, 0), 0.15f), h);
        fg = rgba(255, 255, 255);
        break;
    case Kind::Glass:
        fill = mix(C.glass, C.glass_hover, h);
        line = C.glass_border;
        break;
    case Kind::GlassOn:
        fill = mix(C.accent, C.accent_hover, h);
        fg = C.on_accent;
        break;
    }
    float alpha = enabled ? 1.0f : 0.4f;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * alpha);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = (kind == Kind::Glass || kind == Kind::GlassOn) ? size.y * 0.5f : px(8);
    // Pressing sinks the button by a pixel all round.
    ImVec2 inset(press * px(1), press * px(1));
    if (fill & IM_COL32_A_MASK) dl->AddRectFilled(p + inset, p + size - inset, ink(fill), r);
    if (line) dl->AddRect(p + inset, p + size - inset, ink(line), r, 0, 1.0f);
    ImVec2 tp(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f);
    dl->AddText(tp, ink(fg), label.c_str());
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

static void tip(const char* text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_NoSharedDelay)) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(10), px(6)));
        ImGui::SetTooltip("%s", text);
        ImGui::PopStyleVar();
    }
}

static bool toggle(const char* id, bool* value, bool enabled = true)
{
    ImVec2 size(px(44), px(24));
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, size) && enabled;
    if (pressed) *value = !*value;
    bool hovered = enabled && ImGui::IsItemHovered();
    bool held = enabled && ImGui::IsItemActive();
    float t = anim(ImGui::GetItemID(), *value ? 1.0f : 0.0f, 16.0f);
    float press = anim(ImGui::GetItemID() + 1, held ? 1.0f : 0.0f, 24.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (enabled ? 1.0f : 0.4f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 off = hovered ? mix(C.border_hi, C.faint, 0.35f) : C.border_hi;
    dl->AddRectFilled(p, p + size, ink(mix(off, C.accent, t)), size.y * 0.5f);
    // The knob stretches a little while held, like a finger pressing it.
    float r = size.y * 0.5f - px(3);
    float stretch = press * px(4);
    float travel = size.x - 2.0f * (px(3) + r) - stretch;
    float x0 = p.x + px(3) + r + t * travel;
    float cy = p.y + size.y * 0.5f;
    dl->AddRectFilled(ImVec2(x0 - r, cy - r), ImVec2(x0 + r + stretch, cy + r), ink(mix(C.muted, C.on_accent, t)), r);
    ImGui::PopStyleVar();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

// Segments use the small face (13 px), like the mockup.
static float segmented_width(const char* const* items, int count, float min_item = 0.0f)
{
    ImGui::PushFont(F.label);
    float total = px(4);
    for (int i = 0; i < count; i++)
        total += std::max(ImGui::CalcTextSize(items[i]).x + px(28), min_item);
    ImGui::PopFont();
    return total + px(4);
}

static bool segmented(const char* id, int* current, const char* const* items, int count,
                      float min_item = 0.0f, bool enabled = true)
{
    ImGui::PushFont(F.label);
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = px(34);
    std::vector<float> widths(count);
    float total = px(4);
    for (int i = 0; i < count; i++) {
        widths[i] = std::max(ImGui::CalcTextSize(items[i]).x + px(28), min_item);
        total += widths[i];
    }
    total += px(4);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (enabled ? 1.0f : 0.4f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + ImVec2(total, h), ink(C.nav), px(8));

    // The selection thumb slides and resizes to the new segment.
    float target_x = px(4);
    for (int i = 0; i < *current && i < count; i++) target_x += widths[i];
    float target_w = (*current >= 0 && *current < count) ? widths[*current] : 0.0f;
    float sel_x = anim(ImGui::GetID("##pill"), target_x, 18.0f);
    float sel_w = anim(ImGui::GetID("##pillw"), target_w, 18.0f);
    if (sel_w > 1.0f)
        dl->AddRectFilled(p + ImVec2(sel_x, px(3)), p + ImVec2(sel_x + sel_w, h - px(3)), ink(C.accent), px(6));

    bool changed = false;
    float x = px(4);
    for (int i = 0; i < count; i++) {
        ImGui::SetCursorScreenPos(p + ImVec2(x, 0));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(widths[i], h)) && enabled && *current != i) {
            *current = i;
            changed = true;
        }
        bool hovered = enabled && ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        ImVec2 ts = ImGui::CalcTextSize(items[i]);
        // Text colour follows the thumb as it slides over each segment.
        float cover = std::clamp(1.0f - std::fabs(sel_x - x) / std::max(1.0f, widths[i]), 0.0f, 1.0f);
        ImU32 rest = hovered ? C.text : C.muted;
        ImU32 fg = mix(rest, C.on_accent, cover);
        dl->AddText(p + ImVec2(x + (widths[i] - ts.x) * 0.5f, (h - ts.y) * 0.5f), ink(fg), items[i]);
        x += widths[i];
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(total, h));
    ImGui::PopID();
    ImGui::PopFont();
    return changed;
}

// Horizontal slider with a value label on the right.
static bool slider(const char* id, float* value, float vmin, float vmax, const char* fmt,
                   float display_scale, float width, bool enabled = true)
{
    float label_w = px(46);   // fits "400%" in Geist Mono 13
    float track_w = std::max(px(60), width - label_w);
    float h = px(26);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(track_w, h));
    bool hovered = enabled && ImGui::IsItemHovered();
    bool active = enabled && ImGui::IsItemActive();
    bool changed = false;
    float knob_r = px(8);
    float x0 = p.x + knob_r, x1 = p.x + track_w - knob_r;
    if (active) {
        float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
        float nv = vmin + t * (vmax - vmin);
        if (nv != *value) { *value = nv; changed = true; }
    } else if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        float step = (vmax - vmin) / 50.0f;
        float nv = std::clamp(*value + ImGui::GetIO().MouseWheel * step, vmin, vmax);
        if (nv != *value) { *value = nv; changed = true; }
    }
    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float t = (vmax > vmin) ? std::clamp((*value - vmin) / (vmax - vmin), 0.0f, 1.0f) : 0.0f;
    float kx = x0 + t * (x1 - x0);
    float cy = p.y + h * 0.5f;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (enabled ? 1.0f : 0.4f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float th = px(4);
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), ink(C.border_hi), th);
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(kx, cy + th * 0.5f), ink(C.accent), th);
    // The knob grows on hover and gets a soft halo while dragged.
    float grow = anim(ImGui::GetItemID(), (hovered || active) ? 1.0f : 0.0f);
    float halo = anim(ImGui::GetItemID() + 1, active ? 1.0f : 0.0f, 18.0f);
    if (halo > 0.01f) dl->AddCircleFilled(ImVec2(kx, cy), knob_r + halo * px(6), ink(C.accent_soft), 24);
    dl->AddCircleFilled(ImVec2(kx, cy), knob_r - px(1) + grow * px(1.5f), ink(C.accent), 24);
    char text[48];
    std::snprintf(text, sizeof(text), fmt, *value * display_scale);
    ImGui::PushFont(F.mono);
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(p.x + track_w + label_w - ts.x, cy - ts.y * 0.5f), ink(C.muted), text);
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, h));
    return changed;
}

static float badge_width(const char* text)
{
    ImGui::PushFont(F.label);
    float w = ImGui::CalcTextSize(text).x + px(16);
    ImGui::PopFont();
    return w;
}

static ImVec2 badge(const char* text, ImU32 fg, ImU32 bg, bool advance = true)
{
    ImGui::PushFont(F.label);
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 size(ts.x + px(16), ts.y + px(8));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + size, ink(bg), px(6));
    dl->AddText(p + ImVec2(px(8), px(4)), ink(fg), text);
    ImGui::PopFont();
    if (advance) ImGui::Dummy(size);
    return size;
}

static void text_colored(ImU32 col, const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::TextUnformatted(buffer);
    ImGui::PopStyleColor();
}

static bool input(const char* id, char* buffer, size_t size, float width, bool enabled = true,
                  ImGuiInputTextFlags flags = 0)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(12), px(9)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, px(8));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(C.border));
    ImGui::SetNextItemWidth(width);
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::InputText(id, buffer, size, flags);
    ImGui::EndDisabled();
    // Focus ring that fades in, like the web version's focus-visible outline.
    float focus = anim(ImGui::GetItemID() + 7, ImGui::IsItemActive() ? 1.0f : 0.0f, 16.0f);
    if (focus > 0.01f) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImU32 ring = ImGui::GetColorU32(C.text, 0.35f * focus);
        ImGui::GetWindowDrawList()->AddRect(a - ImVec2(px(2), px(2)), b + ImVec2(px(2), px(2)), ring, px(10), 0, px(2));
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return r;
}

// --- Cards and rows --------------------------------------------------------

struct CardCtx {
    ImVec2 start;
    float width = 0;
    float inner_x = 0;
    float inner_w = 0;
    int rows = 0;
    // current row
    float row_left_bottom = 0;
};
static CardCtx g_card;

static void section(const char* text)
{
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::PushFont(F.label);
    text_colored(C.muted, "%s", text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, px(0)));
}

static void card_begin()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    g_card = CardCtx{};
    g_card.start = ImGui::GetCursorScreenPos();
    g_card.width = ImGui::GetContentRegionAvail().x;
    g_card.inner_x = g_card.start.x + px(16);
    g_card.inner_w = g_card.width - px(32);
    ImGui::SetCursorScreenPos(ImVec2(g_card.inner_x, g_card.start.y + px(4)));
    ImGui::BeginGroup();
}

static void card_end()
{
    ImGui::EndGroup();
    float bottom = ImGui::GetItemRectMax().y + px(4);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSetCurrent(0);
    ImVec2 a = g_card.start, b(g_card.start.x + g_card.width, bottom);
    dl->AddRectFilled(a, b, ink(C.card), px(12));
    dl->AddRect(a, b, ink(C.border), px(12));
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(g_card.start.x, bottom));
    ImGui::Dummy(ImVec2(g_card.width, px(16)));
}

// A settings row: title + description on the left, a control of `control_w`
// right-aligned. A control wider than ~42 % of the card (a text field with its
// buttons) goes on its own line under the text instead, so the description is
// never squeezed into a sliver. Call row_end() after drawing the control.
static void row_begin(const char* title, const char* desc, float control_w, float control_h = 34.0f)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float half_gap = ImGui::GetStyle().ItemSpacing.y * 0.5f;
    if (g_card.rows++ > 0)
        dl->AddLine(ImVec2(g_card.inner_x, p.y - half_gap), ImVec2(g_card.inner_x + g_card.inner_w, p.y - half_gap),
                    ink(C.border));
    float top = p.y + px(14);
    float ch_px = px(control_h);
    bool stacked = control_w > g_card.inner_w * 0.42f;
    // A title without a description is centred against its control.
    float title_top = (!stacked && (!desc || !*desc)) ? top + std::max(0.0f, (ch_px - ImGui::GetTextLineHeight()) * 0.5f) : top;
    ImGui::SetCursorScreenPos(ImVec2(g_card.inner_x, title_top));
    float text_w = stacked ? g_card.inner_w : std::max(px(120), g_card.inner_w - control_w - px(24));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(g_card.inner_x + text_w - ImGui::GetWindowPos().x);   // wrap position is window-local
    ImGui::PushFont(F.strong);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (desc && *desc) {
        ImGui::PushFont(F.small);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C.muted));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(6));
        ImGui::TextWrapped("%s", desc);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImVec2 lmin = ImGui::GetItemRectMin(), lmax = ImGui::GetItemRectMax();
    g_card.row_left_bottom = lmax.y;
    if (stacked) {
        ImGui::SetCursorScreenPos(ImVec2(g_card.inner_x, lmax.y + px(12)));
    } else {
        float y = std::max(top, (lmin.y + lmax.y) * 0.5f - ch_px * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(g_card.inner_x + g_card.inner_w - control_w, y));
    }
    ImGui::BeginGroup();
}

static void row_end()
{
    ImGui::EndGroup();
    float bottom = std::max(ImGui::GetItemRectMax().y, g_card.row_left_bottom) + px(14);
    ImGui::SetCursorScreenPos(ImVec2(g_card.inner_x, bottom));
    ImGui::Dummy(ImVec2(g_card.inner_w, 0));
}

// Free text line inside a card (no control).
static void card_note(ImU32 col, const char* text)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float half_gap = ImGui::GetStyle().ItemSpacing.y * 0.5f;
    if (g_card.rows++ > 0)
        ImGui::GetWindowDrawList()->AddLine(ImVec2(g_card.inner_x, p.y - half_gap),
                                            ImVec2(g_card.inner_x + g_card.inner_w, p.y - half_gap), ink(C.border));
    ImGui::SetCursorScreenPos(ImVec2(g_card.inner_x, p.y + px(12)));
    ImGui::PushFont(F.small);
    ImGui::PushTextWrapPos(g_card.inner_x + g_card.inner_w - ImGui::GetWindowPos().x);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(g_card.inner_x, ImGui::GetItemRectMax().y + px(12)));
    ImGui::Dummy(ImVec2(g_card.inner_w, 0));
}

static void page_header(const char* title, const char* subtitle)
{
    ImGui::PushFont(F.title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::PushFont(F.small);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C.muted));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(6));
    ImGui::TextWrapped("%s", subtitle);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, px(10)));
}

// --- Toasts ----------------------------------------------------------------

struct Toast { std::string text; int kind; double born; };   // kind: 0 info, 1 ok, 2 error
static std::vector<Toast> g_toasts;

static void toast(const std::string& text, int kind = 0)
{
    if (text.empty()) return;
    if (!g_toasts.empty() && g_toasts.back().text == text) {
        g_toasts.back().born = ImGui::GetTime();
        return;
    }
    g_toasts.push_back({text, kind, ImGui::GetTime()});
    if (g_toasts.size() > 4) g_toasts.erase(g_toasts.begin());
}

static void draw_toasts(float bottom_y)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    double now = ImGui::GetTime();
    float y = bottom_y;
    ImGui::PushFont(F.body);
    for (int i = (int)g_toasts.size() - 1; i >= 0; i--) {
        const Toast& t = g_toasts[i];
        double age = now - t.born;
        double life = t.kind == 2 ? 7.0 : 4.0;
        if (age > life) continue;
        float a = (float)std::min({1.0, age / 0.22, (life - age) / 0.4});
        float rise = 1.0f - (1.0f - a) * (1.0f - a);   // ease-out: rises into place, sinks away
        const char* glyph = t.kind == 2 ? icon::warning : (t.kind == 1 ? icon::check : icon::info);
        ImU32 accent = t.kind == 2 ? C.danger : (t.kind == 1 ? C.text : C.muted);
        float wrap = px(380);
        ImVec2 ts = ImGui::CalcTextSize(t.text.c_str(), nullptr, false, wrap);
        ImVec2 size(ts.x + px(56), ts.y + px(24));
        ImVec2 p(vp->Pos.x + vp->Size.x - size.x - px(24), y - size.y + (1.0f - rise) * px(12));
        auto with_alpha = [&](ImU32 c) {
            ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
            v.w *= a;
            return ImGui::ColorConvertFloat4ToU32(v);
        };
        dl->AddRectFilled(p + ImVec2(0, px(6)), p + size + ImVec2(0, px(8)), with_alpha(C.shadow & 0x50FFFFFF), px(14));
        dl->AddRectFilled(p, p + size, with_alpha(C.nav), px(12));
        dl->AddRect(p, p + size, with_alpha(C.border), px(12));
        if (F.icons) dl->AddText(p + ImVec2(px(16), px(12)), with_alpha(accent), glyph);
        dl->AddText(F.body, F.body->FontSize, p + ImVec2(px(42), px(12)), with_alpha(C.text),
                    t.text.c_str(), nullptr, wrap);
        y = p.y - px(10);
    }
    ImGui::PopFont();
    while (!g_toasts.empty() && now - g_toasts.front().born > 8.0) g_toasts.erase(g_toasts.begin());
}

} // namespace ui

// ---------------------------------------------------------------------------
// Local recording / screenshots / gallery state
// ---------------------------------------------------------------------------

struct ExportResult { bool ok = false; std::string path; std::string error; };

struct GalleryUi {
    std::vector<glint::GalleryItem> items;
    bool loaded = false;
    int filter = 0;                  // 0 all, 1 recordings, 2 screenshots
    std::string selected;
    std::string pending_delete;
    double pending_delete_time = 0;
    std::future<ExportResult> export_job;
    bool exporting = false;
};

static GalleryUi g_gallery;

// Gallery thumbnails: decoded and shrunk to 320 px on a worker thread, then
// uploaded between frames (GPU work never runs inside the ImGui frame).
struct Thumb { bool ok = false; std::vector<uint8_t> rgba; int width = 0; int height = 0; };
struct ThumbCache {
    std::map<std::string, VideoTexture> ready;
    std::map<std::string, std::future<Thumb>> jobs;
    std::vector<std::string> failed;
    bool prune = false;              // drop thumbnails of files no longer listed
};
static ThumbCache g_thumbs;

static Thumb make_thumb(const std::string& path)
{
    Thumb t;
    std::vector<uint8_t> full;
    int w = 0, h = 0;
    std::string error;
    if (!glint::load_image_rgba(path, full, w, h, error) || w <= 0 || h <= 0) return t;
    t.width = std::min(320, w);
    t.height = std::max(1, h * t.width / w);
    t.rgba.resize((size_t)t.width * t.height * 4);
    // Box filter: each thumbnail pixel averages the source pixels it covers.
    for (int y = 0; y < t.height; y++) {
        int y0 = y * h / t.height, y1 = std::max(y0 + 1, (y + 1) * h / t.height);
        for (int x = 0; x < t.width; x++) {
            int x0 = x * w / t.width, x1 = std::max(x0 + 1, (x + 1) * w / t.width);
            uint32_t sum[4] = {0, 0, 0, 0}, n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++, n++) {
                    const uint8_t* s = &full[((size_t)sy * w + sx) * 4];
                    for (int c = 0; c < 4; c++) sum[c] += s[c];
                }
            uint8_t* d = &t.rgba[((size_t)y * t.width + x) * 4];
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)(sum[c] / n);
        }
    }
    t.ok = true;
    return t;
}

static void gallery_status(const std::string& text, bool error)
{
    ui::toast(text, error ? 2 : 1);
}

static void gallery_refresh(glint::App& app)
{
    g_gallery.items = glint::list_gallery(app.media_dir());
    g_gallery.loaded = true;
    g_thumbs.failed.clear();   // a file that was still being written may decode now
    g_thumbs.prune = true;
    // A selected file that is gone (deleted outside Glint) is no longer selected.
    if (!g_gallery.selected.empty() &&
        std::none_of(g_gallery.items.begin(), g_gallery.items.end(),
                     [](const glint::GalleryItem& it) { return it.path == g_gallery.selected; }))
        g_gallery.selected.clear();
}

// The media folder is watched while the gallery is open, so files added,
// deleted or renamed outside Glint (Explorer) show up too. A recording being
// written changes it constantly: re-read at most once a second.
static void gallery_follow_folder(glint::App& app)
{
    static glint::FolderWatch watch;
    static double dirty_since = -1.0, last_refresh = -10.0;
    watch.watch(app.media_dir());
    double now = ImGui::GetTime();
    if (watch.changed() && dirty_since < 0) dirty_since = now;
    if (dirty_since >= 0 && now - last_refresh >= 1.0) {
        g_gallery.loaded = false;
        dirty_since = -1.0;
        last_refresh = now;
    }
}

static std::string base_name(const std::string& path)
{
    size_t at = path.find_last_of("/\\");
    return at == std::string::npos ? path : path.substr(at + 1);
}

static std::string format_clock(double seconds)
{
    int total = (int)(seconds + 0.5);
    char buffer[32];
    if (total >= 3600) std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    else std::snprintf(buffer, sizeof(buffer), "%d:%02d", total / 60, total % 60);
    return buffer;
}

static void do_screenshot(glint::App& app)
{
    std::string path, error;
    if (app.take_screenshot(path, error)) {
        glint::logf("screenshot saved: %s", path.c_str());
        gallery_status("Saved screenshot " + base_name(path), false);
        g_gallery.loaded = false;
    } else {
        gallery_status(error.empty() ? "Screenshot could not be saved." : error, true);
    }
}

static bool recorder_running(const glint::RecorderInfo& info)
{
    return info.state == glint::RecorderInfo::State::Armed ||
           info.state == glint::RecorderInfo::State::Recording;
}

static void toggle_local_recording(glint::App& app)
{
    if (recorder_running(app.recording_info())) {
        app.stop_local_recording();
        return;
    }
    std::string error;
    if (!app.start_local_recording(error)) {
        glint::logf("recording: start refused: %s", error.c_str());
        gallery_status(error, true);
    } else {
        glint::logf("recording: started in %s", app.media_dir().c_str());
        ui::toast("Recording on this PC", 0);
    }
}

// Runs between frames (it submits GPU work), never inside the ImGui frame.
static void process_gallery_textures()
{
    for (auto it = g_thumbs.jobs.begin(); it != g_thumbs.jobs.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        Thumb t = it->second.get();
        if (t.ok) upload_frame(g_thumbs.ready[it->first], t.rgba, t.width, t.height);
        else g_thumbs.failed.push_back(it->first);
        it = g_thumbs.jobs.erase(it);
    }
    if (g_thumbs.prune) {
        g_thumbs.prune = false;
        for (auto it = g_thumbs.ready.begin(); it != g_thumbs.ready.end();) {
            bool listed = std::any_of(g_gallery.items.begin(), g_gallery.items.end(),
                                      [&](const glint::GalleryItem& i) { return i.path == it->first; });
            if (listed) { ++it; continue; }
            destroy_texture(it->second);   // waits for the device, safe to release
            it = g_thumbs.ready.erase(it);
        }
    }
}

// Starts at most three thumbnail decodes at a time for files without one.
static void request_thumb(const std::string& path)
{
    if (g_thumbs.ready.count(path) || g_thumbs.jobs.count(path) || g_thumbs.jobs.size() >= 3) return;
    if (std::find(g_thumbs.failed.begin(), g_thumbs.failed.end(), path) != g_thumbs.failed.end()) return;
    g_thumbs.jobs[path] = std::async(std::launch::async, make_thumb, path);
}

static void destroy_thumbs()
{
    g_thumbs.jobs.clear();   // waits for running decodes
    for (auto& t : g_thumbs.ready) destroy_texture(t.second);
    g_thumbs.ready.clear();
}

// ---------------------------------------------------------------------------
// Built-in viewer: screenshots (zoom and pan) and recordings (play, seek,
// volume) open over the gallery, on black in both themes.
// ---------------------------------------------------------------------------

struct ViewerUi {
    bool open = false;
    float t = 0.0f;                        // fade in and out
    std::vector<std::string> list;         // Previous / Next walk the gallery's current filter
    int index = -1;
    std::string path;
    bool video = false;
    std::future<Thumb> image_job;          // the full-size picture, decoded on a worker
    bool loading = false;
    std::string error;
    float zoom = 1.0f;                     // 1 = fit the window
    ImVec2 pan = ImVec2(0, 0);
    glint::MediaPlayer player;
    bool live_sound_paused = false;        // live sound is muted while a recording plays
    double last_activity = 0.0;
    bool scrubbing = false;
    double scrub_to = 0.0;
    double last_scrub = 0.0;
    bool clear_texture = false;
};
static ViewerUi g_viewer;
static VideoTexture g_viewer_tex;

static Thumb load_picture(const std::string& path)
{
    Thumb t;
    std::string error;
    t.ok = glint::load_image_rgba(path, t.rgba, t.width, t.height, error);
    return t;
}

// Runs between frames, like the gallery thumbnails.
static void process_viewer_texture()
{
    if (g_viewer.clear_texture) {
        destroy_texture(g_viewer_tex);
        g_viewer.clear_texture = false;
    }
    if (!g_viewer.open) return;
    if (g_viewer.loading && g_viewer.image_job.valid() &&
        g_viewer.image_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Thumb t = g_viewer.image_job.get();
        g_viewer.loading = false;
        if (t.ok) upload_frame(g_viewer_tex, t.rgba, t.width, t.height);
        else g_viewer.error = "Could not open the picture.";
    }
    if (g_viewer.video) {
        glint::Picture pic;
        if (g_viewer.player.take_frame(pic) && pic.width > 0) upload_frame(g_viewer_tex, pic.data, pic.width, pic.height);
    }
}

static void viewer_restore_sound(glint::App& app)
{
    if (!g_viewer.live_sound_paused) return;
    app.audio().set_enabled(true);
    g_viewer.live_sound_paused = false;
}

static void viewer_show(glint::App& app, int index)
{
    if (index < 0 || index >= (int)g_viewer.list.size()) return;
    g_viewer.player.close();
    if (g_viewer.image_job.valid()) g_viewer.image_job.wait();
    g_viewer.index = index;
    g_viewer.path = g_viewer.list[index];
    std::string ext = g_viewer.path.size() > 4 ? g_viewer.path.substr(g_viewer.path.size() - 4) : "";
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    g_viewer.video = ext != ".png";
    g_viewer.error.clear();
    g_viewer.zoom = 1.0f;
    g_viewer.pan = ImVec2(0, 0);
    g_viewer.scrubbing = false;
    g_viewer.clear_texture = true;
    g_viewer.last_activity = ImGui::GetTime();
    if (g_viewer.video) {
        std::string error;
        if (!g_viewer.player.open(g_viewer.path, error)) {
            g_viewer.error = error;
        } else {
            if (g_viewer.player.has_audio() && app.audio().enabled() && !g_viewer.live_sound_paused) {
                app.audio().set_enabled(false);
                g_viewer.live_sound_paused = true;
            }
            g_viewer.player.play();
        }
        g_viewer.loading = false;
    } else {
        viewer_restore_sound(app);
        g_viewer.image_job = std::async(std::launch::async, load_picture, g_viewer.path);
        g_viewer.loading = true;
    }
}

static void open_viewer(glint::App& app, const std::vector<std::string>& list, const std::string& path)
{
    g_viewer.list = list;
    auto it = std::find(list.begin(), list.end(), path);
    if (it == list.end()) return;
    g_viewer.open = true;
    viewer_show(app, (int)(it - list.begin()));
}

static void close_viewer(glint::App& app)
{
    g_viewer.open = false;
    g_viewer.player.close();
    viewer_restore_sound(app);
}

// Keys while the viewer is open. Space plays or pauses; Left/Right seek 5 s in
// a recording and step through screenshots; Page Up/Down step in both;
// Up/Down change the volume.
static void viewer_keys(glint::App& app)
{
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    glint::MediaPlayer& p = g_viewer.player;
    if (g_viewer.video) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_K, false)) {
            if (p.playing()) p.pause();
            else p.play();
        }
        if (pressed(ImGuiKey_LeftArrow)) p.seek(p.position() - 5.0);
        if (pressed(ImGuiKey_RightArrow)) p.seek(p.position() + 5.0);
        if (pressed(ImGuiKey_UpArrow)) p.set_volume(p.volume() + 0.1f);
        if (pressed(ImGuiKey_DownArrow)) p.set_volume(p.volume() - 0.1f);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) p.seek(0.0);
    } else {
        if (pressed(ImGuiKey_LeftArrow)) viewer_show(app, g_viewer.index - 1);
        if (pressed(ImGuiKey_RightArrow)) viewer_show(app, g_viewer.index + 1);
    }
    if (pressed(ImGuiKey_PageUp)) viewer_show(app, g_viewer.index - 1);
    if (pressed(ImGuiKey_PageDown)) viewer_show(app, g_viewer.index + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) || pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_RightArrow))
        g_viewer.last_activity = ImGui::GetTime();
}

static void draw_viewer(glint::App& app)
{
    using namespace ui;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiIO& io = ImGui::GetIO();
    step_towards(g_viewer.t, g_viewer.open, g_viewer.open ? 0.2f : 0.15f);
    bool popup_open = ImGui::IsPopupOpen("##viewer");
    if (g_viewer.t <= 0.0f && !popup_open) return;
    if (!popup_open) ImGui::OpenPopup("##viewer");

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    bool open = ImGui::BeginPopupModal("##viewer", nullptr,
                                       ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!open) {
        close_viewer(app);
        return;
    }
    if (g_viewer.t <= 0.0f) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    double now = ImGui::GetTime();
    float e = ease_out(g_viewer.t);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(vp->Pos, vp->Pos + vp->Size, ImGui::GetColorU32(IM_COL32(0, 0, 0, 255)));

    glint::MediaPlayer& player = g_viewer.player;
    bool activity = io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 || ImGui::IsAnyMouseDown();
    if (activity) g_viewer.last_activity = now;
    bool paused = g_viewer.video && !player.playing();
    bool chrome = now - g_viewer.last_activity < 2.5 || paused || !g_viewer.error.empty() || g_viewer.scrubbing;
    float ca = anim(ImGui::GetID("##vchrome"), chrome ? 1.0f : 0.0f, 10.0f);
    if (ca < 0.02f) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    // The picture: fit to the window; screenshots zoom around the pointer.
    ImVec2 center = vp->Pos + vp->Size * 0.5f;
    bool have = g_viewer_tex.descriptor && g_viewer_tex.width > 0 && !g_viewer.clear_texture;
    float fit = 1.0f;
    if (have) {
        float tw = (float)g_viewer_tex.width, th = (float)g_viewer_tex.height;
        fit = std::min(vp->Size.x / tw, vp->Size.y / th);
        float s = fit * (g_viewer.video ? 1.0f : g_viewer.zoom);
        ImVec2 size(tw * s, th * s);
        // Keep a zoomed picture covering the window (no empty margins to pan into).
        ImVec2 slack(std::max(0.0f, (size.x - vp->Size.x) * 0.5f), std::max(0.0f, (size.y - vp->Size.y) * 0.5f));
        g_viewer.pan.x = std::clamp(g_viewer.pan.x, -slack.x, slack.x);
        g_viewer.pan.y = std::clamp(g_viewer.pan.y, -slack.y, slack.y);
        ImVec2 c = center + g_viewer.pan;
        dl->AddImage(g_viewer_tex.im_id(), c - size * 0.5f, c + size * 0.5f);
    } else if (g_viewer.error.empty()) {
        for (int i = 0; i < 3; i++) {   // loading
            float k = 0.5f + 0.5f * std::sin((float)now * 5.0f - i * 0.8f);
            dl->AddCircleFilled(center + ImVec2((i - 1) * px(16), 0), px(3) + k * px(1.5f),
                                ImGui::GetColorU32(IM_COL32(250, 250, 250, 255), 0.3f + 0.7f * k), 16);
        }
    } else {
        ImGui::PushFont(F.strong);
        ImVec2 ts = ImGui::CalcTextSize(g_viewer.error.c_str());
        dl->AddText(center - ts * 0.5f, IM_COL32(250, 250, 250, 230), g_viewer.error.c_str());
        ImGui::PopFont();
    }

    bool on_controls = false;
    float m = px(20), ib = px(44);
    if (ca > 0.02f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e * ca);
        // Top left: name and what it is.
        std::string name = base_name(g_viewer.path);
        char meta[96];
        if (g_viewer.video && player.duration() > 0)
            std::snprintf(meta, sizeof(meta), "%s, %dx%d", format_clock(player.duration()).c_str(), player.width(), player.height());
        else if (have)
            std::snprintf(meta, sizeof(meta), "%dx%d, %.0f%%", g_viewer_tex.width, g_viewer_tex.height, fit * g_viewer.zoom * 100.0f);
        else
            meta[0] = 0;
        std::snprintf(meta + std::strlen(meta), sizeof(meta) - std::strlen(meta), "%s%d of %d", meta[0] ? ", " : "",
                      g_viewer.index + 1, (int)g_viewer.list.size());
        ImGui::PushFont(F.strong);
        float nw = ImGui::CalcTextSize(name.c_str()).x;
        ImGui::PopFont();
        ImGui::PushFont(F.small);
        float mw = ImGui::CalcTextSize(meta).x;
        ImGui::PopFont();
        ImVec2 pp = vp->Pos + ImVec2(m, m), ps(nw + mw + px(44), px(44));
        dl->AddRectFilled(pp, pp + ps, ink(C.glass), ps.y * 0.5f);
        dl->AddRect(pp, pp + ps, ink(C.glass_border), ps.y * 0.5f);
        ImGui::PushFont(F.strong);
        dl->AddText(pp + ImVec2(px(18), (ps.y - ImGui::GetFontSize()) * 0.5f), ink(C.text), name.c_str());
        ImGui::PopFont();
        ImGui::PushFont(F.small);
        dl->AddText(pp + ImVec2(px(18) + nw + px(10), (ps.y - ImGui::GetFontSize()) * 0.5f), ink(C.muted), meta);
        ImGui::PopFont();

        // Top right: open in another app, close.
        float tx = vp->Pos.x + vp->Size.x - m - ib;
        ImGui::SetCursorScreenPos(ImVec2(tx, vp->Pos.y + m));
        if (button("##vclose", icon::close, F.icons ? nullptr : "Close", Kind::Glass, ImVec2(F.icons ? ib : px(70), ib)))
            close_viewer(app);
        on_controls |= ImGui::IsItemHovered();
        tip("Back to the gallery  (Esc)");
        ImGui::SetCursorScreenPos(ImVec2(tx - ib - px(8), vp->Pos.y + m));
        if (button("##vext", icon::open_ext, F.icons ? nullptr : "Open", Kind::Glass, ImVec2(F.icons ? ib : px(70), ib))) {
            if (g_viewer.video) player.pause();
            if (!glint::open_with_default_app(g_viewer.path)) gallery_status("Could not open the file.", true);
        }
        on_controls |= ImGui::IsItemHovered();
        tip("Open in another app");

        // Previous / next at the sides.
        if (g_viewer.index > 0) {
            ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x + m, center.y - ib * 0.5f));
            if (button("##vprev", icon::prev, F.icons ? nullptr : "<", Kind::Glass, ImVec2(ib, ib)))
                viewer_show(app, g_viewer.index - 1);
            on_controls |= ImGui::IsItemHovered();
            tip("Previous  (Page Up)");
        }
        if (g_viewer.index + 1 < (int)g_viewer.list.size()) {
            ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x + vp->Size.x - m - ib, center.y - ib * 0.5f));
            if (button("##vnext", icon::next, F.icons ? nullptr : ">", Kind::Glass, ImVec2(ib, ib)))
                viewer_show(app, g_viewer.index + 1);
            on_controls |= ImGui::IsItemHovered();
            tip("Next  (Page Down)");
        }

        if (g_viewer.video && g_viewer.error.empty()) {
            // Bottom: play/pause, time, seek bar, length, volume.
            float bw = std::min(vp->Size.x - m * 2, px(960)), bh = px(56);
            ImVec2 bp(vp->Pos.x + (vp->Size.x - bw) * 0.5f, vp->Pos.y + vp->Size.y - m - bh);
            dl->AddRectFilled(bp + ImVec2(0, px(6)), bp + ImVec2(bw, bh + px(8)), ImGui::GetColorU32(C.shadow, 0.5f), bh * 0.5f);
            dl->AddRectFilled(bp, bp + ImVec2(bw, bh), ink(C.glass), bh * 0.5f);
            dl->AddRect(bp, bp + ImVec2(bw, bh), ink(C.glass_border), bh * 0.5f);
            on_controls |= ImGui::IsMouseHoveringRect(bp, bp + ImVec2(bw, bh));
            float x = bp.x + px(6), cy = bp.y + bh * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(x, cy - px(22)));
            bool playing = player.playing();
            if (button("##vplay", playing ? icon::pause : icon::play, F.icons ? nullptr : (playing ? "Pause" : "Play"),
                       Kind::GlassOn, ImVec2(F.icons ? px(44) : px(70), px(44)))) {
                if (playing) player.pause();
                else player.play();
            }
            tip(playing ? "Pause  (Space)" : "Play  (Space)");
            x += px(44) + px(14);
            double dur = std::max(0.001, player.duration());
            double pos = g_viewer.scrubbing ? g_viewer.scrub_to : player.position();
            ImGui::PushFont(F.mono);
            std::string cur = format_clock(pos), total = format_clock(dur);
            float tw1 = ImGui::CalcTextSize("00:00:00").x * (dur >= 3600 ? 1.0f : 0.63f);
            dl->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), ink(C.text), cur.c_str());
            x += tw1 + px(10);
            float vol_w = px(140), right = bp.x + bw - px(14) - vol_w - px(44) - tw1 - px(12);
            float sw = std::max(px(80), right - x);
            dl->AddText(ImVec2(x + sw + px(12), cy - ImGui::GetFontSize() * 0.5f), ink(C.muted), total.c_str());
            ImGui::PopFont();

            // Seek bar: click or drag; while dragging the picture follows.
            ImGui::SetCursorScreenPos(ImVec2(x, cy - px(12)));
            ImGui::InvisibleButton("##vseek", ImVec2(sw, px(24)));
            bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
            if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            float mouse_t = std::clamp((io.MousePos.x - x) / sw, 0.0f, 1.0f);
            if (act) {
                g_viewer.scrub_to = mouse_t * dur;
                g_viewer.scrubbing = true;
                if (now - g_viewer.last_scrub > 0.08) {
                    player.seek(g_viewer.scrub_to);
                    g_viewer.last_scrub = now;
                }
            } else if (g_viewer.scrubbing) {
                player.seek(g_viewer.scrub_to);
                g_viewer.scrubbing = false;
            }
            float frac = (float)std::clamp(pos / dur, 0.0, 1.0);
            float grow = anim(ImGui::GetItemID(), (hov || act) ? 1.0f : 0.0f);
            float th = px(4) + grow * px(2);
            dl->AddRectFilled(ImVec2(x, cy - th * 0.5f), ImVec2(x + sw, cy + th * 0.5f), ImGui::GetColorU32(C.text, 0.22f), th);
            if (hov && !act)
                dl->AddRectFilled(ImVec2(x, cy - th * 0.5f), ImVec2(x + sw * mouse_t, cy + th * 0.5f), ImGui::GetColorU32(C.text, 0.18f), th);
            dl->AddRectFilled(ImVec2(x, cy - th * 0.5f), ImVec2(x + sw * frac, cy + th * 0.5f), ink(C.text), th);
            if (grow > 0.01f) dl->AddCircleFilled(ImVec2(x + sw * frac, cy), px(7) * grow, ink(C.text), 20);
            if (hov || act) {   // time under the pointer
                ImGui::PushFont(F.mono);
                std::string at = format_clock((act ? frac : mouse_t) * dur);
                ImVec2 ts = ImGui::CalcTextSize(at.c_str());
                ImVec2 tp(std::clamp(io.MousePos.x - ts.x * 0.5f, x, x + sw - ts.x), bp.y - ts.y - px(16));
                dl->AddRectFilled(tp - ImVec2(px(8), px(4)), tp + ts + ImVec2(px(8), px(4)), ink(C.nav), px(6));
                dl->AddText(tp, ink(C.text), at.c_str());
                ImGui::PopFont();
            }

            // Volume.
            float vx = bp.x + bw - px(14) - vol_w - px(44);
            ImGui::SetCursorScreenPos(ImVec2(vx, cy - px(22)));
            float vol = player.volume();
            bool muted = vol <= 0.001f;
            static float before_mute = 1.0f;
            if (button("##vmute", muted ? icon::mute : icon::volume, F.icons ? nullptr : (muted ? "Muted" : "Sound"),
                       Kind::Ghost, ImVec2(px(44), px(44)), player.has_audio())) {
                if (muted) player.set_volume(before_mute > 0.01f ? before_mute : 1.0f);
                else { before_mute = vol; player.set_volume(0.0f); }
            }
            tip(player.has_audio() ? "Mute  (Up/Down for volume)" : "This recording has no sound");
            ImGui::SetCursorScreenPos(ImVec2(vx + px(44), cy - px(13)));
            if (slider("##vvol", &vol, 0.0f, 2.0f, "%.0f%%", 100.0f, vol_w, player.has_audio())) player.set_volume(vol);
        } else if (have) {
            // Bottom: zoom for screenshots. A switch with one choice lit:
            // Fit, 100 % (actual pixels), or neither while zoomed elsewhere.
            // When the picture fits at 100 % (a 1080p screenshot on a 1080p
            // screen) both are the same zoom, and Fit is the one lit.
            static const char* labels[2] = {"Fit", "100%"};
            float one = 1.0f / fit;   // zoom at which one picture pixel is one screen pixel
            bool at_fit = std::fabs(g_viewer.zoom - 1.0f) < 0.01f;
            bool at_one = !at_fit && std::fabs(g_viewer.zoom - one) < 0.01f;
            int pick = at_fit ? 0 : at_one ? 1 : -1;
            float sw = segmented_width(labels, 2, px(64));
            float pad = px(6), h = px(34) + pad * 2;
            ImVec2 bp(vp->Pos.x + (vp->Size.x - sw - pad * 2) * 0.5f, vp->Pos.y + vp->Size.y - m - h);
            dl->AddRectFilled(bp, bp + ImVec2(sw + pad * 2, h), ink(C.glass), px(12));
            dl->AddRect(bp, bp + ImVec2(sw + pad * 2, h), ink(C.glass_border), px(12));
            on_controls |= ImGui::IsMouseHoveringRect(bp, bp + ImVec2(sw + pad * 2, h));
            ImGui::SetCursorScreenPos(bp + ImVec2(pad, pad));
            if (segmented("##vzoom", &pick, labels, 2, px(64))) {
                g_viewer.zoom = pick == 0 ? 1.0f : std::max(1.0f, one);
                g_viewer.pan = ImVec2(0, 0);
            }
            tip("Fit the window or show actual pixels. Scroll to zoom, drag to move, double-click to switch.");
        }
        ImGui::PopStyleVar();
    }

    // Pointer on the picture: click plays/pauses a recording; on a screenshot,
    // scroll zooms around the pointer, drag pans, double-click toggles 100 %.
    bool on_picture = !on_controls && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive();
    // Panning a zoomed screenshot, like any image viewer: press anywhere on
    // the picture (not on the controls) and drag; it pans until the button is
    // let go, even when the pointer crosses the controls.
    static bool dragging = false;
    if (have && !g_viewer.video && !on_controls && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        dragging = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
    if (dragging && g_viewer.zoom > 1.0f) {
        g_viewer.pan = g_viewer.pan + io.MouseDelta;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (on_picture && have) {
        if (g_viewer.video) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (player.playing()) player.pause();
                else player.play();
            }
        } else {
            if (io.MouseWheel != 0.0f) {
                float old = g_viewer.zoom;
                g_viewer.zoom = std::clamp(g_viewer.zoom * std::pow(1.2f, io.MouseWheel), 1.0f, std::max(8.0f, 4.0f / fit));
                ImVec2 rel = io.MousePos - center - g_viewer.pan;
                g_viewer.pan = g_viewer.pan - rel * (g_viewer.zoom / old - 1.0f);
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                float one = 1.0f / fit;
                g_viewer.zoom = g_viewer.zoom > 1.01f ? 1.0f : std::max(1.0f, one);
                g_viewer.pan = ImVec2(0, 0);
            }
        }
    }

    ImGui::PopStyleVar();   // fade
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Shell state
// ---------------------------------------------------------------------------

// What the user should be told about the link. App::running() stays true after
// the worker gives up, so a failed connect is recognised from its message.
enum class Link { Offline, Connecting, Live, Failed };

static Link link_state(glint::App& app)
{
    if (app.connected()) return Link::Live;
    if (!app.running()) return Link::Offline;
    std::string e = app.error();
    if (e.rfind("Cannot connect", 0) == 0 || e.rfind("Video", 0) == 0) return Link::Failed;
    return Link::Connecting;
}

// A failed link because another viewer took the stream (the console streams to
// one viewer at a time), not because the console can't be reached.
static bool watched_elsewhere(glint::App& app)
{
    return app.error().find("open in a browser or on another PC") != std::string::npos;
}

// Settings tabs. The gallery is its own overlay (draw_gallery).
#if SL_LIVE_STREAMING
enum Page { PageConsole, PageVideo, PagePicture, PageAudio, PageCapture, PageStreaming, PageHelp, PageCount };
#else
enum Page { PageConsole, PageVideo, PagePicture, PageAudio, PageCapture, PageHelp, PageCount };
#endif

struct Shell {
    bool settings_open = false;
    bool settings_was_open = false;
    int page = PageConsole;
    bool controls_pinned_hidden = false;
    double last_activity = 0;
    float overlay_alpha = 1.0f;
    bool pointer_on_controls = false;
    int picture = 0;   // 0 fit, 1 fill, 2 no upscaling
    bool fullscreen = false;
    int windowed[4] = {100, 100, 1600, 900};
    int normal[4] = {INT_MIN, INT_MIN, 0, 0};   // last non-maximized, non-fullscreen rect
    char url[512] = {};
    double stop_confirm_until = 0;
    glint::RecorderInfo::State last_rec_state = glint::RecorderInfo::State::Idle;
    std::vector<std::string> recent;   // consoles connected to before, newest first
    bool was_connected = false;
    bool gallery_open = false;
    float settings_t = 0.0f;           // sheet slide, 0 closed .. 1 open
    float gallery_t = 0.0f;            // gallery fade/scale
    double page_shown_at = -1.0;       // settings tab switch time (content fade)
};
static Shell g_shell;

static void set_fullscreen(bool on)
{
    if (!g_window || on == g_shell.fullscreen) return;
    if (on) {
        glfwGetWindowPos(g_window, &g_shell.windowed[0], &g_shell.windowed[1]);
        glfwGetWindowSize(g_window, &g_shell.windowed[2], &g_shell.windowed[3]);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(g_window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(g_window, nullptr, g_shell.windowed[0], g_shell.windowed[1],
                             g_shell.windowed[2], g_shell.windowed[3], 0);
    }
    g_shell.fullscreen = on;
}

static void connect_to(glint::App& app)
{
    app.start(g_shell.url);
    ui::toast(std::string("Connecting to ") + g_shell.url, 0);
}

// Newest first, no duplicates, at most four: the connect screen's Recent list.
static void remember_console(const std::string& url)
{
    if (url.empty()) return;
    auto& r = g_shell.recent;
    r.erase(std::remove(r.begin(), r.end(), url), r.end());
    r.insert(r.begin(), url);
    if (r.size() > 4) r.resize(4);
}

// Per-frame bookkeeping that should run whether or not a page is visible.
static void poll_events(glint::App& app)
{
    bool connected = app.connected();
    if (connected && !g_shell.was_connected) remember_console(app.url());
    g_shell.was_connected = connected;

    glint::RecorderInfo rec = app.recording_info();
    auto prev = g_shell.last_rec_state;
    if (prev != rec.state) {
        if (rec.state == glint::RecorderInfo::State::Idle &&
            (prev == glint::RecorderInfo::State::Finalizing || prev == glint::RecorderInfo::State::Recording ||
             prev == glint::RecorderInfo::State::Armed)) {
            ui::toast(rec.message.empty() ? "Recording saved." : rec.message, 1);
            glint::logf("recording: %s %s (%.1f s, %llu frames)", rec.message.c_str(), rec.path.c_str(), rec.seconds,
                      (unsigned long long)rec.video_frames);
            g_gallery.loaded = false;
        } else if (rec.state == glint::RecorderInfo::State::Failed) {
            ui::toast(rec.error.empty() ? "Recording failed." : rec.error, 2);
            glint::logf("recording: failed: %s", rec.error.c_str());
            g_gallery.loaded = false;
        }
        g_shell.last_rec_state = rec.state;
    }
    if (g_gallery.exporting &&
        g_gallery.export_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ExportResult result = g_gallery.export_job.get();
        g_gallery.exporting = false;
        if (result.ok) gallery_status("Export finished: " + base_name(result.path), false);
        else gallery_status(result.error, true);
        g_gallery.loaded = false;
    }
}

static void handle_shortcuts(glint::App& app)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) g_shell.settings_open = !g_shell.settings_open;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g_viewer.open) close_viewer(app);
        else if (g_shell.settings_open) g_shell.settings_open = false;
        else if (g_shell.gallery_open) g_shell.gallery_open = false;
        else if (g_shell.fullscreen) set_fullscreen(false);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) set_fullscreen(!g_shell.fullscreen);
    if (ctrl) return;
    if (g_viewer.open) {
        viewer_keys(app);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        bool on = !app.audio().enabled();
        app.audio().set_enabled(on);
        ui::toast(on ? "Sound on" : "Sound off", 0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        ui::set_theme(1 - ui::g_theme);
        ui::toast(ui::g_theme ? "Light theme" : "Dark theme", 0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_G, false) && !g_shell.settings_open) g_shell.gallery_open = !g_shell.gallery_open;
    if (g_shell.settings_open || g_shell.gallery_open) return;
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) g_shell.controls_pinned_hidden = !g_shell.controls_pinned_hidden;
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        static const char* names[] = {"Fit window", "Fill window", "No upscaling"};
        g_shell.picture = (g_shell.picture + 1) % 3;
        ui::toast(std::string("Picture size: ") + names[g_shell.picture], 0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        static const char* names[] = {"Latency: Lowest", "Latency: Balanced", "Latency: Smooth"};
        int next = ((int)app.latency() + 1) % 3;
        app.set_latency((glint::App::Latency)next);
        ui::toast(names[next], 0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) do_screenshot(app);
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) toggle_local_recording(app);
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        g_compare.on = !g_compare.on;
        ui::toast(g_compare.on ? "Side by side: scroll to zoom, drag to move. X to tweak, C to leave." : "Side by side off", 0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_X, false) && g_compare.on) g_tweaks_open = !g_tweaks_open;
}

// ---------------------------------------------------------------------------
// Settings pages
// ---------------------------------------------------------------------------

static void page_console(glint::App& app)
{
    using namespace ui;
    auto config = app.config();
    auto status = app.status();
    page_header("Console", "The PlayStation running the Glint payload. Glint watches one console at a time.");

    section("Connection");
    card_begin();
    {
        bool running = app.running();
        const char* action = running ? "Reconnect" : "Connect";
        float bw = button_width(icon::connect, action);
        float iw = std::max(px(300), g_card.inner_w - bw - px(10));   // full width on its own line
        row_begin("Address", "Host and port of the Glint payload, usually port 7231.", iw + px(10) + bw);
        bool enter = input("##url", g_shell.url, sizeof(g_shell.url), iw, true, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, px(10));
        if (button("##connect", icon::connect, action, Kind::Primary, ImVec2(bw, 0)) || enter) connect_to(app);
        row_end();

        Link link = link_state(app);
        const char* state = link == Link::Live ? "Live" : link == Link::Connecting ? "Connecting"
                          : link == Link::Failed ? (watched_elsewhere(app) ? "Elsewhere" : "Unreachable") : "Offline";
        ImU32 fg = link == Link::Live ? C.success : link == Link::Connecting ? C.warn
                 : link == Link::Failed ? C.danger : C.faint;
        ImU32 bg = link == Link::Live ? C.success_soft : link == Link::Connecting ? C.warn_soft
                 : link == Link::Failed ? C.danger_soft : C.nav_active;
        std::string desc = app.error();
        float dw = running ? button_width(nullptr, "Disconnect") : 0.0f;
        float b2 = badge_width(state);
        row_begin("Status", desc.c_str(), b2 + (running ? px(10) + dw : 0.0f), 36);
        // The badge sits on the button's centre line.
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImGui::PushFont(F.label);
        float badge_h = ImGui::GetFontSize() + px(8);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(bp + ImVec2(0, (px(36) - badge_h) * 0.5f));
        badge(state, fg, bg, false);
        ImGui::SetCursorScreenPos(bp + ImVec2(b2, 0));
        ImGui::Dummy(ImVec2(0, px(36)));
        if (running) {
            ImGui::SetCursorScreenPos(bp + ImVec2(b2 + px(10), 0));
            if (button("##disconnect", nullptr, "Disconnect", Kind::Secondary, ImVec2(dw, 0))) app.stop();
        }
        row_end();
    }
    card_end();

    section("Stream");
    card_begin();
    {
        char line[160];
        std::snprintf(line, sizeof(line), "%d x %d at %d fps", config.width, config.height, config.fps);
        row_begin("Picture", config.width > 0 ? line : "Not known until connected.", 0, 20);
        row_end();
        float rx_fps = 0.0f, rx_kbps = 0.0f;
        app.received_rate(rx_fps, rx_kbps);
        std::snprintf(line, sizeof(line), "%.1f fps, %.2f Mbit/s received (last 2 s)", rx_fps, rx_kbps / 1000.0);
        row_begin("Throughput", line, 0, 20);
        row_end();
        std::snprintf(line, sizeof(line), "%llu frames decoded since connecting",
                      (unsigned long long)app.frame_count());
        row_begin("Decoded", line, 0, 20);
        row_end();
    }
    card_end();

    section("Console actions");
    card_begin();
    {
        float bw = button_width(icon::refresh, "Request keyframe");
        row_begin("Refresh picture", "Ask the console for a fresh keyframe. Fixes a smeared or frozen picture.", bw);
        if (button("##kf", icon::refresh, "Request keyframe", Kind::Secondary, ImVec2(bw, 0), app.running())) {
            std::string error;
            if (app.post("/video/keyframe", error)) ui::toast("Keyframe requested", 0);
            else ui::toast("Keyframe request failed: " + error, 2);
        }
        row_end();

        bool confirming = ImGui::GetTime() < g_shell.stop_confirm_until;
        const char* label = confirming ? "Click again to stop" : "Stop capture";
        float sw = button_width(icon::power, label);
        row_begin("Stop capture on the console",
                  "Ends capture on the console and disconnects everyone watching. On some consoles this crashes the "
                  "system, so restart the PS5 before starting the payload again.",
                  sw);
        if (button("##stopcap", icon::power, label, Kind::Danger, ImVec2(sw, 0), app.running())) {
            if (!confirming) {
                g_shell.stop_confirm_until = ImGui::GetTime() + 3.0;
            } else {
                std::string error;
                if (app.post("/stop", error)) ui::toast("Capture stopped on the console", 1);
                else ui::toast("Stop failed: " + error, 2);
                app.stop();
                g_shell.stop_confirm_until = 0;
            }
        }
        row_end();
    }
    card_end();
}


// Driver page of the GPU vendor (PCI vendor id from Vulkan).
static const char* driver_page(uint32_t vendor)
{
    switch (vendor) {
    case 0x1002: return "https://www.amd.com/en/support/download/drivers.html";
    case 0x10DE: return "https://www.nvidia.com/en-us/drivers/";
    case 0x8086: return "https://www.intel.com/content/www/us/en/download-center/home.html";
    default: return nullptr;
    }
}

static std::string driver_version_text(const VkPhysicalDeviceProperties& p)
{
    char b[96];
    if (p.vendorID == 0x10DE)   // NVIDIA packs 10.8.8.6 bits
        std::snprintf(b, sizeof(b), "%u.%u", (p.driverVersion >> 22) & 0x3ff, (p.driverVersion >> 14) & 0xff);
    else
        std::snprintf(b, sizeof(b), "%u.%u.%u", VK_API_VERSION_MAJOR(p.driverVersion),
                      VK_API_VERSION_MINOR(p.driverVersion), VK_API_VERSION_PATCH(p.driverVersion));
    return b;
}

static std::string conversion_in_use(glint::App& app)
{
    return app.gpu_convert() && g_yuv.available ? "Vulkan YUV (GPU)" : (g_yuv.tried && !g_yuv.available ? "CPU (GPU unavailable)" : "CPU");
}

// ---------------------------------------------------------------------------
// Settings -> Picture: the enhancement passes (g_enh). A profile picks a set of
// values; touching any control switches to Custom with the values shown. With
// "Protect frame time" the profile steps down while the passes take longer
// than the GPU budget (measured each frame), so a weak GPU never drops frames.
// ---------------------------------------------------------------------------

enum { PictureOff, PictureLight, PictureBalanced, PictureEnhanced, PictureCustom };
static const char* const kPictureProfiles[] = {"Off", "Light", "Balanced", "Enhanced", "Custom"};
static const char* const* const kUpscalers = kUpscalerNames;
constexpr int kUpscalerCount = 4;

static glint::PictureSettings g_picture;
static std::vector<glint::PicturePreset> g_picture_presets;   // the user's own, saved in picture.conf
static std::string g_picture_active;   // the user preset applied (Custom started from it), "" for a built-in profile
static int g_picture_step = 0;              // profiles stepped down by "Protect frame time"
static int g_picture_step_profile = -1;     // the profile the step belongs to
static double g_picture_over_since = -1.0;

// The values a profile stands for (colour stays neutral; Custom keeps its own).
static glint::PictureSettings picture_preset(int profile)
{
    glint::PictureSettings s;
    s.exposure = 0.0f;
    s.saturation = 1.0f;
    s.shadows = 0.0f;
    switch (profile) {
    case PictureLight:
        s.upscaler = 0; s.sharpness = 0.25f; s.denoise = 0.0f; s.deband = 0.0f; s.dither = 0.0f;
        break;
    case PictureEnhanced:
        s.upscaler = UpscaleAi; s.sharpness = 0.45f; s.denoise = 0.15f; s.deband = 0.35f; s.dither = 0.5f;
        break;
    default:   // Balanced
        s.upscaler = UpscaleFsr; s.sharpness = 0.35f; s.denoise = 0.0f; s.deband = 0.25f; s.dither = 0.5f;
        break;
    }
    return s;
}

// The values in effect: the profile's, or Custom's, after any step-down.
static glint::PictureSettings picture_effective()
{
    glint::PictureSettings v = g_picture.profile == PictureCustom ? g_picture : picture_preset(g_picture.profile);
    if (g_picture_step > 0) {
        if (g_picture.profile == PictureCustom) {
            v.upscaler = std::max(0, v.upscaler - g_picture_step);
            if (g_picture_step >= 1) v.denoise = 0.0f;
            if (g_picture_step >= 2) v.deband = 0.0f;
        } else {
            v = picture_preset(std::max((int)PictureLight, g_picture.profile - g_picture_step));
        }
    }
    v.upscaler = std::clamp(v.upscaler, 0, kUpscalerCount - 1);
    return v;
}

// Once per frame, before the upload: resolves the passes and runs the
// frame-time protection.
static void picture_update()
{
    if (g_picture_step_profile != g_picture.profile) {   // a new profile starts at full strength
        g_picture_step = 0;
        g_picture_step_profile = g_picture.profile;
        g_picture_over_since = -1.0;
    }
    if (g_picture.profile != PictureOff && g_picture.protect && g_enh.shown && g_enh.gpu_ms > g_picture.budget_ms) {
        double now = ImGui::GetTime();
        if (g_picture_over_since < 0) g_picture_over_since = now;
        int floor_step = g_picture.profile == PictureCustom ? 2 : g_picture.profile - PictureLight;
        if (now - g_picture_over_since > 1.5 && g_picture_step < floor_step) {
            g_picture_step++;
            g_picture_over_since = -1.0;
            g_enh.gpu_ms = 0.0f;   // measure the lighter set afresh
            glint::logf("picture: GPU over its %.1f ms budget; stepped down %d", g_picture.budget_ms, g_picture_step);
        }
    } else {
        g_picture_over_since = -1.0;
    }

    EnhanceParams e;
    e.active = g_picture.profile != PictureOff;
    if (e.active) {
        glint::PictureSettings v = picture_effective();
        e.upscaler = v.upscaler;
        e.sharpness = v.sharpness;
        e.denoise = v.denoise;
        e.deband = v.deband;
        e.dither = v.dither;
        e.exposure = v.exposure;
        e.saturation = v.saturation;
        e.shadows = v.shadows;
    }
    g_enh.params = e;
}

// Picture values only (not the profile, protection or budget).
static bool picture_same(const glint::PictureSettings& a, const glint::PictureSettings& b)
{
    return a.upscaler == b.upscaler && a.sharpness == b.sharpness && a.denoise == b.denoise && a.deband == b.deband &&
           a.dither == b.dither && a.exposure == b.exposure && a.saturation == b.saturation && a.shadows == b.shadows;
}

static glint::PicturePreset* picture_find(const std::string& name)
{
    for (glint::PicturePreset& p : g_picture_presets)
        if (p.name == name) return &p;
    return nullptr;
}

// How a user preset relates to what is on now.
enum class PresetState { Other, Applied, Changed };
static PresetState preset_state(const glint::PicturePreset& p)
{
    if (g_picture_active != p.name || g_picture.profile != PictureCustom) return PresetState::Other;
    return picture_same(g_picture, p.values) ? PresetState::Applied : PresetState::Changed;
}

// Makes `values` the Custom profile (keeping protection and budget).
static void picture_set_custom(const glint::PictureSettings& values)
{
    glint::PictureSettings v = values;
    v.profile = PictureCustom;
    v.protect = g_picture.protect;
    v.budget_ms = g_picture.budget_ms;
    g_picture = v;
    g_picture_step = 0;
    g_picture_step_profile = PictureCustom;
}

static void picture_apply_preset(const glint::PicturePreset& p)
{
    picture_set_custom(p.values);
    g_picture_active = p.name;
    ui::toast("Applied preset \"" + p.name + "\"", 0);
}

// What the Profile row says about the profile in use.
static std::string picture_profile_desc()
{
    static const char* const hints[] = {
        "The picture as it comes from the console.",
        "Bilinear upscaling and light sharpening. For older or integrated GPUs.",
        "FSR 1 upscaling, sharpening and debanding.",
        "AI upscaling, denoising and everything else. For a strong GPU.",
        "Your own values.",
    };
    if (g_picture.profile == PictureCustom && !g_picture_active.empty()) {
        if (glint::PicturePreset* p = picture_find(g_picture_active))
            return "Your preset \"" + p->name + "\"" +
                   (preset_state(*p) == PresetState::Changed ? ", changed (not saved)." : ".");
    }
    return hints[g_picture.profile];
}

// The profile switch. A built-in profile ends any applied user preset.
static void picture_profile_control(float min_item)
{
    using namespace ui;
    int prof = g_picture.profile;
    if (segmented("##pprofile", &prof, kPictureProfiles, 5, min_item) && prof != g_picture.profile) {
        g_picture.profile = prof;
        if (prof != PictureCustom) g_picture_active.clear();
    }
}

// The tweaking controls, shared by Settings -> Picture (rows) and the side by
// side panel (columns). Every control shows the values in effect; changing
// one makes them the Custom profile. An applied preset stays the one in use,
// now marked changed until it is updated or reverted.
enum { TweakUpscaler, TweakSharpness, TweakDenoise, TweakDeband, TweakDither, TweakExposure, TweakSaturation,
       TweakShadows, TweakCount };
static void picture_tweak(int which, glint::PictureSettings& v, float width)
{
    using namespace ui;
    switch (which) {
    case TweakUpscaler:  segmented("##pup", &v.upscaler, kUpscalers, kUpscalerCount, width / kUpscalerCount); break;
    case TweakSharpness: slider("##psharp", &v.sharpness, 0.0f, 1.0f, "%.0f%%", 100.0f, width); break;
    case TweakDenoise:   slider("##pdenoise", &v.denoise, 0.0f, 1.0f, "%.0f%%", 100.0f, width); break;
    case TweakDeband:    slider("##pdeband", &v.deband, 0.0f, 1.0f, "%.0f%%", 100.0f, width); break;
    case TweakDither:    slider("##pdither", &v.dither, 0.0f, 1.0f, "%.0f%%", 100.0f, width); break;
    case TweakExposure:  slider("##pexp", &v.exposure, -2.0f, 2.0f, "%+.1f", 1.0f, width); break;
    case TweakSaturation: slider("##psat", &v.saturation, 0.0f, 2.0f, "%.0f%%", 100.0f, width); break;
    default:             slider("##pshadow", &v.shadows, 0.0f, 1.0f, "%.0f%%", 100.0f, width); break;
    }
}
static const char* const kTweakNames[TweakCount] = {"Upscaler", "Sharpness",  "Denoise",    "Debanding",
                                                    "Dithering", "Exposure", "Saturation", "Shadow detail"};

// Commits edited values (if any) as Custom.
static void picture_commit(const glint::PictureSettings& v, const glint::PictureSettings& before)
{
    if (!picture_same(v, before)) picture_set_custom(v);
}

static void page_picture(glint::App& app)
{
    using namespace ui;
    (void)app;
    section("Profile");
    card_begin();
    {
        std::string desc = picture_profile_desc();
        row_begin("Profile", desc.c_str(), segmented_width(kPictureProfiles, 5, px(70)));
        picture_profile_control(px(70));
        row_end();

        char status[160];
        if (g_picture.profile == PictureOff)
            std::snprintf(status, sizeof(status), "Off");
        else if (!g_enh.shown)
            std::snprintf(status, sizeof(status), g_enh.tried && !g_enh.available ? "Not available on this GPU"
                                                                                  : "Waiting for video");
        else {
            const char* up = g_enh.used_upscaler >= 0 ? kUpscalers[g_enh.used_upscaler] : "no upscaling";
            if (g_enh.timestamp_ns > 0)
                std::snprintf(status, sizeof(status), "%.2f ms GPU  -  %d x %d  -  %s", g_enh.gpu_ms, g_enh.out.width,
                              g_enh.out.height, up);
            else
                std::snprintf(status, sizeof(status), "%d x %d  -  %s", g_enh.out.width, g_enh.out.height, up);
        }
        const char* sdesc = g_picture_step > 0 ? "Stepped down: the GPU went over its budget." : nullptr;
        ImGui::PushFont(F.mono);
        float sw = ImGui::CalcTextSize(status).x;
        ImGui::PopFont();
        row_begin("Status", sdesc, sw, 20);
        ImGui::PushFont(F.mono);
        text_colored(C.muted, "%s", status);
        ImGui::PopFont();
        row_end();
    }
    card_end();

    // My presets: the values below saved under a name. The one in use is
    // marked Applied, or Changed once a control moved away from it.
    section("My presets");
    card_begin();
    {
        const glint::PictureSettings current =
            g_picture.profile == PictureCustom ? g_picture : picture_preset(g_picture.profile);
        int remove = -1;
        for (size_t i = 0; i < g_picture_presets.size(); i++) {
            glint::PicturePreset& pr = g_picture_presets[i];
            const glint::PictureSettings& v = pr.values;
            PresetState st = preset_state(pr);
            char desc[200];
            std::snprintf(desc, sizeof(desc), "%s, sharpness %.0f%%, denoise %.0f%%, deband %.0f%%%s",
                          kUpscalers[std::clamp(v.upscaler, 0, kUpscalerCount - 1)], v.sharpness * 100,
                          v.denoise * 100, v.deband * 100,
                          v.exposure != 0 || v.saturation != 1 || v.shadows != 0 ? ", own colour" : "");
            const float dw = button_width(icon::trash, "Delete");
            const float lw = button_width(nullptr, "Apply"), uw = button_width(nullptr, "Update"),
                        rw = button_width(nullptr, "Revert");
            const char* badge_text = st == PresetState::Applied ? "Applied" : st == PresetState::Changed ? "Changed" : nullptr;
            float bw = badge_text ? badge_width(badge_text) + px(10) : 0.0f;
            float controls = bw + (st == PresetState::Changed ? uw + px(8) + rw + px(8)
                                   : st == PresetState::Other ? lw + px(8) : 0.0f) + dw;
            ImGui::PushID((int)i);
            row_begin(pr.name.c_str(), desc, controls, 36);
            if (badge_text) {
                ImVec2 bp = ImGui::GetCursorScreenPos();
                ImGui::PushFont(F.label);
                float badge_h = ImGui::GetFontSize() + px(8);
                ImGui::PopFont();
                ImGui::SetCursorScreenPos(bp + ImVec2(0, (px(36) - badge_h) * 0.5f));
                if (st == PresetState::Applied) badge(badge_text, C.success, C.success_soft, false);
                else badge(badge_text, C.warn, C.warn_soft, false);
                ImGui::SetCursorScreenPos(bp + ImVec2(bw, 0));
            }
            if (st == PresetState::Other) {
                if (button("##papply", nullptr, "Apply", Kind::Secondary, ImVec2(lw, 0))) picture_apply_preset(pr);
                tip("Use this preset");
                ImGui::SameLine(0, px(8));
            } else if (st == PresetState::Changed) {
                if (button("##pupdate", nullptr, "Update", Kind::Primary, ImVec2(uw, 0))) {
                    pr.values = current;
                    pr.values.profile = PictureCustom;
                    ui::toast("Updated preset \"" + pr.name + "\"", 1);
                }
                tip("Save the current values into this preset");
                ImGui::SameLine(0, px(8));
                if (button("##prevert", nullptr, "Revert", Kind::Secondary, ImVec2(rw, 0))) picture_apply_preset(pr);
                tip("Back to the preset as saved");
                ImGui::SameLine(0, px(8));
            }
            if (button("##pdel", icon::trash, "Delete", Kind::Ghost, ImVec2(dw, 0))) remove = (int)i;
            row_end();
            ImGui::PopID();
        }
        if (remove >= 0) {
            ui::toast("Deleted preset \"" + g_picture_presets[remove].name + "\"", 0);
            if (g_picture_active == g_picture_presets[remove].name) g_picture_active.clear();
            g_picture_presets.erase(g_picture_presets.begin() + remove);
        }

        static char name[48] = "";
        std::string trimmed(name);
        trimmed.erase(0, trimmed.find_first_not_of(' '));
        trimmed.erase(trimmed.find_last_not_of(' ') + 1);
        glint::PicturePreset* existing = picture_find(trimmed);
        bool full = (int)g_picture_presets.size() >= glint::kMaxPicturePresets && !existing;
        bool can_save = !trimmed.empty() && !full && g_picture.profile != PictureOff;
        const char* save_label = existing ? "Replace" : "Save";
        float sbw = button_width(icon::add, save_label);
        const char* save_desc = g_picture.profile == PictureOff ? "Turn the picture on to save its values."
                              : full ? "You have 20 presets: delete one to save another."
                              : existing ? "A preset with this name exists: Replace overwrites it."
                              : "Saves the values below under a name and applies it.";
        row_begin("Save current settings", save_desc, px(220) + px(8) + sbw, 36);
        bool enter = input("##pname", name, sizeof(name), px(220), g_picture.profile != PictureOff,
                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, px(8));
        if ((button("##psave", icon::add, save_label, Kind::Primary, ImVec2(sbw, 0), can_save) || (enter && can_save))) {
            glint::PicturePreset pr;
            pr.name = trimmed;
            pr.values = current;
            pr.values.profile = PictureCustom;
            if (existing) *existing = pr;
            else g_picture_presets.push_back(pr);
            picture_set_custom(current);   // saved = applied
            g_picture_active = trimmed;
            ui::toast("Saved preset \"" + trimmed + "\"", 1);
            name[0] = 0;
        }
        row_end();
        if (g_picture_presets.empty())
            card_note(C.muted, "No presets yet. Set things up below, give it a name and save it here.");
    }
    card_end();

    section("Compare");
    card_begin();
    {
        row_begin("Side by side",
                  "The same spot twice, original and enhanced. Scroll to zoom, drag to move, double-click to reset. "
                  "X opens the controls over it.  (C)",
                  px(40), 22);
        if (toggle("##pside", &g_compare.on) && g_compare.on) {
            g_shell.settings_open = false;   // the comparison needs the whole window
            ui::toast("Side by side: scroll to zoom, drag to move. X to tweak, C to leave.", 0);
        }
        row_end();
    }
    card_end();

    glint::PictureSettings v = picture_effective();
    const glint::PictureSettings before = v;
    const float sw = px(260);
    static const char* const tips[TweakCount] = {
        nullptr,
        "Contrast-adaptive: sharpens soft detail, leaves flat areas and edges alone.",
        "Edge-preserving. Smooths speckle in flat areas.",
        "Smooths the steps in skies and gradients.",
        "A hint of noise so gradients don't show steps.",
        "In stops (EV).",
        nullptr,
        "Lifts dark areas, leaves the rest.",
    };
    static const char* const up_hints[] = {
        "Smooth and cheapest.", "Sharper edges, no halos.", "AMD FidelityFX FSR 1: edge-adaptive, clean lines.",
        "FSRCNNX neural network: rebuilds fine detail. Used from 1.3x, else Lanczos."};
    auto tweak_row = [&](int which) {
        float w = which == TweakUpscaler ? segmented_width(kUpscalers, kUpscalerCount, px(72)) : sw;
        row_begin(kTweakNames[which], which == TweakUpscaler ? up_hints[v.upscaler] : tips[which], w);
        picture_tweak(which, v, which == TweakUpscaler ? px(72) * kUpscalerCount : sw);
        row_end();
    };

    section("Upscaling and sharpening");
    card_begin();
    tweak_row(TweakUpscaler);
    tweak_row(TweakSharpness);
    card_note(C.muted, "Upscaling applies when the picture is shown larger than the stream (1080p).");
    card_end();

    section("Compression cleanup");
    card_begin();
    tweak_row(TweakDenoise);
    tweak_row(TweakDeband);
    tweak_row(TweakDither);
    card_end();

    section("Color");
    card_begin();
    tweak_row(TweakExposure);
    tweak_row(TweakSaturation);
    tweak_row(TweakShadows);
    card_note(C.muted, "None of these bring back detail lost to compression.");
    card_end();

    picture_commit(v, before);

    section("Frame time");
    card_begin();
    {
        row_begin("Protect frame time", "Steps the profile down when the passes take longer than the budget.", px(40),
                  22);
        toggle("##pprotect", &g_picture.protect);
        row_end();
        row_begin("GPU budget", "Time the passes may take per frame.", sw);
        slider("##pbudget", &g_picture.budget_ms, 0.5f, 10.0f, "%.1f ms", 1.0f, sw, g_picture.protect);
        row_end();
        float bw = button_width(icon::refresh, "Reset");
        row_begin("Profile defaults", "Back to Balanced with neutral colour.", bw, 36);
        if (button("##preset", icon::refresh, "Reset", Kind::Secondary, ImVec2(bw, 0))) {
            glint::PictureSettings d;
            d.protect = g_picture.protect;
            d.budget_ms = g_picture.budget_ms;
            g_picture = d;
            g_picture_active.clear();
        }
        row_end();
    }
    card_end();
}

// Side by side's control panel (X): the tweaks laid out in columns along the
// bottom, over the picture without dimming it, so every change shows at once
// (g_tweaks_open, with g_compare).

static void draw_picture_tweaks(ImVec2 vp_pos, ImVec2 vp_size)
{
    using namespace ui;
    if (!g_compare.on) g_tweaks_open = false;
    float t = anim(ImGui::GetID("##tweaks_open"), g_tweaks_open ? 1.0f : 0.0f, 14.0f);
    if (t < 0.01f) return;

    // A fixed-width panel centred at the bottom: a top row (profile, upscaler,
    // close), a line saying what is in use, then the sliders in a 4 x 2 grid.
    const float pad = px(20), gap = px(22), row_h = px(34);
    const int cols = 4;
    const float width = std::min(vp_size.x - px(32), px(1080));
    const float cell_w = (width - pad * 2 - gap * (cols - 1)) / cols;
    const float cell_h = px(58);
    const float height = pad + row_h + px(30) + cell_h * 2 + px(6) + pad;
    ImVec2 size(width, height);
    ImVec2 pos(vp_pos.x + (vp_size.x - width) * 0.5f,
               vp_pos.y + vp_size.y - px(16) - height + (1.0f - ease_out(t)) * (height + px(32)));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::min(1.0f, t * 1.5f));
    ImGui::Begin("##tweaks", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos + ImVec2(0, px(6)), pos + size + ImVec2(0, px(8)), ImGui::GetColorU32(C.shadow, 0.5f), px(16));
    dl->AddRectFilled(pos, pos + size, ink(C.glass), px(16));
    dl->AddRect(pos, pos + size, ink(C.glass_border), px(16));
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsAnyItemActive())
        g_shell.pointer_on_controls = true;   // the comparison ignores the mouse here

    glint::PictureSettings v = picture_effective();
    const glint::PictureSettings before = v;

    // Top row.
    ImVec2 o = pos + ImVec2(pad, pad);
    ImGui::SetCursorScreenPos(o);
    picture_profile_control(px(64));
    float x = o.x + segmented_width(kPictureProfiles, 5, px(64)) + px(14);
    ImGui::SetCursorScreenPos(ImVec2(x, o.y));
    picture_tweak(TweakUpscaler, v, px(64) * kUpscalerCount);
    float close_w = button_width(icon::close, "Close");
    ImGui::SetCursorScreenPos(ImVec2(pos.x + width - pad - close_w, o.y));
    if (button("##tclose", icon::close, "Close", Kind::Ghost, ImVec2(close_w, row_h))) g_tweaks_open = false;
    tip("Close the controls  (X)");

    // What is in use, under the top row.
    std::string state = picture_profile_desc();
    if (g_enh.shown && g_enh.used_upscaler >= 0) state += "  Upscaling with " + std::string(kUpscalers[g_enh.used_upscaler]) + ".";
    else if (g_picture.profile != PictureOff) state += "  Zoom in to see the upscaler.";
    ImGui::PushFont(F.small);
    dl->AddText(ImVec2(o.x, o.y + row_h + px(8)), ink(C.muted), state.c_str());
    ImGui::PopFont();

    // The grid: name above, slider (with its value) below.
    static const int grid[cols * 2] = {TweakSharpness, TweakDenoise, TweakDeband,   TweakDither,
                                       TweakExposure,  TweakSaturation, TweakShadows, -1};
    float gy = o.y + row_h + px(30);
    for (int i = 0; i < cols * 2; i++) {
        float cx = o.x + (i % cols) * (cell_w + gap), cy = gy + (i / cols) * cell_h;
        if (grid[i] < 0) {   // the last cell: back to the profile's own values
            float rw = button_width(icon::refresh, "Reset");
            ImGui::SetCursorScreenPos(ImVec2(cx + cell_w - rw, cy + px(18)));
            if (button("##treset", icon::refresh, "Reset", Kind::Secondary, ImVec2(rw, row_h)))
                v = g_picture.profile == PictureCustom ? picture_preset(PictureBalanced) : picture_preset(g_picture.profile);
            tip("Back to the profile's values");
            continue;
        }
        ImGui::PushFont(F.label);
        dl->AddText(ImVec2(cx, cy), ink(C.text), kTweakNames[grid[i]]);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(cx, cy + px(22)));
        ImGui::PushID(grid[i]);
        picture_tweak(grid[i], v, cell_w);
        ImGui::PopID();
    }

    picture_commit(v, before);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void page_video(glint::App& app)
{
    using namespace ui;
    auto config = app.config();
    page_header("Video", "How the console picture is shown on this PC.");

    section("Appearance");
    card_begin();
    {
        static const char* themes[] = {"Dark", "Light"};
        int theme = g_theme;
        row_begin("Theme", "Dark keeps the controls out of the way of the picture.  (T)", segmented_width(themes, 2, px(72)));
        if (segmented("##theme", &theme, themes, 2, px(72))) set_theme(theme);
        row_end();
    }
    card_end();

    section("Picture");
    card_begin();
    {
        static const char* modes[] = {"Fit window", "Fill window", "No upscaling"};
        int mode = g_shell.picture;
        row_begin("Picture size",
                  "Fit shows the whole picture, Fill crops it to cover the window, No upscaling keeps the picture's "
                  "own pixels.  (P)",
                  segmented_width(modes, 3, px(96)));
        if (segmented("##fit", &mode, modes, 3, px(96))) g_shell.picture = mode;
        row_end();

        static const char* lat[] = {"Lowest", "Balanced", "Smooth"};
        int li = (int)app.latency();
        char desc[220];
        if (li == 2)
            std::snprintf(desc, sizeof(desc),
                          "Smooth waits a moment to even out a choppy network and keeps the picture in step with the "
                          "sound. Holding %.0f ms now.  (L)", app.video_hold_ms());
        else if (li == 1)
            std::snprintf(desc, sizeof(desc),
                          "Balanced holds just enough to even out the network's jitter, without waiting for the "
                          "sound. Holding %.0f ms now.  (L)", app.video_hold_ms());
        else
            std::snprintf(desc, sizeof(desc), "Lowest shows every frame the moment it arrives.  (L)");
        row_begin("Latency", desc, segmented_width(lat, 3, px(84)));
        if (segmented("##lat", &li, lat, 3, px(84))) app.set_latency((glint::App::Latency)li);
        row_end();

        bool full = g_shell.fullscreen;
        row_begin("Full screen", "Use the whole monitor.  (F11, Esc to leave)", px(40), 22);
        if (toggle("##full", &full)) set_fullscreen(full);
        row_end();
    }
    card_end();

    section("Stream from the console");
    card_begin();
    {
        // 1080p only: this console refuses 1440p and cannot do 2160p, so there is
        // nothing to choose.
        char res[32];
        if (config.height > 0) std::snprintf(res, sizeof(res), "%d x %d", config.width, config.height);
        else std::snprintf(res, sizeof(res), "Connect first");
        ImGui::PushFont(F.mono);
        float rw = ImGui::CalcTextSize(res).x;
        ImGui::PopFont();
        // A read-only value: right-aligned where a control would sit.
        row_begin("Capture resolution", "1080p at 60 fps, the mode this console captures.", rw, 20);
        ImGui::PushFont(F.mono);
        text_colored(C.muted, "%s", res);
        ImGui::PopFont();
        row_end();
    }
    card_end();

    section("Decoding");
    card_begin();
    {
        static const char* where[] = {"GPU", "CPU"};
        int dec = app.gpu_decode() ? 0 : 1;
        row_begin("Video decoder", "The GPU is lighter on the processor. Use the CPU if the picture glitches.",
                  segmented_width(where, 2, px(64)));
        if (segmented("##dec", &dec, where, 2, px(64))) {
            app.set_gpu_decode(dec == 0);
            ui::toast(dec == 0 ? "Decoding on the GPU (D3D11VA)" : "Decoding on the CPU", 0);
        }
        row_end();
        int conv = app.gpu_convert() ? 0 : 1;
        row_begin("Color conversion", "Where the picture is converted for display. The GPU runs a Vulkan shader.",
                  segmented_width(where, 2, px(64)));
        if (segmented("##conv", &conv, where, 2, px(64))) app.set_gpu_convert(conv == 0);
        row_end();
    }
    card_end();

    section("Graphics");
    card_begin();
    {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
        auto value_row = [&](const char* title, const std::string& value) {
            float w = std::min(ImGui::CalcTextSize(value.c_str()).x, px(420));
            row_begin(title, nullptr, w, 20);
            text_colored(C.muted, "%s", value.c_str());
            row_end();
        };
        value_row("Graphics card", props.deviceName);
        char drv[160];
        std::snprintf(drv, sizeof(drv), "Vulkan %u.%u.%u, driver %s", VK_API_VERSION_MAJOR(props.apiVersion),
                      VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
                      driver_version_text(props).c_str());
        value_row("Driver", drv);
        std::string in_use = app.decoder_description();
        value_row("Decoder in use", in_use.empty() ? std::string("Not decoding") : in_use);
        value_row("Color conversion in use", conversion_in_use(app));
        std::string codec = config.codec.empty() ? "Unknown until connected" : config.codec;
        value_row("Stream codec", codec);
        const char* page = driver_page(props.vendorID);
        float ow = button_width(icon::share, "Open");
        row_begin("Driver support", "The graphics vendor's driver page.", ow);
        if (button("##driver", icon::share, "Open", Kind::Secondary, ImVec2(ow, 0), page != nullptr))
            glint::open_with_default_app(page);
        row_end();
    }
    card_end();
}

static void page_audio(glint::App& app)
{
    using namespace ui;
    glint::AudioPlayer& audio = app.audio();
    glint::AudioStats st = audio.stats();
    page_header("Audio", "Console sound on this PC, and how picture and sound are kept together.");

    section("Playback");
    card_begin();
    {
        bool on = audio.enabled();
        row_begin("Console sound", "Play the console's audio here.  (M)", px(40), 22);
        if (toggle("##sound", &on)) audio.set_enabled(on);
        row_end();
        float vol = audio.volume();
        row_begin("Volume", "Up to 400% for quiet games.", px(300), 26);
        if (slider("##vol", &vol, 0.0f, 4.0f, "%.0f%%", 100.0f, px(300))) audio.set_volume(vol);
        row_end();
    }
    card_end();

    section("Timing");
    card_begin();
    {
        bool automatic = audio.delay_auto();
        row_begin("Automatic buffer",
                  "Sizes the sound buffer from the network's recent hiccups. Turn off to set it yourself.", px(40), 22);
        if (toggle("##auto", &automatic)) {
            if (!automatic) audio.set_delay_ms(audio.effective_delay_ms());
            audio.set_delay_auto(automatic);
        }
        row_end();

        float delay = automatic ? (float)audio.effective_delay_ms() : (float)audio.delay_ms();
        row_begin("Buffer", automatic ? "Chosen automatically right now." : "Bigger rides out more hiccups but adds latency.",
                  px(300), 26);
        if (slider("##delay", &delay, 0.0f, 400.0f, "%.0f ms", 1.0f, px(300), !automatic))
            audio.set_delay_ms((int)std::lround(delay));
        row_end();

        bool smooth = app.latency() == glint::App::Latency::Smooth;
        bool sync = app.video_sync();
        row_begin("Keep picture in sync",
                  smooth ? "Holds the picture back by the time sound needs to reach your speakers."
                         : "Works with Smooth latency (Video page). Lowest and Balanced do not wait for the sound.",
                  px(40), 22);
        if (toggle("##sync", &sync, smooth)) app.set_video_sync(sync);
        row_end();
        sync = sync && smooth;

        float offset = (float)app.av_offset_ms();
        row_begin("A/V offset", "Fine-tune if lips still look early or late. Positive shows the picture later.",
                  px(300), 26);
        if (slider("##offset", &offset, -200.0f, 200.0f, "%+.0f ms", 1.0f, px(300), sync))
            app.set_av_offset_ms((int)std::lround(offset));
        row_end();

        char note[200];
        if (st.playout_ms >= 0)
            std::snprintf(note, sizeof(note), "Sound reaches the speakers %.0f ms after it arrives. Picture held %.0f ms.",
                          st.playout_ms, app.video_hold_ms());
        else
            std::snprintf(note, sizeof(note), "Sound is not playing, so the picture is shown as soon as it decodes.");
        card_note(C.muted, note);
    }
    card_end();

    section("Diagnostics");
    card_begin();
    {
        char line[256];
        std::snprintf(line, sizeof(line), "%s%s%s", st.state.c_str(), st.device.empty() ? "" : ", ",
                      st.device.c_str());
        row_begin("Output", line, px(200), 20);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float w = px(200), h = px(6);
        dl->AddRectFilled(p + ImVec2(0, px(7)), p + ImVec2(w, px(7) + h), ink(C.border), h);
        dl->AddRectFilled(p + ImVec2(0, px(7)), p + ImVec2(w * std::min(1.0f, st.peak), px(7) + h),
                          ink(st.peak >= 0.99f ? C.danger : C.success), h);
        ImGui::Dummy(ImVec2(w, px(20)));
        row_end();
        std::snprintf(line, sizeof(line), "%s, buffered %.0f of %.0f ms, drift ratio %.4f",
                      st.format.empty() ? "-" : st.format.c_str(), st.buffered_ms, st.target_ms, st.ratio);
        row_begin("Stream", line, 0, 20);
        row_end();
        std::snprintf(line, sizeof(line),
                      "%llu underruns (%llu after warm-up), %llu frames dropped, %llu non-finite samples, %llu reconnects",
                      (unsigned long long)st.underruns, (unsigned long long)st.underruns_after_warmup,
                      (unsigned long long)st.dropped_frames,
                      (unsigned long long)(st.nonfinite_in + st.nonfinite_out), (unsigned long long)st.reconnects);
        row_begin("Health", line, 0, 20);
        row_end();
    }
    card_end();
}

static void page_capture(glint::App& app)
{
    using namespace ui;
    auto status = app.status();
    glint::RecorderInfo rec = app.recording_info();
    bool running = recorder_running(rec);
    bool busy = running || rec.state == glint::RecorderInfo::State::Finalizing;
    page_header("Capture", "Screenshots and recordings of the game picture. Glint's own controls are never included.");

    section("Screenshot");
    card_begin();
    {
        float bw = button_width(icon::camera, "Save screenshot");
        row_begin("Screenshot", "PNG of the current picture at full resolution.  (S)", bw);
        if (button("##shot", icon::camera, "Save screenshot", Kind::Primary, ImVec2(bw, 0), app.frame_count() > 0))
            do_screenshot(app);
        row_end();
    }
    card_end();

    section("Record on this PC");
    card_begin();
    {
        static char dir[512];
        static bool dir_init = false;
        if (!dir_init) {
            std::snprintf(dir, sizeof(dir), "%s", app.media_dir().c_str());
            dir_init = true;
        }
        float aw = button_width(nullptr, "Apply"), ow = px(36);
        float iw = std::max(px(320), g_card.inner_w - aw - ow - px(20));   // full width on its own line
        row_begin("Folder", busy ? "Stop the recording before changing the folder."
                                 : "Recordings and screenshots are saved here.",
                  iw + aw + ow + px(20));
        input("##dir", dir, sizeof(dir), iw, !busy);
        ImGui::SameLine(0, px(10));
        if (button("##applydir", nullptr, "Apply", Kind::Secondary, ImVec2(aw, 0), !busy)) {
            app.set_media_dir(dir);
            g_gallery.loaded = false;
            ui::toast("Saving to " + std::string(dir), 0);
        }
        ImGui::SameLine(0, px(10));
        if (button("##opendir", icon::folder, nullptr, Kind::Ghost, ImVec2(ow, 0))) glint::open_with_default_app(app.media_dir());
        tip("Open folder");
        row_end();

        const char* action = running ? "Stop and save" : "Start recording";
        float bw = button_width(running ? icon::stop : icon::record, action);
        std::string desc;
        if (rec.state == glint::RecorderInfo::State::Recording) {
            char t[160];
            std::snprintf(t, sizeof(t), "Recording %s, %s, %.1f MB%s", format_clock(rec.seconds).c_str(),
                          base_name(rec.path).c_str(), rec.bytes / 1048576.0, rec.has_audio ? ", with sound" : ", no sound");
            desc = t;
        } else if (rec.state == glint::RecorderInfo::State::Armed) {
            desc = "Waiting for a keyframe from the console...";
        } else if (rec.state == glint::RecorderInfo::State::Finalizing) {
            desc = "Saving the file...";
        } else {
            desc = "H.264 straight from the console plus AAC sound, saved as MKV.  (R)";
        }
        row_begin("Recording", desc.c_str(), bw);
        if (button("##rec", running ? icon::stop : icon::record, action, running ? Kind::Danger : Kind::Primary,
                   ImVec2(bw, 0), app.connected() && rec.state != glint::RecorderInfo::State::Finalizing))
            toggle_local_recording(app);
        row_end();
        if (!rec.error.empty()) card_note(C.danger, rec.error.c_str());
        if (rec.dropped) {
            char t[96];
            std::snprintf(t, sizeof(t), "%llu packets dropped because the disk was too slow.", (unsigned long long)rec.dropped);
            card_note(C.warn, t);
        }
    }
    card_end();

    section("Record on the console");
    card_begin();
    {
        const char* action = status.recording ? "Stop" : "Start";
        float bw = button_width(status.recording ? icon::stop : icon::record, action);
        char desc[200];
        if (status.recording)
            std::snprintf(desc, sizeof(desc), "Recording on the PlayStation: %s, %.1f s, %.1f MB.",
                          status.recording_name.c_str(), status.recording_seconds, status.recording_bytes / 1048576.0);
        else
            std::snprintf(desc, sizeof(desc), "H.264 without sound, saved on the PlayStation until you stop it.");
        row_begin("Console recording", desc, bw);
        if (button("##conrec", status.recording ? icon::stop : icon::record, action, Kind::Secondary, ImVec2(bw, 0),
                   app.connected())) {
            std::string error;
            if (!app.post(status.recording ? "/record/stop" : "/record/start", error))
                ui::toast("Console recording request failed: " + error, 2);
        }
        row_end();
        if (status.recording_error) card_note(C.danger, "The console reported a recording error.");
    }
    card_end();
}

// Shortens `text` with "..." until it fits `width` in the current font.
static std::string fit_text(const std::string& text, float width)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string s = text;
    while (!s.empty() && ImGui::CalcTextSize((s + "...").c_str()).x > width) s.pop_back();
    return s + "...";
}

static void start_export(const glint::GalleryItem& item)
{
    bool is_mkv = item.name.size() > 4 && item.name.substr(item.name.size() - 4) == ".mkv";
    std::string source = item.path;
    std::string ext = is_mkv ? "mp4" : "mkv";
    g_gallery.exporting = true;
    ui::toast(is_mkv ? "Exporting a copy as MP4..." : "Exporting a copy as MKV...", 0);
    g_gallery.export_job = std::async(std::launch::async, [source, ext]() {
        ExportResult result;
        result.ok = glint::export_copy(source, ext, result.path, result.error);
        return result;
    });
}

static void delete_item(const glint::GalleryItem& item)
{
    std::string error;
    if (glint::delete_gallery_file(item.path, error)) {
        gallery_status("Deleted " + item.name, false);
        if (g_gallery.selected == item.path) g_gallery.selected.clear();
        g_gallery.loaded = false;
    } else {
        gallery_status(error, true);
    }
    g_gallery.pending_delete.clear();
}

// Gallery: recordings and screenshots as a grid, over the whole window. It
// fades in and rises a little; the selected item gets a floating action bar.
static void draw_gallery(glint::App& app)
{
    using namespace ui;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    step_towards(g_shell.gallery_t, g_shell.gallery_open, g_shell.gallery_open ? 0.24f : 0.16f);
    bool popup_open = ImGui::IsPopupOpen("##gallery");
    if (g_shell.gallery_t <= 0.0f && !popup_open) return;
    if (!popup_open) ImGui::OpenPopup("##gallery");

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    bool open = ImGui::BeginPopupModal("##gallery", nullptr,
                                       ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!open) {
        g_shell.gallery_open = false;
        return;
    }
    if (g_shell.gallery_t <= 0.0f) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    gallery_follow_folder(app);
    if (!g_gallery.loaded) gallery_refresh(app);

    float e = ease_out(g_shell.gallery_t);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(vp->Pos, vp->Pos + vp->Size, ink(C.panel));
    float rise = (1.0f - e) * px(14);
    ImVec2 o = vp->Pos + ImVec2(0, rise);

    glint::RecorderInfo rec = app.recording_info();
    bool rec_busy = recorder_running(rec) || rec.state == glint::RecorderInfo::State::Finalizing;
    std::string rec_name = rec_busy ? base_name(rec.path) : std::string();

    // Header: title and folder on the left, filter and actions on the right.
    float pad = px(28), head_h = px(76);
    ImGui::SetCursorScreenPos(o + ImVec2(pad, px(24)));
    ImGui::PushFont(F.title);
    ImGui::TextUnformatted("Gallery");
    ImGui::PopFont();
    ImGui::SameLine(0, px(14));
    ImGui::PushFont(F.small);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(4));
    text_colored(C.muted, "%s", app.media_dir().c_str());
    ImGui::PopFont();

    static const char* filters[] = {"All", "Recordings", "Screenshots"};
    float fw = segmented_width(filters, 3), ow = button_width(icon::folder, "Open folder"), cw = px(40);
    float rx = o.x + vp->Size.x - pad - cw;
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y + px(18)));
    if (button("##gclose", icon::close, F.icons ? nullptr : "Close", Kind::Ghost, ImVec2(cw, px(40))))
        g_shell.gallery_open = false;
    tip("Close  (Esc)");
    rx -= ow + px(10);
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y + px(20)));
    if (button("##gfolder", icon::folder, "Open folder", Kind::Secondary, ImVec2(ow, px(36))))
        glint::open_with_default_app(app.media_dir());
    rx -= fw + px(10);
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y + px(21)));
    segmented("##gfilter", &g_gallery.filter, filters, 3);
    dl->AddLine(ImVec2(vp->Pos.x, o.y + head_h), ImVec2(vp->Pos.x + vp->Size.x, o.y + head_h), ink(C.border));

    std::vector<const glint::GalleryItem*> shown;
    for (const auto& item : g_gallery.items)
        if (g_gallery.filter == 0 || (g_gallery.filter == 1) == item.video) shown.push_back(&item);
    std::vector<std::string> shown_paths;   // what the viewer's Previous / Next walk through
    for (const auto* item : shown) shown_paths.push_back(item->path);

    // Grid.
    ImGui::SetCursorScreenPos(o + ImVec2(0, head_h + 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, px(24)));
    ImGui::BeginChild("##grid", ImVec2(vp->Size.x, vp->Size.y - head_h - 1), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    smooth_scroll();
    const glint::GalleryItem* selected = nullptr;
    if (shown.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), ImGui::GetCursorPosY() + avail.y * 0.35f));
        ImGui::PushFont(F.title);
        const char* t = "Nothing here yet";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(t).x) * 0.5f);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        ImGui::PushFont(F.small);
        const char* d = "Save a screenshot (S) or start a recording (R) and it shows up here.";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(d).x) * 0.5f);
        text_colored(C.muted, "%s", d);
        ImGui::PopFont();
    } else {
        float avail = ImGui::GetContentRegionAvail().x, gap = px(16);
        int cols = std::max(1, (int)((avail + gap) / (px(260) + gap)));
        float cw2 = (avail - gap * (cols - 1)) / cols, thumb_h = cw2 * 9.0f / 16.0f, card_h = thumb_h + px(58);
        ImVec2 start = ImGui::GetCursorScreenPos();
        ImDrawList* gl = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < shown.size(); i++) {
            const glint::GalleryItem& item = *shown[i];
            ImVec2 p = start + ImVec2((i % cols) * (cw2 + gap), (i / cols) * (card_h + gap));
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(item.path.c_str());
            if (ImGui::InvisibleButton("##card", ImVec2(cw2, card_h)))
                g_gallery.selected = g_gallery.selected == item.path ? std::string() : item.path;
            bool hovered = ImGui::IsItemHovered();
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                open_viewer(app, shown_paths, item.path);
            ImGuiID iid = ImGui::GetItemID();
            ImGui::PopID();
            bool is_sel = g_gallery.selected == item.path;
            if (is_sel) selected = &item;
            // Visible cards only: request thumbnails and draw.
            if (!ImGui::IsRectVisible(p, p + ImVec2(cw2, card_h))) continue;
            request_thumb(item.path);
            float h = anim(iid, hovered ? 1.0f : 0.0f);
            float s = anim(iid + 1, is_sel ? 1.0f : 0.0f, 18.0f);
            ImVec2 q = p - ImVec2(0, h * px(2));   // hovered cards lift by two pixels
            if (h > 0.01f)
                gl->AddRectFilled(q + ImVec2(0, px(6)), q + ImVec2(cw2, card_h + px(8)),
                                  ImGui::GetColorU32(C.shadow, 0.35f * h), px(14));
            gl->AddRectFilled(q, q + ImVec2(cw2, card_h), ink(C.card), px(12));
            auto tex = g_thumbs.ready.find(item.path);
            ImVec2 ta = q + ImVec2(1, 1), tb = q + ImVec2(cw2 - 1, thumb_h);
            if (tex != g_thumbs.ready.end() && tex->second.descriptor) {
                // Cover the 16:9 box whatever the picture's shape.
                float src = (float)tex->second.width / std::max(1, tex->second.height), dst = (tb.x - ta.x) / (tb.y - ta.y);
                ImVec2 uv0(0, 0), uv1(1, 1);
                if (src > dst) { uv0.x = (1.0f - dst / src) * 0.5f; uv1.x = 1.0f - uv0.x; }
                else { uv0.y = (1.0f - src / dst) * 0.5f; uv1.y = 1.0f - uv0.y; }
                gl->AddImageRounded(tex->second.im_id(), ta, tb, uv0, uv1, IM_COL32_WHITE, px(11),
                                    ImDrawFlags_RoundCornersTop);
            } else {
                // Placeholder that breathes while the thumbnail is decoded.
                float pulse = g_thumbs.jobs.count(item.path) ? 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 4.0f) : 0.0f;
                gl->AddRectFilled(ta, tb, ink(mix(C.nav, C.card_hover, pulse * 0.6f)), px(11), ImDrawFlags_RoundCornersTop);
                if (F.icons) {
                    const char* g = item.video ? icon::video : icon::photo;
                    ImVec2 gs = ImGui::CalcTextSize(g);
                    gl->AddText((ta + tb) * 0.5f - gs * 0.5f, ink(C.faint), g);
                }
            }
            bool writing = item.name == rec_name;
            if (writing || item.video) {
                ImGui::PushFont(F.mono);
                std::string badge = writing ? "Recording" : format_clock(item.duration);
                ImVec2 bs = ImGui::CalcTextSize(badge.c_str());
                ImVec2 b1 = ImVec2(tb.x - px(8), tb.y - px(8)), b0 = b1 - bs - ImVec2(px(14), px(6));
                gl->AddRectFilled(b0, b1, writing ? ink(C.danger_fill) : ImGui::GetColorU32(rgba(9, 9, 11, 0.78f)), px(6));
                gl->AddText(b0 + ImVec2(px(7), px(3)), ImGui::GetColorU32(rgba(250, 250, 250)), badge.c_str());
                ImGui::PopFont();
            }
            gl->AddLine(ImVec2(q.x, q.y + thumb_h), ImVec2(q.x + cw2, q.y + thumb_h), ink(C.border));
            ImGui::PushFont(F.strong);
            std::string name = fit_text(item.name, cw2 - px(24));
            gl->AddText(q + ImVec2(px(12), thumb_h + px(10)), ink(C.text), name.c_str());
            ImGui::PopFont();
            char meta[160];
            if (item.video)
                std::snprintf(meta, sizeof(meta), "Recording, %dp, %.1f MB%s", item.height, item.size / 1048576.0,
                              item.has_audio ? ", with sound" : "");
            else
                std::snprintf(meta, sizeof(meta), "Screenshot, %.1f MB", item.size / 1048576.0);
            ImGui::PushFont(F.small);
            gl->AddText(q + ImVec2(px(12), thumb_h + px(32)), ink(C.muted), fit_text(meta, cw2 - px(24)).c_str());
            ImGui::PopFont();
            ImU32 ring = mix(mix(C.border, C.border_hi, h), C.accent, s);
            gl->AddRect(q, q + ImVec2(cw2, card_h), ink(ring), px(12), 0, 1.0f + s);
        }
        int rows = ((int)shown.size() + cols - 1) / cols;
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy(ImVec2(avail, rows * (card_h + gap) + px(80)));   // room for the action bar
    }
    ImGui::EndChild();

    // Floating action bar for the selected item; it rises into place.
    float bar_t = anim(ImGui::GetID("##gbar"), selected ? 1.0f : 0.0f, 14.0f);
    static glint::GalleryItem last;   // keeps the bar's content while it slides away
    if (selected) last = *selected;
    if (bar_t > 0.01f && !last.path.empty()) {
        bool writing = last.name == rec_name;
        bool confirm = g_gallery.pending_delete == last.path && ImGui::GetTime() < g_gallery.pending_delete_time;
        bool is_mkv = last.name.size() > 4 && last.name.substr(last.name.size() - 4) == ".mkv";
        const char* play = last.video ? "Play" : "Open";
        const char* exp = is_mkv ? "Export as MP4" : "Export as MKV";
        const char* del = confirm ? "Click again to delete" : "Delete";
        char label[96];
        if (last.video) std::snprintf(label, sizeof(label), "%s", format_clock(last.duration).c_str());
        else std::snprintf(label, sizeof(label), "%dx%d", last.width, last.height);
        ImGui::PushFont(F.small);
        float lw = ImGui::CalcTextSize(label).x;
        ImGui::PopFont();
        float bp = button_width(icon::play, play), be = last.video ? button_width(icon::share, exp) : 0.0f,
              bd = button_width(icon::trash, del);
        float bh = px(40), gap = px(6);
        float bw = px(16) + lw + px(12) + bp + (be > 0 ? gap + be : 0.0f) + gap + bd + px(6);
        ImVec2 bpos(vp->Pos.x + (vp->Size.x - bw) * 0.5f, vp->Pos.y + vp->Size.y - px(28) - px(52) + (1.0f - bar_t) * px(24));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * bar_t);
        // Its own child window, created after the grid so it sits on top of it:
        // drawn on the gallery window, the grid would take every click.
        ImGui::SetCursorScreenPos(bpos);
        ImGui::BeginChild("##gbar", ImVec2(bw, px(64)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        ImDrawList* bl = ImGui::GetWindowDrawList();
        bl->AddRectFilled(bpos + ImVec2(0, px(8)), bpos + ImVec2(bw, px(60)), ImGui::GetColorU32(C.shadow, 0.6f), px(26));
        bl->AddRectFilled(bpos, bpos + ImVec2(bw, px(52)), ink(C.nav), px(26));
        bl->AddRect(bpos, bpos + ImVec2(bw, px(52)), ink(C.border), px(26));
        ImGui::PushFont(F.small);
        bl->AddText(bpos + ImVec2(px(16), (px(52) - ImGui::GetFontSize()) * 0.5f), ink(C.muted), label);
        ImGui::PopFont();
        float x = bpos.x + px(16) + lw + px(12), y = bpos.y + px(6);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (button("##gplay", icon::play, play, Kind::Primary, ImVec2(bp, bh), !writing))
            open_viewer(app, shown_paths, last.path);
        x += bp + gap;
        if (last.video) {
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (button("##gexport", icon::share, exp, Kind::Ghost, ImVec2(be, bh), !writing && !g_gallery.exporting))
                start_export(last);
            x += be + gap;
        }
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (button("##gdelete", icon::trash, del, Kind::Danger, ImVec2(bd, bh), !writing)) {
            if (!confirm) {
                g_gallery.pending_delete = last.path;
                g_gallery.pending_delete_time = ImGui::GetTime() + 3.0;
            } else {
                delete_item(last);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    draw_viewer(app);   // opens over the gallery (a popup inside this one)
    ImGui::PopStyleVar();   // fade
    ImGui::EndPopup();
}

#if SL_LIVE_STREAMING   // live streaming UI: off in this build (see SL_LIVE_STREAMING above)
// ---------------------------------------------------------------------------
// Live streaming state
// ---------------------------------------------------------------------------

struct StreamUi {
    glint::DestinationStore store;
    bool loaded = false;
    std::map<std::string, bool> has_key;     // vault lookups cached per destination
    std::string editing;                     // destination id being edited
    bool editing_new = false;
    char name[128] = {};
    char server[512] = {};
    char key[512] = {};
    std::string pending_remove;
    double pending_remove_until = 0;
    double golive_confirm_until = 0;
    // Account sign-in
    glint::AccountLinker linker;
    char twitch_id[128] = {};
    char youtube_id[256] = {};
    char youtube_secret[256] = {};
    bool youtube_has_secret = false;
};
static StreamUi g_stream;

static void stream_load()
{
    if (g_stream.loaded) return;
    std::string error;
    if (!g_stream.store.load(error)) ui::toast(error, 2);
    g_stream.has_key.clear();
    for (const auto& d : g_stream.store.items()) g_stream.has_key[d.id] = g_stream.store.has_key(d.id);
    glint::OAuthApps apps;
    if (glint::load_oauth_apps(apps, error)) {
        std::snprintf(g_stream.twitch_id, sizeof(g_stream.twitch_id), "%s", apps.twitch_client_id.c_str());
        std::snprintf(g_stream.youtube_id, sizeof(g_stream.youtube_id), "%s", apps.youtube_client_id.c_str());
    } else {
        ui::toast(error, 2);
    }
    std::string secret;
    g_stream.youtube_has_secret = glint::vault::read(glint::vault::target("OAuth", "youtube-client-secret"), secret);
    g_stream.loaded = true;
}

static void stream_save()
{
    std::string error;
    if (!g_stream.store.save(error)) ui::toast(error.empty() ? "Cannot save stream destinations" : error, 2);
}

static int enabled_destinations()
{
    int n = 0;
    for (const auto& d : g_stream.store.items()) n += d.enabled ? 1 : 0;
    return n;
}

// Turned-on destinations with their keys, as the broadcaster wants them.
static bool build_targets(const glint::DestinationStore& store, std::vector<glint::BroadcastTarget>& targets,
                          glint::BroadcastQuality& q, std::string& error)
{
    targets.clear();
    for (const auto& d : store.items()) {
        if (!d.enabled) continue;
        std::string key = store.key(d.id);
        if (key.empty()) {
            error = "A selected platform needs a stream key. Save its key or connect the account first.";
            return false;
        }
        if (d.server.rfind("rtmp://", 0) != 0 && d.server.rfind("rtmps://", 0) != 0) {
            error = d.name + ": the server must start with rtmp:// or rtmps://.";
            return false;
        }
        targets.push_back({d.name, glint::ingest_url(d.server, key)});
    }
    if (targets.empty()) {
        error = "Select at least one streaming destination first.";
        return false;
    }
    if (targets.size() > 5) {
        error = "Select at most five simultaneous destinations.";
        return false;
    }
    q.height = store.quality.height;
    q.fps = store.quality.fps;
    q.video_kbps = store.quality.video_kbps;
    return true;
}

static void go_live(glint::App& app)
{
    stream_load();
    std::vector<glint::BroadcastTarget> targets;
    glint::BroadcastQuality q;
    std::string error;
    if (!build_targets(g_stream.store, targets, q, error)) {
        ui::toast(error, 2);
        return;
    }
    glint::logf("broadcast: go live to %zu destination(s), %dp%d %d kbps", targets.size(), q.height, q.fps, q.video_kbps);
    if (app.start_broadcast(targets, q, error)) {
        ui::toast("Going live to " + std::to_string(targets.size()) + " destination" + (targets.size() == 1 ? "" : "s"), 0);
    } else {
        ui::toast(error, 2);
    }
}

// Going live is public: the first click arms, the second (within 3 s) starts.
static void go_live_or_end(glint::App& app)
{
    if (app.broadcast_status().active) {
        app.stop_broadcast("Live broadcast ended.");
        glint::logf("broadcast: ended by the user");
        ui::toast("Live broadcast ended", 1);
        return;
    }
    double now = ImGui::GetTime();
    if (now < g_stream.golive_confirm_until) {
        g_stream.golive_confirm_until = 0;
        go_live(app);
    } else {
        g_stream.golive_confirm_until = now + 3.0;
    }
}

static std::string host_of(const std::string& server)
{
    size_t at = server.find("://");
    std::string rest = at == std::string::npos ? server : server.substr(at + 3);
    size_t slash = rest.find('/');
    return slash == std::string::npos ? rest : rest.substr(0, slash);
}

static void begin_edit(const glint::StreamDestination& d, bool is_new)
{
    g_stream.editing = d.id;
    g_stream.editing_new = is_new;
    std::snprintf(g_stream.name, sizeof(g_stream.name), "%s", d.name.c_str());
    std::snprintf(g_stream.server, sizeof(g_stream.server), "%s", d.server.c_str());
    std::memset(g_stream.key, 0, sizeof(g_stream.key));
}

static void cancel_edit()
{
    if (g_stream.editing_new) g_stream.store.remove(g_stream.editing);
    g_stream.editing.clear();
    g_stream.editing_new = false;
    std::memset(g_stream.key, 0, sizeof(g_stream.key));
}

static void save_edit()
{
    glint::StreamDestination* d = g_stream.store.find(g_stream.editing);
    if (!d) return;
    std::string server = g_stream.server;
    while (!server.empty() && (server.back() == ' ' || server.back() == '/')) server.pop_back();
    if (server.rfind("rtmp://", 0) != 0 && server.rfind("rtmps://", 0) != 0) {
        ui::toast("The server must start with rtmp:// or rtmps://.", 2);
        return;
    }
    bool has = g_stream.has_key[d->id];
    if (!g_stream.key[0] && !has) {
        ui::toast("Paste your stream key here first.", 2);
        return;
    }
    if (g_stream.key[0]) {
        std::string error;
        if (!g_stream.store.set_key(d->id, g_stream.key, error)) {
            ui::toast(error, 2);
            return;
        }
        g_stream.has_key[d->id] = true;
    }
    d->name = g_stream.name[0] ? g_stream.name : glint::platform_label(d->platform);
    d->server = server;
    if (g_stream.editing_new && enabled_destinations() > 5) d->enabled = false;
    stream_save();
    ui::toast("Saved " + d->name, 1);
    g_stream.editing.clear();
    g_stream.editing_new = false;
    std::memset(g_stream.key, 0, sizeof(g_stream.key));   // don't keep the key around in UI memory
}

static void destination_editor(const glint::StreamDestination& d)
{
    using namespace ui;
    float iw = px(360);
    row_begin("Name", "Shown in Glint only.", iw);
    input("##dname", g_stream.name, sizeof(g_stream.name), iw);
    row_end();
    const char* server_desc = d.platform == "tiktok" ? "Copy the server URL from TikTok LIVE Studio."
                            : d.platform == "custom" ? "Any rtmp:// or rtmps:// ingest URL."
                                                     : "Filled in for you. Change it only to use a nearby ingest.";
    row_begin("Server", server_desc, iw);
    input("##dserver", g_stream.server, sizeof(g_stream.server), iw);
    row_end();
    bool has = g_stream.has_key[d.id];
    row_begin("Stream key", has ? "A key is saved. Leave this empty to keep it." : "Paste your stream key here.", iw);
    input("##dkey", g_stream.key, sizeof(g_stream.key), iw, true, ImGuiInputTextFlags_Password);
    row_end();
    float sw = button_width(icon::check, "Save"), cw = button_width(nullptr, "Cancel");
    row_begin("", "Keys are stored in Windows Credential Manager, never in a file or the log.", sw + cw + px(10));
    if (button("##dsave", icon::check, "Save", Kind::Primary, ImVec2(sw, 0))) save_edit();
    ImGui::SameLine(0, px(10));
    if (button("##dcancel", nullptr, "Cancel", Kind::Secondary, ImVec2(cw, 0))) cancel_edit();
    row_end();
}

static void save_oauth_apps_from_ui()
{
    glint::OAuthApps apps;
    apps.twitch_client_id = g_stream.twitch_id;
    apps.youtube_client_id = g_stream.youtube_id;
    std::string error;
    if (!glint::save_oauth_apps(apps, error)) ui::toast(error, 2);
}

// A finished sign-in becomes (or refreshes) a destination with its key.
static void stream_poll_link()
{
    glint::AccountLink result;
    if (!g_stream.linker.take_result(result)) return;
    stream_load();
    std::string platform = g_stream.linker.platform();
    glint::StreamDestination* d = nullptr;
    for (auto& x : g_stream.store.items())
        if (x.platform == platform && x.server == result.server) d = &x;
    if (!d) {
        d = &g_stream.store.add(platform);
        if (enabled_destinations() > 5) d->enabled = false;
    }
    d->name = platform == "twitch" ? "Twitch (" + result.account + ")" : "YouTube";
    d->server = result.server;
    std::string error;
    if (!g_stream.store.set_key(d->id, result.stream_key, error)) {
        ui::toast(error, 2);
        return;
    }
    g_stream.has_key[d->id] = true;
    stream_save();
    ui::toast(result.message, 1);
}

static void accounts_card()
{
    using namespace ui;
    glint::AccountLink link = g_stream.linker.snapshot();
    std::string who = g_stream.linker.platform();
    bool busy = g_stream.linker.busy();
    using L = glint::AccountLink::State;

    // Twitch: device code.
    {
        bool mine = who == "twitch" && link.state != L::Idle;
        std::string desc = mine ? link.message
                                : "Fetches your stream key with your own app's Client ID (create one at dev.twitch.tv/console).";
        if (mine && link.state == L::WaitingForUser) {
            float cw = px(150), ow = button_width(nullptr, "Open Twitch"), xw = button_width(nullptr, "Cancel");
            row_begin("Twitch", ("Enter this code at " + link.verification_uri + ".").c_str(), cw + ow + xw + px(20), 36);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, p + ImVec2(cw, px(36)), ink(C.accent_soft), px(9));
            ImGui::PushFont(F.strong);
            ImVec2 ts = ImGui::CalcTextSize(link.user_code.c_str());
            dl->AddText(p + ImVec2((cw - ts.x) * 0.5f, (px(36) - ts.y) * 0.5f), ink(C.text), link.user_code.c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(cw, px(36)));
            ImGui::SameLine(0, px(10));
            if (button("##tw_open", nullptr, "Open Twitch", Kind::Primary, ImVec2(ow, 0)))
                glint::open_with_default_app(link.verification_uri);
            ImGui::SameLine(0, px(10));
            if (button("##tw_cancel", nullptr, "Cancel", Kind::Secondary, ImVec2(xw, 0))) g_stream.linker.cancel();
            row_end();
        } else {
            float iw = px(240), bw = button_width(nullptr, "Sign in with Twitch");
            row_begin("Twitch", desc.c_str(), iw + px(10) + bw);
            input("##tw_id", g_stream.twitch_id, sizeof(g_stream.twitch_id), iw, !busy);
            if (ImGui::IsItemDeactivatedAfterEdit()) save_oauth_apps_from_ui();
            ImGui::SameLine(0, px(10));
            if (button("##tw_go", nullptr, "Sign in with Twitch", Kind::Secondary, ImVec2(bw, 0), !busy)) {
                save_oauth_apps_from_ui();
                std::string error;
                if (!g_stream.linker.start_twitch(g_stream.twitch_id, error)) ui::toast(error, 2);
            }
            tip("Client ID of your registered Twitch application");
            row_end();
        }
        if (mine && link.state == L::Failed) card_note(C.danger, link.error.c_str());
    }

    // YouTube: browser sign-in to a loopback address.
    {
        bool mine = who == "youtube" && link.state != L::Idle;
        if (mine && (link.state == L::WaitingForUser || link.state == L::Starting || link.state == L::Fetching)) {
            float xw = button_width(nullptr, "Cancel");
            row_begin("YouTube", link.message.c_str(), xw);
            if (button("##yt_cancel", nullptr, "Cancel", Kind::Secondary, ImVec2(xw, 0))) g_stream.linker.cancel();
            row_end();
        } else {
            float iw = px(240), sw = px(170), bw = button_width(nullptr, "Sign in with YouTube");
            row_begin("YouTube",
                      mine ? link.message.c_str()
                           : "Reads your stream's ingest address and key. Needs a Google desktop OAuth client (ID and secret) "
                             "with the YouTube Data API enabled.",
                      iw + sw + bw + px(20));
            input("##yt_id", g_stream.youtube_id, sizeof(g_stream.youtube_id), iw, !busy);
            if (ImGui::IsItemDeactivatedAfterEdit()) save_oauth_apps_from_ui();
            tip("Client ID");
            ImGui::SameLine(0, px(10));
            input("##yt_secret", g_stream.youtube_secret, sizeof(g_stream.youtube_secret), sw, !busy,
                  ImGuiInputTextFlags_Password);
            if (ImGui::IsItemDeactivatedAfterEdit() && g_stream.youtube_secret[0]) {
                std::string error;
                if (glint::vault::write(glint::vault::target("OAuth", "youtube-client-secret"), g_stream.youtube_secret, error))
                    g_stream.youtube_has_secret = true;
                else
                    ui::toast(error, 2);
                std::memset(g_stream.youtube_secret, 0, sizeof(g_stream.youtube_secret));
            }
            tip(g_stream.youtube_has_secret ? "Client secret saved (type to replace it)" : "Client secret");
            ImGui::SameLine(0, px(10));
            if (button("##yt_go", nullptr, "Sign in with YouTube", Kind::Secondary, ImVec2(bw, 0), !busy)) {
                save_oauth_apps_from_ui();
                std::string secret;
                glint::vault::read(glint::vault::target("OAuth", "youtube-client-secret"), secret);
                std::string error;
                if (!g_stream.linker.start_youtube(g_stream.youtube_id, secret, error)) ui::toast(error, 2);
            }
            row_end();
        }
        if (mine && link.state == L::Failed) card_note(C.danger, link.error.c_str());
    }
    card_note(C.muted, "Signing in creates or updates a destination with the fetched key. Tokens and secrets are kept in "
                       "Windows Credential Manager.");
}

static void page_streaming(glint::App& app)
{
    using namespace ui;
    stream_load();
    glint::BroadcastStatus st = app.broadcast_status();
    bool live = st.active;
    page_header("Live streaming",
                "Send the game picture and console sound to Twitch, YouTube or any RTMP server, up to five at once. "
                "Glint's own controls are never sent.");

    section("Broadcast");
    card_begin();
    {
        int n = enabled_destinations();
        bool confirming = ImGui::GetTime() < g_stream.golive_confirm_until;
        const char* label = live ? "End broadcast" : (confirming ? "Click again to go live" : "Go live");
        float bw = button_width(live ? icon::stop : icon::stream, label);
        std::string desc;
        if (live) {
            int up = 0;
            for (const auto& d : st.destinations) up += d.state == glint::BroadcastDestinationStatus::State::Live;
            desc = "Live for " + format_clock(st.seconds) + " on " + std::to_string(up) + " of " +
                   std::to_string(st.destinations.size()) + " destination" + (st.destinations.size() == 1 ? "" : "s") + ".";
        } else if (st.stopping) {
            desc = "Closing the connections...";
        } else if (!app.connected()) {
            desc = "Connect to a console before starting a live broadcast.";
        } else if (n == 0) {
            desc = "Turn on at least one destination below.";
        } else {
            desc = "Starts every destination that is turned on (" + std::to_string(n) + ").";
        }
        row_begin("Go live", desc.c_str(), bw);
        if (button("##golive", live ? icon::stop : icon::stream, label, live ? Kind::Danger : Kind::Primary,
                   ImVec2(bw, 0), live || (app.connected() && n > 0 && !st.stopping)))
            go_live_or_end(app);
        row_end();

        for (const auto& d : st.destinations) {
            using S = glint::BroadcastDestinationStatus::State;
            ImU32 fg = d.state == S::Live ? C.success : d.state == S::Connecting ? C.warn : d.state == S::Failed ? C.danger : C.faint;
            ImU32 bg = d.state == S::Live ? C.success_soft : d.state == S::Connecting ? C.warn_soft
                     : d.state == S::Failed ? C.danger_soft : C.nav_active;
            char line[300];
            if (d.state == S::Failed)
                std::snprintf(line, sizeof(line), "%s", d.error.c_str());
            else
                std::snprintf(line, sizeof(line), "%.0f kbps sent, %.1f MB%s%s", d.kbps, d.bytes / 1048576.0,
                              d.dropped_packets ? ", some packets dropped (slow upload)" : "",
                              d.state == S::Live ? (", live " + format_clock(d.live_seconds)).c_str() : "");
            const char* name = glint::broadcast_state_name(d.state);
            row_begin(d.name.c_str(), line, badge_width(name), 24);
            badge(name, fg, bg);
            row_end();
        }
        if (live || st.stopping) {
            char note[200];
            const char* enc = st.encoder == "h264_amf" ? "AMD AMF (GPU)" : st.encoder == "h264_mf" ? "Media Foundation"
                            : st.encoder == "libx264" ? "x264 (CPU)" : "starting";
            std::snprintf(note, sizeof(note), "Encoder %s  -  %dx%d at %d fps  -  %llu frames skipped", enc, st.width,
                          st.height, st.fps, (unsigned long long)st.frames_dropped);
            card_note(C.muted, note);
        }
        if (!st.error.empty()) card_note(C.danger, st.error.c_str());
    }
    card_end();

    section("Destinations");
    card_begin();
    {
        auto& items = g_stream.store.items();
        if (items.empty())
            row_begin("No destinations yet", "Add a platform below, then paste its stream key.", 0, 20), row_end();
        for (size_t i = 0; i < items.size(); i++) {
            glint::StreamDestination& d = items[i];
            ImGui::PushID(d.id.c_str());
            if (g_stream.editing == d.id) {
                destination_editor(d);
                ImGui::PopID();
                continue;
            }
            bool has = g_stream.has_key[d.id];
            std::string desc = std::string(glint::platform_label(d.platform)) + "  -  " +
                               (d.server.empty() ? "no server yet" : host_of(d.server)) + "  -  " +
                               (has ? "key saved" : "no stream key");
            bool confirm = g_stream.pending_remove == d.id && ImGui::GetTime() < g_stream.pending_remove_until;
            float ib = px(36);
            float rw = confirm ? button_width(icon::trash, "Remove?") : ib;
            row_begin(d.name.c_str(), desc.c_str(), px(40) + px(14) + ib + px(10) + rw);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(p + ImVec2(0, px(7)));
            bool on = d.enabled;
            if (toggle("##on", &on, !live)) {
                if (on && enabled_destinations() >= 5) {
                    ui::toast("Five destinations is the most you can broadcast to at once.", 2);
                } else {
                    d.enabled = on;
                    stream_save();
                }
            }
            tip(live ? "Stop broadcasting to change it." : "Include in the next broadcast");
            ImGui::SetCursorScreenPos(p + ImVec2(px(40) + px(14), 0));
            if (button("##edit", icon::settings, nullptr, Kind::Ghost, ImVec2(ib, 0), !live && g_stream.editing.empty()))
                begin_edit(d, false);
            tip("Edit destination");
            ImGui::SameLine(0, px(10));
            if (button("##remove", icon::trash, confirm ? "Remove?" : nullptr, confirm ? Kind::Danger : Kind::Ghost,
                       ImVec2(rw, 0), !live && g_stream.editing.empty())) {
                if (!confirm) {
                    g_stream.pending_remove = d.id;
                    g_stream.pending_remove_until = ImGui::GetTime() + 3.0;
                } else {
                    std::string name = d.name;
                    g_stream.store.remove(d.id);
                    g_stream.has_key.erase(g_stream.pending_remove);
                    g_stream.pending_remove.clear();
                    stream_save();
                    ui::toast("Removed " + name + " and its stream key", 1);
                    ImGui::PopID();
                    row_end();
                    break;
                }
            }
            if (!confirm) tip("Remove this destination");
            row_end();
            ImGui::PopID();
        }

        static const char* platforms[] = {"twitch", "youtube", "tiktok", "custom"};
        static const char* labels[] = {"Twitch", "YouTube", "TikTok", "Custom"};
        float total = 0;
        for (const char* l : labels) total += button_width(icon::add, l) + px(8);
        row_begin("Add a destination", g_stream.editing.empty() ? "Pick a platform; the server is filled in when known."
                                                               : "Save or cancel the destination you are editing.",
                  total - px(8));
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine(0, px(8));
            ImGui::PushID(i);
            if (button("##add", icon::add, labels[i], Kind::Secondary, ImVec2(button_width(icon::add, labels[i]), 0),
                       !live && g_stream.editing.empty())) {
                glint::StreamDestination& d = g_stream.store.add(platforms[i]);
                g_stream.has_key[d.id] = false;
                if (enabled_destinations() > 5) d.enabled = false;
                begin_edit(d, true);
            }
            ImGui::PopID();
        }
        row_end();
    }
    card_end();

    section("Accounts");
    card_begin();
    accounts_card();
    card_end();

    section("Quality");
    card_begin();
    {
        glint::StreamQuality& q = g_stream.store.quality;
        bool changed = false;
        static const char* res[] = {"480p", "720p", "1080p"};
        static const int heights[] = {480, 720, 1080};
        int ri = q.height == 480 ? 0 : q.height == 720 ? 1 : 2;
        row_begin("Resolution", live ? "Stop broadcasting to change it." : "Scaled from the console picture.",
                  segmented_width(res, 3, px(72)));
        if (segmented("##bres", &ri, res, 3, px(72), !live)) { q.height = heights[ri]; changed = true; }
        row_end();
        static const char* rates[] = {"30 fps", "60 fps"};
        int fi = q.fps == 30 ? 0 : 1;
        row_begin("Frame rate", nullptr, segmented_width(rates, 2, px(72)));
        if (segmented("##bfps", &fi, rates, 2, px(72), !live)) { q.fps = fi == 0 ? 30 : 60; changed = true; }
        row_end();
        float kbps = (float)q.video_kbps;
        row_begin("Video bitrate", "Higher looks cleaner but needs a faster upload.", px(300), 26);
        if (slider("##bkbps", &kbps, 1000.0f, 12000.0f, "%.0f kbps", 1.0f, px(300), !live)) {
            q.video_kbps = (int)(std::lround(kbps / 250.0f) * 250);
            changed = true;
        }
        row_end();
        card_note(C.muted, "6000 kbps at 1080p60 suits most connections. Encoding uses the GPU when it can (AMD AMF or "
                           "Media Foundation) and falls back to x264 on the CPU. Keyframe every 2 s, AAC 160 kbps sound.");
        // Save once the change is complete (a slider drag saves on release).
        static bool pending = false;
        pending |= changed;
        if (pending && !ImGui::IsMouseDown(0)) {
            stream_save();
            pending = false;
        }
    }
    card_end();
}
#endif // SL_LIVE_STREAMING

static float keycaps_width(const char* const* keys, int count)
{
    using namespace ui;
    ImGui::PushFont(F.small);
    float w = 0;
    for (int i = 0; i < count; i++) w += std::max(ImGui::CalcTextSize(keys[i]).x + px(14), px(26)) + (i ? px(6) : 0.0f);
    ImGui::PopFont();
    return w;
}

static void keycaps(const char* const* keys, int count)
{
    using namespace ui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(F.small);
    for (int i = 0; i < count; i++) {
        if (i) ImGui::SameLine(0, px(6));
        ImVec2 ts = ImGui::CalcTextSize(keys[i]);
        ImVec2 size(std::max(ts.x + px(14), px(26)), px(26));
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, p + size, ink(C.input), px(6));
        dl->AddRect(p, p + size, ink(C.border_hi), px(6));
        dl->AddLine(p + ImVec2(px(3), size.y - 1), p + ImVec2(size.x - px(3), size.y - 1), ink(C.border_hi));
        dl->AddText(p + ImVec2((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f), ink(C.text), keys[i]);
        ImGui::Dummy(size);
    }
    ImGui::PopFont();
}

static std::string system_info(glint::App& app)
{
    std::string out = "Glint " GLINT_VERSION "\n" + glint::windows_version() + "\n";
    VkPhysicalDeviceProperties props = {};
    if (g_PhysicalDevice) {
        vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
        char gpu[320];
        std::snprintf(gpu, sizeof(gpu), "GPU: %s (Vulkan %u.%u.%u, driver 0x%08x)\n", props.deviceName,
                      VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                      VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
        out += gpu;
    }
    auto config = app.config();
    auto status = app.status();
    glint::AudioStats a = app.audio().stats();
    glint::BroadcastStatus b = app.broadcast_status();
    char line[1024];
    float rx_fps = 0.0f, rx_kbps = 0.0f;
    app.received_rate(rx_fps, rx_kbps);
    std::snprintf(line, sizeof(line),
                  "Console: %s, %s, %dx%d@%d %s, received %.1f fps %.0f kbps (console reports %.1f fps %.0f kbps "
                  "since its encoder started), %d viewer(s)\n"
                  "Decoded frames: %llu, picture held %.0f ms\n"
                  "Audio: %s, device '%s', %s, buffer %.0f/%.0f ms (%s), playout %.0f ms, underruns %llu, drops %llu\n"
                  "Broadcast: %s, encoder %s, %zu destination(s)\n",
                  app.url().c_str(), app.connected() ? "connected" : "not connected", config.width, config.height,
                  config.fps, config.codec.c_str(), rx_fps, rx_kbps, status.fps, status.kbps, status.clients,
                  (unsigned long long)app.frame_count(), app.video_hold_ms(), a.state.c_str(), a.device.c_str(),
                  a.format.c_str(), a.buffered_ms, a.target_ms, a.auto_delay ? "auto" : "manual", a.playout_ms,
                  (unsigned long long)a.underruns, (unsigned long long)a.dropped_frames,
                  b.active ? "live" : "off", b.encoder.empty() ? "-" : b.encoder.c_str(), b.destinations.size());
    out += line;
    out += "Settings folder: " + glint::settings_dir() + "\nMedia folder: " + app.media_dir() + "\n";
    out += "Stream keys, tokens and OAuth settings are not included in this file.\n";
    return out;
}

static void page_help(glint::App& app)
{
    using namespace ui;
    page_header("Help", "Your settings and logs, and the keyboard shortcuts.");
    section("Diagnostics");
    card_begin();
    {
        float ow = button_width(icon::folder, "Open");
        std::string dir = glint::settings_dir();
        row_begin("Settings folder", dir.c_str(), ow);
        if (button("##open_settings", icon::folder, "Open", Kind::Secondary, ImVec2(ow, 0)))
            glint::open_with_default_app(dir);
        row_end();
        std::string log = glint::log_path();
        row_begin("Log", log.empty() ? "Not open." : log.c_str(), ow);
        if (button("##open_log", icon::folder, "Open", Kind::Secondary, ImVec2(ow, 0), !log.empty()))
            glint::open_with_default_app(log);
        row_end();
        float cw = button_width(icon::share, "Create file");
        row_begin("Diagnostics file",
                  "A .zip with the logs, your settings and system details, saved in Downloads, for reporting a problem. "
                  "Stream keys, tokens and OAuth settings are never included.",
                  cw);
        if (button("##diag", icon::share, "Create file", Kind::Primary, ImVec2(cw, 0))) {
            std::string path, error;
            glint::logf("diagnostics requested");
            if (glint::create_diagnostics(glint::downloads_dir(), system_info(app), path, error)) {
                ui::toast("Saved " + base_name(path), 1);
                glint::reveal_in_explorer(path);
            } else {
                ui::toast(error, 2);
            }
        }
        row_end();
    }
    card_end();

    struct Item { const char* keys[2]; int n; const char* what; };
    static const Item player[] = {
        {{"M"}, 1, "Sound on or off"},
        {{"S"}, 1, "Save a screenshot"},
        {{"R"}, 1, "Start or stop recording on this PC"},
        {{"P"}, 1, "Picture size: fit, fill, no upscaling"},
        {{"L"}, 1, "Latency: lowest, balanced, smooth"},
        {{"H"}, 1, "Hide or show the controls"},
        {{"G"}, 1, "Open or close the gallery"},
        {{"F11"}, 1, "Full screen"},
    };
    static const Item app_items[] = {
        {{"Ctrl", ","}, 2, "Open or close Settings"},
        {{"T"}, 1, "Dark or light theme"},
        {{"Esc"}, 1, "Close Settings or the gallery, or leave full screen"},
    };
    section("While watching");
    card_begin();
    for (const Item& it : player) {
        row_begin(it.what, nullptr, keycaps_width(it.keys, it.n), 26);
        keycaps(it.keys, it.n);
        row_end();
    }
    card_end();
    section("Anywhere");
    card_begin();
    for (const Item& it : app_items) {
        row_begin(it.what, nullptr, keycaps_width(it.keys, it.n), 26);
        keycaps(it.keys, it.n);
        row_end();
    }
    card_end();
}

// Settings: a sheet that slides in from the right over a dimmed picture, so
// picture and latency changes show while you make them. Click the picture or
// press Esc to close.
static void draw_settings(glint::App& app)
{
    using namespace ui;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    step_towards(g_shell.settings_t, g_shell.settings_open, g_shell.settings_open ? 0.28f : 0.2f);
    bool popup_open = ImGui::IsPopupOpen("##settings");
    if (g_shell.settings_t <= 0.0f && !popup_open) return;
    if (!popup_open) ImGui::OpenPopup("##settings");

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    bool open = ImGui::BeginPopupModal("##settings", nullptr,
                                       ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!open) {
        g_shell.settings_open = false;
        return;
    }
    if (g_shell.settings_t <= 0.0f) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    float e = ease_out(g_shell.settings_t);
    float w = std::min(px(600), vp->Size.x);
    float x0 = vp->Pos.x + vp->Size.x - w * e;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(vp->Pos, vp->Pos + vp->Size, ImGui::GetColorU32(C.scrim, e));
    // Soft shadow along the sheet's left edge.
    for (int i = 1; i <= 4; i++)
        dl->AddRectFilled(ImVec2(x0 - px(6) * i, vp->Pos.y), ImVec2(x0, vp->Pos.y + vp->Size.y),
                          ImGui::GetColorU32(C.shadow, 0.06f * e));
    if (g_shell.settings_open && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::GetIO().MousePos.x < x0 && ImGui::IsWindowHovered())
        g_shell.settings_open = false;

    ImGui::SetCursorScreenPos(ImVec2(x0, vp->Pos.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(C.panel));
    ImGui::BeginChild("##sheet", ImVec2(w, vp->Size.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImVec2 origin = ImGui::GetWindowPos();
    ImDrawList* sl = ImGui::GetWindowDrawList();
    sl->AddLine(origin, origin + ImVec2(0, vp->Size.y), ink(C.border));

    // Header.
    ImGui::SetCursorScreenPos(origin + ImVec2(px(24), px(22)));
    ImGui::PushFont(F.title);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(origin + ImVec2(w - px(24) - px(40), px(14)));
    if (button("##close", icon::close, F.icons ? nullptr : "Close", Kind::Ghost, ImVec2(F.icons ? px(40) : px(70), px(40))))
        g_shell.settings_open = false;
    tip("Close  (Esc)");

    // Tabs: text on a hairline, no rounding. The active tab is full-strength
    // with a 2 px underline.
#if SL_LIVE_STREAMING
    static const char* tabs[PageCount] = {"Console", "Video", "Picture", "Audio", "Capture", "Streaming", "Help"};
#else
    static const char* tabs[PageCount] = {"Console", "Video", "Picture", "Audio", "Capture", "Help"};
#endif
    ImVec2 tp = origin + ImVec2(px(24), px(64));
    float th = px(44);
    sl->AddLine(ImVec2(origin.x, tp.y + th), ImVec2(origin.x + w, tp.y + th), ink(C.border));
    ImGui::PushFont(F.label);
    // Equal cells across the sheet's content width; the underline spans the
    // active cell and slides to the next one.
    float cell = (w - px(48)) / PageCount;
    float tab_x[PageCount], tab_w[PageCount];
    for (int i = 0; i < PageCount; i++) {
        tab_x[i] = tp.x + i * cell;
        tab_w[i] = cell;
    }
    float ux = anim(ImGui::GetID("##tabx"), tab_x[g_shell.page], 18.0f);
    sl->AddRectFilled(ImVec2(ux, tp.y + th - px(2)), ImVec2(ux + cell, tp.y + th), ink(C.text));
    for (int i = 0; i < PageCount; i++) {
        ImVec2 cp(tab_x[i], tp.y);
        ImGui::SetCursorScreenPos(cp);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tab_w[i], th)) && g_shell.page != i) {
            g_shell.page = i;
            g_shell.page_shown_at = ImGui::GetTime();
        }
        bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        float hv = anim(ImGui::GetItemID(), hovered ? 1.0f : 0.0f);
        ImGui::PopID();
        bool active = g_shell.page == i;
        ImVec2 ts = ImGui::CalcTextSize(tabs[i]);
        sl->AddText(cp + ImVec2((tab_w[i] - ts.x) * 0.5f, (th - ts.y) * 0.5f - px(1)),
                    ink(active ? C.text : mix(C.muted, C.text, hv)), tabs[i]);
    }
    ImGui::PopFont();

    // Content, which fades and slides in a little when the tab changes.
    float footer_h = px(44);
    float top = px(64) + th + px(1);
    float since = (float)(ImGui::GetTime() - g_shell.page_shown_at);
    float fade = ease_out(std::clamp(since / 0.22f, 0.0f, 1.0f));
    ImGui::SetCursorScreenPos(origin + ImVec2(0, top + (1.0f - fade) * px(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(16)));
    ImGui::BeginChild("##content", ImVec2(w, vp->Size.y - top - footer_h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    static int scrolled_page = -1;   // a new tab starts at the top
    smooth_scroll(scrolled_page != g_shell.page);
    scrolled_page = g_shell.page;
    switch (g_shell.page) {
    case PageConsole: page_console(app); break;
    case PageVideo: page_video(app); break;
    case PagePicture: page_picture(app); break;
    case PageAudio: page_audio(app); break;
    case PageCapture: page_capture(app); break;
#if SL_LIVE_STREAMING
    case PageStreaming: page_streaming(app); break;
#endif
    default: page_help(app); break;
    }
    ImGui::Dummy(ImVec2(0, px(16)));
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Footer.
    float fy = vp->Size.y - footer_h;
    sl->AddLine(origin + ImVec2(0, fy), origin + ImVec2(w, fy), ink(C.border));
    ImGui::PushFont(F.small);
    float fs = ImGui::GetFontSize();
    float mw = draw_bolt(sl, origin + ImVec2(px(24), fy + px(14) + fs * 0.1f), fs * 0.85f, ink(C.faint));
    sl->AddText(origin + ImVec2(px(24) + mw + px(6), fy + px(14)), ink(C.faint), "Glint " GLINT_VERSION);
    const char* note = "Changes apply right away";
    sl->AddText(origin + ImVec2(w - px(24) - ImGui::CalcTextSize(note).x, fy + px(14)), ink(C.faint), note);
    ImGui::PopFont();

    ImGui::EndChild();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Player view
// ---------------------------------------------------------------------------

// Side by side: two panels showing the same part of the picture, the original
// on the left and the enhanced one on the right. Scroll zooms around the
// pointer, dragging pans, a double-click goes back to the whole picture.
static void draw_compare(const VideoTexture& texture, ImVec2 pos, ImVec2 size)
{
    using namespace ui;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(pos, pos + size, IM_COL32(0, 0, 0, 255));
    Compare& c = g_compare;
    const float gap = px(2);
    const float pw = (size.x - gap) * 0.5f, ph = size.y;
    const float aspect = (float)texture.width / texture.height;
    // The whole picture's size on screen at this zoom.
    const float fit_w = std::min(pw, ph * aspect), fit_h = fit_w / aspect;
    const float W = fit_w * c.zoom, H = fit_h * c.zoom;
    // The part of it a panel shows (UV), and where that sits in the panel.
    const float uw = std::min(1.0f, pw / W), uh = std::min(1.0f, ph / H);
    c.center.x = std::clamp(c.center.x, uw * 0.5f, 1.0f - uw * 0.5f);
    c.center.y = std::clamp(c.center.y, uh * 0.5f, 1.0f - uh * 0.5f);
    const ImVec2 uv0(c.center.x - uw * 0.5f, c.center.y - uh * 0.5f), uv1(c.center.x + uw * 0.5f, c.center.y + uh * 0.5f);
    const ImVec2 shown(uw * W, uh * H);
    const ImVec2 left = pos + ImVec2((pw - shown.x) * 0.5f, (ph - shown.y) * 0.5f);
    const ImVec2 right = left + ImVec2(pw + gap, 0);

    // The enhancement upscales to the zoomed size, so a zoomed-in view shows
    // what the upscaler really does at that size (capped at 4K by the passes).
    ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
    g_enh.want_w = (int)std::lround(W * fb.x);
    g_enh.want_h = (int)std::lround(H * fb.y);

    dl->AddImage(texture.im_id(), left, left + shown, uv0, uv1);
    bool enhanced = g_enh.shown && g_enh.out_descriptor;
    dl->AddImage(enhanced ? (ImTextureID)(intptr_t)g_enh.out_descriptor : texture.im_id(), right, right + shown, uv0,
                 uv1);
    dl->AddRectFilled(pos + ImVec2(pw, 0), pos + ImVec2(pw + gap, ph), ink(C.glass_border));

    // Labels.
    char label[64];
    if (!enhanced) std::snprintf(label, sizeof(label), "Enhanced: Picture is Off");
    else if (g_enh.used_upscaler >= 0) std::snprintf(label, sizeof(label), "Enhanced  -  %s", kUpscalerNames[g_enh.used_upscaler]);
    else std::snprintf(label, sizeof(label), "Enhanced");
    char zoom[160];
    std::snprintf(zoom, sizeof(zoom), "%.1fx  -  scroll to zoom, drag to move, double-click to reset  -  X to tweak, C to leave", c.zoom);
    ImGui::PushFont(F.label);
    auto pill = [&](ImVec2 p, const char* text) {
        ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddRectFilled(p, p + ts + ImVec2(px(20), px(12)), ink(C.glass), px(8));
        dl->AddText(p + ImVec2(px(10), px(6)), ink(C.text), text);
    };
    // Below the status pill and the round buttons; the hint at the top centre,
    // clear of the command bar.
    pill(pos + ImVec2(px(20), px(80)), "Original");
    pill(pos + ImVec2(pw + gap + px(20), px(80)), label);
    ImVec2 zs = ImGui::CalcTextSize(zoom);
    pill(pos + ImVec2((size.x - zs.x) * 0.5f - px(10), px(22)), zoom);
    ImGui::PopFont();

    // Input, only over the picture (not over controls, Settings or the gallery).
    ImGuiIO& io = ImGui::GetIO();
    bool free = !g_shell.pointer_on_controls && !g_shell.settings_open && !g_shell.gallery_open && !g_viewer.open;
    ImVec2 m = io.MousePos;
    auto inside = [&](ImVec2 a) { return m.x >= a.x && m.y >= a.y && m.x < a.x + shown.x && m.y < a.y + shown.y; };
    bool over = free && (inside(left) || inside(right));
    if (over && io.MouseWheel != 0.0f) {
        ImVec2 o = inside(left) ? left : right;
        ImVec2 f((m.x - o.x) / shown.x, (m.y - o.y) / shown.y);   // where the pointer is, 0..1 of the view
        ImVec2 at(uv0.x + f.x * uw, uv0.y + f.y * uh);          // ...in picture UV: keep it under the pointer
        c.zoom = std::clamp(c.zoom * std::pow(1.25f, io.MouseWheel), 1.0f, 8.0f);
        float nw = std::min(1.0f, pw / (fit_w * c.zoom)), nh = std::min(1.0f, ph / (fit_h * c.zoom));
        c.center = ImVec2(at.x - (f.x - 0.5f) * nw, at.y - (f.y - 0.5f) * nh);
    }
    if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) c.dragging = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) c.dragging = false;
    if (c.dragging) {
        c.center.x -= io.MouseDelta.x / W;
        c.center.y -= io.MouseDelta.y / H;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (over && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        c.zoom = 1.0f;
        c.center = ImVec2(0.5f, 0.5f);
    }
}

static void draw_video(const VideoTexture& texture, ImVec2 pos, ImVec2 size)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!texture.descriptor || texture.width <= 0 || texture.height <= 0) {
        dl->AddRectFilled(pos, pos + size, ui::C.panel);   // the connect screen's ground
        return;
    }
    if (g_compare.on) {
        draw_compare(texture, pos, size);
        return;
    }
    dl->AddRectFilled(pos, pos + size, IM_COL32(0, 0, 0, 255));   // letterbox stays black in both themes
    float src = (float)texture.width / texture.height;
    float dst = size.x / size.y;
    ImVec2 uv0(0, 0), uv1(1, 1);
    ImVec2 a = pos, b = pos + size;
    if (g_shell.picture == 2 && texture.width <= size.x && texture.height <= size.y) {
        // No upscaling: the picture's own pixels, centred on whole pixels.
        a.x = std::floor(pos.x + (size.x - texture.width) * 0.5f);
        a.y = std::floor(pos.y + (size.y - texture.height) * 0.5f);
        b = ImVec2(a.x + texture.width, a.y + texture.height);
    } else if (g_shell.picture != 1) {
        if (dst > src) {
            float w = size.y * src;
            a.x = pos.x + (size.x - w) * 0.5f;
            b.x = a.x + w;
        } else {
            float h = size.x / src;
            a.y = pos.y + (size.y - h) * 0.5f;
            b.y = a.y + h;
        }
    } else {
        if (dst > src) {
            float vis = src / dst;   // visible fraction of height
            uv0.y = (1.0f - vis) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        } else {
            float vis = dst / src;
            uv0.x = (1.0f - vis) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        }
    }
    // The whole picture's size in screen pixels (Fill crops it): what the
    // enhancement passes upscale to for the next frame.
    ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
    g_enh.want_w = (int)std::lround((b.x - a.x) / std::max(1e-4f, uv1.x - uv0.x) * fb.x);
    g_enh.want_h = (int)std::lround((b.y - a.y) / std::max(1e-4f, uv1.y - uv0.y) * fb.y);
    if (g_enh.shown && g_enh.out_descriptor) dl->AddImage((ImTextureID)(intptr_t)g_enh.out_descriptor, a, b, uv0, uv1);
    else dl->AddImage(texture.im_id(), a, b, uv0, uv1);
}

// Glass pill over the picture with a coloured dot; returns its size.
static ImVec2 glass_pill(ImVec2 p, const char* text, ImU32 dot, float pulse = 0.0f)
{
    using namespace ui;
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 size(ts.x + px(42), px(36));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + size, ink(C.glass), size.y * 0.5f);
    dl->AddRect(p, p + size, ink(C.glass_border), size.y * 0.5f);
    ImVec2 c = p + ImVec2(px(18), size.y * 0.5f);
    if (pulse > 0.0f) dl->AddCircleFilled(c, px(4) + pulse * px(6), ImGui::GetColorU32(dot, 0.35f * (1.0f - pulse)), 20);
    dl->AddCircleFilled(c, px(4), ink(dot), 16);
    dl->AddText(p + ImVec2(px(30), (size.y - ts.y) * 0.5f), ink(C.text), text);
    return size;
}

// Status pill: console, picture, the frame strip (one bar per frame for the
// last second; a late frame grows and turns amber), hold and bitrate. Click it
// for the console settings.
static ImVec2 status_pill(glint::App& app, ImVec2 p, double now)
{
    using namespace ui;
    auto config = app.config();
    Link link = link_state(app);
    std::string name = config.platform.empty() ? "PS5" : config.platform;
    char picture[32] = "";
    if (link == Link::Live && config.height > 0) std::snprintf(picture, sizeof(picture), "%dp%d", config.height, config.fps);
    const char* state = link == Link::Connecting ? "Connecting"
                      : link == Link::Failed ? (watched_elsewhere(app) ? "Watching elsewhere" : "Can't reach the console")
                      : link == Link::Offline ? "Not connected" : picture;
    std::vector<float> late = link == Link::Live ? app.recent_lateness(60) : std::vector<float>();
    char hold[24] = "", rate[24] = "";
    if (link == Link::Live) {
        if (app.latency() != glint::App::Latency::Lowest) std::snprintf(hold, sizeof(hold), "%.0f ms", app.video_hold_ms());
        float rx_fps = 0.0f, rx_kbps = 0.0f;
        app.received_rate(rx_fps, rx_kbps);
        std::snprintf(rate, sizeof(rate), "%.1f Mbit/s", rx_kbps / 1000.0f);
    }

    float gap = px(12), strip_w = late.empty() ? 0.0f : px(3) * 60;
    ImGui::PushFont(F.strong);
    float name_w = ImGui::CalcTextSize(name.c_str()).x;
    ImGui::PopFont();
    ImGui::PushFont(F.small);
    float state_w = *state ? ImGui::CalcTextSize(state).x : 0.0f;
    ImGui::PopFont();
    ImGui::PushFont(F.mono);
    float hold_w = *hold ? ImGui::CalcTextSize(hold).x : 0.0f, rate_w = *rate ? ImGui::CalcTextSize(rate).x : 0.0f;
    ImGui::PopFont();
    float w = px(14) + px(8) + px(8) + name_w;
    for (float part : {state_w, strip_w, hold_w, rate_w})
        if (part > 0) w += gap + part;
    w += px(16);
    ImVec2 size(w, px(44));

    ImGui::SetCursorScreenPos(p);
    bool clicked = ImGui::InvisibleButton("##statuspill", size);
    bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float h = anim(ImGui::GetItemID(), hovered ? 1.0f : 0.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + size, ink(mix(C.glass, C.glass_hover, h)), size.y * 0.5f);
    dl->AddRect(p, p + size, ink(C.glass_border), size.y * 0.5f);

    float cy = p.y + size.y * 0.5f, x = p.x + px(14);
    ImU32 dot = link == Link::Live ? C.text : link == Link::Failed ? C.danger : link == Link::Connecting ? C.muted : C.faint;
    ImVec2 c(x + px(4), cy);
    if (link == Link::Connecting) {
        float pulse = (float)std::fmod(now, 1.2) / 1.2f;
        dl->AddCircleFilled(c, px(4) + pulse * px(6), ImGui::GetColorU32(dot, 0.4f * (1.0f - pulse)), 20);
    } else if (link == Link::Live) {
        dl->AddCircleFilled(c, px(7), ImGui::GetColorU32(dot, 0.15f), 20);
    }
    dl->AddCircleFilled(c, px(4), ink(dot), 16);
    x += px(16);
    ImGui::PushFont(F.strong);
    dl->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), ink(C.text), name.c_str());
    ImGui::PopFont();
    x += name_w;
    if (state_w > 0) {
        x += gap;
        ImGui::PushFont(F.small);
        dl->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), ink(link == Link::Failed ? C.danger : C.muted), state);
        ImGui::PopFont();
        x += state_w;
    }
    if (strip_w > 0) {
        x += gap;
        float base = cy + px(9);
        float sx = x + strip_w - late.size() * px(3);   // newest frame on the right
        for (float ms : late) {
            bool is_late = ms > 12.0f;
            float bh = is_late ? std::min(px(18), px(6) + ms * 0.25f * S) : px(3) + std::min(ms, 12.0f) * 0.25f * S;
            dl->AddRectFilled(ImVec2(sx, base - bh), ImVec2(sx + px(2), base),
                              is_late ? ink(C.warn) : ImGui::GetColorU32(C.text, 0.55f), px(1));
            sx += px(3);
        }
        x += strip_w;
    }
    ImGui::PushFont(F.mono);
    if (hold_w > 0) {
        x += gap;
        dl->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), ink(mix(C.muted, C.text, 0.5f)), hold);
        x += hold_w;
    }
    if (rate_w > 0) {
        x += gap;
        dl->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), ink(C.muted), rate);
    }
    ImGui::PopFont();
    if (clicked) {
        g_shell.page = PageConsole;
        g_shell.page_shown_at = ImGui::GetTime();
        g_shell.settings_open = true;
    }
    return size;
}

// No picture: connect to a console, or say what is happening. Plain on the
// ground, no card, like the mockup; it fades in when the state changes.
static void draw_empty_state(glint::App& app, ImVec2 center)
{
    using namespace ui;
    Link link = link_state(app);
    static Link shown = Link::Offline;
    static double changed_at = 0;
    if (link != shown) { shown = link; changed_at = ImGui::GetTime(); }
    float fade = ease_out(std::clamp((float)(ImGui::GetTime() - changed_at) / 0.25f, 0.0f, 1.0f));

    float w = std::min(px(440), ImGui::GetMainViewport()->Size.x - px(32));
    static float last_h = px(300);
    ImVec2 p(center.x - w * 0.5f, center.y - last_h * 0.5f + (1.0f - fade) * px(8));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
    ImGui::SetCursorScreenPos(p);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(p.x + w - ImGui::GetWindowPos().x);

    const char* title = link == Link::Live ? "Waiting for the picture"
                      : link == Link::Connecting ? "Connecting to your PS5" : "Connect to your PS5";
    {
        // The brand above the title: bolt and name.
        ImGui::PushFont(F.strong);
        ImVec2 at = ImGui::GetCursorScreenPos();
        float bh = px(22), fs = ImGui::GetFontSize();
        float bw = draw_bolt(ImGui::GetWindowDrawList(), at, bh, ink(C.text));
        ImGui::SetCursorScreenPos(at + ImVec2(bw + px(8), (bh - fs) * 0.5f));
        ImGui::TextUnformatted("Glint");
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(at + ImVec2(0, bh + px(18)));
    }
    ImGui::PushFont(F.display);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::PushFont(F.small);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C.muted));
    if (link == Link::Live)
        ImGui::TextWrapped("Connected. The first keyframe usually arrives within a second or two.");
    else if (link == Link::Connecting)
        ImGui::TextWrapped("Talking to the Glint payload at %s", g_shell.url);
    else
        ImGui::TextWrapped("Start the Glint payload on the console, then enter its address. The payload listens on port 7231.");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (link == Link::Connecting) {
        // Three dots that take turns, then a way out.
        ImGui::Dummy(ImVec2(0, px(10)));
        ImVec2 d = ImGui::GetCursorScreenPos() + ImVec2(px(4), px(4));
        for (int i = 0; i < 3; i++) {
            float t = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.0f - i * 0.8f);
            ImGui::GetWindowDrawList()->AddCircleFilled(d + ImVec2(i * px(14), 0), px(3) + t * px(1.5f),
                                                        ImGui::GetColorU32(C.text, 0.3f + 0.7f * t), 16);
        }
        ImGui::Dummy(ImVec2(0, px(18)));
        if (button("##cancel_connect", nullptr, "Cancel", Kind::Secondary)) app.stop();
    } else if (link != Link::Live) {
        ImGui::Dummy(ImVec2(0, px(14)));
        ImGui::PushFont(F.strong);
        ImGui::TextUnformatted("Console address");
        ImGui::PopFont();
        float bw = button_width(nullptr, "Connect", 18.0f);
        ImGui::PushFont(F.mono);
        bool enter = input("##url_empty", g_shell.url, sizeof(g_shell.url), w - bw - px(8), true,
                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopFont();
        ImGui::SameLine(0, px(8));
        if (button("##connect_empty", nullptr, "Connect", Kind::Primary, ImVec2(bw, ImGui::GetItemRectSize().y)) || enter)
            connect_to(app);
        if (link == Link::Failed) {
            bool elsewhere = watched_elsewhere(app);
            ImGui::PushFont(F.small);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(elsewhere ? C.muted : C.danger));
            if (F.icons) {
                ImGui::TextUnformatted(elsewhere ? icon::info : icon::warning);
                ImGui::SameLine(0, px(8));
            }
            if (elsewhere)
                ImGui::TextWrapped("Glint is open in a browser or on another PC, and the PS5 streams to one at a "
                                   "time. Connect to watch here.");
            else
                ImGui::TextWrapped("No answer from %s. Check that the Glint payload is running on the PS5.", g_shell.url);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        if (!g_shell.recent.empty()) {
            ImGui::Dummy(ImVec2(0, px(14)));
            ImGui::PushFont(F.small);
            text_colored(C.muted, "Recent");
            ImGui::PopFont();
            ImVec2 lp = ImGui::GetCursorScreenPos();
            float row_h = px(48);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            std::string pick;
            for (size_t i = 0; i < g_shell.recent.size(); i++) {
                ImVec2 rp = lp + ImVec2(0, i * row_h);
                ImGui::SetCursorScreenPos(rp);
                ImGui::PushID((int)i);
                if (ImGui::InvisibleButton("##recent", ImVec2(w, row_h))) pick = g_shell.recent[i];
                bool hovered = ImGui::IsItemHovered();
                if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                float hv = anim(ImGui::GetItemID(), hovered ? 1.0f : 0.0f);
                ImGui::PopID();
                ImDrawFlags corners = (i == 0 ? ImDrawFlags_RoundCornersTop : 0) |
                                      (i + 1 == g_shell.recent.size() ? ImDrawFlags_RoundCornersBottom : 0);
                if (hv > 0.01f)
                    dl->AddRectFilled(rp, rp + ImVec2(w, row_h), ImGui::GetColorU32(C.card_hover, hv), px(12),
                                      corners ? corners : ImDrawFlags_RoundCornersNone);
                if (i > 0) dl->AddLine(rp + ImVec2(px(16), 0), rp + ImVec2(w - px(16), 0), ink(C.border));
                std::string addr = g_shell.recent[i];
                if (addr.rfind("http://", 0) == 0) addr = addr.substr(7);
                ImGui::PushFont(F.mono);
                dl->AddText(rp + ImVec2(px(16), (row_h - ImGui::GetFontSize()) * 0.5f), ink(C.text), addr.c_str());
                ImGui::PopFont();
                ImGui::PushFont(F.small);
                const char* go = "Connect";
                ImVec2 gs = ImGui::CalcTextSize(go);
                dl->AddText(rp + ImVec2(w - px(16) - gs.x, (row_h - gs.y) * 0.5f), ink(mix(C.faint, C.text, hv)), go);
                ImGui::PopFont();
            }
            dl->AddRect(lp, lp + ImVec2(w, row_h * g_shell.recent.size()), ink(C.border), px(12));
            ImGui::SetCursorScreenPos(lp);
            ImGui::Dummy(ImVec2(w, row_h * g_shell.recent.size()));
            if (!pick.empty()) {
                std::snprintf(g_shell.url, sizeof(g_shell.url), "%s", pick.c_str());
                connect_to(app);
            }
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    last_h = ImGui::GetItemRectSize().y;
    ImGui::PopStyleVar();
}

static void draw_ui(glint::App& app, VideoTexture& texture)
{
    using namespace ui;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    update_theme(io.DeltaTime);
    poll_events(app);
#if SL_LIVE_STREAMING
    stream_poll_link();
#endif
    handle_shortcuts(app);

    bool has_video = texture.descriptor && texture.width > 0;
    Link link = link_state(app);
    draw_video(texture, vp->Pos, vp->Size);

    // Controls fade after 3 s without input, unless something needs them.
    double now = ImGui::GetTime();
    bool activity = io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 ||
                    ImGui::IsAnyMouseDown() || io.InputQueueCharacters.Size > 0;
    if (activity) g_shell.last_activity = now;
    // Pointer outside the window: fade sooner.
    double idle_limit = ImGui::IsMousePosValid() ? 3.0 : 1.0;
    bool overlays = g_shell.settings_open || g_shell.gallery_open;
    bool visible = !g_shell.controls_pinned_hidden &&
                   (now - g_shell.last_activity < idle_limit || overlays || !has_video || g_shell.pointer_on_controls);
    if (g_shell.controls_pinned_hidden && activity && ImGui::IsAnyMouseDown()) {
        // Any click brings the controls back when they were hidden with H.
        g_shell.controls_pinned_hidden = false;
        visible = true;
    }
    g_shell.overlay_alpha += ((visible ? 1.0f : 0.0f) - g_shell.overlay_alpha) * std::min(1.0f, io.DeltaTime * 10.0f);
    float alpha = g_shell.overlay_alpha;
    if (alpha < 0.02f && !overlays) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##shell", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollWithMouse);

    if (!has_video) draw_empty_state(app, vp->Pos + vp->Size * 0.5f);

    glint::RecorderInfo rec = app.recording_info();
#if SL_LIVE_STREAMING
    glint::BroadcastStatus bst = app.broadcast_status();
#endif
    bool rec_on = recorder_running(rec);
    float m = px(20);

    // Recording (and live, when streaming is built in) stays visible at the top
    // centre even when the controls have faded: you always know you are capturing.
    {
        std::string live_text, rec_text;
#if SL_LIVE_STREAMING
        if (bst.active) {
            int up = 0;
            for (const auto& d : bst.destinations) up += d.state == glint::BroadcastDestinationStatus::State::Live;
            live_text = "Live " + format_clock(bst.seconds) + ", " + std::to_string(up) + " of " +
                        std::to_string(bst.destinations.size());
        } else if (bst.stopping) {
            live_text = "Ending the broadcast";
        }
        bool live_active = bst.active;
#else
        bool live_active = false;
#endif
        if (rec.state != glint::RecorderInfo::State::Idle && rec.state != glint::RecorderInfo::State::Failed)
            rec_text = rec.state == glint::RecorderInfo::State::Recording
                           ? "Recording " + format_clock(rec.seconds)
                           : (rec.state == glint::RecorderInfo::State::Armed ? "Recording starts at the next keyframe" : "Saving");
        if ((!live_text.empty() || !rec_text.empty()) && has_video) {
            float lw = live_text.empty() ? 0.0f : ImGui::CalcTextSize(live_text.c_str()).x + px(42);
            float rw2 = rec_text.empty() ? 0.0f : ImGui::CalcTextSize(rec_text.c_str()).x + px(42);
            float total = lw + rw2 + (lw > 0 && rw2 > 0 ? px(10) : 0.0f);
            float x0 = vp->Pos.x + (vp->Size.x - total) * 0.5f, y0 = vp->Pos.y + m + px(4);
            float beat = (float)std::fmod(now, 1.4) / 1.4f;
            if (lw > 0) glass_pill(ImVec2(x0, y0), live_text.c_str(), live_active ? C.danger_fill : C.faint, live_active ? beat : 0.0f);
            if (rw2 > 0)
                glass_pill(ImVec2(x0 + total - rw2, y0), rec_text.c_str(), C.danger_fill,
                           rec.state == glint::RecorderInfo::State::Recording ? beat : 0.0f);
        }
    }

    g_shell.pointer_on_controls = false;
    if (alpha > 0.03f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        float drop = (1.0f - alpha) * px(10);   // chrome slides out of the way as it fades

        // Top-left: status.
        if (has_video || link == Link::Live) {
            status_pill(app, vp->Pos + ImVec2(m, m - drop), now);
            if (ImGui::IsItemHovered()) g_shell.pointer_on_controls = true;
        }

        // Top-right: gallery, full screen, settings.
        float ib = px(44);
        float tx = vp->Pos.x + vp->Size.x - m - ib, ty = vp->Pos.y + m - drop;
        ImGui::SetCursorScreenPos(ImVec2(tx, ty));
        if (button("##settings_btn", icon::settings, F.icons ? nullptr : "Settings", Kind::Glass,
                   ImVec2(F.icons ? ib : px(90), ib))) {
            g_shell.page_shown_at = ImGui::GetTime();
            g_shell.settings_open = true;
        }
        if (ImGui::IsItemHovered()) g_shell.pointer_on_controls = true;
        tip("Settings  (Ctrl+,)");
        tx -= ib + px(8);
        ImGui::SetCursorScreenPos(ImVec2(tx, ty));
        if (button("##full_btn", g_shell.fullscreen ? icon::window : icon::full, F.icons ? nullptr : "Full",
                   Kind::Glass, ImVec2(F.icons ? ib : px(60), ib)))
            set_fullscreen(!g_shell.fullscreen);
        if (ImGui::IsItemHovered()) g_shell.pointer_on_controls = true;
        tip(g_shell.fullscreen ? "Leave full screen  (F11)" : "Full screen  (F11)");
        tx -= ib + px(8);
        ImGui::SetCursorScreenPos(ImVec2(tx, ty));
        if (button("##gallery_btn", icon::library, F.icons ? nullptr : "Gallery", Kind::Glass,
                   ImVec2(F.icons ? ib : px(80), ib)))
            g_shell.gallery_open = true;
        if (ImGui::IsItemHovered()) g_shell.pointer_on_controls = true;
        tip("Gallery  (G)");

        // Bottom: the command bar, only while there is a picture to act on
        // (side by side's control panel takes its place).
        if (has_video && !(g_compare.on && g_tweaks_open)) {
            bool sound = app.audio().enabled();
            float bh = px(44), gap = px(6), pad = px(6), sep = px(13);
            float bw_sound = px(44), vol_w = px(150);
            std::string rec_label = rec_on ? format_clock(rec.seconds) : "Record";
            float bw_shot = button_width(icon::camera, "Screenshot");
            float bw_rec = button_width(icon::record, rec_label.c_str());
#if SL_LIVE_STREAMING
            bool live_on = bst.active;
            bool live_confirm = ImGui::GetTime() < g_stream.golive_confirm_until;
            std::string live_label = live_on ? "Live " + format_clock(bst.seconds) : (live_confirm ? "Click again to go live" : "Go live");
            float bw_live = button_width(icon::stream, live_label.c_str()) + gap;
#else
            float bw_live = 0.0f;
#endif
            static const char* lat[] = {"Lowest", "Balanced", "Smooth"};
            float lat_w = segmented_width(lat, 3);
            float bw_hide = px(44);
            float dock_w = pad * 2 + bw_sound + vol_w + sep + bw_shot + gap + bw_rec + bw_live + sep + lat_w + gap + bw_hide;
            float dock_h = bh + pad * 2;
            ImVec2 dp(vp->Pos.x + (vp->Size.x - dock_w) * 0.5f, vp->Pos.y + vp->Size.y - m - dock_h + drop * 1.4f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(dp + ImVec2(0, px(6)), dp + ImVec2(dock_w, dock_h + px(8)), ImGui::GetColorU32(C.shadow, 0.5f),
                              dock_h * 0.5f);
            dl->AddRectFilled(dp, dp + ImVec2(dock_w, dock_h), ink(C.glass), dock_h * 0.5f);
            dl->AddRect(dp, dp + ImVec2(dock_w, dock_h), ink(C.glass_border), dock_h * 0.5f);
            if (ImGui::IsMouseHoveringRect(dp, dp + ImVec2(dock_w, dock_h))) g_shell.pointer_on_controls = true;

            float x = dp.x + pad, by = dp.y + pad;
            auto separator = [&]() {
                dl->AddLine(ImVec2(x + sep * 0.5f, dp.y + px(16)), ImVec2(x + sep * 0.5f, dp.y + dock_h - px(16)),
                            ink(C.glass_border));
                x += sep;
            };
            ImGui::SetCursorScreenPos(ImVec2(x, by));
            if (button("##dock_sound", sound ? icon::volume : icon::mute, F.icons ? nullptr : (sound ? "Sound" : "Muted"),
                       sound ? Kind::Ghost : Kind::Danger, ImVec2(bw_sound, bh)))
                app.audio().set_enabled(!sound);
            tip(sound ? "Mute  (M)" : "Sound on  (M)");
            x += bw_sound;
            ImGui::SetCursorScreenPos(ImVec2(x, by + (bh - px(26)) * 0.5f));
            float vol = app.audio().volume();
            if (slider("##dock_vol", &vol, 0.0f, 4.0f, "%.0f%%", 100.0f, vol_w, sound)) app.audio().set_volume(vol);
            x += vol_w;
            separator();
            ImGui::SetCursorScreenPos(ImVec2(x, by));
            if (button("##dock_shot", icon::camera, "Screenshot", Kind::Ghost, ImVec2(bw_shot, bh)))
                do_screenshot(app);
            tip("Save a screenshot  (S)");
            x += bw_shot + gap;
            ImGui::SetCursorScreenPos(ImVec2(x, by));
            if (button("##dock_rec", icon::record, rec_label.c_str(), rec_on ? Kind::Danger : Kind::Ghost, ImVec2(bw_rec, bh),
                       app.connected() && rec.state != glint::RecorderInfo::State::Finalizing))
                toggle_local_recording(app);
            tip(rec_on ? "Stop and save the recording  (R)" : "Record on this PC  (R)");
            x += bw_rec;
#if SL_LIVE_STREAMING
            x += gap;
            ImGui::SetCursorScreenPos(ImVec2(x, by));
            if (button("##dock_live", icon::stream, live_label.c_str(),
                       live_on ? Kind::Danger : (live_confirm ? Kind::GlassOn : Kind::Ghost), ImVec2(bw_live - gap, bh),
                       live_on || (app.connected() && !bst.stopping))) {
                stream_load();
                go_live_or_end(app);
            }
            tip(live_on ? "End the live broadcast" : (live_confirm ? "Goes live on every destination that is on"
                                                                   : "Go live (asks once more)"));
            x += bw_live - gap;
#endif
            separator();
            ImGui::SetCursorScreenPos(ImVec2(x, by + (bh - px(34)) * 0.5f));
            int li = (int)app.latency();
            if (segmented("##dock_lat", &li, lat, 3)) app.set_latency((glint::App::Latency)li);
            tip("Latency: Lowest shows frames at once, Balanced smooths jitter, Smooth also waits for the sound  (L)");
            x += lat_w + gap;
            ImGui::SetCursorScreenPos(ImVec2(x, by));
            if (button("##dock_hide", icon::hide, F.icons ? nullptr : "Hide", Kind::Ghost, ImVec2(bw_hide, bh)))
                g_shell.controls_pinned_hidden = true;
            tip("Hide the controls  (H)");
        }
        ImGui::PopStyleVar();
    }

    draw_settings(app);
    draw_gallery(app);
    ImGui::End();
    draw_picture_tweaks(vp->Pos, vp->Size);

    draw_toasts(vp->Pos.y + vp->Size.y - px(110));
}

static void draw_headless(glint::App& app, int frames)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (app.frame_count() < (uint64_t)frames &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto config = app.config();
    auto status = app.status();
    std::printf("decoded=%llu frames\n", (unsigned long long)app.frame_count());
    std::printf("stream=%s %dx%d@%d codec=%s\n", config.platform.c_str(), config.width,
                config.height, config.fps, config.codec.c_str());
    std::printf("status=%.1f fps %.1f kbps clients=%d\n", status.fps, status.kbps, status.clients);
    float rx_fps = 0.0f, rx_kbps = 0.0f;
    app.received_rate(rx_fps, rx_kbps);
    std::printf("received=%.1f fps %.1f kbps (last 2 s)\n", rx_fps, rx_kbps);
    static const char* modes[] = {"Lowest", "Balanced", "Smooth"};
    std::printf("latency=%s hold=%.0f ms late=%llu frames\n", modes[(int)app.latency()], app.video_hold_ms(),
                (unsigned long long)app.late_frames());
    if (app.frame_count() == 0 || !app.connected())
        std::fprintf(stderr, "error=%s\n", app.error().c_str());
}

// Headless audio soak: video decode + audio playback for `seconds`, printing
// the counters every 10 s. With `exercise`, volume/delay are changed live.
// --decoder cpu|gpu for the headless modes (-1: app default).
static int g_decoder_override = -1;

static int run_audio_soak(const std::string& url, int seconds, bool exercise, int delay_ms)
{
    glint::App app;
    if (g_decoder_override >= 0) app.set_gpu_decode(g_decoder_override == 1);
    glint::AudioPlayer& audio = app.audio();
    if (delay_ms >= 0 || exercise) audio.set_delay_auto(false);   // manual delay under test
    if (delay_ms >= 0) audio.set_delay_ms(delay_ms);
    int threads_before = glint::process_thread_count();
    app.start(url);

    auto t0 = std::chrono::steady_clock::now();
    while (!audio.active() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(15))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!audio.active()) {
        std::fprintf(stderr, "audio never started: state=%s err=%s video=%s\n",
                     audio.stats().state.c_str(), audio.last_error().c_str(),
                     app.error().c_str());
        app.stop();
        return 2;
    }
    int threads_running = glint::process_thread_count();
    std::printf("audio started: threads %d -> %d\n", threads_before, threads_running);

    auto print = [&](const char* tag, int t) {
        glint::AudioStats s = audio.stats();
        std::printf("[%s t=%3ds] %s dev='%s' fmt=%s decoded=%llu | in=%llu out=%llu cb=%llu "
                    "buf=%.0f/%.0fms ratio=%.4f underruns=%llu(warm+%llu) drops=%llu/%lluf "
                    "nonfinite in=%llu out=%llu clip=%llu peak=%.3f(max %.3f) rms=%.3f "
                    "vol=%.2f delay=%s%d playout=%.0fms vhold=%.0fms reconnects=%llu | pktgap max=%.0fms >50=%llu >100=%llu >200=%llu >400=%llu cbgap max=%.0fms >40=%llu\n",
                    tag, t, s.state.c_str(), s.device.c_str(), s.format.c_str(),
                    (unsigned long long)app.frame_count(),
                    (unsigned long long)s.frames_in, (unsigned long long)s.frames_out,
                    (unsigned long long)s.callbacks, s.buffered_ms, s.target_ms, s.ratio,
                    (unsigned long long)s.underruns,
                    (unsigned long long)s.underruns_after_warmup,
                    (unsigned long long)s.drop_events, (unsigned long long)s.dropped_frames,
                    (unsigned long long)s.nonfinite_in, (unsigned long long)s.nonfinite_out,
                    (unsigned long long)s.clipped_samples, s.peak, s.peak_max, s.rms,
                    audio.volume(), s.auto_delay ? "auto " : "", audio.effective_delay_ms(), s.playout_ms,
                    app.video_hold_ms(), (unsigned long long)s.reconnects,
                    s.max_packet_gap_ms, (unsigned long long)s.packet_gaps_50ms,
                    (unsigned long long)s.packet_gaps_100ms,
                    (unsigned long long)s.packet_gaps_200ms,
                    (unsigned long long)s.packet_gaps_400ms, s.max_callback_gap_ms,
                    (unsigned long long)s.callback_gaps_40ms);
        std::fflush(stdout);
    };

    static const struct { float vol; int delay; bool on; } steps[] = {
        {1.0f, 100, true}, {2.0f, 100, true}, {0.5f, 100, true}, {4.0f, 100, true},
        {1.0f, 400, true}, {1.0f, 0, true}, {1.0f, 100, false}, {1.0f, 100, true},
    };
    for (int t = 1; t <= seconds; t++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (exercise && t % 10 == 0) {
            size_t i = (size_t)(t / 10 - 1) % (sizeof(steps) / sizeof(steps[0]));
            audio.set_volume(steps[i].vol);
            audio.set_delay_ms(steps[i].delay);
            audio.set_enabled(steps[i].on);
            std::printf("set volume=%.2f delay=%d sound=%s\n", steps[i].vol, steps[i].delay,
                        steps[i].on ? "on" : "off");
        }
        if (t % 10 == 0 || t == seconds || t == 3) print("soak", t);
    }
    glint::AudioStats final_stats = audio.stats();
    app.stop();
    int threads_after = glint::process_thread_count();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    int threads_settled = glint::process_thread_count();
    glint::AudioStats after = audio.stats();
    std::printf("threads after stop: %d immediately, %d after 3 s\n", threads_after, threads_settled);
    std::printf("after stop: active=%d device_open=%d state=%s threads %d -> %d (before start %d)\n",
                audio.active() ? 1 : 0, after.device_open ? 1 : 0, after.state.c_str(),
                threads_running, threads_after, threads_before);
    bool pass = final_stats.nonfinite_in == 0 && final_stats.nonfinite_out == 0 &&
                final_stats.frames_out > 0 && final_stats.peak_max > 0.0f &&
                !audio.active() && !after.device_open;
    std::printf("result: %s underruns_after_warmup=%llu drops=%llu\n", pass ? "PASS" : "FAIL",
                (unsigned long long)final_stats.underruns_after_warmup,
                (unsigned long long)final_stats.dropped_frames);
    return pass ? 0 : 1;
}

// Start/stop the whole session repeatedly and report the process thread count.
static int run_audio_cycles(const std::string& url, int cycles)
{
    int baseline = glint::process_thread_count();
    std::printf("baseline threads %d\n", baseline);
    for (int i = 1; i <= cycles; i++) {
        glint::App app;
        app.start(url);
        auto t0 = std::chrono::steady_clock::now();
        while (!app.audio().active() &&
               std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::seconds(3));
        int running = glint::process_thread_count();
        auto stats = app.audio().stats();
        app.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int soon = glint::process_thread_count();
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::printf("cycle %d: running threads %d, 0.5 s after stop %d, 3.5 s after stop %d, "
                    "audio active=%d frames_out=%llu\n",
                    i, running, soon, glint::process_thread_count(), app.audio().active() ? 1 : 0,
                    (unsigned long long)stats.frames_out);
        std::fflush(stdout);
    }
    return 0;
}

// Headless local-recording test: record for `seconds`, take one screenshot
// half-way, finalize and print the recorder's view of the file.
static int run_record_test(const std::string& url, int seconds, const std::string& dir,
                           bool disconnect_ends)
{
    glint::App app;
    if (!dir.empty()) app.set_media_dir(dir);
    app.start(url);
    auto t0 = std::chrono::steady_clock::now();
    while ((!app.connected() || !app.audio().active() || app.frame_count() < 30) &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!app.connected()) {
        std::fprintf(stderr, "not connected: %s\n", app.error().c_str());
        return 2;
    }

    std::string error;
    if (!app.start_local_recording(error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        app.stop();
        return 3;
    }
    std::printf("recording into %s\n", app.media_dir().c_str());
    bool shot_done = false;
    for (int t = 1; t <= seconds; t++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!shot_done && t >= seconds / 2) {
            std::string path;
            if (app.take_screenshot(path, error)) std::printf("screenshot: %s\n", path.c_str());
            else std::printf("screenshot failed: %s\n", error.c_str());
            shot_done = true;
        }
        if (t % 10 == 0 || t == seconds) {
            glint::RecorderInfo info = app.recording_info();
            std::printf("[rec t=%2ds] %s | %s | frames=%llu audio=%llu fr dropped=%llu %.2f MB %.2f s\n",
                        t, glint::recorder_state_name(info.state), info.path.c_str(),
                        (unsigned long long)info.video_frames,
                        (unsigned long long)info.audio_frames, (unsigned long long)info.dropped,
                        info.bytes / 1048576.0, info.seconds);
            std::fflush(stdout);
        }
    }
    if (disconnect_ends) {
        // Close the app while recording: App::stop() must finalize the file.
        app.stop();
    } else {
        app.stop_local_recording();
    }
    for (int i = 0; i < 100 && app.recording_info().state == glint::RecorderInfo::State::Finalizing; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    glint::RecorderInfo info = app.recording_info();
    std::printf("final: %s | %s | frames=%llu audio=%llu fr %.2f MB %.2f s | %s %s\n",
                glint::recorder_state_name(info.state), info.path.c_str(),
                (unsigned long long)info.video_frames, (unsigned long long)info.audio_frames,
                info.bytes / 1048576.0, info.seconds, info.message.c_str(), info.error.c_str());
    app.stop();
    return info.state == glint::RecorderInfo::State::Idle && info.video_frames > 0 ? 0 : 1;
}

// Headless gallery test: list, export the newest recording to MP4, verify the
// copy shows up, then delete only that copy again.
static int run_gallery_test(const std::string& dir)
{
    auto print_items = [&](const char* tag) {
        auto items = glint::list_gallery(dir);
        std::printf("%s: %zu item(s)\n", tag, items.size());
        for (const auto& item : items)
            std::printf("  %-44s %s %dx%d %.2f s %.2f MB audio=%d\n", item.name.c_str(),
                        item.video ? "video" : "image", item.width, item.height, item.duration,
                        item.size / 1048576.0, item.has_audio ? 1 : 0);
        return items;
    };
    auto items = print_items("gallery");
    const glint::GalleryItem* video = nullptr;
    for (const auto& item : items)
        if (item.video && item.name.find("-export-") == std::string::npos) { video = &item; break; }
    if (!video) { std::fprintf(stderr, "no recording to export\n"); return 1; }

    std::string out, error;
    if (!glint::export_copy(video->path, "mp4", out, error)) {
        std::fprintf(stderr, "export failed: %s\n", error.c_str());
        return 2;
    }
    std::printf("exported: %s\n", out.c_str());
    print_items("after export");
    if (!glint::delete_gallery_file(out, error)) {
        std::fprintf(stderr, "delete failed: %s\n", error.c_str());
        return 3;
    }
    print_items("after deleting the export");
    return 0;
}

#if SL_LIVE_STREAMING
// Headless broadcast test: stream the console to the given RTMP URLs for
// `seconds` (use a local ingest such as `ffmpeg -listen 1 -i rtmp://...`).
static int run_broadcast_test(const std::string& url, int seconds, const std::vector<std::string>& targets,
                              const glint::BroadcastQuality& quality)
{
    glint::App app;
    app.start(url);
    auto t0 = std::chrono::steady_clock::now();
    while ((!app.connected() || app.frame_count() < 30 || !app.audio().active()) &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!app.connected()) {
        std::fprintf(stderr, "not connected: %s\n", app.error().c_str());
        return 2;
    }
    std::vector<glint::BroadcastTarget> list;
    for (size_t i = 0; i < targets.size(); i++) list.push_back({"dest" + std::to_string(i + 1), targets[i]});
    std::string error;
    if (!app.start_broadcast(list, quality, error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 3;
    }
    auto print = [&](const char* tag) {
        glint::BroadcastStatus st = app.broadcast_status();
        std::printf("[%s] active=%d enc=%s %dx%d@%d in=%llu enc=%llu dropped=%llu err='%s'\n", tag, st.active ? 1 : 0,
                    st.encoder.c_str(), st.width, st.height, st.fps, (unsigned long long)st.frames_in,
                    (unsigned long long)st.frames_encoded, (unsigned long long)st.frames_dropped, st.error.c_str());
        for (const auto& d : st.destinations)
            std::printf("    %s: %s %.0f kbps %.2f MB dropped=%llu live=%.1fs %s\n", d.name.c_str(),
                        glint::broadcast_state_name(d.state), d.kbps, d.bytes / 1048576.0,
                        (unsigned long long)d.dropped_packets, d.live_seconds, d.error.c_str());
        std::fflush(stdout);
    };
    for (int t = 1; t <= seconds; t++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (t % 5 == 0 || t == seconds) {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "t=%ds", t);
            print(tag);
        }
    }
    app.stop_broadcast("test finished");
    auto s0 = std::chrono::steady_clock::now();
    while (app.broadcast_status().stopping && std::chrono::steady_clock::now() - s0 < std::chrono::seconds(15))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("stopped in %.2f s\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count());
    print("final");
    app.stop();
    return 0;
}

// Destination store test. Run with GLINT_SETTINGS_DIR and GLINT_VAULT_PREFIX set to
// throwaway values: it writes, re-reads and deletes one destination and key.
static int run_store_test()
{
    const char* dir = std::getenv("GLINT_SETTINGS_DIR");
    const char* prefix = std::getenv("GLINT_VAULT_PREFIX");
    if (!dir || !prefix) {
        std::fprintf(stderr, "set GLINT_SETTINGS_DIR and GLINT_VAULT_PREFIX first\n");
        return 2;
    }
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) failures++;
    };
    std::string error;
    glint::DestinationStore store;
    check(store.load(error), "load (empty)");
    glint::StreamDestination& d = store.add("twitch");
    std::string id = d.id;
    d.name = "Test Twitch";
    store.quality.height = 720;
    store.quality.fps = 30;
    store.quality.video_kbps = 4500;
    const std::string secret = "live_test_0123456789abcdef";
    check(store.set_key(id, secret, error), "set key in vault");
    check(!store.set_key(id, "has space", error), "reject key with a space");
    check(store.save(error), "save");

    glint::DestinationStore again;
    check(again.load(error), "reload");
    glint::StreamDestination* r = again.find(id);
    check(r && r->name == "Test Twitch" && r->server == "rtmps://live.twitch.tv/app" && r->enabled,
          "destination fields round-trip");
    check(again.quality.height == 720 && again.quality.fps == 30 && again.quality.video_kbps == 4500,
          "quality round-trips");
    check(again.has_key(id) && again.key(id) == secret, "key readable from vault");
    check(glint::ingest_url(r ? r->server : "", again.key(id)) == "rtmps://live.twitch.tv/app/" + secret,
          "ingest URL = server/key");
    std::ifstream in(std::filesystem::u8path(again.path()), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();   // Windows cannot replace a file that is still open
    check(!text.empty() && text.find(secret) == std::string::npos, "key is NOT in the conf file");
    check(text.rfind("GLINT-DESTINATIONS-1", 0) == 0, "conf header");

    again.remove(id);
    check(again.save(error), "save after remove");
    check(!again.has_key(id), "vault entry deleted with the destination");
    glint::DestinationStore last;
    last.load(error);
    check(last.items().empty(), "no destinations left");

    glint::OAuthApps apps;
    apps.twitch_client_id = "abc123";
    check(glint::save_oauth_apps(apps, error), "save oauth apps");
    glint::OAuthApps apps2;
    check(glint::load_oauth_apps(apps2, error) && apps2.twitch_client_id == "abc123", "oauth apps round-trip");
    apps.twitch_client_id = "bad id";
    check(!glint::save_oauth_apps(apps, error), "reject client id with a space");
    std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

// Go-live test: the Go live button's path (saved destinations, keys from the
// vault) without the UI. Honour GLINT_SETTINGS_DIR / GLINT_VAULT_PREFIX.
static int run_golive_test(const std::string& url, int seconds)
{
    glint::DestinationStore store;
    std::string error;
    if (!store.load(error)) {
        std::fprintf(stderr, "load: %s\n", error.c_str());
        return 2;
    }
    std::vector<glint::BroadcastTarget> targets;
    glint::BroadcastQuality q;
    if (!build_targets(store, targets, q, error)) {
        std::printf("refused: %s\n", error.c_str());
        return 3;
    }
    std::printf("%zu destination(s), %dp%d at %d kbps:", targets.size(), q.height, q.fps, q.video_kbps);
    for (auto& t : targets) std::printf(" [%s]", t.name.c_str());   // URLs carry keys: never printed
    std::printf("\n");
    std::vector<std::string> urls;
    for (auto& t : targets) urls.push_back(t.url);
    return run_broadcast_test(url, seconds, urls, q);
}
#endif // SL_LIVE_STREAMING

// Offline decoder test on a recording: Annex-B packets as the payload sends
// them, decoded as fast as possible. Prints speed and CPU cost, and writes one
// FNV-1a hash of the luma plane per frame so GPU and CPU output can be diffed.
static int run_decode_file_test(const std::string& path, bool gpu, const std::string& hash_out)
{
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(in, nullptr) < 0) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 2;
    }
    int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVBitStreamFilter* f = av_bsf_get_by_name("h264_mp4toannexb");
    AVBSFContext* bsf = nullptr;
    if (vi < 0 || !f || av_bsf_alloc(f, &bsf) < 0) return 2;
    avcodec_parameters_copy(bsf->par_in, in->streams[vi]->codecpar);
    bsf->time_base_in = in->streams[vi]->time_base;
    av_bsf_init(bsf);

    glint::H264Decoder dec;
    std::string error;
    if (!dec.open(error, gpu)) {
        std::fprintf(stderr, "open: %s\n", error.c_str());
        return 3;
    }
    std::FILE* hashes = hash_out.empty() ? nullptr : std::fopen(hash_out.c_str(), "wb");
    uint64_t frames = 0, failures = 0;
    std::string format_name;
    auto tap = [&](const AVFrame* fr) {
        uint32_t h = 2166136261u;
        for (int y = 0; y < fr->height; y++) {
            const uint8_t* row = fr->data[0] + (size_t)y * fr->linesize[0];
            for (int x = 0; x < fr->width; x++) h = (h ^ row[x]) * 16777619u;
        }
        if (hashes) std::fprintf(hashes, "%08x\n", h);
        if (format_name.empty()) format_name = av_get_pix_fmt_name((AVPixelFormat)fr->format);
    };
    glint::Picture picture;
    int w = 0, h = 0;
    AVPacket* pkt = av_packet_alloc();
    double cpu0 = glint::process_cpu_seconds();
    auto t0 = std::chrono::steady_clock::now();
    auto feed = [&](AVPacket* p) {
        if (av_bsf_send_packet(bsf, p) < 0) return;
        while (av_bsf_receive_packet(bsf, pkt) >= 0) {
            if (dec.decode(pkt->data, (size_t)pkt->size, (uint32_t)pkt->pts, picture, error, tap)) {
                frames++;
                w = picture.width;
                h = picture.height;
            }
            else if (!error.empty()) failures++, error.clear();
            av_packet_unref(pkt);
        }
    };
    AVPacket* src = av_packet_alloc();
    while (av_read_frame(in, src) >= 0) {
        if (src->stream_index == vi) feed(src);
        av_packet_unref(src);
    }
    feed(nullptr);
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double cpu = glint::process_cpu_seconds() - cpu0;
    if (hashes) std::fclose(hashes);
    std::printf("decoder=%s frames=%llu (%dx%d %s) errors=%llu wall=%.2fs (%.0f fps) cpu=%.2fs (%.2f ms/frame)\n",
                dec.description().c_str(), (unsigned long long)frames, w, h, format_name.c_str(),
                (unsigned long long)failures, wall, frames / std::max(wall, 1e-6), cpu,
                frames ? cpu * 1000.0 / frames : 0.0);
    av_packet_free(&pkt);
    av_packet_free(&src);
    av_bsf_free(&bsf);
    avformat_close_input(&in);
    return frames ? 0 : 1;
}

// GPU colour conversion test (needs the Vulkan device, so it runs after window
// creation): converts frames of a recording on the GPU and on the CPU,
// compares the RGBA results and times both display paths.
static int run_yuv_test(const std::string& path, bool gpu_decode)
{
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(in, nullptr) < 0)
        return 2;
    int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVBSFContext* bsf = nullptr;
    av_bsf_alloc(av_bsf_get_by_name("h264_mp4toannexb"), &bsf);
    avcodec_parameters_copy(bsf->par_in, in->streams[vi]->codecpar);
    bsf->time_base_in = in->streams[vi]->time_base;
    av_bsf_init(bsf);
    glint::H264Decoder dec;
    std::string error;
    dec.open(error, gpu_decode);
    dec.set_output(glint::H264Decoder::Output::Yuv);

    VideoTexture tex;
    glint::Picture pic;
    uint64_t n = 0;
    double gpu_ms = 0, cpu_ms = 0;
    int compared = 0, timed = 0;
    double sum_diff = 0;
    int max_diff = 0;
    uint64_t over2 = 0, pixels = 0;
    AVPacket* src = av_packet_alloc();
    AVPacket* pkt = av_packet_alloc();
    using clk = std::chrono::steady_clock;
    while (av_read_frame(in, src) >= 0 && n < 600) {
        if (src->stream_index != vi) { av_packet_unref(src); continue; }
        av_bsf_send_packet(bsf, src);
        av_packet_unref(src);
        while (av_bsf_receive_packet(bsf, pkt) >= 0) {
            bool ok = dec.decode(pkt->data, (size_t)pkt->size, (uint32_t)pkt->pts, pic, error);
            av_packet_unref(pkt);
            if (!ok) continue;
            n++;
            // GPU path: upload planes + compute shader.
            auto t0 = clk::now();
            if (!upload_yuv(tex, pic)) {
                std::printf("GPU conversion unavailable: %s\n", g_yuv.error.c_str());
                return 1;
            }
            auto t1 = clk::now();
            // CPU path: swscale to RGBA + RGBA upload (into a scratch texture).
            std::vector<uint8_t> cpu_rgba;
            glint::picture_to_rgba(pic, cpu_rgba);
            static VideoTexture cpu_tex;
            upload_frame(cpu_tex, cpu_rgba, pic.width, pic.height);
            auto t2 = clk::now();
            if (n > 30) {   // skip warm-up
                gpu_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                cpu_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
                timed++;
            }
            if (n % 100 == 0) {
                std::vector<uint8_t> gpu_rgba;
                read_texture(tex, gpu_rgba);
                const char* dump = std::getenv("GLINT_YUV_DUMP");   // raw RGBA of frame 300 for offline checks
                if (dump && n == 300) {
                    std::ofstream(std::string(dump) + "/gpu.rgba", std::ios::binary)
                        .write((const char*)gpu_rgba.data(), (std::streamsize)gpu_rgba.size());
                    std::ofstream(std::string(dump) + "/cpu.rgba", std::ios::binary)
                        .write((const char*)cpu_rgba.data(), (std::streamsize)cpu_rgba.size());
                    std::ofstream(std::string(dump) + (pic.format == glint::Picture::Format::Nv12 ? "/frame.nv12" : "/frame.i420"),
                                  std::ios::binary)
                        .write((const char*)pic.data.data(), (std::streamsize)pic.data.size());
                }
                for (size_t i = 0; i + 3 < gpu_rgba.size(); i += 4) {
                    for (int c = 0; c < 3; c++) {
                        int d = std::abs((int)gpu_rgba[i + c] - (int)cpu_rgba[i + c]);
                        sum_diff += d;
                        max_diff = std::max(max_diff, d);
                        if (d > 2) over2++;
                    }
                    pixels++;
                }
                compared++;
            }
        }
    }
    std::printf("frames=%llu format=%s %dx%d bt709=%d full_range=%d decoder=%s\n", (unsigned long long)n,
                pic.format == glint::Picture::Format::Nv12 ? "NV12" : "I420", pic.width, pic.height, pic.bt709 ? 1 : 0,
                pic.full_range ? 1 : 0, dec.description().c_str());
    std::printf("GPU vs CPU over %d frames: mean |diff| %.3f, max %d, channel values off by >2: %.4f%%\n", compared,
                pixels ? sum_diff / (pixels * 3.0) : 0.0, max_diff, pixels ? over2 * 100.0 / (pixels * 3.0) : 0.0);
    std::printf("display path per frame (wall, includes GPU wait): GPU convert %.2f ms, CPU convert %.2f ms\n",
                timed ? gpu_ms / timed : 0.0, timed ? cpu_ms / timed : 0.0);
    destroy_texture(tex);
    av_packet_free(&src);
    av_packet_free(&pkt);
    av_bsf_free(&bsf);
    avformat_close_input(&in);
    return 0;   // accuracy is judged offline against an exact reference (GPU and CPU differ by design)
}

// --- settings that survive a restart ------------------------------------------

static glint::AppSettings capture_settings(glint::App& app)
{
    glint::AppSettings s;
    s.console_url = app.url().empty() ? std::string(g_shell.url) : app.url();
    s.recent = g_shell.recent;
    s.theme = ui::g_theme;
    glint::AudioPlayer& audio = app.audio();
    s.sound = g_viewer.live_sound_paused || audio.enabled();   // muted only while a recording plays
    s.volume = audio.volume();
    s.delay_auto = audio.delay_auto();
    s.delay_ms = audio.delay_ms();
    s.video_sync = app.video_sync();
    s.av_offset_ms = app.av_offset_ms();
    s.media_dir = app.media_dir() == glint::default_media_dir() ? std::string() : app.media_dir();
    s.picture = g_shell.picture;
    s.gpu_decode = app.gpu_decode();
    // The user's choice; a CPU fallback forced by a missing GPU path is not saved as one.
    s.gpu_convert = app.gpu_convert() || (g_yuv.tried && !g_yuv.available);
    s.latency = (int)app.latency();
    s.enhance = g_picture;
    s.picture_presets = g_picture_presets;
    s.picture_active = g_picture_active;
    s.window_x = g_shell.normal[0];
    s.window_y = g_shell.normal[1];
    s.window_w = g_shell.normal[2];
    s.window_h = g_shell.normal[3];
    s.maximized = g_window && !g_shell.fullscreen && glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED) != 0;
    s.fullscreen = g_shell.fullscreen;
    return s;
}

static void apply_settings(glint::App& app, const glint::AppSettings& s, bool keep_media_dir)
{
    glint::AudioPlayer& audio = app.audio();
    audio.set_enabled(s.sound);
    audio.set_volume(s.volume);
    audio.set_delay_ms(s.delay_ms);
    audio.set_delay_auto(s.delay_auto);
    app.set_video_sync(s.video_sync);
    app.set_av_offset_ms(s.av_offset_ms);
    if (!keep_media_dir && !s.media_dir.empty()) app.set_media_dir(s.media_dir);
    g_shell.picture = s.picture;
    app.set_gpu_decode(s.gpu_decode);
    app.set_gpu_convert(s.gpu_convert);
    app.set_latency((glint::App::Latency)std::clamp(s.latency, 0, 2));
    g_picture = s.enhance;
    g_picture_presets = s.picture_presets;
    g_picture_active = s.picture_active;
    g_shell.recent = s.recent;
    ui::set_theme(s.theme, true);
}

// A saved position is used only if its title bar lands on a connected monitor.
static bool position_visible(int x, int y, int w)
{
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; i++) {
        int mx, my, mw, mh;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        int px = x + std::min(w / 2, 120), py = y + 16;
        if (px >= mx && px < mx + mw && py >= my && py < my + mh) return true;
    }
    return false;
}

// Settings test: save, reload, compare, and that bad values fall back.
static int run_settings_test()
{
    if (!std::getenv("GLINT_SETTINGS_DIR")) {
        std::fprintf(stderr, "set GLINT_SETTINGS_DIR first\n");
        return 2;
    }
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) failures++;
    };
    glint::AppSettings a;
    std::vector<std::string> warnings;
    glint::load_settings(a, warnings);
    check(a == glint::AppSettings{} && warnings.empty(), "defaults when nothing is saved");
    a.console_url = "http://10.0.0.1:7231";
    a.sound = false;
    a.volume = 2.5f;
    a.delay_auto = false;
    a.delay_ms = 180;
    a.video_sync = false;
    a.av_offset_ms = -40;
    a.media_dir = "C:\\Captures\\Glint";
    a.picture = 2;
    a.gpu_decode = false;
    a.latency = 1;
    a.window_x = 120;
    a.window_y = 80;
    a.window_w = 1600;
    a.window_h = 900;
    a.maximized = true;
    a.theme = 1;
    a.recent = {"http://10.0.0.1:7231", "http://127.0.0.1:7231"};
    std::string error;
    check(glint::save_settings(a, error), "save");
    glint::AppSettings b;
    warnings.clear();
    glint::load_settings(b, warnings);
    check(a == b, "every value round-trips");
    check(warnings.empty(), "no warnings on a clean file");

    // Corrupt one value and one whole file: the rest must survive. audio.conf
    // has the header Screenlink wrote before the rename, which must still load.
    std::string dir = glint::settings_dir();
    {
        std::ofstream f(std::filesystem::u8path(dir) / "audio.conf", std::ios::binary | std::ios::trunc);
        f << "SCREENLINK-AUDIO-1\nsound=0\nvolume=99\ndelay_ms=180\n";
    }
    {
        std::ofstream f(std::filesystem::u8path(dir) / "window.conf", std::ios::binary | std::ios::trunc);
        f << "garbage\n";
    }
    glint::AppSettings c;
    warnings.clear();
    glint::load_settings(c, warnings);
    check(c.volume == 1.0f && c.sound == false && c.delay_ms == 180, "out-of-range volume falls back, others kept");
    check(c.picture == 0 && c.window_w == 0, "unknown window.conf ignored");
    check(c.media_dir == a.media_dir && c.console_url == a.console_url, "other files unaffected");
    check(warnings.size() == 2, "two warnings reported");
    for (auto& w : warnings) std::printf("     warning: %s\n", w.c_str());
    a.media_dir = "line\nbreak";
    check(!glint::save_settings(a, error), "refuse a value with a line break");

    // Diagnostics bundle.
    glint::log_open(dir);
    glint::logf("settings test log line");
    std::string zip;
    check(glint::create_diagnostics(dir, "system info test\n", zip, error), "create diagnostics zip");
    std::printf("     %s\n", zip.c_str());
    glint::log_close();
    std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    glint::attach_parent_console();   // no console window of our own (-mwindows)
    std::string url = "http://127.0.0.1:" + std::to_string(glint::kPayloadPort);
    bool url_given = false;
    std::string yuv_test_file;
    bool yuv_test_gpu = true;
    int broadcast_test = 0;
    std::vector<std::string> broadcast_urls;
    glint::BroadcastQuality broadcast_quality;
    std::string gallery_test_dir;
    int start_page = 0;
    bool start_gallery = false;   // --gallery: open the gallery at start
    std::string start_view;       // --view <file>: open that screenshot or recording in the viewer
    int start_theme = -1;         // --theme dark|light: start in that theme (kept like a choice made in Settings)
    bool record_disconnect = false;
    int record_test = 0;
    std::string record_dir;
    int headless = 0;
    int headless_latency = 0;   // --latency lowest|balanced|smooth for --headless
    int audio_soak = 0;
    bool audio_exercise = false;
    int audio_delay = -1;
    int audio_cycles = 0;
    bool fullscreen = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) { url = argv[++i]; url_given = true; }
        else if (std::strcmp(argv[i], "--headless") == 0 && i + 1 < argc) headless = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--latency") == 0 && i + 1 < argc) {
            std::string v = argv[++i];
            headless_latency = v == "smooth" ? 2 : v == "balanced" ? 1 : 0;
        }
        else if (std::strcmp(argv[i], "--decoder") == 0 && i + 1 < argc) g_decoder_override = std::strcmp(argv[++i], "gpu") == 0 ? 1 : 0;
        else if (std::strcmp(argv[i], "--audio-soak") == 0 && i + 1 < argc) audio_soak = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--audio-exercise") == 0) audio_exercise = true;
        else if (std::strcmp(argv[i], "--record-test") == 0 && i + 1 < argc) record_test = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--gallery-test") == 0 && i + 1 < argc) gallery_test_dir = argv[++i];
        else if (std::strcmp(argv[i], "--record-disconnect") == 0) record_disconnect = true;
        else if (std::strcmp(argv[i], "--page") == 0 && i + 1 < argc) start_page = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--gallery") == 0) start_gallery = true;
        else if (std::strcmp(argv[i], "--view") == 0 && i + 1 < argc) { start_view = argv[++i]; start_gallery = true; }
        else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc) start_theme = std::strcmp(argv[++i], "light") == 0 ? 1 : 0;
#if SL_LIVE_STREAMING
        else if (std::strcmp(argv[i], "--broadcast-test") == 0 && i + 1 < argc) broadcast_test = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--broadcast-url") == 0 && i + 1 < argc) broadcast_urls.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--broadcast-height") == 0 && i + 1 < argc) broadcast_quality.height = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--broadcast-fps") == 0 && i + 1 < argc) broadcast_quality.fps = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--broadcast-kbps") == 0 && i + 1 < argc) broadcast_quality.video_kbps = std::atoi(argv[++i]);
#endif
        else if (std::strcmp(argv[i], "--yuv-test") == 0 && i + 1 < argc) yuv_test_file = argv[++i];
        else if (std::strcmp(argv[i], "--yuv-test-cpu-decode") == 0) yuv_test_gpu = false;
        else if (std::strcmp(argv[i], "--record-dir") == 0 && i + 1 < argc) record_dir = argv[++i];
        else if (std::strcmp(argv[i], "--audio-cycles") == 0 && i + 1 < argc) audio_cycles = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--audio-delay") == 0 && i + 1 < argc) audio_delay = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--fullscreen") == 0) fullscreen = true;
        else if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("Glint " GLINT_VERSION "\n");
            return 0;
        }
    }

    for (int i = 1; i < argc; i++) {
#if SL_LIVE_STREAMING
        if (std::strcmp(argv[i], "--store-test") == 0) return run_store_test();   // stream destinations
#endif
        if (std::strcmp(argv[i], "--settings-test") == 0) return run_settings_test();
        if (std::strcmp(argv[i], "--decode-file") == 0 && i + 1 < argc) {
            // --decode-file <mkv> [gpu|cpu] [hash-file]
            bool gpu = i + 2 < argc && std::strcmp(argv[i + 2], "gpu") == 0;
            std::string out = i + 3 < argc ? argv[i + 3] : "";
            return run_decode_file_test(argv[i + 1], gpu, out);
        }
#if SL_LIVE_STREAMING   // live streaming test modes (off with the feature)
        if (std::strcmp(argv[i], "--oauth-test") == 0 && i + 1 < argc) {
            // Sign-in flow against $GLINT_OAUTH_TEST_BASE (a local mock): --oauth-test twitch|youtube [client_id]
            std::string which = argv[i + 1];
            std::string client = i + 2 < argc ? argv[i + 2] : "mockclient123";
            glint::AccountLinker linker;
            std::string err;
            bool started = which == "twitch" ? linker.start_twitch(client, err) : linker.start_youtube(client, "mock-secret", err);
            if (!started) {
                std::printf("refused: %s\n", err.c_str());
                return 3;
            }
            int last = -1;
            auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
                glint::AccountLink s = linker.snapshot();
                if ((int)s.state != last) {
                    last = (int)s.state;
                    std::printf("state %d: %s%s%s\n", last, s.message.c_str(), s.user_code.empty() ? "" : "  code=",
                                s.user_code.c_str());
                    std::fflush(stdout);
                }
                if (s.state == glint::AccountLink::State::Done || s.state == glint::AccountLink::State::Failed) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            glint::AccountLink result;
            if (!linker.take_result(result)) {
                std::printf("FAILED: %s\n", linker.snapshot().error.c_str());
                return 1;
            }
            std::string k = result.stream_key;
            std::printf("DONE account=%s server=%s key=%s (%zu chars, masked)\n", result.account.c_str(),
                        result.server.c_str(), k.size() > 2 ? ("****" + k.substr(k.size() - 2)).c_str() : "?", k.size());
            glint::AccountLink again;
            std::printf("second take_result: %s\n", linker.take_result(again) ? "returned the key again (BAD)" : "refused (ok)");
            return 0;
        }
        if (std::strcmp(argv[i], "--tls-probe") == 0 && i + 1 < argc) {
            // TLS handshake only, no data: host[:port]
            std::string hp = argv[i + 1], err;
            size_t c = hp.rfind(':');
            std::string host = c == std::string::npos ? hp : hp.substr(0, c);
            int port = c == std::string::npos ? 443 : std::atoi(hp.c_str() + c + 1);
            bool ok = glint::tls_probe(host, port, err);
            std::printf("%s %s:%d %s\n", ok ? "TRUSTED" : "REFUSED", host.c_str(), port, err.c_str());
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--golive-test") == 0 && i + 1 < argc) return run_golive_test(url, std::atoi(argv[i + 1]));
#endif
    }
#if SL_LIVE_STREAMING
    if (broadcast_test > 0) return run_broadcast_test(url, broadcast_test, broadcast_urls, broadcast_quality);
#endif
    if (!gallery_test_dir.empty()) return run_gallery_test(gallery_test_dir);
    if (record_test > 0) return run_record_test(url, record_test, record_dir, record_disconnect);
    if (audio_cycles > 0) return run_audio_cycles(url, audio_cycles);
    if (audio_soak > 0) return run_audio_soak(url, audio_soak, audio_exercise, audio_delay);

    if (headless > 0) {
        glint::App app;
        app.set_latency((glint::App::Latency)headless_latency);
        app.start(url);
        draw_headless(app, headless);
        bool ok = app.frame_count() > 0;
        app.stop();
        return ok ? 0 : 1;
    }

    glint::log_open(glint::settings_dir());
    glint::logf("Glint " GLINT_VERSION " starting on %s; settings in %s",
              glint::windows_version().c_str(), glint::settings_dir().c_str());
    glint::AppSettings settings;
    {
        std::vector<std::string> warnings;
        glint::load_settings(settings, warnings);
        for (const auto& w : warnings) glint::logf("settings: %s", w.c_str());
    }
    if (!url_given && !settings.console_url.empty()) url = settings.console_url;

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);   // place it before showing, no jump
    float scale = 1.0f;
    {
        float sx = 1.0f, sy = 1.0f;
        glfwGetMonitorContentScale(glfwGetPrimaryMonitor(), &sx, &sy);
        if (sx > 0.5f && sx < 4.0f) scale = sx;
    }
    int win_w = settings.window_w > 0 ? settings.window_w : (int)(1440 * scale);
    int win_h = settings.window_h > 0 ? settings.window_h : (int)(860 * scale);
    GLFWwindow* window = glfwCreateWindow(win_w, win_h, "Glint", nullptr, nullptr);
    if (!window) return 1;
    g_window = window;
    if (settings.window_w > 0 && settings.window_x != INT_MIN &&
        position_visible(settings.window_x, settings.window_y, settings.window_w))
        glfwSetWindowPos(window, settings.window_x, settings.window_y);
    glfwShowWindow(window);
    if (settings.maximized) glfwMaximizeWindow(window);
    glfwGetWindowPos(window, &g_shell.normal[0], &g_shell.normal[1]);
    g_shell.normal[2] = win_w;
    g_shell.normal[3] = win_h;
    if (settings.window_w <= 0) g_shell.normal[2] = g_shell.normal[3] = 0;   // nothing saved yet

    if (!glfwVulkanSupported()) {
        std::printf("GLFW: Vulkan Not Supported\n");
        return 1;
    }

    if (fullscreen || settings.fullscreen) set_fullscreen(true);

    ImVector<const char*> extensions;
    uint32_t extensions_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    for (uint32_t i = 0; i < extensions_count; i++) extensions.push_back(glfw_extensions[i]);
    SetupVulkan(extensions);

    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
    check_vk_result(err);

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, w, h);

    create_upload_resources();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ui::S = scale;
    ui::apply_style(scale);

    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.RenderPass = wd->RenderPass;
    init_info.Subpass = 0;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = g_Allocator;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    static std::string ini_path = (std::filesystem::u8path(glint::settings_dir()) / "imgui.ini").u8string();
    io.IniFilename = ini_path.c_str();
    if (!yuv_test_file.empty()) {
        ui::load_fonts(scale);
        ImGui_ImplVulkan_NewFrame();   // builds the font texture (AddTexture needs the backend ready)
        int rc = run_yuv_test(yuv_test_file, yuv_test_gpu);
        yuv_converter_destroy();
        if (g_UploadFence) vkDestroyFence(g_Device, g_UploadFence, g_Allocator);
    if (g_UploadPool) vkDestroyCommandPool(g_Device, g_UploadPool, g_Allocator);
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        CleanupVulkanWindow();
        CleanupVulkan();
        glfwDestroyWindow(window);
        glfwTerminate();
        glint::log_close();
        return rc;
    }
    ui::load_fonts(scale);

    glint::App app;
    VideoTexture texture;
    texture.live = true;   // gets the picture enhancement passes
    if (!record_dir.empty()) app.set_media_dir(record_dir);
    apply_settings(app, settings, !record_dir.empty());
    std::snprintf(g_shell.url, sizeof(g_shell.url), "%s", url.c_str());
    if (start_page > 0 && start_page <= PageCount) {
        g_shell.settings_open = true;          // --page N opens Settings on tab N (1 = Console)
        g_shell.page = start_page - 1;
    }
    if (start_gallery) g_shell.gallery_open = true;
    if (start_theme >= 0) ui::set_theme(start_theme, true);
    uint64_t frame_seen = 0;

    app.set_frame_notify([] { glfwPostEmptyEvent(); });   // before start(): read by the decode thread
    app.start(url);
    glint::AppSettings saved = capture_settings(app);
    double dirty_since = -1.0;

    // Render when a new video frame arrives or input comes in, and otherwise at
    // 60 Hz for animations: redrawing at the monitor's refresh (e.g. 155 Hz)
    // for 60 fps video only burns CPU.
    double last_render = 0.0;
    while (!glfwWindowShouldClose(window)) {
        {
            double wait = 1.0 / 60.0 - (glfwGetTime() - last_render);
            if (wait > 0.0) glfwWaitEventsTimeout(wait);
            else glfwPollEvents();
            last_render = glfwGetTime();
        }

        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 &&
            (g_SwapChainRebuild || g_MainWindowData.Width != fb_width ||
             g_MainWindowData.Height != fb_height)) {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device,
                                                   &g_MainWindowData, g_QueueFamily, g_Allocator,
                                                   fb_width, fb_height, g_MinImageCount);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        process_gallery_textures();
        process_viewer_texture();
        if (!start_view.empty()) {   // --view, on the first frame (the viewer needs ImGui running)
            open_viewer(app, {start_view}, start_view);
            start_view.clear();
        }

        picture_update();
        auto frame = app.take_frame(frame_seen);
        if (frame && frame->width > 0 && frame->height > 0) {
            present_picture(texture, *frame, app);
            enhance_dump_maybe();
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        draw_ui(app, texture);

        // Remember the normal window rectangle, then save changed settings once
        // they have settled for a second (and no drag is in progress).
        if (!g_shell.fullscreen && !glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) {
            glfwGetWindowPos(window, &g_shell.normal[0], &g_shell.normal[1]);
            glfwGetWindowSize(window, &g_shell.normal[2], &g_shell.normal[3]);
        }
        glint::AppSettings current = capture_settings(app);
        if (current != saved) {
            if (dirty_since < 0) dirty_since = ImGui::GetTime();
            if (ImGui::GetTime() - dirty_since > 1.0 && !ImGui::IsAnyMouseDown()) {
                std::string error;
                if (glint::save_settings(current, error)) saved = current;
                else glint::logf("settings: %s", error.c_str());
                dirty_since = -1.0;
            }
        } else {
            dirty_since = -1.0;
        }

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!minimized) {
            wd->ClearValue.color.float32[0] = 0.0f;
            wd->ClearValue.color.float32[1] = 0.0f;
            wd->ClearValue.color.float32[2] = 0.0f;
            wd->ClearValue.color.float32[3] = 1.0f;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }
    }

    {
        std::string error;
        glint::AppSettings last = capture_settings(app);
        if (last != saved && !glint::save_settings(last, error)) glint::logf("settings: %s", error.c_str());
    }
    app.stop();
    glint::logf("exit");
    glint::log_close();
    err = vkDeviceWaitIdle(g_Device);
    check_vk_result(err);
    destroy_texture(texture);
    g_viewer.player.close();
    destroy_texture(g_viewer_tex);
    destroy_thumbs();
    enhance_destroy();
    yuv_converter_destroy();

    if (g_UploadFence) vkDestroyFence(g_Device, g_UploadFence, g_Allocator);
    if (g_UploadPool) vkDestroyCommandPool(g_Device, g_UploadPool, g_Allocator);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow();
    CleanupVulkan();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
