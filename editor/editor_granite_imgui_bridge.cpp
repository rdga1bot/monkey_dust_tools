#include "editor_granite_imgui_bridge.h"

#ifdef MD_USE_GRANITE

#include <monkey_dust/render/granite_backend.h>
#include <monkey_dust/platform/md_log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

// RENDER-BACKEND-STAGE-6 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md
// §2.3/§2.4). See header's doc comment for the two pitfalls already fixed
// by reusing md::GraniteBackend::RenderFrameWithOverlay() (engine/src/
// render/granite_backend.cpp), which owns the exact sequence probes/
// granite_m3_imgui_dynamic_rendering.cpp proved live.
namespace md::editor {
namespace {

bool s_ready = false;

PFN_vkVoidFunction ImGuiVulkanLoader(const char* function_name, void* user_data) {
    auto instance = static_cast<VkInstance>(user_data);
    auto get_proc_addr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    return get_proc_addr(instance, function_name);
}

void CheckVkResult(VkResult err) {
    if (err != VK_SUCCESS) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] ImGui Vulkan error: %d", (int)err);
    }
}

void DrawIntoActiveDynamicRenderingScope(void* /*user*/, void* vk_command_buffer) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_command_buffer);
}

}  // namespace

bool GraniteImGuiBridge_Init(SDL_Window* window) {
    if (!md::GraniteBackend::Get().IsReady()) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] Init(): md::GraniteBackend not ready");
        return false;
    }

    md::GraniteVulkanHandles h = md::GraniteBackend::Get().GetVulkanHandlesForImGui();
    if (!h.dynamic_rendering_supported) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] Init(): dynamicRendering feature not enabled");
        return false;
    }

    // Operates on whatever ImGui context is CURRENT -- the caller (main.cpp)
    // already created the real editor's own context before calling this.
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, &ImGuiVulkanLoader, h.instance)) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] ImGui_ImplVulkan_LoadFunctions failed");
        return false;
    }

    VkFormat swapchain_format = (VkFormat)h.swapchain_format;
    VkPipelineRenderingCreateInfoKHR pipeline_rendering_info = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
    pipeline_rendering_info.colorAttachmentCount = 1;
    pipeline_rendering_info.pColorAttachmentFormats = &swapchain_format;

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = (VkInstance)h.instance;
    init_info.PhysicalDevice = (VkPhysicalDevice)h.physical_device;
    init_info.Device = (VkDevice)h.device;
    init_info.QueueFamily = h.queue_family;
    init_info.Queue = (VkQueue)h.queue;
    init_info.DescriptorPoolSize = 64;
    init_info.MinImageCount = 2;
    init_info.ImageCount = 2;
    init_info.UseDynamicRendering = true;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo = pipeline_rendering_info;
    init_info.CheckVkResultFn = &CheckVkResult;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] ImGui_ImplVulkan_Init failed");
        return false;
    }

    s_ready = true;
    return true;
}

void GraniteImGuiBridge_Shutdown() {
    if (!s_ready) return;
    ImGui_ImplVulkan_Shutdown();
    s_ready = false;
}

void GraniteImGuiBridge_NewFrame() {
    if (!s_ready) return;
    ImGui_ImplVulkan_NewFrame();
}

void GraniteImGuiBridge_RenderCurrentDrawData() {
    if (!s_ready) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] RenderCurrentDrawData: not initialized");
        return;
    }
    if (!md::GraniteBackend::Get().RenderFrameWithOverlay(&DrawIntoActiveDynamicRenderingScope, nullptr)) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] RenderFrameWithOverlay: no work this tick");
    }
}

}  // namespace md::editor

#else  // !MD_USE_GRANITE -- empty TU, zero Granite/Vulkan dependency

namespace md::editor {
bool GraniteImGuiBridge_Init(SDL_Window*) { return false; }
void GraniteImGuiBridge_Shutdown() {}
void GraniteImGuiBridge_NewFrame() {}
void GraniteImGuiBridge_RenderCurrentDrawData() {}
}  // namespace md::editor

#endif  // MD_USE_GRANITE
