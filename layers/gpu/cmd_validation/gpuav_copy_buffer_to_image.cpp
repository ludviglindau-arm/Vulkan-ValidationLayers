/* Copyright (c) 2018-2024 The Khronos Group Inc.
 * Copyright (c) 2018-2024 Valve Corporation
 * Copyright (c) 2018-2024 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gpu/core/gpuav.h"
#include "gpu/cmd_validation/gpuav_cmd_validation_common.h"
#include "gpu/resources/gpuav_subclasses.h"
// Generated shaders
#include "generated/cmd_validation_copy_buffer_to_image_comp.h"

namespace gpuav {

std::unique_ptr<CommandResources> Validator::AllocatePreCopyBufferToImageValidationResources(
    const Location &loc, VkCommandBuffer cmd_buffer, const VkCopyBufferToImageInfo2 *copy_buffer_to_img_info) {
    if (aborted_) {
        return nullptr;
    }

    if (!gpuav_settings.validate_buffer_copies) {
        return nullptr;
    }

    // No need to perform validation if VK_EXT_depth_range_unrestricted is enabled
    if (IsExtEnabled(device_extensions.vk_ext_depth_range_unrestricted)) {
        return nullptr;
    }

    auto image_state = Get<vvl::Image>(copy_buffer_to_img_info->dstImage);
    if (!image_state) {
        InternalError(cmd_buffer, loc, "AllocatePreCopyBufferToImageValidationResources: Unrecognized image");
        return nullptr;
    }

    // Only need to perform validation for depth image having a depth format that is not unsigned normalized.
    // For unsigned normalized formats, depth is by definition in range [0, 1]
    if (!IsValueIn(image_state->create_info.format, {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT})) {
        return nullptr;
    }

    auto cb_state = GetWrite<CommandBuffer>(cmd_buffer);
    if (!cb_state) {
        InternalError(cmd_buffer, loc, "AllocatePreCopyBufferToImageValidationResources: Unrecognized command buffer");
        return nullptr;
    }

    PreCopyBufferToImageResources::SharedResources *shared_resources =
        GetSharedCopyBufferToImageValidationResources(cb_state->GetValidationCmdCommonDescriptorSetLayout(), loc);
    if (!shared_resources) {
        return nullptr;
    }

    CommandResources cmd_resources;
    cmd_resources.command = loc.function;

    auto copy_buffer_to_img_resources = std::make_unique<PreCopyBufferToImageResources>();
    CommandResources &base = *copy_buffer_to_img_resources;
    base = cmd_resources;
    copy_buffer_to_img_resources->src_buffer = copy_buffer_to_img_info->srcBuffer;

    // Allocate buffer that will be used to store pRegions
    uint32_t max_texels_count_in_regions = copy_buffer_to_img_info->pRegions[0].imageExtent.width *
                                           copy_buffer_to_img_info->pRegions[0].imageExtent.height *
                                           copy_buffer_to_img_info->pRegions[0].imageExtent.depth;
    {
        // Needs to be kept in sync with copy_buffer_to_image.comp
        struct BufferImageCopy {
            uint32_t src_buffer_byte_offset;
            uint32_t start_layer;
            uint32_t layer_count;
            uint32_t row_extent;
            uint32_t slice_extent;
            uint32_t layer_extent;
            uint32_t pad_[2];
            int32_t image_offset[4];
            uint32_t image_extent[4];
        };

        VkBufferCreateInfo buffer_info = vku::InitStructHelper();
        const VkDeviceSize uniform_block_constants_byte_size = (4 +  // image extent
                                                                1 +  // block size
                                                                1 +  // gpu copy regions count
                                                                2    // pad
                                                                ) *
                                                               sizeof(uint32_t);
        buffer_info.size = uniform_block_constants_byte_size + sizeof(BufferImageCopy) * copy_buffer_to_img_info->regionCount;
        buffer_info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        uint32_t mem_type_index = 0;
        vmaFindMemoryTypeIndexForBufferInfo(vma_allocator_, &buffer_info, &alloc_info, &mem_type_index);

        alloc_info.pool = shared_resources->copy_regions_pool;
        VkResult result =
            vmaCreateBuffer(vma_allocator_, &buffer_info, &alloc_info, &copy_buffer_to_img_resources->copy_src_regions_buffer,
                            &copy_buffer_to_img_resources->copy_src_regions_allocation, nullptr);
        if (result != VK_SUCCESS) {
            InternalError(cmd_buffer, loc, "Unable to allocate device memory for GPU copy of pRegions. Aborting GPU-AV.", true);
            return nullptr;
        }

        uint32_t *gpu_regions_u32_ptr = nullptr;
        result = vmaMapMemory(vma_allocator_, copy_buffer_to_img_resources->copy_src_regions_allocation,
                              reinterpret_cast<void **>(&gpu_regions_u32_ptr));

        if (result != VK_SUCCESS) {
            InternalError(cmd_buffer, loc, "Unable to map device memory for GPU copy of pRegions. Aborting GPU-AV.", true);
            return nullptr;
        }

        const uint32_t block_size = image_state->create_info.format == VK_FORMAT_D32_SFLOAT ? 4 : 5;
        uint32_t gpu_regions_count = 0;
        BufferImageCopy *gpu_regions_ptr =
            reinterpret_cast<BufferImageCopy *>(&gpu_regions_u32_ptr[uniform_block_constants_byte_size / sizeof(uint32_t)]);
        for (const auto &cpu_region : vvl::make_span(copy_buffer_to_img_info->pRegions, copy_buffer_to_img_info->regionCount)) {
            if (cpu_region.imageSubresource.aspectMask != VK_IMAGE_ASPECT_DEPTH_BIT) {
                continue;
            }

            // Read offset above kU32Max cannot be indexed in the validation shader
            if (const VkDeviceSize max_buffer_read_offset =
                    cpu_region.bufferOffset + static_cast<VkDeviceSize>(block_size) * cpu_region.imageExtent.width *
                                                  cpu_region.imageExtent.height * cpu_region.imageExtent.depth;
                max_buffer_read_offset > static_cast<VkDeviceSize>(vvl::kU32Max)) {
                continue;
            }

            BufferImageCopy &gpu_region = gpu_regions_ptr[gpu_regions_count];
            gpu_region.src_buffer_byte_offset = static_cast<uint32_t>(cpu_region.bufferOffset);
            gpu_region.start_layer = cpu_region.imageSubresource.baseArrayLayer;
            gpu_region.layer_count = cpu_region.imageSubresource.layerCount;
            gpu_region.row_extent = std::max(cpu_region.bufferRowLength, image_state->create_info.extent.width * block_size);
            gpu_region.slice_extent =
                std::max(cpu_region.bufferImageHeight, image_state->create_info.extent.height * gpu_region.row_extent);
            gpu_region.layer_extent = image_state->create_info.extent.depth * gpu_region.slice_extent;
            gpu_region.image_offset[0] = cpu_region.imageOffset.x;
            gpu_region.image_offset[1] = cpu_region.imageOffset.y;
            gpu_region.image_offset[2] = cpu_region.imageOffset.z;
            gpu_region.image_offset[3] = 0;
            gpu_region.image_extent[0] = cpu_region.imageExtent.width;
            gpu_region.image_extent[1] = cpu_region.imageExtent.height;
            gpu_region.image_extent[2] = cpu_region.imageExtent.depth;
            gpu_region.image_extent[3] = 0;

            max_texels_count_in_regions =
                std::max(max_texels_count_in_regions,
                         cpu_region.imageExtent.width * cpu_region.imageExtent.height * cpu_region.imageExtent.depth);

            ++gpu_regions_count;
        }

        if (gpu_regions_count == 0) {
            // Nothing to validate
            vmaUnmapMemory(vma_allocator_, copy_buffer_to_img_resources->copy_src_regions_allocation);
            copy_buffer_to_img_resources->Destroy(*this);
            return nullptr;
        }

        gpu_regions_u32_ptr[0] = image_state->create_info.extent.width;
        gpu_regions_u32_ptr[1] = image_state->create_info.extent.height;
        gpu_regions_u32_ptr[2] = image_state->create_info.extent.depth;
        gpu_regions_u32_ptr[3] = 0;
        gpu_regions_u32_ptr[4] = block_size;
        gpu_regions_u32_ptr[5] = gpu_regions_count;
        gpu_regions_u32_ptr[6] = 0;
        gpu_regions_u32_ptr[7] = 0;

        vmaUnmapMemory(vma_allocator_, copy_buffer_to_img_resources->copy_src_regions_allocation);
    }

    // Update descriptor set
    {
        VkResult result = desc_set_manager_->GetDescriptorSet(&copy_buffer_to_img_resources->desc_pool, shared_resources->ds_layout,
                                                              &copy_buffer_to_img_resources->desc_set);
        if (result != VK_SUCCESS) {
            InternalError(cmd_buffer, loc,
                          "Unable to allocate descriptor set for copy buffer to image validation. Aborting GPU-AV");
            return nullptr;
        }

        std::array<VkDescriptorBufferInfo, 2> descriptor_buffer_infos = {};
        // Copy source buffer
        descriptor_buffer_infos[0].buffer = copy_buffer_to_img_info->srcBuffer;
        descriptor_buffer_infos[0].offset = 0;
        descriptor_buffer_infos[0].range = VK_WHOLE_SIZE;
        // Copy regions buffer
        descriptor_buffer_infos[1].buffer = copy_buffer_to_img_resources->copy_src_regions_buffer;
        descriptor_buffer_infos[1].offset = 0;
        descriptor_buffer_infos[1].range = VK_WHOLE_SIZE;

        std::array<VkWriteDescriptorSet, descriptor_buffer_infos.size()> desc_writes = {};
        for (const auto [i, desc_buffer_info] : vvl::enumerate(descriptor_buffer_infos.data(), descriptor_buffer_infos.size())) {
            desc_writes[i] = vku::InitStructHelper();
            desc_writes[i].dstBinding = static_cast<uint32_t>(i);
            desc_writes[i].descriptorCount = 1;
            desc_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            desc_writes[i].pBufferInfo = desc_buffer_info;
            desc_writes[i].dstSet = copy_buffer_to_img_resources->desc_set;
        }
        DispatchUpdateDescriptorSets(device, static_cast<uint32_t>(desc_writes.size()), desc_writes.data(), 0, nullptr);
    }
    // Save current graphics pipeline state
    RestorablePipelineState restorable_state(*cb_state, VK_PIPELINE_BIND_POINT_COMPUTE);

    // Insert diagnostic dispatch
    DispatchCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, shared_resources->pipeline);

    BindValidationCmdsCommonDescSet(cb_state, VK_PIPELINE_BIND_POINT_COMPUTE, shared_resources->pipeline_layout, 0,
                                    static_cast<uint32_t>(cb_state->per_command_resources.size()));
    DispatchCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, shared_resources->pipeline_layout,
                                  glsl::kDiagPerCmdDescriptorSet, 1, &copy_buffer_to_img_resources->desc_set, 0, nullptr);
    // correct_count == max texelsCount?
    const uint32_t group_count_x = max_texels_count_in_regions / 64 + uint32_t(max_texels_count_in_regions % 64 > 0);
    DispatchCmdDispatch(cmd_buffer, group_count_x, 1, 1);

    // Restore the previous compute pipeline state.
    restorable_state.Restore(cmd_buffer);

    return copy_buffer_to_img_resources;
}

PreCopyBufferToImageResources::SharedResources *Validator::GetSharedCopyBufferToImageValidationResources(
    VkDescriptorSetLayout error_output_set_layout, const Location &loc) {
    if (auto shared_resources = shared_validation_resources_map.find(typeid(PreCopyBufferToImageResources::SharedResources));
        shared_resources != shared_validation_resources_map.end()) {
        return reinterpret_cast<PreCopyBufferToImageResources::SharedResources *>(shared_resources->second.get());
    }

    auto shared_resources = std::make_unique<PreCopyBufferToImageResources::SharedResources>();

    VkResult result = VK_SUCCESS;
    const std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},  // copy source buffer
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},  // copy regions buffer
    };

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = vku::InitStructHelper();
    ds_layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ds_layout_ci.pBindings = bindings.data();
    result = DispatchCreateDescriptorSetLayout(device, &ds_layout_ci, nullptr, &shared_resources->ds_layout);
    if (result != VK_SUCCESS) {
        InternalError(device, loc, "Unable to create descriptor set layout. Aborting GPU-AV");
        return nullptr;
    }

    // Allocate buffer memory pool that will be used to create the buffers holding copy regions
    VkBufferCreateInfo buffer_info = vku::InitStructHelper();
    buffer_info.size = 4096;  // Dummy value
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    uint32_t mem_type_index = 0;
    vmaFindMemoryTypeIndexForBufferInfo(vma_allocator_, &buffer_info, &alloc_info, &mem_type_index);
    VmaPoolCreateInfo pool_create_info = {};
    pool_create_info.memoryTypeIndex = mem_type_index;
    pool_create_info.blockSize = 0;
    pool_create_info.maxBlockCount = 0;
    pool_create_info.flags = VMA_POOL_CREATE_LINEAR_ALGORITHM_BIT;
    result = vmaCreatePool(vma_allocator_, &pool_create_info, &shared_resources->copy_regions_pool);
    if (result != VK_SUCCESS) {
        InternalError(device, loc, "Unable to create VMA memory pool for buffer to image copies validation. Aborting GPU-AV");
        return nullptr;
    }

    std::array<VkDescriptorSetLayout, 2> set_layouts = {{error_output_set_layout, shared_resources->ds_layout}};
    VkPipelineLayoutCreateInfo pipeline_layout_ci = vku::InitStructHelper();
    pipeline_layout_ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    pipeline_layout_ci.pSetLayouts = set_layouts.data();
    result = DispatchCreatePipelineLayout(device, &pipeline_layout_ci, nullptr, &shared_resources->pipeline_layout);
    if (result != VK_SUCCESS) {
        InternalError(device, loc, "Unable to create pipeline layout. Aborting GPU-AV");
        return nullptr;
    }

    VkShaderModuleCreateInfo shader_module_ci = vku::InitStructHelper();
    shader_module_ci.codeSize = cmd_validation_copy_buffer_to_image_comp_size * sizeof(uint32_t);
    shader_module_ci.pCode = cmd_validation_copy_buffer_to_image_comp;
    VkShaderModule validation_shader = VK_NULL_HANDLE;
    result = DispatchCreateShaderModule(device, &shader_module_ci, nullptr, &validation_shader);
    if (result != VK_SUCCESS) {
        InternalError(device, loc, "Unable to create shader module. Aborting GPU-AV");
        return nullptr;
    }

    // Create pipeline
    VkPipelineShaderStageCreateInfo pipeline_stage_ci = vku::InitStructHelper();
    pipeline_stage_ci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_stage_ci.module = validation_shader;
    pipeline_stage_ci.pName = "main";

    VkComputePipelineCreateInfo pipeline_ci = vku::InitStructHelper();
    pipeline_ci.stage = pipeline_stage_ci;
    pipeline_ci.layout = shared_resources->pipeline_layout;

    result = DispatchCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &shared_resources->pipeline);
    if (result != VK_SUCCESS) {
        InternalError(device, loc, "Failed to create compute pipeline for copy buffer to image validation. Aborting GPU-AV.");
    }

    DispatchDestroyShaderModule(device, validation_shader, nullptr);

    const auto elt = shared_validation_resources_map.insert(
        {typeid(PreCopyBufferToImageResources::SharedResources), std::move(shared_resources)});

    assert(elt.second);

    return reinterpret_cast<PreCopyBufferToImageResources::SharedResources *>(elt.first->second.get());
}

void PreCopyBufferToImageResources::SharedResources::Destroy(Validator &validator) {
    if (ds_layout != VK_NULL_HANDLE) {
        DispatchDestroyDescriptorSetLayout(validator.device, ds_layout, nullptr);
        ds_layout = VK_NULL_HANDLE;
    }
    if (pipeline_layout != VK_NULL_HANDLE) {
        DispatchDestroyPipelineLayout(validator.device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
    }
    if (pipeline != VK_NULL_HANDLE) {
        DispatchDestroyPipeline(validator.device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (copy_regions_pool != VK_NULL_HANDLE) {
        vmaDestroyPool(validator.vma_allocator_, copy_regions_pool);
        copy_regions_pool = VK_NULL_HANDLE;
    }
}

void PreCopyBufferToImageResources::Destroy(Validator &validator) {
    if (desc_set != VK_NULL_HANDLE) {
        validator.desc_set_manager_->PutBackDescriptorSet(desc_pool, desc_set);
        desc_set = VK_NULL_HANDLE;
        desc_pool = VK_NULL_HANDLE;
    }

    if (copy_src_regions_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(validator.vma_allocator_, copy_src_regions_buffer, copy_src_regions_allocation);
        copy_src_regions_buffer = VK_NULL_HANDLE;
        copy_src_regions_allocation = VK_NULL_HANDLE;
    }

    CommandResources::Destroy(validator);
}

}  // namespace gpuav
