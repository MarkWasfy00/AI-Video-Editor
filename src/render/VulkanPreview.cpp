#include "render/VulkanPreview.h"

#include <QDebug>
#include <QPointer>
#include <QVulkanDeviceFunctions>
#include <QVulkanFunctions>
#include <QWidget>

#include <array>
#include <cstring>
#include <vector>

namespace ve {

namespace {

// SPIR-V generated at build time by glslc (see CMakeLists.txt).
const std::uint32_t kVertSpv[] =
#include "composite.vert.inc"
;
const std::uint32_t kFragSpv[] =
#include "composite.frag.inc"
;

constexpr int kMaxLayers = 16;

class VulkanRenderer : public QVulkanWindowRenderer {
public:
    explicit VulkanRenderer(VulkanPreviewWindow* w) : m_window(w) {}

    void initResources() override;
    void initSwapChainResources() override {}
    void releaseSwapChainResources() override {}
    void releaseResources() override;
    void startNextFrame() override;

private:
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory imageMemory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
        bool shaderReadable = false;
        VideoFramePtr uploaded;
    };

    uint32_t memoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    bool ensureSlot(Slot& s, int w, int h);
    void destroySlot(Slot& s);
    void uploadSlot(VkCommandBuffer cb, Slot& s, const VideoFramePtr& frame);
    VkShaderModule createShader(const std::uint32_t* code, size_t bytes);

