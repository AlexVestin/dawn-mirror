// Copyright 2019 The Dawn Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dawn_native/vulkan/BackendVk.h"

#include "common/BitSetIterator.h"
#include "common/Log.h"
#include "common/SystemUtils.h"
#include "dawn_native/Instance.h"
#include "dawn_native/VulkanBackend.h"
#include "dawn_native/vulkan/AdapterVk.h"
#include "dawn_native/vulkan/UtilsVulkan.h"
#include "dawn_native/vulkan/VulkanError.h"

// TODO(crbug.com/dawn/283): Link against the Vulkan Loader and remove this.
#if defined(DAWN_ENABLE_SWIFTSHADER)
#    if defined(DAWN_PLATFORM_LINUX) || defined(DAWN_PLATFORM_FUSCHIA)
constexpr char kSwiftshaderLibName[] = "libvk_swiftshader.so";
#    elif defined(DAWN_PLATFORM_WINDOWS)
constexpr char kSwiftshaderLibName[] = "vk_swiftshader.dll";
#    elif defined(DAWN_PLATFORM_MACOS)
constexpr char kSwiftshaderLibName[] = "libvk_swiftshader.dylib";
#    else
#        error "Unimplemented Swiftshader Vulkan backend platform"
#    endif
#endif

#if defined(DAWN_PLATFORM_LINUX)
#    if defined(DAWN_PLATFORM_ANDROID)
constexpr char kVulkanLibName[] = "libvulkan.so";
#    else
constexpr char kVulkanLibName[] = "libvulkan.so.1";
#    endif
#elif defined(DAWN_PLATFORM_WINDOWS)
constexpr char kVulkanLibName[] = "vulkan-1.dll";
#elif defined(DAWN_PLATFORM_MACOS)
constexpr char kVulkanLibName[] = "libvulkan.dylib";
#elif defined(DAWN_PLATFORM_FUCHSIA)
constexpr char kVulkanLibName[] = "libvulkan.so";
#else
#    error "Unimplemented Vulkan backend platform"
#endif

namespace dawn_native { namespace vulkan {

    namespace {

        VKAPI_ATTR VkBool32 VKAPI_CALL
        OnDebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                             VkDebugUtilsMessageTypeFlagsEXT /* messageTypes */,
                             const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                             void* /* pUserData */) {
            dawn::WarningLog() << pCallbackData->pMessage;
            ASSERT((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) == 0);

            return VK_FALSE;
        }

