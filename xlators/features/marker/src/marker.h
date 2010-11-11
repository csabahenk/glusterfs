/*Copyright (c) 2008-2010 Gluster, Inc. <http://www.gluster.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "xlator.h"
#include "defaults.h"

#define MARKER_XATTR_PREFIX "trusted.glusterfs"
#define XTIME               "xtime"
#define VOLUME_MARK         "volume-mark"
#define VOLUME_UUID         "volume-uuid"
#define TIMESTAMP_FILE      "timestamp-file"

/*initialize the local variable*/
#define INIT_LOCAL(_frame, _local, _inode) do {                 \
                _frame->local = _local;                         \
                _local->inode = inode_ref (_inode);             \
                _local->pid = frame->root->pid;                 \
        } while (0)

/* try alloc and if it fails, goto label */
#define ALLOCATE_OR_GOTO(var, type, label) do {                  \
                var = GF_CALLOC (sizeof (type), 1,               \
                                 gf_marker_mt_##type);           \
                if (!var) {                                      \
                        gf_log (this->name, GF_LOG_ERROR,        \
                                "out of memory :(");             \
                        goto label;                              \
                }                                                \
        } while (0)

struct marker_local{
        uint32_t        timebuf[2];
        inode_t        *inode;
        pid_t           pid;
};
typedef struct marker_local marker_local_t;

struct marker_conf{
        char        *volume_uuid;
        char        *timestamp_file;
        char        *marker_xattr;
        char        *volume_mark;
};

typedef struct marker_conf marker_conf_t;
