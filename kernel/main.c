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

  /* Get page size  */
  rc = rdma_interface->get_page_size(vaddr, len, NULL, &page_size);
  if (rc < 0) {
    pr_err("Get page size: 0x%016llx (len: %llx) failed: %d\n", vaddr, len, rc);
    return rc;
  }
  pr_info("PAGE_SIZE: %lu\n", page_size);

  /* Get physical pages */
  rc = rdma_interface->get_pages(vaddr, len, NULL, NULL, &info, NULL, NULL);
  if (rc < 0) {
    pr_err("Get pages: 0x%016llx (len: %llx) failed: %d\n", vaddr, len, rc);
    return rc;
  }

  sgt = info->pages;
  nents = sgt->nents;
  pr_info("PEER   : Get 0x%016llx (len: %llu) mapped to %d pages\n", vaddr, len,
          nents);

  // Extract first page address
  if (nents > 0) {
    sg = sgt->sgl; // First entry
    paddr = sg_dma_address(sg);
    // Optional: Log all pages for debugging
    for_each_sg(sgt->sgl, sg, nents, i) {
      pr_info("PEER   : segment_%d dma_address 0x%llx length 0x%x dma_length "
              "0x%x\n",
              i, sg_dma_address(sg), sg->length, sg_dma_len(sg));
    }
  } else {
    paddr = 0; // No pages found
  }

  rc = rdma_interface->put_pages(&info);
  if (rc < 0) {
    pr_err("Could not put pages back: %d\n", rc);
    // Continue to copy back results if possible, but return error
  }

  /* Copy results back to user space */
  params.paddr = paddr;
  params.nents = nents;
  if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
    pr_err("copy_to_user failed on pointer %p\n", (void *)arg);
    return -EFAULT;
  }

  /* Return original error code if put_pages failed, otherwise 0 */
  return rc;
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

  dmabuf = dma_buf_get(dmabuf_fd);
  if (IS_ERR(dmabuf)) {
    rc = PTR_ERR(dmabuf);
    pr_err("Get dmabuf (fd: %d) failed: %d\n", dmabuf_fd, rc);
    return rc;
  }

  /* Check if dmabuf size matches expected length */
  if (dmabuf->size < len) {
    pr_warn("DMA-BUF size (%zu) is smaller than requested length (%llu)\n",
            dmabuf->size, len);
    // Decide if this is an error or just a warning
    // len = dmabuf->size; // Optionally adjust length
  }

  attach = dma_buf_attach(dmabuf, gpt_misc_dev.this_device);
  if (IS_ERR(attach)) {
    rc = PTR_ERR(attach);
    pr_err("Attach dmabuf failed: %d\n", rc);
    goto fail_put;
  }

  sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
  if (IS_ERR(sgt)) {
    rc = PTR_ERR(sgt);
    pr_err("Map dmabuf attachment failed: %d\n", rc);
    sgt = NULL;
    goto fail_detach;
  }

  nents = sgt->nents;
  pr_info("DMA-BUF: Get 0x%016llx (len: %llu) (fd: %d)mapped to %d pages\n",
          vaddr, len, dmabuf_fd, nents);

  // Extract first page address
  if (nents > 0) {
    sg = sgt->sgl;
    paddr = sg_dma_address(sg);
    // Optional: Log all pages for debugging
    for_each_sg(sgt->sgl, sg, nents, i) {
      pr_info("DMA-BUF: segment_%d dma_address 0x%llx length 0x%x dma_length "
              "0x%x\n",
              i, sg_dma_address(sg), sg->length, sg_dma_len(sg));
    }
  } else {
    paddr = 0; // No pages found
  }

  /* Cleanup */
  dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
fail_detach:
  dma_buf_detach(dmabuf, attach);
fail_put:
  dma_buf_put(dmabuf);

  /* Copy results back to user space */
  if (rc == 0) { // Only copy back if mapping was successful
    params.paddr = paddr;
    params.nents = nents;
    if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
      pr_err("copy_to_user failed on pointer %p\n", (void *)arg);
      return -EFAULT;
    }
  }

  return rc;
}

