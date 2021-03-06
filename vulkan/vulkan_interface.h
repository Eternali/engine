// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_VULKAN_VULKAN_INTERFACE_H_
#define FLUTTER_VULKAN_VULKAN_INTERFACE_H_

#include <string>

#include "lib/fxl/build_config.h"
#include "lib/fxl/logging.h"

#define VULKAN_LINK_STATICALLY OS_FUCHSIA

#if OS_ANDROID
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif  // VK_USE_PLATFORM_ANDROID_KHR
#endif  // OS_ANDROID

#if OS_FUCHSIA
#ifndef VK_USE_PLATFORM_MAGMA_KHR
#define VK_USE_PLATFORM_MAGMA_KHR 1
#endif  // VK_USE_PLATFORM_MAGMA_KHR
#endif  // OS_FUCHSIA

#if !VULKAN_LINK_STATICALLY
#define VK_NO_PROTOTYPES 1
#endif  // !VULKAN_LINK_STATICALLY

#include <vulkan/vulkan.h>

#ifndef NDEBUG

#define VK_CALL_LOG_ERROR(expression)                      \
  ({                                                       \
    __typeof__(expression) _rc = (expression);             \
    if (_rc != VK_SUCCESS) {                               \
      FXL_DLOG(INFO) << "Vulkan call '" << #expression     \
                     << "' failed with error "             \
                     << vulkan::VulkanResultToString(_rc); \
    }                                                      \
    _rc;                                                   \
  })

#else  // NDEBUG

#define VK_CALL_LOG_ERROR(expression) (expression)

#endif  // NDEBUG

namespace vulkan {

std::string VulkanResultToString(VkResult result);

}  // namespace vulkan

#endif  // FLUTTER_VULKAN_VULKAN_INTERFACE_H_
