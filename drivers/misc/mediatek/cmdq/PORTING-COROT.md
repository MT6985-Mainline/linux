# Corot secure display providers

The MT6985 MTEE secure CMDQ and DMA-BUF heap providers are imported and adapted
from MiCode `corot-t-oss` commit
`cb2ab59a621b17aafa4a3f349c84d9f18adf7aaf`.
The complete display migration and device verification remain the parent task;
source compilation alone does not verify secure firmware or panel output.

## Providers and dependencies

- `mailbox/cmdq-sec-helper.c`: packet secure metadata, MTEE selection, payload
  and secure-address metadata handling, completion and diagnostic helpers.
- `mailbox/cmdq-sec-mailbox.c`: real MTEE task submission, cancellation, secure
  execution cookies and notification loop. Cookie memory uses the last page of
  the authentic `mediatek,me_cmdq_reserved` region.
- `mailbox/cmdq_sec_mtee.c`: KREE command and shared-memory sessions, registered
  IWC buffers and secure command execution using the original command IDs and
  `com.mediatek.geniezone.*` service names.
- `../geniezone/`: authentic SMC, Trusty virtio/IRQ/IPC, TZ_SYSTEM, KREE and
  GZ_MAIN providers required to bind `mediatek,trusty-mtee-v1`.
- `../trusted_mem/`: authentic trusted-memory device/region/peer management,
  KREE region allocation and protected SSheap page allocation.
- `../../../dma-buf/heaps/mtk_sec_heap.c`: real secure region/page exporters,
  allocations, DMA mapping and release; classifier checks exporter operations.
  Secure handle APIs retain `u64` values.
- `../../../dma-buf/heaps/mtk_heap.c`: real kernel DMA-BUF naming and clearing,
  replacing the previous no-op inline and old naming stub.

Required built-in selections are `MTK_CMDQ_MBOX_EXT`, `MTK_CMDQ_SECURE`,
`MTK_GENIEZONE`, `MTK_GZ_MAIN`, `MTK_TRUSTED_MEMORY_SUBSYSTEM`,
`MTK_PROT_MEM_SUPPORT`, `MTK_SECURE_MEM_SUPPORT`, `MTK_WFD_SMEM_SUPPORT`,
`DMABUF_HEAPS`, `DMABUF_HEAPS_MTK_SECURE` and `MTK_IOMMU_MT6985`.
The GenieZone umbrella selects KREE, TZ_SYSTEM, SMC, virtio/ring, IRQ and IPC.
The parent Mediatek Kconfig/Makefile must source the local GenieZone and trusted
memory Kconfigs and enter each directory once.

## Linux 7.2 adaptations

DMA-heap allocation callbacks use `u32 fd_flags` and `u64 heap_flags`.
DMA-BUF exporters return a distinct SG table for each mapping and free it on
matching unmap; stable per-domain IOVA mappings remain exporter-private.
Copied SG tables preserve their own chain pointers, check IOMMU domain identity
and retain the device references needed for eventual DMA unmap.

Platform removal, class creation, virtqueue information, page pin/PFN lookup,
string copying and reserved-memory operation tables use current Linux APIs.
The optional GZ log and alternative SSMR reserved-memory declarations use
`reserved_mem_ops.node_init(unsigned long, struct reserved_mem *)`.
Allocation frequency hints use real Linux frequency QoS at MiCode's MT6985
little/super 2 GHz and big 2.4 GHz targets. FF-A uses a discovered firmware
partition through the current FF-A bus and memory operations.

## Failure and lifetime handling

Secure CMDQ registers its mailbox only after the real clock, DMA mask, MMIO,
shared cookie and workqueues are ready. Probe failure balances device/OF refs,
workqueues and coherent memory. Late WSM preparation is per-controller delayed
work and is canceled during removal.

KREE session zero remains valid: separate ownership flags track command and
memory sessions. Every extra IWC registration error propagates. Registered
memory is retained if secure unregister fails. Failed secure cancellation does
not report a stopped thread.

Secure submission clears stale acceptance before validating/allocating its real
task, sets acceptance only after successful allocation, and keeps it through
fast callbacks. Secure wait resets the flag under the client channel mutex,
after the native flush helper has observed acceptance.

Heap mapping/cache failures undo the actual DMA mapping. Region accounting
updates under the heap lock; DMA-BUF export failure releases both the trusted
reference and accounting. Partial failed heap probes remain unavailable until
a complete retry. SSheap releases SG tables and pool accounting and retains
pages if FF-A reclaim or MTEE unassign fails.

Corot uses MTEE/KREE. Generic `CONFIG_TEE`/OP-TEE is independent of proprietary
Trustonic/Microtrust TEEC. Unsupported legacy REE/TEE commands and unavailable
MTEE support return errors; no fake-success firmware/session provider is used.
Secure CMDQ and region heaps bind permanent SoC devices and suppress manual
unbind, consistent with the built-in configuration and permanent heap nodes.

## Validation recorded on 2026-11-15

- All 39 selected/affected C sources passed ARM64 clang syntax checks against
  prepared `out/r351` kernel headers and flags.
- The central secure Kbuild target passed; evidence is the latest
  `out/r351/secure-build.log` section, `2026-11-15 13:43:18`.
- `llvm-nm` confirms strong text definitions and `__export_symbol_*` markers for
  `cmdq_sec_pkt_set_data`, `cmdq_sec_pkt_set_mtee`, `cmdq_sec_mbox_stop`,
  `is_mtk_sec_heap_dmabuf` and `dmabuf_to_secure_handle` in the actual objects.
  The MTEE object references the real KREE session, shared-memory and service
  APIs. The secure stop implementation occupies 376 bytes of ARM64 code.
- The secure handle public declaration, implementation and buffer storage all
  use `u64`.
- Whole-kernel link and real firmware/panel output are still pending the parent
  task. No device operation was performed by this task.
