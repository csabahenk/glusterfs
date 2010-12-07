#ifndef _LIBXLATOR_H
#define _LIBXLATOR_H


#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "xlator.h"
#include "logging.h"
#include "defaults.h"
#include "common-utils.h"
#include "compat.h"
#include "compat-errno.h"


#define MARKER_XATTR_PREFIX "trusted.glusterfs"
#define XTIME               "xtime"
#define VOLUME_MARK         "volume-mark"
#define GF_XATTR_MARKER_KEY MARKER_XATTR_PREFIX "." VOLUME_MARK
#define UUID_SIZE 36
#define VMARK_HSIZE (1 + 1 + 16)           /* major + minor + uuid */
#define VMARK_SIZE (VMARK_HSIZE + 1 + 4 + 4)   /* retval + sec + usec */

struct marker_str {
        char vmark[VMARK_SIZE];

        uint32_t             host_timebuf[2];
        uint32_t             net_timebuf[2];
        int32_t              call_count;
        unsigned             has_xtime:1;
};

static inline int
marker_has_volinfo (struct marker_str *marker)
{
        return (int)marker->vmark[0];
}

typedef int32_t (*mark_getxattr_cbk_t) (call_frame_t *frame, void *cookie,
                                        xlator_t *this, int op_ret,
                                        int op_errno, dict_t *dict);

void
cluster_markerxtime_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                        int op_ret, int op_errno, dict_t *dict,
                        struct marker_str *local, char *vol_uuid);

void
cluster_markeruuid_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                        int op_ret, int op_errno, dict_t *dict,
                        struct marker_str *marker, char *vol_uuid);

int32_t
cluster_getmarkerattr (call_frame_t *frame,xlator_t *this, loc_t *loc,
                       const char *name, struct marker_str *marker, char *vol,
                       mark_getxattr_cbk_t cluster_getmarkerattr_cbk);

int
match_uuid_local (const char *name, char *uuid);




#endif /* !_LIBXLATOR_H */
