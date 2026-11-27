// SPDX-License-Identifier: GPL-2.0-only
/*
 * haydn diag: UBSAN array-bounds handler local to camera.ko, so an
 * out-of-bounds index is printed with its file, line and value instead of
 * trapping (the GKI vmlinux does not export the generic handler).
 */
#include <linux/kernel.h>
#include <linux/printk.h>
#include "../../../../lib/ubsan.h"

void __ubsan_handle_out_of_bounds(void *_data, void *index);

void __ubsan_handle_out_of_bounds(void *_data, void *index)
{
	struct out_of_bounds_data *data = _data;

	if (test_and_set_bit(31, &data->location.reported))
		return;
	pr_err("UBSAN camera: array-index-out-of-bounds in %s:%u:%u, index %ld (0x%lx), array type %s\n",
	       data->location.file_name, data->location.line & ~(1U << 31),
	       data->location.column, (long)index, (unsigned long)index,
	       data->array_type->type_name);
	dump_stack();
}
