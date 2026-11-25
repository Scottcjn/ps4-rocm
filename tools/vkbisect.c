/* vkbisect: find the FIRST GPU operation that hangs on PS4 Linux (RADV).
 *
 *   usage: vkbisect <test> <mem> [queue]
 *     test : fill | copy | shader       (shader = trivial compute writing gl_GlobalInvocationID)
 *     mem  : local | host              (DEVICE_LOCAL heap vs HOST_VISIBLE non-local heap)
 *     queue: any | compute | graphics  (queue family to submit on; default any)
 *
 * Exit: 0 = ran + verified, 1 = wrong data, 2 = hang/device lost (fence timeout 5 s), 3 = setup error.
 * One test per process on purpose: a lost device poisons the whole Vulkan instance.
 * Shader comes from vkbisect.spv next to the binary (glslc vkbisect.comp -o vkbisect.spv).
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 4096u
#define CK(x) do{ VkResult r_=(x); if(r_){ fprintf(stderr,"%s -> %d\n",#x,r_); exit(r_==VK_ERROR_DEVICE_LOST?2:3);} }while(0)

static VkDevice dev; static VkPhysicalDevice pd;

static uint32_t pick_mem(uint32_t bits, int want_local){
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd,&mp);
  for(uint32_t i=0;i<mp.memoryTypeCount;i++){
    VkMemoryPropertyFlags f=mp.memoryTypes[i].propertyFlags;
    int local=(f&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)!=0, vis=(f&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)!=0;
    if((bits&(1u<<i)) && vis && local==want_local) return i;   /* host-visible so we can verify */
  }
  fprintf(stderr,"no host-visible %s memory type\n",want_local?"local":"non-local"); exit(3);
}

static VkBuffer mkbuf(VkDeviceMemory *m, void **p, int local){
  VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,0,0,N*4,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  VkBuffer b; CK(vkCreateBuffer(dev,&bi,0,&b));
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,b,&mr);
  VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,mr.size,pick_mem(mr.memoryTypeBits,local)};
  CK(vkAllocateMemory(dev,&ai,0,m)); CK(vkBindBufferMemory(dev,b,*m,0)); CK(vkMapMemory(dev,*m,0,N*4,0,p));
  return b;
}

static uint32_t *load_spv(const char *path, size_t *sz){
  FILE *f=fopen(path,"rb"); if(!f){perror(path); exit(3);}
  fseek(f,0,SEEK_END); *sz=ftell(f); fseek(f,0,SEEK_SET);
  uint32_t *d=malloc(*sz); if(fread(d,1,*sz,f)!=*sz) exit(3); fclose(f); return d;
}

