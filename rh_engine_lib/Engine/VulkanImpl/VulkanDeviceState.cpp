#include <common.h>

#include "SyncPrimitives/VulkanCPUSyncPrimitive.h"
#include "SyncPrimitives/VulkanGPUSyncPrimitive.h"
#include "VulkanBuffer.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCommon.h"
#include "VulkanConvert.h"
#include "VulkanDescriptorSet.h"
#include "VulkanDescriptorSetAllocator.h"
#include "VulkanDescriptorSetLayout.h"
#include "VulkanDeviceState.h"
#include "VulkanFrameBuffer.h"
#include "VulkanImage.h"
#include "VulkanImageView.h"
#include "VulkanPipeline.h"
#include "VulkanPipelineLayout.h"
#include "VulkanRenderPass.h"
#include "VulkanSampler.h"
#include "VulkanShader.h"
#include "VulkanWin32Window.h"

#include <Engine/Common/ScopedPtr.h>
#include <memory_resource>
#include <numeric>
#include <ranges>
#include <Engine/Definitions.h>

using namespace rh;
using namespace rh::engine;
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

static VKAPI_ATTR VkBool32 VKAPI_CALL
VkDebugCallback( VkDebugUtilsMessageSeverityFlagBitsEXT /*messageSeverity*/,
                 VkDebugUtilsMessageTypeFlagsEXT /* messageType*/,
                 const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
                 void * /*pUserData */ )
{
    debug::DebugLogger::Log( ToRHString( pCallbackData->pMessage ) );
    return VK_FALSE;
}

VkResult
CreateDebugUtilsMessengerEXT( vk::Instance instance,
                              const VkDebugUtilsMessengerCreateInfoEXT *info,
                              VkDebugUtilsMessengerEXT *                ext )
{

    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        instance.getProcAddr( "vkCreateDebugUtilsMessengerEXT" ) );

    if ( func != nullptr )
        return func( static_cast<VkInstance>( instance ), info, nullptr, ext );
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void DestroyDebugUtilsMessengerEXT( vk::Instance             instance,
                                    VkDebugUtilsMessengerEXT callback )
{
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        instance.getProcAddr( "vkDestroyDebugUtilsMessengerEXT" ) );

    if ( func != nullptr )
        func( static_cast<VkInstance>( instance ), callback, nullptr );
}

