/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The flash of the board: the device the update code writes (flash.h) and the layout of what
 * it holds, both the platform's.
 *
 * THE LAYOUT is a table of the platform (plat_flash_layout()): the device's geometry, the
 * sectors of the boot control metadata, and the two slots, each of which holds one image (a
 * header and the payload after it, lib/image). The metadata lie in sectors of their own
 * before the slots, so that an erase of a slot never touches them. Every area starts at a
 * sector and is a whole number of sectors. The update code takes the areas from the table and
 * never computes an address of its own, so a target with another part has only its own table:
 * the host's is below, the Cortex-M3 build of phase 7 will have its own.
 *
 * THE HOST (platform/posix/posix_flash.c): 2 MiB and 8 KiB, the geometry of a 3 V serial NOR
 * (Winbond W25Q128JV: 4 KiB sectors, 256-byte pages) with 8-byte program units, the unit of a
 * flash with an error-correcting code per double word, so that the write-once rule of flash.h
 * is checked at the size a target would need:
 *
 *     area      offset      size        sectors
 *     meta      0x000000    0x002000    0 - 1       two sectors, used in turn by lib/boot
 *     slot A    0x002000    0x100000    2 - 257     1 MiB
 *     slot B    0x102000    0x100000    258 - 513   1 MiB
 *
 * A slot of 1 MiB holds an image of the release and of the debug build of the node, stripped,
 * after the 256-byte image header (tests/system/test_flash_file.py checks the release build).
 * Since phase 5 the unstripped release build, its debug information included, is larger; an
 * image never carries symbols.
 * The AddressSanitizer build is larger than a slot and is never an image; docs/update.md has
 * the sizes. The device is a file the node maps (`--flash-file`); docs/update.md has the rest.
 *
 * THE DEVICE (plat_flash_device()) follows every rule of flash.h. An operation blocks the
 * calling task for the time the modelled part takes (a program 0.4 ms, a sector erase 45 ms
 * on the host), so it is called from a task that may wait that long: on the node the one task
 * that writes the flash, at the lowest priority. A loss of supply during an operation (on the
 * host, a cut armed with `--flash-cut` or the simulation command flash.cut) ends the run as a
 * power loss: the call does not return, and the next start finds what the cut left.
 */
#ifndef FCN_PLAT_FLASH_H
#define FCN_PLAT_FLASH_H

#include <stdint.h>

#include "flash/flash.h"

/** The slots of the layout: slot 0 is A, slot 1 is B. */
#define PLAT_FLASH_SLOTS 2U

/** A part of the device: whole sectors from `offset` on. */
typedef struct {
    uint32_t offset; /**< bytes from the start of the device, a multiple of the sector size */
    uint32_t size;   /**< bytes, a multiple of the sector size, not 0 */
} plat_flash_area_t;

/** The platform's flash layout. Constant. */
typedef struct {
    flash_geometry_t geometry;                /**< of the whole device */
    plat_flash_area_t meta;                   /**< the boot control metadata */
    plat_flash_area_t slot[PLAT_FLASH_SLOTS]; /**< the image slots, A and B */
    /** The target the images in the slots are built for, lib/image's IMAGE_TARGET_*: the node's
     *  slot writer accepts a downloaded image only for it (lib/bootimg), as the bootloader starts
     *  only such images. The host: IMAGE_TARGET_HOST. */
    uint32_t image_target;
} plat_flash_layout_t;

/** The layout of this platform's flash; valid whether or not this run has a flash device.
 *  Any context. */
const plat_flash_layout_t *plat_flash_layout(void);

/**
 * The flash device of this run, with the geometry of plat_flash_layout(), or NULL when the
 * run has none (on the host: no `--flash-file`). Constant while the image runs. Its operations
 * may only be called by one task at a time and never from an interrupt handler (flash.h).
 */
const flash_dev_t *plat_flash_device(void);

#endif /* FCN_PLAT_FLASH_H */
