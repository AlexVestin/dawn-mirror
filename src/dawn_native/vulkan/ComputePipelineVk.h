// Copyright 2018 The Dawn Authors
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

#ifndef DAWNNATIVE_VULKAN_COMPUTEPIPELINEVK_H_
#define DAWNNATIVE_VULKAN_COMPUTEPIPELINEVK_H_

#include "dawn_native/ComputePipeline.h"

#include "common/vulkan_platform.h"
#include "dawn_native/Error.h"

namespace dawn_native { namespace vulkan {

    class Device;

    class ComputePipeline final : public ComputePipelineBase {
      public:
        static Ref<ComputePipeline> CreateUninitialized(
            Device* device,
            const ComputePipelineDescriptor* descriptor);
        static void InitializeAsync(Ref<ComputePipelineBase> computePipeline,
                                    WGPUCreateComputePipelineAsyncCallback callback,
                                    void* userdata);

        VkPipeline GetHandle() const;

        MaybeError Initialize() override;

        // Dawn API
        void SetLabelImpl() override;

      private:
        ~ComputePipeline() override;
        void DestroyImpl() override;
        using ComputePipelineBase::ComputePipelineBase;

        VkPipeline mHandle = VK_NULL_HANDLE;
    };

}}  // namespace dawn_native::vulkan

#endif  // DAWNNATIVE_VULKAN_COMPUTEPIPELINEVK_H_