VulkanDeviceState::VulkanDeviceState()
{
    using namespace rh::debug;
    DebugLogger::Log( "VulkanDeviceState initialization" );

    auto vkGetInstanceProcAddr =
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>( "vkGetInstanceProcAddr" );
    VULKAN_HPP_DEFAULT_DISPATCHER.init( vkGetInstanceProcAddr );

    m_aExtensions.emplace_back( VK_KHR_SURFACE_EXTENSION_NAME );
    m_aExtensions.emplace_back( VK_KHR_WIN32_SURFACE_EXTENSION_NAME );
    m_aExtensions.emplace_back(
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME );

    if constexpr ( gDebugEnabled )
    {
        m_aExtensions.emplace_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );

        m_aLayers.emplace_back( "VK_LAYER_KHRONOS_validation" );
        m_aLayers.emplace_back( "VK_LAYER_LUNARG_monitor" );
    }
    // Validate layer support
    {
        auto layer_properties = vk::enumerateInstanceLayerProperties();
        std::set<std::string_view> required_layers{};
        for ( auto l : m_aLayers )
            required_layers.insert( l );
        std::set<std::string_view> supported_layers{};
        for ( auto &l : layer_properties.value )
            supported_layers.insert( std::string_view( l.layerName ) );

        std::set<std::string_view> unsupported_layers{};

        std::ranges::set_difference(
            required_layers, supported_layers,
            std::inserter( unsupported_layers, unsupported_layers.end() ) );
        if ( !unsupported_layers.empty() )
        {
            DebugLogger::Error(
                "VulkanDeviceState initialization failed, required "
                "instance layers are unsupported:" );
            for ( const auto l : unsupported_layers )
                DebugLogger::ErrorFmt( "\tinstance layer %s is unsupported!",
                                       l );
            return;
        }
    }
    // Validate extension support
    {
        auto extension_properties = vk::enumerateInstanceExtensionProperties();

        std::set<std::string_view> required_extensions{};
        for ( auto ext : m_aExtensions )
            required_extensions.insert( ext );
        std::set<std::string_view> supported_extensions{};
        for ( auto &l : extension_properties.value )
            supported_extensions.insert( std::string_view( l.extensionName ) );

        std::vector<std::string_view> unsupported_extensions{};
        std::ranges::set_difference(
            required_extensions, supported_extensions,
            std::inserter( unsupported_extensions,
                           unsupported_extensions.end() ) );
        if ( !unsupported_extensions.empty() )
        {
            DebugLogger::Error(
                "VulkanDeviceState initialization failed, required "
                "instance extensions are unsupported:" );
            for ( const auto &ext : unsupported_extensions )
                DebugLogger::ErrorFmt( "\textension %s is unsupported!",
                                       LogLevel::Info, ext );
            return;
        }
    }

    static const char *app_name    = "Render Hook App";
    static const char *engine_name = "RenderHook";
    // App info
    vk::ApplicationInfo app_info{};
    app_info.pApplicationName = app_name;
    app_info.pEngineName      = engine_name;

    // Instance info
    vk::InstanceCreateInfo inst_info{};
    inst_info.pApplicationInfo = &app_info;
    inst_info.enabledExtensionCount =
        static_cast<uint32_t>( m_aExtensions.size() );
    inst_info.ppEnabledExtensionNames = m_aExtensions.data();
    inst_info.enabledLayerCount   = static_cast<uint32_t>( m_aLayers.size() );
    inst_info.ppEnabledLayerNames = m_aLayers.data();

    // Create vulkan instance
    if ( !CALL_VK_API( vk::createInstance( &inst_info, nullptr, &m_vkInstance ),
                       TEXT( "VulkanRenderer failed to initialize: Failed to "
                             "initialize Vulkan Instance!" ) ) )
        return;

    VULKAN_HPP_DEFAULT_DISPATCHER.init( m_vkInstance );
    auto adapter_res = m_vkInstance.enumeratePhysicalDevices();
    // Enumerate GPUS
    if ( !CALL_VK_API( adapter_res.result,
                       TEXT( "VulkanRenderer failed to initialize: Failed to "
                             "enumerate GPU's!" ) ) )
        return;

    m_aAdapters = adapter_res.value;
    // Query GPU info
    for ( auto gpu : m_aAdapters )
        m_aAdaptersInfo.emplace_back( gpu );

    // TODO: Move to constexpr debug checks
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.pfnUserCallback = VkDebugCallback;

    if constexpr ( gDebugEnabled )
    {
        createInfo.messageType |=
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.messageSeverity |=
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        if ( !CALL_VK_API(
                 CreateDebugUtilsMessengerEXT( m_vkInstance, &createInfo,
                                               &m_debugCallback ),
                 TEXT( "VulkanRenderer failed to initialize: Failed to create "
                       "debug utils messenger!" ) ) )
            return;
    }

    DISPLAY_DEVICE display_device{};
    display_device.cb = sizeof( DISPLAY_DEVICE );

    unsigned int display_id = 0;

    rh::debug::DebugLogger::Log( "DisplayDeviceInfo Enumeration:\t" );

    while ( EnumDisplayDevices( nullptr, display_id, &display_device, 0 ) != 0 )
    {
        display_id++;

        StringStream ss;
        ss << "DisplayDeviceInfo:\t"
           << "\nDeviceName:\t" << display_device.DeviceName
           << "\nDeviceString:\t" << display_device.DeviceString;
        debug::DebugLogger::Log( ss.str() );

        if ( display_device.StateFlags & DISPLAY_DEVICE_ACTIVE )
        {
            DisplayInfo display_info = { display_device.DeviceName, {} };

            unsigned int display_mode_id = 0;

            DEVMODE device_mode{};
            device_mode.dmSize = sizeof( DEVMODE );

            debug::DebugLogger::Log( "DisplaySettings Enumeration:\t" );

            while ( EnumDisplaySettings( display_info.m_sDisplayName.c_str(),
                                         display_mode_id, &device_mode ) )
            {
                StringStream stringStream;
                stringStream << "DisplayModeInfo:\t"
                             << "\nBitsPerPixel\t" << device_mode.dmBitsPerPel
                             << "\nWidth\t" << device_mode.dmPelsWidth
                             << "\nHeight:\t" << device_mode.dmPelsHeight
                             << "\nFrequency:\t"
                             << device_mode.dmDisplayFrequency;
                debug::DebugLogger::Log( stringStream.str() );

                display_info.m_aDisplayModes.push_back(
                    { static_cast<uint32_t>( device_mode.dmPelsWidth ),
                      static_cast<uint32_t>( device_mode.dmPelsHeight ),
                      static_cast<uint32_t>( device_mode.dmDisplayFrequency ),
                      0 } );

                display_mode_id++;
            }

            m_aDisplayInfos.push_back( display_info );
        }
    }
}

VulkanDeviceState::~VulkanDeviceState()
{
#ifdef _DEBUG
    DestroyDebugUtilsMessengerEXT( m_vkInstance, m_debugCallback );
#endif
    m_vkInstance.destroy();
}

