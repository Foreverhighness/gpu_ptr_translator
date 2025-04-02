#ifndef _GPU_PTR_TRANSLATOR_H
#define _GPU_PTR_TRANSLATOR_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * 定义唯一的魔术字，用于生成ioctl命令
 */
#define GPU_PTR_TRANSLATOR_IOCTL_MAGIC 'G'

struct gpt_ioctl_get_pages_args {
  __u64 vaddr;  // [输入] 需要转换的 GPU 虚拟地址
  __u64 length; // [输入] 内存区域长度（Bytes）

  __u64 paddr;
  __u32 nents;
};

struct gpt_ioctl_dmabuf_get_pages_args {
  __u64 vaddr;     // [输入] 需要转换的 GPU 虚拟地址
  __u64 length;    // [输入] 内存区域长度（Bytes）
  __s32 dmabuf_fd; // [输入] 关联的 DMA-BUF 文件描述符

  __u64 paddr;
  __u32 nents;
};

/*
 * 生成ioctl命令码
 * _IOWR: 表示该命令需要读写参数（双向传输）
 */
#define GPU_PTR_TRANSLATOR_IOCTL_GET_PAGES                                     \
  _IOWR(GPU_PTR_TRANSLATOR_IOCTL_MAGIC, 1, struct gpt_ioctl_get_pages_args)

#define GPU_PTR_TRANSLATOR_IOCTL_DMABUF_GET_PAGES                              \
  _IOWR(GPU_PTR_TRANSLATOR_IOCTL_MAGIC, 2, struct gpt_ioctl_dmabuf_get_pages_args)

#endif /* _GPU_PTR_TRANSLATOR_H */
