/*
  Copyright (c) 2009-2010 Z RESEARCH, Inc. <http://www.zresearch.com>
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

#include "quick-read.h"

        
int32_t
qr_lookup_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, inode_t *inode,
               struct stat *buf, dict_t *dict)
{
        data_t    *content = NULL;
        qr_file_t *qr_file = NULL;
        uint64_t   value = 0;
        int        ret = -1;
        qr_conf_t *conf = NULL;

        if ((op_ret == -1) || (dict == NULL)) {
                goto out;
        }

        conf = this->private;

        content = dict_get (dict, GLUSTERFS_CONTENT_KEY);
        if (content == NULL) {
                goto out;
        }

        if (buf->st_size > conf->max_file_size) {
                goto out;
        }

        if (S_ISDIR (buf->st_mode)) {
                goto out;
        }

        ret = inode_ctx_get (inode, this, &value);
        if (ret == -1) {
                qr_file = CALLOC (1, sizeof (*qr_file));
                if (qr_file == NULL) {
                        op_ret = -1;
                        op_errno = ENOMEM;
                        goto out;
                }
                
                LOCK_INIT (&qr_file->lock);
                inode_ctx_put (inode, this, (uint64_t)(long)qr_file);
        } else {
                qr_file = (qr_file_t *)(long)value;
                if (qr_file == NULL) {
                        op_ret = -1;
                        op_errno = EINVAL;
                        goto out;
                }
        }

        LOCK (&qr_file->lock);
        {
                if (qr_file->xattr) {
                        dict_unref (qr_file->xattr);
                        qr_file->xattr = NULL;
                }

                qr_file->xattr = dict_ref (dict);
                qr_file->stbuf = *buf;
                gettimeofday (&qr_file->tv, NULL);
        }
        UNLOCK (&qr_file->lock);

out:
	STACK_UNWIND (frame, op_ret, op_errno, inode, buf, dict);
        return 0;
}


int32_t
qr_lookup (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xattr_req)
{
        qr_conf_t *conf = NULL;
        dict_t    *new_req_dict = NULL;
        int32_t    op_ret = -1, op_errno = -1;
        data_t    *content = NULL; 
        uint64_t   requested_size = 0, size = 0; 

        conf = this->private;
        if (conf == NULL) {
                op_ret = -1;
                op_errno = EINVAL;
                goto unwind;
        }

        if ((xattr_req == NULL) && (conf->max_file_size > 0)) {
                new_req_dict = xattr_req = dict_new ();
                if (xattr_req == NULL) {
                        op_ret = -1;
                        op_errno = ENOMEM;
                        gf_log (this->name, GF_LOG_ERROR, "out of memory");
                        goto unwind;
                }
        }

        if (xattr_req) {
                content = dict_get (xattr_req, GLUSTERFS_CONTENT_KEY);
                if (content) {
                        requested_size = data_to_uint64 (content);
                }
        }

        if (((conf->max_file_size > 0) && (content == NULL))
            || (conf->max_file_size != requested_size)) {
                size = (conf->max_file_size > requested_size) ?
                        conf->max_file_size : requested_size;

                op_ret = dict_set (xattr_req, GLUSTERFS_CONTENT_KEY,
                                   data_from_uint64 (size));
                if (op_ret < 0) {
                        op_ret = -1;
                        op_errno = ENOMEM;
                        goto unwind;
                }
        }

	STACK_WIND (frame, qr_lookup_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->lookup, loc, xattr_req);

        if (new_req_dict) {
                dict_unref (new_req_dict);
        }

        return 0;

unwind:
        STACK_UNWIND (frame, op_ret, op_errno, NULL, NULL, NULL);

        if (new_req_dict) {
                dict_unref (new_req_dict);
        }

        return 0;
}


int32_t 
init (xlator_t *this)
{
	char      *str = NULL;
        int32_t    ret = -1;
        qr_conf_t *conf = NULL;
 
        if (!this->children || this->children->next) {
                gf_log (this->name, GF_LOG_ERROR,
                        "FATAL: volume (%s) not configured with exactly one "
			"child", this->name);
                return -1;
        }

	if (!this->parents) {
		gf_log (this->name, GF_LOG_WARNING,
			"dangling volume. check volfile ");
	}

        conf = CALLOC (1, sizeof (*conf));
        if (conf == NULL) {
                gf_log (this->name, GF_LOG_ERROR,
                        "out of memory");
                ret = -1;
                goto out;
        }

        ret = dict_get_str (this->options, "max-file-size", 
                            &str);
        if (ret == 0) {
                ret = gf_string2bytesize (str, &conf->max_file_size);
                if (ret != 0) {
                        gf_log (this->name, GF_LOG_ERROR, 
                                "invalid number format \"%s\" of \"option "
                                "max-file-size\"", 
                                str);
                        ret = -1;
                        goto out;
                }
        }

        conf->cache_timeout = -1;
        ret = dict_get_str (this->options, "cache-timeout", &str);
        if (ret == 0) {
                ret = gf_string2uint_base10 (str, 
                                             (unsigned int *)&conf->cache_timeout);
                if (ret != 0) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "invalid cache-timeout value %s", str);
                        ret = -1;
                        goto out;
                } 
        }

        this->private = conf;
out:
        if ((ret == -1) && conf) {
                FREE (conf);
        }

        return ret;
}


void
fini (xlator_t *this)
{
        return;
}


struct xlator_fops fops = {
	.lookup      = qr_lookup,
};


struct xlator_mops mops = {
};


struct xlator_cbks cbks = {
};

struct volume_options options[] = {
        { .key  = {"cache-timeout"}, 
          .type = GF_OPTION_TYPE_INT,
          .min = 1,
          .max = 60
        },
        { .key  = {"max-file-size"}, 
          .type = GF_OPTION_TYPE_SIZET, 
          .min  = 0,
          .max  = 1 * GF_UNIT_MB 
        },
};