bool VulkanDeviceState::Init()
{
    using namespace rh::debug;
    DebugLogger::LogFmt( "VulkanDeviceState initialization for GPU %u:",
                         LogLevel::Info, m_uiCurrentAdapter );

    auto queue_family_properties =
        m_aAdapters[m_uiCurrentAdapter].getQueueFamilyProperties();
    uint32_t i = 0;
    for ( const auto &queue_family : queue_family_properties )
    {
        if ( queue_family.queueFlags & vk::QueueFlagBits::eGraphics )
            m_iGraphicsQueueFamilyIdx = i;
        if ( queue_family.queueFlags & vk::QueueFlagBits::eTransfer )
            m_iCopyQueueFamilyIdx = i;
        i++;
    }
    DebugLogger::LogFmt( "Graphics Queue id - %u; Copy Queue id - %u",
                         LogLevel::Info, m_iGraphicsQueueFamilyIdx,
                         m_iCopyQueueFamilyIdx );

    float                     queuePriority[] = { 1.0f };
    std::vector<const char *> device_extensions;
    device_extensions.emplace_back( VK_KHR_SWAPCHAIN_EXTENSION_NAME );
#ifdef ARCH_64BIT
    device_extensions.emplace_back(
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME );
    device_extensions.emplace_back( VK_KHR_BIND_MEMORY_2_EXTENSION_NAME );
    device_extensions.emplace_back( VK_NV_RAY_TRACING_EXTENSION_NAME );
    device_extensions.emplace_back( VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME );
    device_extensions.emplace_back( VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME );
    device_extensions.emplace_back( VK_KHR_MAINTENANCE3_EXTENSION_NAME );
    device_extensions.emplace_back(
        VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME );

    vk::PhysicalDeviceDescriptorIndexingFeaturesEXT indexFeature{};
    indexFeature.runtimeDescriptorArray                    = true;
    indexFeature.descriptorBindingPartiallyBound           = true;
    indexFeature.descriptorBindingUpdateUnusedWhilePending = true;
    vk::PhysicalDeviceScalarBlockLayoutFeaturesEXT scalarFeature{};
    scalarFeature.scalarBlockLayout = true;

#endif
    vk::DeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.queueFamilyIndex          = m_iGraphicsQueueFamilyIdx;
    queueCreateInfo.queueCount                = 1;
    queueCreateInfo.pQueuePriorities          = queuePriority;

    /* TODO: Implement copy queue for copy operations
    vk::DeviceQueueCreateInfo copyQueueCreateInfo = {};
    copyQueueCreateInfo.queueFamilyIndex          = m_iCopyQueueFamilyIdx;
    copyQueueCreateInfo.queueCount                = 1;
    copyQueueCreateInfo.pQueuePriorities          = queuePriority;*/

    vk::DeviceCreateInfo info{};
#ifdef ARCH_64BIT
    indexFeature.pNext = &scalarFeature;
    vk::PhysicalDeviceFeatures2 enabledFeatures2{};
    enabledFeatures2.pNext = &indexFeature;
    info.pNext             = &enabledFeatures2;
#endif

    info.pQueueCreateInfos    = &queueCreateInfo;
    info.queueCreateInfoCount = 1;
    info.enabledExtensionCount =
        static_cast<uint32_t>( device_extensions.size() );
    info.ppEnabledExtensionNames = device_extensions.data();

    auto extensions =
        m_aAdapters[m_uiCurrentAdapter].enumerateDeviceExtensionProperties();
    for ( auto ext : extensions.value )
        debug::DebugLogger::LogFmt( "Device Extension supported: %s",
                                    LogLevel::Info, ext.extensionName.data() );

    auto device_res = m_aAdapters[m_uiCurrentAdapter].createDevice( info );
    if ( !CALL_VK_API( device_res.result,
                       TEXT( "Failed to create logical device!" ) ) )
        return false;
    m_vkDevice = device_res.value;

    VULKAN_HPP_DEFAULT_DISPATCHER.init( m_vkDevice );

    m_vkMainQueue = m_vkDevice.getQueue( m_iGraphicsQueueFamilyIdx, 0 );

    auto command_pool_res = m_vkDevice.createCommandPool(
        { vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
          m_iGraphicsQueueFamilyIdx } );

    if ( !CALL_VK_API( command_pool_res.result,
                       TEXT( "Failed to create main command pool!" ) ) )
        return false;
    m_vkCommandPool = command_pool_res.value;

    VulkanMemoryAllocatorCreateInfo vk_malloc_ci{};
    vk_malloc_ci.mPhysicalDevice = m_aAdapters[m_uiCurrentAdapter];
    vk_malloc_ci.mDevice         = m_vkDevice;
    mDefaultAllocator            = new VulkanMemoryAllocator( vk_malloc_ci );

    DebugLogger::Log( "VulkanDeviceState initialization finished" );
    return true;
}

bool VulkanDeviceState::Shutdown()
{
    delete mDefaultAllocator;
    delete mMainCmdBuffer;
    mMainCmdBuffer = nullptr;
    m_vkDevice.destroyCommandPool( m_vkCommandPool );
    m_vkDevice.destroy();
    return true;
}