int main(int argc,char**argv){
  if(argc<3){ fprintf(stderr,"usage: %s fill|copy|shader local|host [any|compute|graphics]\n",argv[0]); return 3; }
  const char *test=argv[1]; int local=!strcmp(argv[2],"local"); const char *qsel=argc>3?argv[3]:"any";

  VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO,0,"vkbisect",1,0,0,VK_API_VERSION_1_1};
  VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,0,0,&app};
  VkInstance inst; CK(vkCreateInstance(&ici,0,&inst));
  uint32_t n=1; CK(vkEnumeratePhysicalDevices(inst,&n,&pd));

  uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,0);
  VkQueueFamilyProperties qp[8]; if(qn>8)qn=8; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qp);
  int qf=-1;
  for(uint32_t i=0;i<qn;i++){
    int g=(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)!=0, c=(qp[i].queueFlags&VK_QUEUE_COMPUTE_BIT)!=0;
    if(!strcmp(qsel,"graphics")&&g){qf=i;break;}
    if(!strcmp(qsel,"compute")&&c&&!g){qf=i;break;}
    if(!strcmp(qsel,"any")&&c){qf=i;break;}
  }
  if(qf<0){ fprintf(stderr,"no '%s' queue family\n",qsel); return 3; }
  printf("queue family %d (flags 0x%x), mem %s, test %s\n",qf,qp[qf].queueFlags,local?"local":"host",test);

  float pri=1; VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,0,0,(uint32_t)qf,1,&pri};
  VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,0,0,1,&qci};
  CK(vkCreateDevice(pd,&dci,0,&dev)); VkQueue q; vkGetDeviceQueue(dev,qf,0,&q);

  VkDeviceMemory ma,mb; uint32_t *pa,*pb;
  VkBuffer a=mkbuf(&ma,(void**)&pa,local), b=mkbuf(&mb,(void**)&pb,local);
  for(uint32_t i=0;i<N;i++){ pa[i]=i*7u+1u; pb[i]=0xDEADBEEF; }

  VkCommandPoolCreateInfo pci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,0,0,(uint32_t)qf};
  VkCommandPool pool; CK(vkCreateCommandPool(dev,&pci,0,&pool));
  VkCommandBufferAllocateInfo cai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
  VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cai,&cb));
  VkCommandBufferBeginInfo cbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,0,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  CK(vkBeginCommandBuffer(cb,&cbi));

  uint32_t expect_mode=0;   /* 0 = 0x12345678 fill, 1 = copy of a, 2 = gid */
  if(!strcmp(test,"fill")){ vkCmdFillBuffer(cb,b,0,N*4,0x12345678); expect_mode=0; }
  else if(!strcmp(test,"copy")){ VkBufferCopy r={0,0,N*4}; vkCmdCopyBuffer(cb,a,b,1,&r); expect_mode=1; }
  else if(!strcmp(test,"shader")){
    size_t sz; uint32_t *code=load_spv(argc>4?argv[4]:"vkbisect.spv",&sz);
    VkShaderModuleCreateInfo smi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,0,0,sz,code};
    VkShaderModule sm; CK(vkCreateShaderModule(dev,&smi,0,&sm));
    VkDescriptorSetLayoutBinding bd={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0};
    VkDescriptorSetLayoutCreateInfo dli={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,0,0,1,&bd};
    VkDescriptorSetLayout dl; CK(vkCreateDescriptorSetLayout(dev,&dli,0,&dl));
    VkPipelineLayoutCreateInfo pli={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,0,0,1,&dl};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev,&pli,0,&pl));
    VkComputePipelineCreateInfo cpi={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,0,0,
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_COMPUTE_BIT,sm,"main",0},pl};
    VkPipeline pipe; CK(vkCreateComputePipelines(dev,0,1,&cpi,0,&pipe));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo dpi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,0,0,1,1,&ps};
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev,&dpi,0,&dp));
    VkDescriptorSetAllocateInfo dsa={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,0,dp,1,&dl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev,&dsa,&ds));
    VkDescriptorBufferInfo dbi={b,0,N*4};
    VkWriteDescriptorSet w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,0,ds,0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,0,&dbi};
    vkUpdateDescriptorSets(dev,1,&w,0,0);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&ds,0,0);
    vkCmdDispatch(cb,N/64,1,1); expect_mode=2;
  } else { fprintf(stderr,"unknown test %s\n",test); return 3; }

  VkMemoryBarrier mbar={VK_STRUCTURE_TYPE_MEMORY_BARRIER,0,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mbar,0,0,0,0);
  CK(vkEndCommandBuffer(cb));

  VkFenceCreateInfo fi={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CK(vkCreateFence(dev,&fi,0,&fence));
  VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cb};
  CK(vkQueueSubmit(q,1,&si,fence));
  VkResult wr=vkWaitForFences(dev,1,&fence,VK_TRUE,5000000000ull);
  if(wr!=VK_SUCCESS){ printf("RESULT HANG (wait=%d)\n",wr); return 2; }

  uint32_t bad=0;
  for(uint32_t i=0;i<N;i++){
    uint32_t e = expect_mode==0 ? 0x12345678u : expect_mode==1 ? i*7u+1u : i;
    if(pb[i]!=e){ if(bad<4) printf("  [%u] got 0x%08x want 0x%08x\n",i,pb[i],e); bad++; }
  }
  printf("RESULT %s (%u/%u wrong)\n",bad?"WRONG":"OK",bad,N);
  return bad?1:0;
}
