// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Standalone probe: does this Vulkan driver blend in LINEAR space
 *         through an _SRGB view over a MUTABLE_FORMAT UNORM image?
 *
 * WHAT THIS IS FOR. The vk_native compose path (#1589/#1610) composites into
 * a runtime-private image created UNORM + MUTABLE_FORMAT and attached through
 * its _SRGB view, so the fixed-function blender decodes, blends in linear
 * light and re-encodes on write. The whole colour model rests on the driver
 * honouring that. This probe answers it in isolation, in about a second, with
 * no runtime, no window and no display processor involved.
 *
 * WHEN TO RE-RUN IT. Not per commit — it tests the DRIVER, not our code, so
 * it is deliberately NOT a ctest (a test needing a live Vulkan device either
 * gets skipped in CI and rots, or fails on machines without one). Re-run it
 * on a NEW GPU VENDOR OR DRIVER STACK. Android is the leg where this could
 * still bite: the result below is Apple M1 Pro / MoltenVK and says nothing
 * about Adreno or Mali.
 *
 * MEASURED, macOS 26 / Apple M1 Pro / MoltenVK, 2026-09-21:
 *   A  image=UNORM+MUTABLE, view=_SRGB   centre BGRA = (188,188,188,128)
 *   B  image=SRGB(native),  view=_SRGB   centre BGRA = (188,188,188,128)
 *   A == B, and 188 (not 128) => blending is LINEAR, sRGB attachment honoured.
 * The alpha reading 128 rather than 188 is a second, free check: sRGB formats
 * encode the COLOUR channels only and leave alpha linear, so an encode that
 * had leaked into alpha would show up here.
 *
 * READING THE RESULT. It blends 50%% linear white over linear black, so the
 * stored byte names the failure:
 *   188  correct   - decode, blend in linear, re-encode
 *   128  no conversion - the attachment's sRGB-ness was ignored
 *   A != B          - the mutable-view route specifically is broken; use a
 *                     natively-_SRGB private image instead
 *
 * BUILD AND RUN (macOS; adjust the paths for another platform):
 *   glslangValidator -V v.vert --vn VERT_SPV -o vert.h
 *   glslangValidator -V v.frag --vn FRAG_SPV -o frag.h
 *   cc -O1 -o probe vk_srgb_blend_probe.c -I$VULKAN_SDK/include -L$VULKAN_SDK/lib -lvulkan
 *   ./probe
 * The two shaders are trivial (a fullscreen triangle and a push-constant
 * colour) and are listed at the top of the file below.
 *
 * SHADERS
 *   v.vert: #version 450
 *           void main(){ vec2 uv=vec2((gl_VertexIndex<<1)&2, gl_VertexIndex&2);
 *                        gl_Position=vec4(uv*2.0-1.0,0.0,1.0); }
 *   v.frag: #version 450
 *           layout(push_constant) uniform P { vec4 c; } pc;
 *           layout(location=0) out vec4 o;
 *           void main(){ o = pc.c; }
 */

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vert.h"
#include "frag.h"
#define W 64
#define H 64
#define CK(x) do{ VkResult r_=(x); if(r_!=VK_SUCCESS){ printf("FAIL %s -> %d\n", #x, r_); exit(1);} }while(0)
static VkDevice dev; static VkPhysicalDevice phys; static VkQueue q; static uint32_t qfam;
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want){
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys,&mp);
  for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((bits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&want)==want) return i;
  printf("no memtype\n"); exit(1);
}
// returns the read-back BGRA pixel at centre
static void run_case(const char *label, int mutable_unorm, unsigned char out[4]){
  VkFormat img_fmt = mutable_unorm ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_B8G8R8A8_SRGB;
  VkFormat view_fmt = VK_FORMAT_B8G8R8A8_SRGB;
  VkFormat list[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
  VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,NULL,2,list};
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.pNext = mutable_unorm ? &fl : NULL;
  ici.flags = mutable_unorm ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
  ici.imageType=VK_IMAGE_TYPE_2D; ici.format=img_fmt; ici.extent=(VkExtent3D){W,H,1};
  ici.mipLevels=1; ici.arrayLayers=1; ici.samples=VK_SAMPLE_COUNT_1_BIT; ici.tiling=VK_IMAGE_TILING_OPTIMAL;
  ici.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage img; CK(vkCreateImage(dev,&ici,NULL,&img));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img,&mr);
  VkMemoryAllocateInfo mai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr.size,memtype(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  VkDeviceMemory mem; CK(vkAllocateMemory(dev,&mai,NULL,&mem)); CK(vkBindImageMemory(dev,img,mem,0));
  VkImageViewCreateInfo vci={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image=img; vci.viewType=VK_IMAGE_VIEW_TYPE_2D; vci.format=view_fmt;
  vci.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VkImageView view; CK(vkCreateImageView(dev,&vci,NULL,&view));
  VkAttachmentDescription att={0}; att.format=view_fmt; att.samples=VK_SAMPLE_COUNT_1_BIT;
  att.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ar={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sp={0}; sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount=1; sp.pColorAttachments=&ar;
  VkRenderPassCreateInfo rpci={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,NULL,0,1,&att,1,&sp,0,NULL};
  VkRenderPass rp; CK(vkCreateRenderPass(dev,&rpci,NULL,&rp));
  VkFramebufferCreateInfo fbci={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,NULL,0,rp,1,&view,W,H,1};
  VkFramebuffer fb; CK(vkCreateFramebuffer(dev,&fbci,NULL,&fb));
  VkShaderModuleCreateInfo smv={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,sizeof(VERT_SPV),VERT_SPV};
  VkShaderModuleCreateInfo smf={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,sizeof(FRAG_SPV),FRAG_SPV};
  VkShaderModule vs,fs; CK(vkCreateShaderModule(dev,&smv,NULL,&vs)); CK(vkCreateShaderModule(dev,&smf,NULL,&fs));
  VkPushConstantRange pcr={VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
  VkPipelineLayoutCreateInfo plci={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,0,NULL,1,&pcr};
  VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev,&plci,NULL,&pl));
  VkPipelineShaderStageCreateInfo st[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_VERTEX_BIT,vs,"main",NULL},
                                          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_FRAGMENT_BIT,fs,"main",NULL}};
  VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport vp={0,0,W,H,0,1}; VkRect2D sc={{0,0},{W,H}};
  VkPipelineViewportStateCreateInfo vps={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,NULL,0,1,&vp,1,&sc};
  VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.lineWidth=1.0f;
  VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba={0}; ba.blendEnable=VK_TRUE;
  ba.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  ba.colorBlendOp=VK_BLEND_OP_ADD; ba.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;
  ba.alphaBlendOp=VK_BLEND_OP_ADD; ba.colorWriteMask=0xF;
  VkPipelineColorBlendStateCreateInfo cb={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,NULL,0,VK_FALSE,VK_LOGIC_OP_CLEAR,1,&ba,{0,0,0,0}};
  VkGraphicsPipelineCreateInfo gp={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount=2; gp.pStages=st; gp.pVertexInputState=&vi; gp.pInputAssemblyState=&ia; gp.pViewportState=&vps;
  gp.pRasterizationState=&rs; gp.pMultisampleState=&ms; gp.pColorBlendState=&cb; gp.layout=pl; gp.renderPass=rp;
  VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gp,NULL,&pipe));
  VkCommandPoolCreateInfo cpci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,NULL,0,qfam};
  VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpci,NULL,&cp));
  VkCommandBufferAllocateInfo cbai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,NULL,cp,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
  VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev,&cbai,&cmd));
  VkBufferCreateInfo bci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,NULL,0,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_SHARING_MODE_EXCLUSIVE,0,NULL};
  VkBuffer buf; CK(vkCreateBuffer(dev,&bci,NULL,&buf));
  VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev,buf,&bmr);
  VkMemoryAllocateInfo bmai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,bmr.size,memtype(bmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  VkDeviceMemory bmem; CK(vkAllocateMemory(dev,&bmai,NULL,&bmem)); CK(vkBindBufferMemory(dev,buf,bmem,0));
  VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,NULL,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,NULL};
  CK(vkBeginCommandBuffer(cmd,&bi));
  VkClearValue cv; cv.color=(VkClearColorValue){{0.0f,0.0f,0.0f,1.0f}};
  VkRenderPassBeginInfo rbi={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,NULL,rp,fb,{{0,0},{W,H}},1,&cv};
  vkCmdBeginRenderPass(cmd,&rbi,VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
  float base[4]={0.0f,0.0f,0.0f,1.0f};            // opaque linear black
  vkCmdPushConstants(cmd,pl,VK_SHADER_STAGE_FRAGMENT_BIT,0,16,base); vkCmdDraw(cmd,3,1,0,0);
  float over[4]={1.0f,1.0f,1.0f,0.5f};            // linear white at 50%
  vkCmdPushConstants(cmd,pl,VK_SHADER_STAGE_FRAGMENT_BIT,0,16,over); vkCmdDraw(cmd,3,1,0,0);
  vkCmdEndRenderPass(cmd);
  VkBufferImageCopy reg={0}; reg.bufferRowLength=W; reg.bufferImageHeight=H;
  reg.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; reg.imageExtent=(VkExtent3D){W,H,1};
  vkCmdCopyImageToBuffer(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buf,1,&reg);
  CK(vkEndCommandBuffer(cmd));
  VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
  CK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
  void *m; CK(vkMapMemory(dev,bmem,0,W*H*4,0,&m));
  unsigned char *px=(unsigned char*)m + ((H/2)*W + (W/2))*4;
  out[0]=px[0]; out[1]=px[1]; out[2]=px[2]; out[3]=px[3];
  printf("  %-26s image=%s view=_SRGB  centre BGRA = (%3u,%3u,%3u,%3u)\n", label,
         mutable_unorm?"UNORM+MUTABLE":"SRGB(native)", out[0],out[1],out[2],out[3]);
  vkUnmapMemory(dev,bmem);
}
int main(void){
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion=VK_API_VERSION_1_2;
  const char *iexts[]={"VK_KHR_portability_enumeration","VK_KHR_get_physical_device_properties2"};
  VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.flags=0x00000001; ici.pApplicationInfo=&ai; ici.enabledExtensionCount=2; ici.ppEnabledExtensionNames=iexts;
  VkInstance inst; CK(vkCreateInstance(&ici,NULL,&inst));
  uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,NULL); VkPhysicalDevice pds[8]; if(n>8)n=8;
  vkEnumeratePhysicalDevices(inst,&n,pds); phys=pds[0];
  VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys,&pp);
  printf("device: %s  (maxPushConstantsSize=%u)\n", pp.deviceName, pp.limits.maxPushConstantsSize);
  uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,NULL); VkQueueFamilyProperties qf[8]; if(qn>8)qn=8;
  vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,qf);
  qfam=0; for(uint32_t i=0;i<qn;i++) if(qf[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){qfam=i;break;}
  float pr=1.0f; VkDeviceQueueCreateInfo dq={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,NULL,0,qfam,1,&pr};
  const char *dexts[]={"VK_KHR_portability_subset","VK_KHR_image_format_list"};
  VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&dq;
  dci.enabledExtensionCount=2; dci.ppEnabledExtensionNames=dexts;
  CK(vkCreateDevice(phys,&dci,NULL,&dev)); vkGetDeviceQueue(dev,qfam,0,&q);
  printf("blend 50%% linear-white over linear-black, read back:\n");
  printf("  expected 188 if blending is LINEAR (sRGB attachment honoured), 128 if it is not\n");
  unsigned char a[4],b[4];
  run_case("A mutable-UNORM+SRGB view", 1, a);
  run_case("B native SRGB image",      0, b);
  int same = (a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3]);
  printf("\nRESULT: A %s B   (%s)\n", same?"==":"!=", same?"mutable view behaves like a native sRGB image":"MUTABLE-VIEW ROUTE DIVERGES");
  printf("RESULT: blending is %s\n", (a[0]>=180&&a[0]<=195)?"LINEAR (188-ish) - sRGB attachment honoured":
                                      ((a[0]>=120&&a[0]<=136)?"ENCODED (128-ish) - sRGB attachment IGNORED":"UNEXPECTED"));
  return 0;
}