        // A debug callback specifically for instance creation so that we don't fire an ASSERT when
        // the instance fails creation in an expected manner (for example the system not having
        // Vulkan drivers).
        VKAPI_ATTR VkBool32 VKAPI_CALL OnInstanceCreationDebugUtilsCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
            VkDebugUtilsMessageTypeFlagsEXT /* messageTypes */,
            const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
            void* /* pUserData */) {
            dawn::WarningLog() << pCallbackData->pMessage;
            return VK_FALSE;
        }

    }  // anonymous namespace

    Backend::Backend(InstanceBase* instance)
        : BackendConnection(instance, wgpu::BackendType::Vulkan) {
    }

    Backend::~Backend() {
        if (mDebugUtilsMessenger != VK_NULL_HANDLE) {
            mFunctions.DestroyDebugUtilsMessengerEXT(mInstance, mDebugUtilsMessenger, nullptr);
            mDebugUtilsMessenger = VK_NULL_HANDLE;
        }

        // VkPhysicalDevices are destroyed when the VkInstance is destroyed
        if (mInstance != VK_NULL_HANDLE) {
            mFunctions.DestroyInstance(mInstance, nullptr);
            mInstance = VK_NULL_HANDLE;
        }
    }

    const VulkanFunctions& Backend::GetFunctions() const {
        return mFunctions;
    }

    VkInstance Backend::GetVkInstance() const {
        return mInstance;
    }

    const VulkanGlobalInfo& Backend::GetGlobalInfo() const {
        return mGlobalInfo;
    }

    MaybeError Backend::LoadVulkan(bool useSwiftshader) {
        // First try to load the system Vulkan driver, if that fails,
        // try to load with Swiftshader. Note: The system driver could potentially be Swiftshader
        // if it was installed.
        if (mVulkanLib.Open(kVulkanLibName)) {
            return {};
        }
        dawn::WarningLog() << std::string("Couldn't open ") + kVulkanLibName;

        // If |useSwiftshader == true|, fallback and try to directly load the Swiftshader
        // library.
        if (useSwiftshader) {
#if defined(DAWN_ENABLE_SWIFTSHADER)
            if (mVulkanLib.Open(kSwiftshaderLibName)) {
                return {};
            }
            dawn::WarningLog() << std::string("Couldn't open ") + kSwiftshaderLibName;
#else
            UNREACHABLE();
#endif  // defined(DAWN_ENABLE_SWIFTSHADER)
        }

        return DAWN_INTERNAL_ERROR("Couldn't load Vulkan");
    }

    MaybeError Backend::Initialize(bool useSwiftshader) {
        DAWN_TRY(LoadVulkan(useSwiftshader));

        // These environment variables need only be set while loading procs and gathering device
        // info.
        ScopedEnvironmentVar vkICDFilenames;
        ScopedEnvironmentVar vkLayerPath;

        if (useSwiftshader) {
#if defined(DAWN_SWIFTSHADER_VK_ICD_JSON)
            std::string fullSwiftshaderICDPath =
                GetExecutableDirectory() + DAWN_SWIFTSHADER_VK_ICD_JSON;
            if (!vkICDFilenames.Set("VK_ICD_FILENAMES", fullSwiftshaderICDPath.c_str())) {
                return DAWN_INTERNAL_ERROR("Couldn't set VK_ICD_FILENAMES");
            }
#else
            dawn::WarningLog() << "Swiftshader enabled but Dawn was not built with "
                                  "DAWN_SWIFTSHADER_VK_ICD_JSON.";
#endif
        }

        if (GetInstance()->IsBackendValidationEnabled()) {
#if defined(DAWN_ENABLE_VULKAN_VALIDATION_LAYERS)
            std::string vkDataDir = GetExecutableDirectory() + DAWN_VK_DATA_DIR;
            if (!vkLayerPath.Set("VK_LAYER_PATH", vkDataDir.c_str())) {
                return DAWN_INTERNAL_ERROR("Couldn't set VK_LAYER_PATH");
            }
#else
            dawn::WarningLog() << "Backend validation enabled but Dawn was not built with "
                                  "DAWN_ENABLE_VULKAN_VALIDATION_LAYERS.";
#endif
        }

        DAWN_TRY(mFunctions.LoadGlobalProcs(mVulkanLib));

        DAWN_TRY_ASSIGN(mGlobalInfo, GatherGlobalInfo(*this));

        VulkanGlobalKnobs usedGlobalKnobs = {};
        DAWN_TRY_ASSIGN(usedGlobalKnobs, CreateInstance());
        *static_cast<VulkanGlobalKnobs*>(&mGlobalInfo) = usedGlobalKnobs;

        DAWN_TRY(mFunctions.LoadInstanceProcs(mInstance, mGlobalInfo));

        if (usedGlobalKnobs.HasExt(InstanceExt::DebugUtils)) {
            DAWN_TRY(RegisterDebugUtils());
        }

        DAWN_TRY_ASSIGN(mPhysicalDevices, GetPhysicalDevices(*this));

        return {};
    }

    std::vector<std::unique_ptr<AdapterBase>> Backend::DiscoverDefaultAdapters() {
        std::vector<std::unique_ptr<AdapterBase>> adapters;

        for (VkPhysicalDevice physicalDevice : mPhysicalDevices) {
            std::unique_ptr<Adapter> adapter = std::make_unique<Adapter>(this, physicalDevice);

            if (GetInstance()->ConsumedError(adapter->Initialize())) {
                continue;
            }

            adapters.push_back(std::move(adapter));
        }

        return adapters;
    }

    ResultOrError<VulkanGlobalKnobs> Backend::CreateInstance() {
        VulkanGlobalKnobs usedKnobs = {};
        std::vector<const char*> layerNames;
        InstanceExtSet extensionsToRequest = mGlobalInfo.extensions;

        auto UseLayerIfAvailable = [&](VulkanLayer layer) {
            if (mGlobalInfo.layers[layer]) {
                layerNames.push_back(GetVulkanLayerInfo(layer).name);
                usedKnobs.layers.set(layer, true);
                extensionsToRequest |= mGlobalInfo.layerExtensions[layer];
            }
        };

        // vktrace works by instering a layer, but we hide it behind a macro because the vktrace
        // layer crashes when used without vktrace server started. See this vktrace issue:
        // https://github.com/LunarG/VulkanTools/issues/254
        // Also it is good to put it in first position so that it doesn't see Vulkan calls inserted
        // by other layers.
#if defined(DAWN_USE_VKTRACE)
        UseLayerIfAvailable(VulkanLayer::LunargVkTrace);
#endif
        // RenderDoc installs a layer at the system level for its capture but we don't want to use
        // it unless we are debugging in RenderDoc so we hide it behind a macro.
#if defined(DAWN_USE_RENDERDOC)
        UseLayerIfAvailable(VulkanLayer::RenderDocCapture);
#endif

        if (GetInstance()->IsBackendValidationEnabled()) {
            UseLayerIfAvailable(VulkanLayer::Validation);
        }

        // Always use the Fuchsia swapchain layer if available.
        UseLayerIfAvailable(VulkanLayer::FuchsiaImagePipeSwapchain);

        // Available and known instance extensions default to being requested, but some special
        // cases are removed.
        usedKnobs.extensions = extensionsToRequest;

        std::vector<const char*> extensionNames;
        for (InstanceExt ext : IterateBitSet(extensionsToRequest)) {
            const InstanceExtInfo& info = GetInstanceExtInfo(ext);

            if (info.versionPromoted > mGlobalInfo.apiVersion) {
                extensionNames.push_back(info.name);
            }
        }

        VkApplicationInfo appInfo;
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pNext = nullptr;
        appInfo.pApplicationName = nullptr;
        appInfo.applicationVersion = 0;
        appInfo.pEngineName = nullptr;
        appInfo.engineVersion = 0;
        // Vulkan 1.0 implementations were required to return VK_ERROR_INCOMPATIBLE_DRIVER if
        // apiVersion was larger than 1.0. Meanwhile, as long as the instance supports at least
        // Vulkan 1.1, an application can use different versions of Vulkan with an instance than
        // it does with a device or physical device. So we should set apiVersion to Vulkan 1.0
        // if the instance only supports Vulkan 1.0. Otherwise we set apiVersion to Vulkan 1.2,
        // treat 1.2 as the highest API version dawn targets.
        if (mGlobalInfo.apiVersion == VK_MAKE_VERSION(1, 0, 0)) {
            appInfo.apiVersion = mGlobalInfo.apiVersion;
        } else {
            appInfo.apiVersion = VK_MAKE_VERSION(1, 2, 0);
        }

        VkInstanceCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pNext = nullptr;
        createInfo.flags = 0;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledLayerCount = static_cast<uint32_t>(layerNames.size());
        createInfo.ppEnabledLayerNames = layerNames.data();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensionNames.size());
        createInfo.ppEnabledExtensionNames = extensionNames.data();

        PNextChainBuilder createInfoChain(&createInfo);

        // Register the debug callback for instance creation so we receive message for any errors
        // (validation or other).
        VkDebugUtilsMessengerCreateInfoEXT utilsMessengerCreateInfo;
        if (usedKnobs.HasExt(InstanceExt::DebugUtils)) {
            utilsMessengerCreateInfo.flags = 0;
            utilsMessengerCreateInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            utilsMessengerCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            utilsMessengerCreateInfo.pfnUserCallback = OnInstanceCreationDebugUtilsCallback;
            utilsMessengerCreateInfo.pUserData = nullptr;

            createInfoChain.Add(&utilsMessengerCreateInfo,
                                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
        }

        // Try to turn on synchronization validation if the instance was created with backend
        // validation enabled.
        VkValidationFeaturesEXT validationFeatures;
        VkValidationFeatureEnableEXT kEnableSynchronizationValidation =
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        if (GetInstance()->IsBackendValidationEnabled() &&
            usedKnobs.HasExt(InstanceExt::ValidationFeatures)) {
            validationFeatures.enabledValidationFeatureCount = 1;
            validationFeatures.pEnabledValidationFeatures = &kEnableSynchronizationValidation;
            validationFeatures.disabledValidationFeatureCount = 0;
            validationFeatures.pDisabledValidationFeatures = nullptr;

            createInfoChain.Add(&validationFeatures, VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT);
        }

        DAWN_TRY(CheckVkSuccess(mFunctions.CreateInstance(&createInfo, nullptr, &mInstance),
                                "vkCreateInstance"));

        return usedKnobs;
    }

    MaybeError Backend::RegisterDebugUtils() {
        VkDebugUtilsMessengerCreateInfoEXT createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.pNext = nullptr;
        createInfo.flags = 0;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        createInfo.pfnUserCallback = OnDebugUtilsCallback;
        createInfo.pUserData = nullptr;

        return CheckVkSuccess(mFunctions.CreateDebugUtilsMessengerEXT(
                                  mInstance, &createInfo, nullptr, &*mDebugUtilsMessenger),
                              "vkCreateDebugUtilsMessengerEXT");
    }

    BackendConnection* Connect(InstanceBase* instance, bool useSwiftshader) {
        Backend* backend = new Backend(instance);

        if (instance->ConsumedError(backend->Initialize(useSwiftshader))) {
            delete backend;
            return nullptr;
        }

        return backend;
    }

}}  // namespace dawn_native::vulkan