bool VulkanDeviceState::GetAdaptersCount( unsigned int &count )
{
    count = static_cast<unsigned int>( m_aAdapters.size() );
    return true;
}

bool VulkanDeviceState::GetAdapterInfo( unsigned int id, String &info )
{
    if ( id >= m_aAdapters.size() )
        return false;

    info = ToRHString( m_aAdapters[id].getProperties().deviceName );
    return true;
}

bool VulkanDeviceState::GetCurrentAdapter( unsigned int &id )
{
    id = m_uiCurrentAdapter;
    return true;
}

bool VulkanDeviceState::SetCurrentAdapter( unsigned int id )
{
    if ( id > m_aAdapters.size() )
        return false;
    m_uiCurrentAdapter = id;
    return true;
}

bool VulkanDeviceState::GetOutputCount( unsigned int  adapterId,
                                        unsigned int &count )
{
    if ( adapterId > m_aAdapters.size() )
        return false;

    count = static_cast<uint32_t>( m_aDisplayInfos.size() );
    return true;
}

bool VulkanDeviceState::GetOutputInfo( unsigned int id, String &info )
{
    if ( id >= m_aDisplayInfos.size() )
        return false;
    info = m_aDisplayInfos[id].m_sDisplayName;
    return true;
}

bool VulkanDeviceState::GetCurrentOutput( unsigned int &id )
{
    id = m_uiCurrentOutput;

    return true;
}

bool VulkanDeviceState::SetCurrentOutput( unsigned int id )
{
    if ( id >= m_aDisplayInfos.size() )
        return false;
    m_uiCurrentOutput = id;
    return true;
}

bool VulkanDeviceState::GetDisplayModeCount( unsigned int  outputId,
                                             unsigned int &count )
{
    count = static_cast<uint32_t>(
        m_aDisplayInfos[outputId].m_aDisplayModes.size() );
    return true;
}

bool VulkanDeviceState::GetDisplayModeInfo( unsigned int     id,
                                            DisplayModeInfo &info )
{
    if ( id >= m_aDisplayInfos[m_uiCurrentOutput].m_aDisplayModes.size() )
        return false;
    info = m_aDisplayInfos[m_uiCurrentOutput].m_aDisplayModes[id];
    return true;
}

bool VulkanDeviceState::GetCurrentDisplayMode( unsigned int &id )
{
    id = m_uiCurrentAdapterMode;
    return true;
}

bool VulkanDeviceState::SetCurrentDisplayMode( unsigned int id )
{
    m_uiCurrentAdapterMode = id;
    return true;
}

IDescriptorSetLayout *VulkanDeviceState::CreateDescriptorSetLayout(
    const DescriptorSetLayoutCreateParams &params )
{
    return new VulkanDescriptorSetLayout(
        { .mDevice = m_vkDevice, .mBindingList = params.mBindings } );
}

IDescriptorSetAllocator *VulkanDeviceState::CreateDescriptorSetAllocator(
    const DescriptorSetAllocatorCreateParams &params )
{
    return new VulkanDescriptorSetAllocator( {
        .mDevice          = m_vkDevice,
        .mDescriptorPools = params.mDescriptorPools,
        .mSetLimit        = params.mMaxSets,
    } );
}

IPipelineLayout *VulkanDeviceState::CreatePipelineLayout(
    const PipelineLayoutCreateParams &params )
{
    return new VulkanPipelineLayout(
        { .mDevice               = m_vkDevice,
          .mDescriptorSetLayouts = params.mSetLayouts } );
}

IWindow *VulkanDeviceState::CreateDeviceWindow( HWND              window,
                                                const OutputInfo &info )
{
    auto display_mode =
        m_aDisplayInfos[m_uiCurrentOutput].m_aDisplayModes[info.displayModeId];
    return new VulkanWin32Window(
        { .mWndHandle       = window,
          .mInstance        = m_vkInstance,
          .mGPU             = m_aAdapters[m_uiCurrentAdapter],
          .mDevice          = m_vkDevice,
          .mPresentQueue    = m_vkMainQueue,
          .mPresentQueueIdx = m_iGraphicsQueueFamilyIdx,
          .mWindowParams{ .mWidth  = display_mode.width,
                          .mHeight = display_mode.height } } );
}

ISyncPrimitive *VulkanDeviceState::CreateSyncPrimitive( SyncPrimitiveType type )
{
    switch ( type )
    {
    case SyncPrimitiveType::GPU:
        return new VulkanGPUSyncPrimitive( m_vkDevice );
    case SyncPrimitiveType::CPU:
        return new VulkanCPUSyncPrimitive( m_vkDevice );
    default: break;
    }
    return nullptr;
}