// --- IOCTL Dispatch ---
static const struct ioctl_handler_map {
  int (*handler)(struct file *filp, unsigned long arg);
  unsigned int cmd;
} handlers[] = {
    {ioctl_get_pages, GPU_PTR_TRANSLATOR_IOCTL_GET_PAGES},
    {ioctl_dmabuf_get_pages, GPU_PTR_TRANSLATOR_IOCTL_DMABUF_GET_PAGES},
    {NULL, 0}};

/*
 * gpt_unlocked_ioctl - Process ioctl commands
 * @filp: file struct pointer
 * @cmd:  ioctl command code
 * @arg:  user space argument pointer
 * Return: 0 on success, negative error code on failure
 * Core function: dispatches ioctl commands to appropriate handlers.
 */
static long gpt_unlocked_ioctl(struct file *filp, unsigned int cmd,
                               unsigned long arg) {
  int rc = -ENOTTY, i;

  BUG_ON(rdma_interface == NULL);

  for (i = 0; handlers[i].handler != NULL; ++i) {
    if (cmd == handlers[i].cmd) {
      rc = handlers[i].handler(filp, arg);
      break;
    }
  }

  if (handlers[i].handler == NULL) {
    pr_warn("Unknown ioctl command received: 0x%x\n", cmd);
  }

  return rc;
}

/* File operations structure: defines driver supported operations */
static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = gpt_unlocked_ioctl,
};

/* Misc device definition */
static struct miscdevice gpt_misc_dev = {
    .minor = MISC_DYNAMIC_MINOR, // Auto-assign minor number
    .name = GPT_DEVICE_NAME,     // Device node name (/dev/gpu_ptr_translator)
    .fops = &fops,               // Associate file operations
    .mode = S_IRUSR | S_IRGRP | S_IROTH, // Device Permission（r--r--r--）
};

/* Symbol reference: dynamically get AMD kernel interface */
static int (*p2p_query_rdma_interface)(const struct amd_rdma_interface **);

/*
 * Module initialization function:
 * 1. Get RDMA interface
 * 2. Register misc device
 */
static int __init gpu_ptr_translator_init(void) {
  int rc;

  /* Dynamically look up AMD kernel symbol */
  p2p_query_rdma_interface =
      (int (*)(const struct amd_rdma_interface **))symbol_request(
          amdkfd_query_rdma_interface);

  if (!p2p_query_rdma_interface) {
    pr_err("Can not get symbol amdkfd_query_rdma_interface, please load "
           "amdgpu driver\n");
    rdma_interface = NULL;
    return -ENOENT;
  }

  /* Actually get the interface pointer */
  rc = p2p_query_rdma_interface(&rdma_interface);
  if (rc < 0) {
    pr_err("Can not get RDMA Interface (result = %d)\n", rc);
    goto err_symbol;
  }

  /* Register the misc device (creates /dev/gpu_ptr_translator) */
  rc = misc_register(&gpt_misc_dev);
  if (rc < 0) {
    pr_err("Can not register device (result = %d)\n", rc);
    goto err_symbol;
  }

  pr_info("GPU Ptr Translator loaded (/dev/%s)\n", GPT_DEVICE_NAME);
  return 0;

err_symbol:
  symbol_put(amdkfd_query_rdma_interface);
  rdma_interface = NULL;
  return rc;
}

/*
 * Module exit function:
 * 1. Deregister device
 * 2. Release symbol reference
 */
static void __exit gpu_ptr_translator_exit(void) {
  /* Deregister the misc device */
  misc_deregister(&gpt_misc_dev);

  /* Release the AMD interface symbol reference if it was acquired */
  if (p2p_query_rdma_interface) {
    symbol_put(amdkfd_query_rdma_interface);
  }
  rdma_interface = NULL;

  pr_info("GPU Translator unloaded\n");
}

/* Register module entry/exit points */
module_init(gpu_ptr_translator_init);
module_exit(gpu_ptr_translator_exit);
