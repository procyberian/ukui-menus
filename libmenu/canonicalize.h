/* Return the canonical absolute name of a given file.
 * Copyright (C) 1996-2001, 2002 Free Software Foundation, Inc.
 * This file is part of the GNU C Library.
 *
 * Copyright (C) 2002 Red Hat, Inc. (trivial port to GLib)
 *
 * The GNU C Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The GNU C Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#ifndef G_CANONICALIZE_H
#define G_CANONICALIZE_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

char* menu_canonicalize_file_name(const char* name, gboolean allow_missing_basename);

#ifdef __cplusplus
}
#endif

#endif /* G_CANONICALIZE_H */
