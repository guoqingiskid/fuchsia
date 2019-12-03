// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_BOARD_X86_INCLUDE_NHLT_H_
#define ZIRCON_SYSTEM_DEV_BOARD_X86_INCLUDE_NHLT_H_

#include <acpica/acpi.h>
#include <ddk/driver.h>

__BEGIN_CDECLS

// Look for NHLT blob in the device pointed to by object and publish
// it as metadata on the PCI device.
// @param dev sys device pointer
// @param bbn base bus number of the PCI root the device is on
// @param adr ADR value for the device
// @param object handle to the device
zx_status_t nhlt_publish_metadata(zx_device_t* dev, uint8_t bbn, uint64_t adr, ACPI_HANDLE object);

__END_CDECLS

#endif  // ZIRCON_SYSTEM_DEV_BOARD_X86_INCLUDE_NHLT_H_
