/**
  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/IoLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PlatformSecLib.h>

#include "PlatformRegisters.h"

VOID
DisableWatchDogTimer ()
{
  // Disable the AP watchdog directly. The memory map lookup is not reliable
  // this early in SEC, and the watchdog must be stopped before any code that
  // can block for more than its timeout.
  MmioWrite32 (WDT_BASE, WDT_MODE_KEY);
}

VOID
EnableConstantBlending ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR OvlRegion;

  // Locate "Display OVL" Memory Region
  Status = LocateMemoryRegionByName ("Display OVL", &OvlRegion);
  if (EFI_ERROR (Status)) {
    return;
  }

  // Enable Constant Blending
  MmioOr32 (OvlRegion.Address + OVL_PITCH_OFFSET (0), OVL_CONST_BLEND);
}

VOID
PlatformInitialize ()
{
  // Disable WatchDog Timer
  DisableWatchDogTimer ();

  // Enable Constant Blending
  EnableConstantBlending ();
}