IRenderPass *
VulkanDeviceState::CreateRenderPass( const RenderPassCreateParams &params )
{
    return new VulkanRenderPass( { .mDevice = m_vkDevice, .mDesc = params } );
}

IShader *VulkanDeviceState::CreateShader( const ShaderDesc &params )
{
    return new VulkanShader( { .mDevice = m_vkDevice, .mDesc = params } );
}

ICommandBuffer *VulkanDeviceState::GetMainCommandBuffer()
{
    if ( mMainCmdBuffer != nullptr )
        return mMainCmdBuffer;

    vk::CommandBufferAllocateInfo cmd_buffer_alloc_info{};
    cmd_buffer_alloc_info.level              = vk::CommandBufferLevel::ePrimary;
    cmd_buffer_alloc_info.commandPool        = m_vkCommandPool;
    cmd_buffer_alloc_info.commandBufferCount = 1;
    auto cmd_buffer =
        m_vkDevice.allocateCommandBuffers( cmd_buffer_alloc_info );

    if ( !CALL_VK_API( cmd_buffer.result,
                       TEXT( "Failed to allocate main command buffer!" ) ) )
        return nullptr;
    return ( mMainCmdBuffer = new VulkanCommandBuffer(
                 m_vkDevice, m_vkCommandPool, cmd_buffer.value[0] ) );
}

ICommandBuffer *rh::engine::VulkanDeviceState::CreateCommandBuffer()
{
    vk::CommandBufferAllocateInfo cmd_buffer_alloc_info{};
    cmd_buffer_alloc_info.level              = vk::CommandBufferLevel::ePrimary;
    cmd_buffer_alloc_info.commandPool        = m_vkCommandPool;
    cmd_buffer_alloc_info.commandBufferCount = 1;
    auto cmd_buffer =
        m_vkDevice.allocateCommandBuffers( cmd_buffer_alloc_info );
    if ( !CALL_VK_API( cmd_buffer.result,
                       TEXT( "Failed to allocate command buffer!" ) ) )
        return nullptr;
    return new VulkanCommandBuffer( m_vkDevice, m_vkCommandPool,
                                    cmd_buffer.value[0] );
}

void VulkanDeviceState::ExecuteCommandBuffer( ICommandBuffer *buffer,
                                              ISyncPrimitive *waitFor,
                                              ISyncPrimitive *signal )
{
    auto vk_cmd_buffer_ptr = dynamic_cast<VulkanCommandBuffer *>( buffer );
    auto cmd_buffer        = vk_cmd_buffer_ptr->GetBuffer();
    vk::Semaphore signal_semaphores;
    vk::Semaphore wait_semaphores;

    vk::SubmitInfo queue_submit_info{};
    queue_submit_info.commandBufferCount = 1;
    queue_submit_info.pCommandBuffers    = &cmd_buffer;

    if ( waitFor != nullptr )
    {
        std::array<vk::PipelineStageFlags, 1> stage_flags{
            vk::PipelineStageFlagBits::eTopOfPipe };
        auto vk_wait_for_sp = dynamic_cast<VulkanGPUSyncPrimitive *>( waitFor );
        assert( vk_wait_for_sp );
        wait_semaphores = ( static_cast<vk::Semaphore>( *vk_wait_for_sp ) );
        queue_submit_info.waitSemaphoreCount = 1;
        queue_submit_info.pWaitSemaphores    = &wait_semaphores;
        queue_submit_info.pWaitDstStageMask  = stage_flags.data();
    }

    if ( signal != nullptr )
    {
        auto vk_signal_sp = dynamic_cast<VulkanGPUSyncPrimitive *>( signal );
        assert( vk_signal_sp );
        signal_semaphores = ( static_cast<vk::Semaphore>( *vk_signal_sp ) );
        queue_submit_info.signalSemaphoreCount = 1;
        queue_submit_info.pSignalSemaphores    = &signal_semaphores;
    }
    auto vk_cmd_buf_exec_sp = dynamic_cast<VulkanCPUSyncPrimitive *>(
        vk_cmd_buffer_ptr->ExecutionFinishedPrimitive() );

    auto result = m_vkMainQueue.submit(
        { queue_submit_info },
        vk_cmd_buf_exec_sp ? static_cast<vk::Fence>( *vk_cmd_buf_exec_sp )
                           : nullptr );
    CALL_VK_API( result, TEXT( "Failed to submit cmd buffer!" ) );
}

IFrameBuffer *
VulkanDeviceState::CreateFrameBuffer( const FrameBufferCreateParams &params )
{
    std::vector<vk::ImageView> image_views;
    image_views.reserve( params.imageViews.Size() );

    std::ranges::transform(
        params.imageViews, std::back_inserter( image_views ),
        []( IImageView *img_view_ptr )
        {
            auto impl_img_view =
                dynamic_cast<VulkanImageView *>( img_view_ptr );
            return static_cast<vk::ImageView>( *impl_img_view );
        } );

    auto render_pass_impl =
        dynamic_cast<VulkanRenderPass *>( params.renderPass );
    return new VulkanFrameBuffer( m_vkDevice, image_views, params.width,
                                  params.height, *render_pass_impl );
}

