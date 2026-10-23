/** @file
  Device tree and initrd memory, from the device tree the previous stage handed over.

  The previous stage (LK) passes the physical address of a flattened device tree
  in x0, which SEC keeps in a HOB. This driver:

  * maps the tree and the initrd it names into the GCD as reserved memory, so
    every access is translated and no allocator can hand the ranges out;
  * publishes the initrd as a read-only block device, so a FAT image in the boot
    image's ramdisk shows up as a normal volume for UniXNU and DarwinBoot.

  Nothing here depends on a hardware address; every range comes from the DT.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

//
// The media the FAT driver sees. An eMMC sector is 512 bytes, and the
// bootloader reports the initrd as a byte range, so the media covers whole
// sectors only.
//
#define RAM_BLOCK_SIZE  512

//
// The tree is read before its own size is known, so a window around the
// pointer is mapped first. A device tree is far smaller than this; the range is
// two aligned 2 MiB blocks so a tree that straddles a 2 MiB boundary is covered.
//
#define FDT_WINDOW_ALIGN  SIZE_2MB
#define FDT_WINDOW_SIZE   (2 * SIZE_2MB)

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

/**
  Add a range to the GCD as reserved memory and map it write-back.

  Reserved keeps the firmware from allocating over it while every access still
  works; the page tables follow the GCD, so a range that is not added here is
  not necessarily reachable. A range the platform already added is left alone,
  and the attributes are only requested for the part that exists.

  @param[in] Start  First byte of the range.
  @param[in] End    End of the range, exclusive.
  @param[in] Name   Range name for the log.
**/
STATIC
VOID
MapReserved (
  IN UINT64       Start,
  IN UINT64       End,
  IN CONST CHAR8  *Name
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  UINT64                           Base;
  UINT64                           Limit;

  if (End <= Start) {
    return;
  }

  Base  = Start & ~((UINT64)SIZE_4KB - 1);
  Limit = ALIGN_VALUE (End, SIZE_4KB);
  if ((Limit <= Base) || ((Limit - Base) < SIZE_4KB)) {
    return;
  }

  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MERLINXMEM: %a 0x%lx-0x%lx: no descriptor %r\n", Name, Base, Limit, Status));
    return;
  }

  if (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeReserved,
                    Base,
                    Limit - Base,
                    EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB
                    );
    if (!EFI_ERROR (Status)) {
      Status = gDS->SetMemorySpaceAttributes (Base, Limit - Base, EFI_MEMORY_WB);
    }

    DEBUG ((DEBUG_ERROR, "MERLINXMEM: %a 0x%lx-0x%lx reserved %r\n", Name, Base, Limit - 1, Status));
    return;
  }

  //
  // Part of the range is already described (a firmware region or a range the
  // platform added). It is reachable, so only the attributes are requested.
  //
  Status = gDS->SetMemorySpaceAttributes (Base, Limit - Base, EFI_MEMORY_WB);
  DEBUG ((DEBUG_ERROR, "MERLINXMEM: %a 0x%lx-0x%lx already mapped, write-back %r\n", Name, Base, Limit - 1, Status));
}

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0x09F3212E, 0x61B0, 0x4296, { 0xA8, 0xD2, 0x07, 0x28, 0x62, 0xC8, 0x50, 0x03 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8                   *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA      mMedia;

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

/**
  Publish the initrd /chosen names as a read-only block device.

  The FAT volume of a boot payload lives in the boot image's ramdisk, which the
  bootloader places at linux,initrd-start and reports the end of in
  linux,initrd-end. Both properties are cells, either one or two each.

  @param[in] Fdt  Flattened device tree, already mapped.
**/
STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  EFI_STATUS  Status;
  INT32       Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32       StartLength;
  INT32       EndLength;
  UINT64      Start;
  UINT64      End;
  UINT64      Size;
  EFI_HANDLE  Handle;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "MERLINXMEM: no /chosen\n"));
    return;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, "MERLINXMEM: no initrd in /chosen\n"));
    return;
  }

  Start = ReadCells (StartProp, StartLength / 4);
  End   = ReadCells (EndProp, EndLength / 4);
  if (End <= Start) {
    DEBUG ((DEBUG_ERROR, "MERLINXMEM: initrd 0x%lx-0x%lx is empty\n", Start, End));
    return;
  }

  Size = End - Start;
  if ((Size % RAM_BLOCK_SIZE) != 0) {
    //
    // The bootloader reports the file size, not the sector count. Keep the
    // whole sectors and say how many bytes the media loses; bootimg pads the
    // ramdisk so this does not happen with an image the build driver packed.
    //
    DEBUG ((DEBUG_ERROR, "MERLINXMEM: initrd 0x%lx-0x%lx drops %lu bytes, not a whole number of sectors\n",
            Start, End, Size % RAM_BLOCK_SIZE));
    Size &= ~((UINT64)RAM_BLOCK_SIZE - 1);
  }

  if (Size < RAM_BLOCK_SIZE) {
    DEBUG ((DEBUG_ERROR, "MERLINXMEM: initrd 0x%lx-0x%lx is shorter than one sector\n", Start, End));
    return;
  }

  MapReserved (Start, Start + Size, "initrd");

  mRamBase                = (UINT8 *)(UINTN)Start;
  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (Size / RAM_BLOCK_SIZE) - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  DEBUG ((DEBUG_ERROR, "MERLINXMEM: initrd block device 0x%lx size 0x%lx %r\n", Start, Size, Status));
}

EFI_STATUS
EFIAPI
MerlinxBootMemoryDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             Dtb;
  UINT64             Window;

  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if (Hob == NULL) {
    DEBUG ((DEBUG_ERROR, "MERLINXMEM: no entry argument HOB\n"));
    return EFI_SUCCESS;
  }

  Dtb = *(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if ((Dtb == 0) || ((Dtb & 3) != 0)) {
    DEBUG ((DEBUG_ERROR, "MERLINXMEM: x0 0x%lx is not a device tree address\n", Dtb));
    return EFI_SUCCESS;
  }

  //
  // The tree is read to find the initrd, so its window has to be translated
  // first. MapReserved aligns down, so the pointer is inside the window.
  //
  Window = Dtb & ~((UINT64)FDT_WINDOW_ALIGN - 1);
  MapReserved (Window, Window + FDT_WINDOW_SIZE, "device tree");

  if (FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0) {
    DEBUG ((DEBUG_ERROR, "MERLINXMEM: x0 0x%lx is not a device tree\n", Dtb));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "MERLINXMEM: device tree at 0x%lx, %u bytes\n", Dtb, (UINT32)FdtTotalSize ((VOID *)(UINTN)Dtb)));

  PublishInitrd ((VOID *)(UINTN)Dtb);
  return EFI_SUCCESS;
}
