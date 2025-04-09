#ifndef _GPU_PTR_TRANSLATOR_H
#define _GPU_PTR_TRANSLATOR_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * Define unique magic char for ioctl commands
 */
#define GPU_PTR_TRANSLATOR_IOCTL_MAGIC 'G'

struct gpt_ioctl_get_pages_args {
  __u64 vaddr;  // [Input] GPU virtual address to translate
  __u64 length; // [Input] Length of the memory region (bytes)

  // [Output] Populated by kernel
  __u64 paddr; // [Output] DMA address of the *first* segment
  __u32 nents; // [Output] Total number of scatter-gather segments
};

struct gpt_ioctl_dmabuf_get_pages_args {
  __u64 vaddr;     // [Input] GPU virtual address
  __u64 length;    // [Input] Length of the memory region (bytes)
  __s32 dmabuf_fd; // [Input] Associated DMA-BUF file descriptor

  // [Output] Populated by kernel
  __u64 paddr; // [Output] DMA address of the *first* segment
  __u32 nents; // [Output] Total number of scatter-gather segments
};

/* ---- IOCTL Command Definitions ---- */

/* The macro _IOWR indicates that the command requires both reading and writing
 * of parameters (bidirectional data transfer).  */
#define GPU_PTR_TRANSLATOR_IOCTL_GET_PAGES                                     \
  _IOWR(GPU_PTR_TRANSLATOR_IOCTL_MAGIC, 1, struct gpt_ioctl_get_pages_args)

#define GPU_PTR_TRANSLATOR_IOCTL_DMABUF_GET_PAGES                              \
  _IOWR(GPU_PTR_TRANSLATOR_IOCTL_MAGIC, 2,                                     \
        struct gpt_ioctl_dmabuf_get_pages_args)

#endif /* _GPU_PTR_TRANSLATOR_H */
