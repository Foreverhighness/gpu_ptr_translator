#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "amd_rdma.h"
#include "gpu_ptr_translator.h"

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Foreverhighness@github.com");
MODULE_DESCRIPTION("GPU Addresss Translation Driver");

#define GPT_DEVICE_NAME "gpu_ptr_translator"

static struct miscdevice gpt_misc_dev;
static const struct amd_rdma_interface *rdma_interface;

static int ioctl_get_pages(struct file *filp, unsigned long arg) {
  struct gpt_ioctl_get_pages_args params = {0};
  struct pid *current_pid;
  struct amd_p2p_info *info;
  struct sg_table *sgt;
  struct scatterlist *sg;
  int rc, i;
  unsigned long page_size;
  u64 vaddr, len, paddr;
  u32 nents;

  if (copy_from_user(&params, (void __user *)arg, sizeof(params))) {
    pr_err("copy_from_user failed on pointer %p\n", (void *)arg);
    return -EFAULT;
  }
  vaddr = params.vaddr;
  len = params.length;

  /* 获取当前进程的PID结构（用于地址空间识别） */
  current_pid = get_task_pid(current, PIDTYPE_PID);

  /* 获取页大小 */
  rc = rdma_interface->get_page_size(vaddr, len, current_pid, &page_size);
  if (rc < 0) {
    pr_err("Get page size: 0x%016llx (len: %llx) failed: %d\n", vaddr, len, rc);
    return rc;
  }
  pr_info("PAGE_SIZE: %lu\n", page_size);

  /* 实际获取物理页 */
  rc = rdma_interface->get_pages(vaddr, len, current_pid, NULL, &info, NULL,
                                 NULL);
  if (rc < 0) {
    pr_err("Get pages: 0x%016llx (len: %llx) failed: %d\n", vaddr, len, rc);
    return rc;
  }

  sgt = info->pages;
  nents = sgt->nents;
  pr_info("PEER   : Get 0x%016llx (len: %llu) mapped to %d pages\n", vaddr, len,
          nents);
  for_each_sg(sgt->sgl, sg, nents, i) {
    if (i == 0) {
      paddr = sg_dma_address(sg);
    }
    pr_info("PEER   : segment_%d dma_address 0x%llx length 0x%x dma_length "
            "0x%x\n",
            i, sg_dma_address(sg), sg->length, sg_dma_len(sg));
  }

  rc = rdma_interface->put_pages(&info);
  if (rc < 0) {
    pr_err("Could not put pages back: %d\n", rc);
    return rc;
  }

  /* 将结果回写用户空间 */
  params.paddr = paddr;
  params.nents = nents;
  if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
    pr_err("copy_to_user failed on pointer %p\n", (void *)arg);
    return -EFAULT;
  }

  return 0;
}

static int ioctl_dmabuf_get_pages(struct file *filp, unsigned long arg) {
  struct gpt_ioctl_dmabuf_get_pages_args params = {0};
  struct dma_buf *dmabuf;
  struct dma_buf_attachment *attach;
  struct sg_table *sgt;
  struct scatterlist *sg;
  int i, rc, dmabuf_fd;
  u64 vaddr, len, paddr;
  u32 nents;

  rc = copy_from_user(&params, (void __user *)arg, sizeof(params));
  if (rc < 0) {
    pr_err("copy_from_user failed on pointer %p\n", (void *)arg);
    return rc;
  }
  vaddr = params.vaddr;
  len = params.length;
  dmabuf_fd = params.dmabuf_fd;

  /* 通过文件描述符获取 DMA-BUF 对象 */
  dmabuf = dma_buf_get(dmabuf_fd);
  if (IS_ERR(dmabuf)) {
    rc = PTR_ERR(dmabuf);
    pr_err("Get dmabuf failed: %d\n", rc);
    return rc;
  }

  /* 创建缓冲区附件（用于后续映射） */
  attach = dma_buf_attach(dmabuf, gpt_misc_dev.this_device);
  if (IS_ERR(attach)) {
    rc = PTR_ERR(attach);
    pr_err("Attach dmabuf failed: %d\n", rc);
    goto fail_put;
  }

  /* 实际执行物理地址映射 */
  sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
  if (IS_ERR(sgt)) {
    rc = PTR_ERR(sgt);
    pr_err("Map dmabuf failed: %d\n", rc);
    goto fail_detach;
  }

  pr_info("DMA-BUF: Get 0x%016llx (len: %llu) mapped to %d pages\n", vaddr, len,
          sgt->nents);
  nents = sgt->nents;
  for_each_sg(sgt->sgl, sg, nents, i) {
    if (i == 0) {
      paddr = sg_dma_address(sg);
    }
    pr_info("DMA-BUF: segment_%d dma_address 0x%llx length 0x%x dma_length "
            "0x%x\n",
            i, sg_dma_address(sg), sg->length, sg_dma_len(sg));
  }

  /* 清理阶段 */
  dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
fail_detach:
  dma_buf_detach(dmabuf, attach);
fail_put:
  dma_buf_put(dmabuf);

  /* 将结果回写用户空间 */
  if (rc == 0) {
    params.paddr = paddr;
    params.nents = nents;
    if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
      pr_err("copy_to_user failed on pointer %p\n", (void *)arg);
      return -EFAULT;
    }
  }

  return rc;
}