    VulkanPreviewWindow* m_window;
    QVulkanDeviceFunctions* m_df = nullptr;
    QVulkanFunctions* m_f = nullptr;
    VkDevice m_dev = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_memProps{};

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    std::vector<std::array<Slot, kMaxLayers>> m_slots; // [frame in flight][layer]
};

uint32_t VulkanRenderer::memoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (m_memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

VkShaderModule VulkanRenderer::createShader(const std::uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (m_df->vkCreateShaderModule(m_dev, &ci, nullptr, &mod) != VK_SUCCESS)
        qWarning("Vulkan: failed to create shader module");
    return mod;
}

void VulkanRenderer::initResources()
{
    QVulkanInstance* inst = m_window->vulkanInstance();
    m_dev = m_window->device();
    m_df = inst->deviceFunctions(m_dev);
    m_f = inst->functions();
    m_f->vkGetPhysicalDeviceMemoryProperties(m_window->physicalDevice(), &m_memProps);

    const VkPhysicalDeviceProperties* props = m_window->physicalDeviceProperties();
    m_window->setDescription(QStringLiteral("Vulkan %1.%2 - %3")
                                 .arg(VK_API_VERSION_MAJOR(props->apiVersion))
                                 .arg(VK_API_VERSION_MINOR(props->apiVersion))
                                 .arg(QString::fromUtf8(props->deviceName)));

    // Sampler
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    m_df->vkCreateSampler(m_dev, &si, nullptr, &m_sampler);

    // Descriptor set layout: one combined image sampler
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 1;
    dl.pBindings = &binding;
    m_df->vkCreateDescriptorSetLayout(m_dev, &dl, nullptr, &m_setLayout);

    const int frames = m_window->concurrentFrameCount();
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(frames * kMaxLayers)};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = uint32_t(frames * kMaxLayers);
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    m_df->vkCreateDescriptorPool(m_dev, &pi, nullptr, &m_pool);

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(ShaderParams);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    m_df->vkCreatePipelineLayout(m_dev, &pli, nullptr, &m_pipelineLayout);

    VkPipelineCacheCreateInfo pc{};
    pc.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    m_df->vkCreatePipelineCache(m_dev, &pc, nullptr, &m_cache);

    // Pipeline
    VkShaderModule vs = createShader(kVertSpv, sizeof(kVertSpv));
    VkShaderModule fs = createShader(kFragSpv, sizeof(kFragSpv));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = m_window->sampleCountFlagBits();

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.alphaBlendOp = VK_BLEND_OP_ADD;
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = m_pipelineLayout;
    gp.renderPass = m_window->defaultRenderPass();
    if (m_df->vkCreateGraphicsPipelines(m_dev, m_cache, 1, &gp, nullptr, &m_pipeline) != VK_SUCCESS)
        qWarning("Vulkan: failed to create graphics pipeline");

    if (vs) m_df->vkDestroyShaderModule(m_dev, vs, nullptr);
    if (fs) m_df->vkDestroyShaderModule(m_dev, fs, nullptr);

    m_slots.assign(size_t(frames), {});
}

void VulkanRenderer::destroySlot(Slot& s)
{
    if (s.set) m_df->vkFreeDescriptorSets(m_dev, m_pool, 1, &s.set);
    if (s.view) m_df->vkDestroyImageView(m_dev, s.view, nullptr);
    if (s.image) m_df->vkDestroyImage(m_dev, s.image, nullptr);
    if (s.imageMemory) m_df->vkFreeMemory(m_dev, s.imageMemory, nullptr);
    if (s.mapped) m_df->vkUnmapMemory(m_dev, s.stagingMemory);
    if (s.staging) m_df->vkDestroyBuffer(m_dev, s.staging, nullptr);
    if (s.stagingMemory) m_df->vkFreeMemory(m_dev, s.stagingMemory, nullptr);
    s = Slot{};
}

bool VulkanRenderer::ensureSlot(Slot& s, int w, int h)
{
    if (s.image && s.width == w && s.height == h) return true;
    destroySlot(s);

    // Device-local sampled image
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {uint32_t(w), uint32_t(h), 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_df->vkCreateImage(m_dev, &ii, nullptr, &s.image) != VK_SUCCESS) { destroySlot(s); return false; }

    VkMemoryRequirements req;
    m_df->vkGetImageMemoryRequirements(m_dev, s.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || m_df->vkAllocateMemory(m_dev, &ai, nullptr, &s.imageMemory) != VK_SUCCESS) {
        destroySlot(s);
        return false;
    }
    m_df->vkBindImageMemory(m_dev, s.image, s.imageMemory, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = s.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (m_df->vkCreateImageView(m_dev, &vi, nullptr, &s.view) != VK_SUCCESS) { destroySlot(s); return false; }

    // Host-visible staging buffer, persistently mapped
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_df->vkCreateBuffer(m_dev, &bi, nullptr, &s.staging) != VK_SUCCESS) { destroySlot(s); return false; }
    m_df->vkGetBufferMemoryRequirements(m_dev, s.staging, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || m_df->vkAllocateMemory(m_dev, &ai, nullptr, &s.stagingMemory) != VK_SUCCESS) {
        destroySlot(s);
        return false;
    }
    m_df->vkBindBufferMemory(m_dev, s.staging, s.stagingMemory, 0);
    m_df->vkMapMemory(m_dev, s.stagingMemory, 0, bytes, 0, &s.mapped);

    // Descriptor set
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = m_pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &m_setLayout;
    if (m_df->vkAllocateDescriptorSets(m_dev, &dai, &s.set) != VK_SUCCESS) { s.set = VK_NULL_HANDLE; destroySlot(s); return false; }
    VkDescriptorImageInfo dii{m_sampler, s.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = s.set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    m_df->vkUpdateDescriptorSets(m_dev, 1, &wr, 0, nullptr);

    s.width = w;
    s.height = h;
    s.shaderReadable = false;
    return true;
}

void VulkanRenderer::uploadSlot(VkCommandBuffer cb, Slot& s, const VideoFramePtr& frame)
{
    std::memcpy(s.mapped, frame->rgba.data(), size_t(frame->width) * frame->height * 4);

    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = s.shaderReadable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = s.shaderReadable ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    m_df->vkCmdPipelineBarrier(cb,
        s.shaderReadable ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {uint32_t(frame->width), uint32_t(frame->height), 1};
    m_df->vkCmdCopyBufferToImage(cb, s.staging, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    m_df->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &b);
    s.shaderReadable = true;
    s.uploaded = frame;
}

void VulkanRenderer::startNextFrame()
{
    VkCommandBuffer cb = m_window->currentCommandBuffer();
    const int fi = m_window->currentFrame();
    const CompositeFramePtr framePtr = m_window->frame();
    CompositeFrame empty;
    const CompositeFrame& frame = framePtr ? *framePtr : empty;
    const int n = std::min<int>(int(frame.layers.size()), kMaxLayers);
    auto& frameSlots = m_slots[size_t(fi)];

    // 1. Texture uploads (must happen outside the render pass).
    for (int i = 0; i < n; ++i) {
        const Layer& layer = frame.layers[size_t(i)];
        if (!layer.frame || layer.opacity <= 0.0) continue;
        Slot& s = frameSlots[size_t(i)];
        if (!ensureSlot(s, layer.frame->width, layer.frame->height)) continue;
        if (s.uploaded != layer.frame) uploadSlot(cb, s, layer.frame);
    }

    // 2. Render pass
    const QSize sz = m_window->swapChainImageSize();
    VkClearValue clear[3]{};
    clear[0].color = {{0.10f, 0.10f, 0.12f, 1.0f}};
    clear[1].depthStencil = {1.0f, 0};
    clear[2].color = clear[0].color;

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_window->defaultRenderPass();
    rp.framebuffer = m_window->currentFramebuffer();
    rp.renderArea.extent = {uint32_t(sz.width()), uint32_t(sz.height())};
    rp.clearValueCount = m_window->sampleCountFlagBits() > VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
    rp.pClearValues = clear;
    m_df->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

    const QRect vpRect = letterbox(sz, frame.canvasWidth, frame.canvasHeight);
    if (!vpRect.isEmpty() && m_pipeline) {
        // Canvas background
        VkClearAttachment ca{};
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = 0;
        ca.clearValue.color = {{float(frame.background.redF()), float(frame.background.greenF()),
                                float(frame.background.blueF()), 1.0f}};
        VkClearRect cr{};
        cr.rect = {{vpRect.x(), vpRect.y()}, {uint32_t(vpRect.width()), uint32_t(vpRect.height())}};
        cr.layerCount = 1;
        m_df->vkCmdClearAttachments(cb, 1, &ca, 1, &cr);

        VkViewport vp{float(vpRect.x()), float(vpRect.y()), float(vpRect.width()), float(vpRect.height()), 0.0f, 1.0f};
        m_df->vkCmdSetViewport(cb, 0, 1, &vp);
        m_df->vkCmdSetScissor(cb, 0, 1, &cr.rect);
        m_df->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        for (int i = 0; i < n; ++i) {
            const Layer& layer = frame.layers[size_t(i)];
            const Slot& s = frameSlots[size_t(i)];
            if (!layer.frame || layer.opacity <= 0.0 || !s.shaderReadable || s.uploaded != layer.frame) continue;
            m_df->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &s.set, 0, nullptr);
            const ShaderParams p = makeShaderParams(layer, frame.canvasWidth, frame.canvasHeight, NdcConvention::Vulkan);
            m_df->vkCmdPushConstants(cb, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                     0, sizeof(ShaderParams), &p);
            m_df->vkCmdDraw(cb, 4, 1, 0, 0);
        }
    }

    m_df->vkCmdEndRenderPass(cb);
    m_window->frameReady();
}

void VulkanRenderer::releaseResources()
{
    m_df->vkDeviceWaitIdle(m_dev);
    for (auto& frameSlots : m_slots)
        for (auto& s : frameSlots) destroySlot(s);
    m_slots.clear();
    if (m_pipeline) m_df->vkDestroyPipeline(m_dev, m_pipeline, nullptr);
    if (m_cache) m_df->vkDestroyPipelineCache(m_dev, m_cache, nullptr);
    if (m_pipelineLayout) m_df->vkDestroyPipelineLayout(m_dev, m_pipelineLayout, nullptr);
    if (m_pool) m_df->vkDestroyDescriptorPool(m_dev, m_pool, nullptr);
    if (m_setLayout) m_df->vkDestroyDescriptorSetLayout(m_dev, m_setLayout, nullptr);
    if (m_sampler) m_df->vkDestroySampler(m_dev, m_sampler, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_cache = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
}

} // namespace

// ---------------------------------------------------------------------------

QVulkanInstance* sharedVulkanInstance(QString* error)
{
    static QVulkanInstance* instance = nullptr;
    static bool attempted = false;
    static QString lastError;
    if (!attempted) {
        attempted = true;
        auto* inst = new QVulkanInstance;
        inst->setApiVersion(QVersionNumber(1, 0));
#ifndef NDEBUG
        if (inst->supportedLayers().contains("VK_LAYER_KHRONOS_validation"))
            inst->setLayers({"VK_LAYER_KHRONOS_validation"});
#endif
        if (!inst->create()) {
            lastError = QStringLiteral("vkCreateInstance failed (VkResult %1)").arg(int(inst->errorCode()));
            delete inst;
        } else {
            instance = inst; // intentionally lives for the whole process
        }
    }
    if (!instance && error) *error = lastError;
    return instance;
}

QVulkanWindowRenderer* VulkanPreviewWindow::createRenderer()
{
    return new VulkanRenderer(this);
}

void VulkanPreviewWindow::setFrame(CompositeFramePtr frame)
{
    m_frame = std::move(frame);
    requestUpdate();
}

QList<VulkanDeviceInfo> vulkanDevices()
{
    QList<VulkanDeviceInfo> out;
    QVulkanInstance* inst = sharedVulkanInstance();
    if (!inst) return out;
    VulkanPreviewWindow probe;
    probe.setVulkanInstance(inst);
    for (const VkPhysicalDeviceProperties& p : probe.availablePhysicalDevices()) {
        QString type;
        switch (p.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: type = QStringLiteral("discrete"); break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: type = QStringLiteral("integrated"); break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: type = QStringLiteral("virtual"); break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: type = QStringLiteral("cpu"); break;
        default: type = QStringLiteral("other"); break;
        }
        out.push_back({QString::fromUtf8(p.deviceName), type,
                       QStringLiteral("%1.%2.%3").arg(VK_API_VERSION_MAJOR(p.apiVersion))
                           .arg(VK_API_VERSION_MINOR(p.apiVersion)).arg(VK_API_VERSION_PATCH(p.apiVersion))});
    }
    return out;
}

VulkanPreview::VulkanPreview(QVulkanInstance* instance, const QString& deviceName, QWidget* parent)
{
    m_window = new VulkanPreviewWindow;
    m_window->setVulkanInstance(instance);
    m_window->setFlags(QVulkanWindow::PersistentResources);

    // Pick the physical device: the requested one, else the first discrete GPU.
    const auto devices = m_window->availablePhysicalDevices();
    int index = -1;
    for (int i = 0; i < devices.size() && index < 0; ++i)
        if (!deviceName.isEmpty() && QString::fromUtf8(devices[i].deviceName) == deviceName) index = i;
    for (int i = 0; i < devices.size() && index < 0; ++i)
        if (devices[i].deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) index = i;
    if (index < 0 && !devices.isEmpty()) index = 0;
    if (index >= 0) {
        m_window->setPhysicalDeviceIndex(index);
        m_deviceName = QString::fromUtf8(devices[index].deviceName);
    }
    m_container = QWidget::createWindowContainer(m_window, parent);
    m_container->setMinimumSize(320, 180);
    m_container->setFocusPolicy(Qt::NoFocus);
}

VulkanPreview::~VulkanPreview()
{
    delete m_container; // also destroys the hosted window
}

void VulkanPreview::setFrame(CompositeFramePtr frame)
{
    m_window->setFrame(std::move(frame));
}

QString VulkanPreview::backendDescription() const
{
    return m_window->description();
}

} // namespace ve
