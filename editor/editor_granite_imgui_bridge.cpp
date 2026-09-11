#include "editor_granite_imgui_bridge.h"

#ifdef MD_USE_GRANITE

#include <monkey_dust/render/granite_backend.h>
#include <monkey_dust/platform/md_log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"

// RENDER-BACKEND-STAGE-6 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.3).
// Permanent port of probes/granite_m3_imgui_dynamic_rendering.cpp -- see
// that file's doc comment for the two non-obvious pitfalls (is_legacy_
// layout() rejecting VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, and
// vkCmdBeginRendering/EndRendering needing Device::get_device_table()
// instead of the global volk-resolved symbols) already fixed here by
// reusing md::GraniteBackend::RenderFrameWithOverlay() (engine/src/render/
// granite_backend.cpp), which owns that exact sequence now.
namespace md::editor {
namespace {

ImGuiContext* s_ctx = nullptr;
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

    // Own, dedicated context -- see header's doc comment for why (two
    // independent GPU devices on one window, not a replacement for the
    // real editor's SDL_GPU-backed ImGui context).
    ImGuiContext* prev = ImGui::GetCurrentContext();
    s_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(s_ctx);
    ImGui_ImplSDL3_InitForVulkan(window);

    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, &ImGuiVulkanLoader, h.instance)) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] ImGui_ImplVulkan_LoadFunctions failed");
        ImGui::SetCurrentContext(prev);
        ImGui::DestroyContext(s_ctx);
        s_ctx = nullptr;
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
        ImGui::SetCurrentContext(prev);
        ImGui::DestroyContext(s_ctx);
        s_ctx = nullptr;
        return false;
    }

    ImGui::SetCurrentContext(prev);
    s_ready = true;
    return true;
}

void GraniteImGuiBridge_Shutdown() {
    if (!s_ready) return;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s_ctx);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::SetCurrentContext(prev);
    ImGui::DestroyContext(s_ctx);
    s_ctx = nullptr;
    s_ready = false;
}

void GraniteImGuiBridge_RenderEditorOverlay(void* /*user*/, const md::render_backend::RenderFrameParams& /*params*/) {
    if (!s_ready) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] RenderEditorOverlay: not initialized");
        return;
    }

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s_ctx);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    ImGui::Begin("Granite backend (Крок 3)");
    ImGui::Text("ImGui over Granite via VK_KHR_dynamic_rendering");
    ImGui::Text("Permanent code -- docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.3");
    ImGui::End();
    ImGui::Render();

    if (!md::GraniteBackend::Get().RenderFrameWithOverlay(&DrawIntoActiveDynamicRenderingScope, nullptr)) {
        MD_LOG(MD_LOG_WARNING, "[GraniteImGuiBridge] RenderFrameWithOverlay: no work this tick");
    }

    ImGui::SetCurrentContext(prev);
}

}  // namespace md::editor

#else  // !MD_USE_GRANITE -- empty TU, zero Granite/Vulkan dependency

namespace md::editor {
bool GraniteImGuiBridge_Init(SDL_Window*) { return false; }
void GraniteImGuiBridge_Shutdown() {}
void GraniteImGuiBridge_RenderEditorOverlay(void*, const md::render_backend::RenderFrameParams&) {}
}  // namespace md::editor

#endif  // MD_USE_GRANITE