static const struct ioctl_handler_map {
  int (*handler)(struct file *filp, unsigned long arg);
  unsigned int cmd;
} handlers[] = {
    {ioctl_get_pages, GPU_PTR_TRANSLATOR_IOCTL_GET_PAGES},
    {ioctl_dmabuf_get_pages, GPU_PTR_TRANSLATOR_IOCTL_DMABUF_GET_PAGES},
    {NULL, 0}};

/*
 * gpt_unlocked_ioctl - 处理IOCTL命令
 * @filp: 文件结构指针
 * @cmd:  IOCTL命令号
 * @arg:  用户空间参数指针
 * 返回值： 0成功，负数为错误码
 * 核心功能：处理 GET_PAGES 和 DMABUF_GET_PAGES 命令
 */
static long gpt_unlocked_ioctl(struct file *filp, unsigned int cmd,
                               unsigned long arg) {
  int rc = -ENOTTY, i;
  for (i = 0; handlers[i].handler != NULL; ++i) {
    if (cmd == handlers[i].cmd) {
      rc = handlers[i].handler(filp, arg);
      break;
    }
  }
  return rc;
}

/* 文件操作结构体：定义驱动支持的操作 */
static const struct file_operations fops = {
    .owner = THIS_MODULE,                 // 模块引用计数
    .unlocked_ioctl = gpt_unlocked_ioctl, // IOCTL handler
};

/* misc设备定义 */
static struct miscdevice gpt_misc_dev = {
    .minor = MISC_DYNAMIC_MINOR, // 自动分配次设备号
    .name = GPT_DEVICE_NAME,     // 设备节点名称
    .fops = &fops,               // 关联文件操作
    .mode = 0666,                // 设备权限（rw-rw-rw-）
};

/* 符号引用：动态获取AMD内核接口 */
static int (*p2p_query_rdma_interface)(const struct amd_rdma_interface **);

/*
 * 模块初始化函数：
 * 1. 获取 RDMA 接口
 * 2. 注册 misc 设备
 */
static int __init gpu_ptr_translator_init(void) {
  int rc;

  /* 动态查找AMD内核符号 */
  p2p_query_rdma_interface =
      (int (*)(const struct amd_rdma_interface **))symbol_request(
          amdkfd_query_rdma_interface);
  if (!p2p_query_rdma_interface) {
    pr_err("Can not get symbol amdkfd_query_rdma_interface, please load "
           "amdgpu driver\n");
    return -ENOENT;
  }

  /* 实际获取接口指针 */
  rc = p2p_query_rdma_interface(&rdma_interface);
  if (rc < 0) {
    pr_err("Can not get RDMA Interface (result = %d)\n", rc);
    goto err_symbol;
  }

  /* 注册misc设备（自动创建设备节点） */
  rc = misc_register(&gpt_misc_dev);
  if (rc < 0) {
    pr_err("Can not register device (result = %d)\n", rc);
    goto err_symbol;
  }

  pr_info("GPU Ptr Translator loaded\n");
  return 0;

err_symbol:
  symbol_put(amdkfd_query_rdma_interface);
  return rc;
}

/*
 * 模块退出函数：
 * 1. 注销设备
 * 2. 释放符号引用
 */
static void __exit gpu_ptr_translator_exit(void) {
  /* 注销misc设备 */
  misc_deregister(&gpt_misc_dev);

  /* 释放AMD接口符号引用 */
  if (p2p_query_rdma_interface) {
    symbol_put(amdkfd_query_rdma_interface);
  }
  pr_info("GPU Translator unloaded\n");
}

/* 注册模块入口/出口 */
module_init(gpu_ptr_translator_init);
module_exit(gpu_ptr_translator_exit);
