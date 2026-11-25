/* vkprobe: headless Vulkan compute probe (no surface/display extensions).
 * vulkaninfo dies on PS4 Linux because VK_KHR_display fails with amdgpu.dc=0. */
#include <vulkan/vulkan.h>
#include <stdio.h>
int main(void){
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO,0,"vkprobe",1,0,0,VK_API_VERSION_1_3};
  VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,0,0,&ai};
  VkInstance inst; VkResult r=vkCreateInstance(&ci,0,&inst);
  if(r){printf("vkCreateInstance failed: %d\n",r);return 1;}
  uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,0);
  printf("physical devices: %u\n",n); if(!n) return 2;
  VkPhysicalDevice d[8]; if(n>8)n=8; vkEnumeratePhysicalDevices(inst,&n,d);
  for(uint32_t i=0;i<n;i++){
    VkPhysicalDeviceVulkan12Features f12={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,&f12};
    VkPhysicalDeviceFeatures2 f={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&f11};
    vkGetPhysicalDeviceFeatures2(d[i],&f);
    VkPhysicalDeviceShaderCorePropertiesAMD core={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_AMD};
    VkPhysicalDeviceSubgroupProperties sg={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,&core};
    VkPhysicalDeviceDriverProperties dp={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,&sg};
    VkPhysicalDeviceProperties2 p={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&dp};
    vkGetPhysicalDeviceProperties2(d[i],&p);
    VkPhysicalDeviceProperties *P=&p.properties; uint32_t v=P->apiVersion;
    printf("[%u] %s  api %u.%u.%u  type %d  vendor 0x%x device 0x%x\n",i,P->deviceName,
      VK_API_VERSION_MAJOR(v),VK_API_VERSION_MINOR(v),VK_API_VERSION_PATCH(v),P->deviceType,P->vendorID,P->deviceID);
    printf("    driver: %s %s\n",dp.driverName,dp.driverInfo);
    printf("    subgroupSize %u  maxComputeSharedMem %u  maxWorkGroupInvocations %u\n",
      sg.subgroupSize,P->limits.maxComputeSharedMemorySize,P->limits.maxComputeWorkGroupInvocations);
    printf("    shaderFloat16 %u shaderInt8 %u storageBuffer16BitAccess %u storageBuffer8BitAccess %u shaderInt16 %u shaderInt64 %u\n",
      f12.shaderFloat16,f12.shaderInt8,f11.storageBuffer16BitAccess,f12.storageBuffer8BitAccess,f.features.shaderInt16,f.features.shaderInt64);
    if(core.shaderEngineCount)
      printf("    AMD cores: %u shader engines x %u CU/SE = %u CUs, %u SIMD/CU, wave %u\n",
        core.shaderEngineCount,core.computeUnitsPerShaderArray*core.shaderArraysPerEngineCount,
        core.shaderEngineCount*core.shaderArraysPerEngineCount*core.computeUnitsPerShaderArray,
        core.simdPerComputeUnit,core.wavefrontSize);
    VkPhysicalDeviceMemoryProperties m; vkGetPhysicalDeviceMemoryProperties(d[i],&m);
    for(uint32_t h=0;h<m.memoryHeapCount;h++)
      printf("    heap %u: %llu MiB flags 0x%x\n",h,(unsigned long long)(m.memoryHeaps[h].size>>20),m.memoryHeaps[h].flags);
  }
  vkDestroyInstance(inst,0); return 0;
}
