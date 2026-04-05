#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#define VK_CHECK(call) do { VkResult _r = (call); if (_r != VK_SUCCESS) { fprintf(stderr, "VK err %d at %s:%d\n", _r, __FILE__, __LINE__); exit(1); } } while(0)
static uint32_t *read_spirv(const char *path, size_t *sz) {
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long s=ftell(f); fseek(f,0,SEEK_SET);
    uint32_t *b=malloc(s); fread(b,1,s,f); fclose(f); *sz=(size_t)s; return b;
}
int main(int argc, char **argv) {
    const char *spv = argc>1 ? argv[1] : "/tmp/static_write.spv";
    VkInstance inst; VkDevice dev; VkPhysicalDevice phys; VkQueue queue;
    { VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      VK_CHECK(vkCreateInstance(&ci,NULL,&inst)); }
    uint32_t c=1; vkEnumeratePhysicalDevices(inst,&c,&phys);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys,&props);
    printf("Device: %s\n", props.deviceName);
    { float p=1.0f;
      VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,NULL,0,0,1,&p};
      VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,NULL,0,1,&qci};
      VK_CHECK(vkCreateDevice(phys,&dci,NULL,&dev)); }
    vkGetDeviceQueue(dev,0,0,&queue);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys,&mp);
    /* SSBO buffer */
    VkBuffer buf; VkDeviceMemory mem;
    { VkBufferCreateInfo bci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bci.size=16; bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      VK_CHECK(vkCreateBuffer(dev,&bci,NULL,&buf));
      VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,buf,&mr);
      uint32_t mt=UINT32_MAX;
      for(uint32_t i=0;i<mp.memoryTypeCount;i++)
          if((mr.memoryTypeBits&(1<<i))&&(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){mt=i;break;}
      VkMemoryAllocateInfo mai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr.size,mt};
      VK_CHECK(vkAllocateMemory(dev,&mai,NULL,&mem));
      VK_CHECK(vkBindBufferMemory(dev,buf,mem,0));
      uint32_t *p; VK_CHECK(vkMapMemory(dev,mem,0,16,0,(void**)&p));
      memset(p,0,16); vkUnmapMemory(dev,mem); }
    /* Compute pipeline */
    size_t spv_size; uint32_t *spv_code=read_spirv(spv,&spv_size);
    if(!spv_code){printf("Cannot read %s\n",spv);return 1;}
    VkShaderModule sm;
    { VkShaderModuleCreateInfo ci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,spv_size,spv_code};
      VK_CHECK(vkCreateShaderModule(dev,&ci,NULL,&sm)); } free(spv_code);
    VkDescriptorSetLayout dsl;
    { VkDescriptorSetLayoutBinding b={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
      VkDescriptorSetLayoutCreateInfo ci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,1,&b};
      VK_CHECK(vkCreateDescriptorSetLayout(dev,&ci,NULL,&dsl)); }
    VkPipelineLayout pl;
    { VkPipelineLayoutCreateInfo ci={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,1,&dsl};
      VK_CHECK(vkCreatePipelineLayout(dev,&ci,NULL,&pl)); }
    VkPipeline pipeline;
    { VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=sm; ci.stage.pName="main";
      ci.layout=pl;
      VkResult r=vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&ci,NULL,&pipeline);
      if(r!=VK_SUCCESS){printf("PIPELINE FAILED: %d\n",r);return 1;}
      printf("COMPUTE PIPELINE CREATED\n"); }
    /* Descriptor set */
    VkDescriptorPool dp;
    { VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
      VkDescriptorPoolCreateInfo ci={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,NULL,0,1,1,&ps};
      VK_CHECK(vkCreateDescriptorPool(dev,&ci,NULL,&dp)); }
    VkDescriptorSet ds;
    { VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,NULL,dp,1,&dsl};
      VK_CHECK(vkAllocateDescriptorSets(dev,&ai,&ds)); }
    { VkDescriptorBufferInfo bi={buf,0,16};
      VkWriteDescriptorSet w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,ds,0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,NULL,&bi,NULL};
      vkUpdateDescriptorSets(dev,1,&w,0,NULL); }
    /* Command buffer: ONLY compute dispatch, NO draws */
    VkCommandPool cp;
    { VkCommandPoolCreateInfo ci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,NULL,0,0};
      VK_CHECK(vkCreateCommandPool(dev,&ci,NULL,&cp)); }
    VkCommandBuffer cmd;
    { VkCommandBufferAllocateInfo ai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,NULL,cp,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
      VK_CHECK(vkAllocateCommandBuffers(dev,&ai,&cmd)); }
    { VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      VK_CHECK(vkBeginCommandBuffer(cmd,&bi));
      vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
      vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&ds,0,NULL);
      vkCmdDispatch(cmd,1,1,1);
      VK_CHECK(vkEndCommandBuffer(cmd)); }
    printf("Submitting compute dispatch...\n");
    { VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO,NULL,0,NULL,NULL,1,&cmd};
      VK_CHECK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));
      VK_CHECK(vkQueueWaitIdle(queue)); }
    /* Verify */
    { uint32_t *p; VK_CHECK(vkMapMemory(dev,mem,0,16,0,(void**)&p));
      printf("Output: [%u, %u, %u, %u]\n", p[0],p[1],p[2],p[3]);
      if(p[0]==3&&p[1]==1&&p[2]==0&&p[3]==0) printf("GPU COMPUTE DISPATCH: PASS\n");
      else if(p[0]==0&&p[1]==0&&p[2]==0&&p[3]==0) printf("GPU COMPUTE: DATA NOT WRITTEN\n");
      else printf("GPU COMPUTE: UNEXPECTED VALUES\n");
      vkUnmapMemory(dev,mem); }
    vkDestroyCommandPool(dev,cp,NULL); vkDestroyDescriptorPool(dev,dp,NULL);
    vkDestroyPipeline(dev,pipeline,NULL); vkDestroyShaderModule(dev,sm,NULL);
    vkDestroyPipelineLayout(dev,pl,NULL); vkDestroyDescriptorSetLayout(dev,dsl,NULL);
    vkDestroyBuffer(dev,buf,NULL); vkFreeMemory(dev,mem,NULL);
    vkDestroyDevice(dev,NULL); vkDestroyInstance(inst,NULL);
    return 0;
}