IPipeline *VulkanDeviceState::CreateRasterPipeline(
    const RasterPipelineCreateParams &params )
{
    return new VulkanPipeline( { params, m_vkDevice } );
}

IBuffer *VulkanDeviceState::CreateBuffer( const BufferCreateInfo &params )
{
    return new VulkanBuffer( { params, m_vkDevice, mDefaultAllocator } );
}

IImageBuffer *
VulkanDeviceState::CreateImageBuffer( const ImageBufferCreateParams &params )
{
    auto result = new VulkanImage( { params, m_vkDevice, mDefaultAllocator } );
    if ( params.mPreinitData.Size() <= 0 )
        return result;

    // Compute staging buffer size
    uint32_t staging_buffer_size = std::accumulate(
        params.mPreinitData.begin(), params.mPreinitData.end(), 0u,
        []( uint32_t                   t,
            const ImageBufferInitData &x ) { // Sometimes c++ is just ugly
            return t + x.mSize;
        } );

    ScopedPointer staging_buffer =
        CreateBuffer( { .mSize  = staging_buffer_size,
                        .mUsage = BufferUsage::StagingBuffer } );

    auto buffer_memory = staging_buffer->Lock();

    std::vector<ImageMemoryBarrierInfo>   barriers;
    std::vector<BufferToImageCopySubInfo> copy_regions;
    for ( uint32_t offset = 0, i = 0; i < params.mMipLevels; i++ )
    {
        const auto &pre_init_data = params.mPreinitData[i];
        auto        mip_w         = (std::max)( params.mWidth >> i, 1u );
        auto        mip_h         = (std::max)( params.mHeight >> i, 1u );

        // Ignore zero sized mipmaps, can happen on some textures due to some
        // error in mip-map generation software
        if ( pre_init_data.mSize <= 0 )
        {
            rh::debug::DebugLogger::Error(
                "Encountered 0 sized mip-map, you probably should fix this" );
            continue;
        }

        auto mip_region = ImageSubresourceRegion{ i, 0, 1 };
        copy_regions.push_back(
            { BufferRegion{ offset, /*pre_init_data.mStride*/ 0,
                            /*pre_init_data.mSize / pre_init_data.mStride*/ 0 },
              ImageRegion{ mip_region, 0, 0, 0, mip_w, mip_h, 1 } } );
        std::memcpy( reinterpret_cast<char *>( buffer_memory ) + offset,
                     pre_init_data.mData, pre_init_data.mSize );
        offset += pre_init_data.mSize;
    }
    staging_buffer->Unlock();

    ScopedPointer copy_cmd_buff = CreateCommandBuffer();
    copy_cmd_buff->BeginRecord();

    copy_cmd_buff->PipelineBarrier( {
        .mSrcStage            = PipelineStage::Host,
        .mDstStage            = PipelineStage::Transfer,
        .mImageMemoryBarriers = { {
            .mImage           = result,
            .mSrcLayout       = ImageLayout::Undefined,
            .mDstLayout       = ImageLayout::TransferDst,
            .mSrcMemoryAccess = MemoryAccessFlags::Unknown,
            .mDstMemoryAccess = MemoryAccessFlags::TransferWrite,
            .mSubresRange     = { .baseMipLevel   = 0,
                              .levelCount     = params.mMipLevels,
                              .baseArrayLayer = 0,
                              .layerCount     = 1 },
        } },
    } );

    copy_cmd_buff->CopyBufferToImage(
        { .mBuffer      = staging_buffer,
          .mImage       = result,
          .mImageLayout = ImageLayout::TransferDst,
          .mRegions     = copy_regions } );

    // make image shader read-only
    copy_cmd_buff->PipelineBarrier( {
        .mSrcStage            = PipelineStage::Transfer,
        .mDstStage            = PipelineStage::PixelShader,
        .mImageMemoryBarriers = { {
            .mImage           = result,
            .mSrcLayout       = ImageLayout::TransferDst,
            .mDstLayout       = ImageLayout::ShaderReadOnly,
            .mSrcMemoryAccess = MemoryAccessFlags::TransferWrite,
            .mDstMemoryAccess = MemoryAccessFlags::ShaderRead,
            .mSubresRange     = { .baseMipLevel   = 0,
                              .levelCount     = params.mMipLevels,
                              .baseArrayLayer = 0,
                              .layerCount     = 1 },
        } },
    } );

    copy_cmd_buff->EndRecord();

    ExecuteCommandBuffer( copy_cmd_buff, nullptr, nullptr );
    Wait( { copy_cmd_buff->ExecutionFinishedPrimitive() } );

    return result;
}

void VulkanDeviceState::Wait(
    const ArrayProxy<ISyncPrimitive *> &primitiveList )
{
    std::array<char, sizeof( vk::Fence ) * 8> fence_arena_data{};
    std::pmr::monotonic_buffer_resource       fence_arena{
        std::data( fence_arena_data ), std::size( fence_arena_data ) };
    std::pmr::vector<vk::Fence> fence_list{ &fence_arena };

    std::ranges::transform(
        primitiveList, std::back_inserter( fence_list ),
        []( ISyncPrimitive *fence ) -> vk::Fence
        {
            auto fence_impl = dynamic_cast<VulkanCPUSyncPrimitive *>( fence );
            return *fence_impl;
        } );

    if ( !CALL_VK_API( m_vkDevice.waitForFences( fence_list, true, ~0u ),
                       "Wait for fences failed!" ) )
        return;

    m_vkDevice.resetFences( fence_list );
}

VulkanBottomLevelAccelerationStructure *VulkanDeviceState::CreateBLAS(
    const AccelerationStructureCreateInfo &create_info )
{
    return new VulkanBottomLevelAccelerationStructure(
        { create_info, m_vkDevice, mDefaultAllocator } );
}

ISampler *VulkanDeviceState::CreateSampler( const SamplerDesc &params )
{
    return new VulkanSampler( { params, m_vkDevice } );
}

IImageView *rh::engine::VulkanDeviceState::CreateImageView(
    const ImageViewCreateInfo &params )
{
    return new VulkanImageView( { params, m_vkDevice } );
}

void VulkanDeviceState::UpdateDescriptorSets(
    const DescriptorSetUpdateInfo &params )
{
    vk::WriteDescriptorSet write_desc_set{};
    write_desc_set.dstSet = *dynamic_cast<VulkanDescriptorSet *>( params.mSet );
    write_desc_set.dstBinding      = params.mBinding;
    write_desc_set.descriptorType  = Convert( params.mDescriptorType );
    write_desc_set.descriptorCount = static_cast<uint32_t>(
        (std::max)( { params.mBufferUpdateInfo.Size(),
                      params.mASUpdateInfo.Size(),
                      params.mImageUpdateInfo.Size() } ) );

    std::vector<vk::DescriptorBufferInfo> buffer_list;
    std::ranges::transform(
        params.mBufferUpdateInfo, std::back_inserter( buffer_list ),
        []( const BufferUpdateInfo &info ) -> vk::DescriptorBufferInfo
        {
            vk::DescriptorBufferInfo buffer_info{};
            buffer_info.buffer = *dynamic_cast<VulkanBuffer *>( info.mBuffer );
            buffer_info.offset = info.mOffset;
            buffer_info.range  = info.mRange;
            return buffer_info;
        } );

    std::vector<vk::DescriptorImageInfo> image_list{};
    std::ranges::transform(
        params.mImageUpdateInfo, std::back_inserter( image_list ),
        []( const ImageUpdateInfo &info ) -> vk::DescriptorImageInfo
        {
            vk::DescriptorImageInfo image_info{};
            if ( info.mSampler )
                image_info.sampler =
                    *dynamic_cast<VulkanSampler *>( info.mSampler );
            if ( info.mView )
                image_info.imageView =
                    *dynamic_cast<VulkanImageView *>( info.mView );
            image_info.imageLayout = Convert( info.mLayout );
            return image_info;
        } );

    write_desc_set.dstArrayElement = params.mArrayStartIdx;
    write_desc_set.pBufferInfo     = buffer_list.data();
    write_desc_set.pImageInfo      = image_list.data();
    if ( params.mASUpdateInfo.Size() > 0 )
    {
        vk::WriteDescriptorSetAccelerationStructureNV
            writeDescriptorSetAccelerationStructureNv{};
        writeDescriptorSetAccelerationStructureNv.accelerationStructureCount =
            static_cast<uint32_t>( params.mASUpdateInfo.Size() );
        std::vector<vk::AccelerationStructureNV> as_list;

        std::ranges::transform(
            params.mASUpdateInfo, std::back_inserter( as_list ),
            []( const AccelStructUpdateInfo &info )
            {
                return static_cast<VulkanTopLevelAccelerationStructure *>(
                           info.mTLAS )
                    ->GetImpl();
            } );
        writeDescriptorSetAccelerationStructureNv.pAccelerationStructures =
            as_list.data();

        write_desc_set.pNext = &writeDescriptorSetAccelerationStructureNv;

        m_vkDevice.updateDescriptorSets( { write_desc_set }, {} );
    }
    else
        m_vkDevice.updateDescriptorSets( { write_desc_set }, {} );
}

void VulkanDeviceState::WaitForGPU()
{
    auto result = m_vkDevice.waitIdle();
    CALL_VK_API( result, TEXT( "Failed to wait for gpu to go idle!" ) );
}
VulkanTopLevelAccelerationStructure *
VulkanDeviceState::CreateTLAS( const TLASCreateInfo &create_info )
{
    return new VulkanTopLevelAccelerationStructure(
        { create_info, m_vkDevice, mDefaultAllocator } );
}
VulkanRayTracingPipeline *VulkanDeviceState::CreateRayTracingPipeline(
    const RayTracingPipelineCreateInfo &create_info )
{
    return new VulkanRayTracingPipeline(
        { create_info, m_vkDevice,
          m_aAdaptersInfo[m_uiCurrentAdapter].GetRayTracingInfo() } );
}

VulkanComputePipeline *VulkanDeviceState::CreateComputePipeline(
    const ComputePipelineCreateParams &params )
{
    return new VulkanComputePipeline( { params, m_vkDevice } );
}

void VulkanDeviceState::DispatchToGPU(
    const ArrayProxy<CommandBufferSubmitInfo> &buffers )
{
    // TODO: rewrite or something
    std::vector<std::vector<vk::Semaphore>>          q_sm_waitable_vec{};
    std::vector<std::vector<vk::PipelineStageFlags>> q_sm_stage_flags_vec{};
    std::vector<vk::SubmitInfo>                      queue_submit_info_vec{};

    // std::array<vk::PipelineStageFlags, 1> stage_flags{
    //    vk::PipelineStageFlagBits::eBottomOfPipe };

    std::ranges::transform(
        buffers, std::back_inserter( queue_submit_info_vec ),
        [&q_sm_waitable_vec, &q_sm_stage_flags_vec](
            const CommandBufferSubmitInfo &submitInfo ) -> vk::SubmitInfo
        {
            q_sm_waitable_vec.emplace_back();
            q_sm_stage_flags_vec.emplace_back();

            auto *buffer_impl =
                dynamic_cast<VulkanCommandBuffer *>( submitInfo.mCmdBuffer );
            auto *signal_impl = dynamic_cast<VulkanGPUSyncPrimitive *>(
                submitInfo.mToSignalDep );

            vk::SubmitInfo vk_submit{};
            vk_submit.pCommandBuffers    = &buffer_impl->GetBuffer();
            vk_submit.commandBufferCount = 1;
            if ( !submitInfo.mWaitForDep.empty() )
            {
                std::ranges::transform(
                    submitInfo.mWaitForDep,
                    std::back_inserter( q_sm_waitable_vec.back() ),
                    []( ISyncPrimitive *s ) {
                        return dynamic_cast<VulkanGPUSyncPrimitive *>( s )
                            ->GetImpl();
                    } );
                std::ranges::transform(
                    submitInfo.mWaitForDep,
                    std::back_inserter( q_sm_stage_flags_vec.back() ),
                    []( ISyncPrimitive * /*s*/ )
                    { return vk::PipelineStageFlagBits::eTopOfPipe; } );
            }
            if ( !q_sm_waitable_vec.back().empty() )
            {
                vk_submit.pWaitSemaphores = q_sm_waitable_vec.back().data();
                vk_submit.waitSemaphoreCount =
                    static_cast<uint32_t>( q_sm_waitable_vec.back().size() );
                vk_submit.pWaitDstStageMask =
                    q_sm_stage_flags_vec.back().data();
            }
            if ( signal_impl )
            {
                vk_submit.pSignalSemaphores    = &signal_impl->GetImpl();
                vk_submit.signalSemaphoreCount = 1;
            }
            return vk_submit;
        } );

    auto vk_cmd_buffer_ptr = dynamic_cast<VulkanCommandBuffer *>(
        buffers.Data()[buffers.Size() - 1].mCmdBuffer );
    auto vk_cmd_buf_exec_sp = dynamic_cast<VulkanCPUSyncPrimitive *>(
        vk_cmd_buffer_ptr->ExecutionFinishedPrimitive() );
    auto result = m_vkMainQueue.submit(
        queue_submit_info_vec,
        vk_cmd_buf_exec_sp ? static_cast<vk::Fence>( *vk_cmd_buf_exec_sp )
                           : nullptr );
    CALL_VK_API( result, TEXT( "Failed to submit gpu work!" ) );
}

VulkanImGUI *VulkanDeviceState::CreateImGUI( IWindow *wnd )
{
    return new VulkanImGUI(
        { dynamic_cast<VulkanWin32Window *>( wnd )->GetHandle(), m_vkInstance,
          m_aAdapters[m_uiCurrentAdapter], m_vkDevice,
          m_iGraphicsQueueFamilyIdx, m_vkMainQueue } );
}

const DeviceLimitsInfo &VulkanDeviceState::GetLimits()
{
    return m_aAdaptersInfo[m_uiCurrentAdapter].GetLimitsInfo();
}
