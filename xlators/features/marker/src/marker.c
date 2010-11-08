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
#include "marker.h"
#include "marker-mem-types.h"

int32_t
marker_start_setxattr (call_frame_t *, xlator_t *);

int
marker_loc_fill (loc_t *loc, inode_t *inode, inode_t *parent, char *path)
{
        int     ret = -EFAULT;

        if (!loc)
                return ret;

        if (inode) {
                loc->inode = inode_ref (inode);
                loc->ino = inode->ino;
        }

        if (parent)
                loc->parent = inode_ref (parent);

        loc->path = gf_strdup (path);
        if (!loc->path) {
                gf_log ("loc fill", GF_LOG_ERROR, "strdup failed");
                goto loc_wipe;
        }

        loc->name = strrchr (loc->path, '/');
        if (loc->name)
                loc->name++;
        else
                goto loc_wipe;

        ret = 0;
loc_wipe:
        if (ret < 0)
                loc_wipe (loc);

        return ret;
}

int
marker_inode_loc_fill (inode_t *inode, loc_t *loc)
{
        char            *resolvedpath = NULL;
        inode_t         *parent       = NULL;
        int              ret               = -EFAULT;

        if ((!inode) || (!loc))
                return ret;

        if ((inode) && (inode->ino == 1)){
                loc->parent = NULL;
                goto ignore_parent;
        }

        parent = inode_parent (inode, 0, NULL);
        if (!parent){
                goto err;
        }

ignore_parent:
        ret = inode_path (inode, NULL, &resolvedpath);
        if (ret < 0)
                goto err;

        ret = marker_loc_fill (loc, inode, parent, resolvedpath);
        if (ret < 0)
                goto err;

err:
        if (parent)
                inode_unref (parent);

        if (resolvedpath)
                GF_FREE (resolvedpath);

        return ret;
}

char *
create_dict_key (marker_conf_t *priv)
{
        char *key = NULL;

        key = (char *) GF_MALLOC (strlen (priv->marker_xattr) + 1, gf_common_mt_char);

        strcpy (key, priv->marker_xattr);

        return key;
}

int32_t
marker_trav_parent (marker_local_t *local,loc_t *loc)
{
        inode_t *inode = NULL;

        inode = local->inode;

        if (local->inode->ino == 1)
                local->inode = inode_ref (local->inode);
        else
                local->inode = inode_ref (loc->parent);

        inode_unref (inode);

        return 0;
}

int32_t
marker_error_handler (xlator_t *this)
{
        marker_conf_t        *priv = NULL;

        priv = (marker_conf_t *) this->private;

        priv->timestamp_file = NULL;

        return 0;
}

int32_t
marker_free_local (marker_local_t *local)
{
        inode_unref (local->inode);

        GF_FREE (local);

        return 0;
}

int32_t
stat_stampfile (xlator_t *this, marker_conf_t *priv, char **status)
{
        int32_t     ret;
        struct stat buf;

        if (priv->timestamp_file != NULL){
                if (stat (priv->timestamp_file, &buf) == -1)
                        gf_log (this->name, GF_LOG_ERROR, "Cant stat on timestamp-file");
                else{
                        ret = gf_asprintf (status, "%s:%u.%u",
                                          priv->volume_uuid, htonl (buf.st_ctime),
                                          htonl ( ST_CTIM_NSEC (&buf)/1000));

                        gf_log (this->name, GF_LOG_DEBUG, "volume mark value is %s", status);
                }
        } else {
                ret = gf_asprintf (status, "%s:FAILURE", priv->volume_uuid);

                gf_log (this->name, GF_LOG_DEBUG, "volume mark value is %s", status);
        }

        return 0;
}

int32_t
marker_getxattr_stampfile_cbk (call_frame_t *frame, xlator_t *this,
                                const char *name, char *stampfile_status)
{
        int32_t   ret;
        dict_t   *dict = NULL;

        dict = get_new_dict ();

        ret = dict_set_str (dict, (char *)name, stampfile_status);
        gf_log (this->name, GF_LOG_INFO, "USER:unwinding");

        STACK_UNWIND_STRICT (getxattr, frame, 0, 0, dict);

        dict_destroy (dict);

        return 0;
}

int32_t
check_user (call_frame_t *frame, xlator_t *this, const char *name)
{
        int32_t              ret              = 0;
        char                *stampfile_status = NULL;
        marker_conf_t       *priv             = NULL;

        priv = (marker_conf_t *)this->private;

        if (frame->root->pid != -1){        //fop not initiated by geosyn
                ret = -1;
                goto out;
        }

        if (name && strcmp (name, priv->volume_mark) == 0)
                stat_stampfile(this, priv, &stampfile_status);
        else {
                ret = -1;
                goto out;
        }

        marker_getxattr_stampfile_cbk (frame, this, name, stampfile_status);

out:        return ret;
}

int32_t
marker_getxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                      int32_t op_ret, int32_t op_errno, dict_t *dict)
{
        gf_log (this->name, GF_LOG_INFO, "USER: UNWINDING");

        STACK_UNWIND_STRICT (getxattr, frame, op_ret, op_errno, dict);
        return 0;
}

int32_t
marker_getxattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
                  const char *name)
{
        int32_t        ret;

        gf_log (this->name, GF_LOG_INFO, "USER:PID = %d", frame->root->pid);

        ret = check_user (frame, this, name);

        if (ret != 0)
                STACK_WIND (frame, marker_getxattr_cbk, FIRST_CHILD(this),
                                FIRST_CHILD(this)->fops->getxattr, loc, name);

        return 0;
}


int32_t
marker_setxattr_done (call_frame_t *frame)
{
        marker_local_t        *local = NULL;

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_DESTROY (frame->root);

        marker_free_local (local);

        return 0;
}

int
marker_setxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                      int32_t op_ret, int32_t op_errno)
{
        int32_t         done = 0;
        marker_local_t *local = NULL;
        loc_t           loc;

        local = (marker_local_t*) frame->local;

        if (op_ret == -1){
                gf_log (this->name, GF_LOG_ERROR,
                        "op_ret error %s", strerror (op_errno));

                if (op_errno == ENOSPC){
                        marker_error_handler (this);
                }
                done = 1;
                goto out;
        }

        if (local->inode->ino == 1){
                done = 1;
                goto out;
        }

        marker_inode_loc_fill (local->inode, &loc);

        marker_trav_parent (local, &loc);

        loc_wipe (&loc);

        marker_start_setxattr (frame, this);

out:    if (done){
                marker_setxattr_done (frame);//free frame
        }

        return 0;
}

int32_t
marker_start_setxattr (call_frame_t *frame, xlator_t *this)
{
        char            *key   = NULL;
        int32_t          ret;
        loc_t            loc;
        dict_t          *dict  = NULL;
        marker_local_t  *local = NULL;
        marker_conf_t   *priv  = NULL;

        priv = this->private;

        local = (marker_local_t*) frame->local;

        key = create_dict_key (priv);

        dict = dict_new ();

        ret = dict_set_static_bin (dict, key, (void *)local->timebuf, 8);

        marker_inode_loc_fill (local->inode, &loc);

        gf_log (this->name, GF_LOG_INFO, "path = %s", loc.path);

        STACK_WIND (frame, marker_setxattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->setxattr, &loc, dict, 0);

        loc_wipe (&loc); //when to free dict

        dict_unref (dict);

        return 0;
}

void
marker_gettimeofday (marker_local_t *local)
{
        struct timeval tv;

        gettimeofday (&tv, NULL);

        local->timebuf [0] = htonl (tv.tv_sec);
        local->timebuf [1] = htonl (tv.tv_usec);

        return;
}

int32_t
marker_create_frame (xlator_t *this, marker_local_t *local)
{
        call_frame_t *frame = NULL;

        frame = create_frame (this, this->ctx->pool);

        frame->local = (void *) local;

        marker_start_setxattr (frame, this);

        return 0;
}

int32_t
marker_writev_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
                    struct iatt *postbuf)
{
        int32_t             ret     = 0;
        pid_t               pid;
        marker_local_t     *local   = NULL;
        marker_conf_t      *priv    = NULL;

        priv = (marker_conf_t *)this->private;

        local = (marker_local_t *) frame->local;

        if (op_ret == -1){
                gf_log (this->name, GF_LOG_ERROR, "error occurred "
                        "while write, %s", strerror (op_errno));
                ret = -1;
        }

        pid = frame->root->pid;

        frame->local = NULL;

        STACK_UNWIND_STRICT (writev, frame, op_ret, op_errno, prebuf, postbuf);

        if (ret == -1 || pid == -1){
                marker_free_local (local);
        } else {

                marker_gettimeofday (local);

                marker_create_frame (this, local);
        }

        return 0;
}

int32_t
marker_writev (call_frame_t *frame,
                        xlator_t *this,
                        fd_t *fd,
                        struct iovec *vector,
                        int32_t count,
                        off_t offset,
                        struct iobref *iobref)
{
        marker_local_t  *local = NULL;

        local = (marker_local_t *) GF_MALLOC (sizeof (*local),
                                              gf_marker_mt_marker_local);

        if (!local)
                gf_log (this->name, GF_LOG_ERROR, "cant allocate memory");

        local->inode = inode_ref (fd->inode);

        frame->local = (void *) local;

        STACK_WIND (frame, marker_writev_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->writev, fd, vector, count, offset,
                    iobref);
        return 0;
}

int32_t
mem_acct_init (xlator_t *this)
{
        int     ret = -1;

        if (!this)
                return ret;

        ret = xlator_mem_acct_init (this, gf_marker_mt_end + 1);

        if (ret != 0) {
                gf_log(this->name, GF_LOG_ERROR, "Memory accounting init"
                                "failed");
                return ret;
        }

        return ret;
}

int32_t
init(xlator_t *this)
{
        dict_t *options = NULL;
        data_t *data    = NULL;
        int32_t ret        = 0;
        marker_conf_t *priv = NULL;

        options = this->options;

        priv = (marker_conf_t *) GF_MALLOC (sizeof (*priv), gf_marker_mt_marker_priv);

        if( (data = dict_get (options, "volume-uuid")) != NULL){

                priv->volume_uuid = data->data;

                ret = gf_asprintf (& (priv->marker_xattr),
                                "trusted.glusterfs.%s.xtime", priv->volume_uuid);

                gf_log (this->name, GF_LOG_DEBUG,
                        "the volume-uuid = %s", priv->volume_uuid);
        } else {
                gf_log (this->name, GF_LOG_ERROR,
                        "the volume-uuid is not found");
        }

        if ((data = dict_get (options, "timestamp-file")) != NULL){
                priv->timestamp_file = data->data;

                gf_log (this->name, GF_LOG_DEBUG, "the timestamp-file is = %s", priv->timestamp_file);

        } else {
                gf_log (this->name, GF_LOG_INFO,
                        "the timestamp-file is not found");
        }

        ret = gf_asprintf (&priv->volume_mark, "trusted.glusterfs.volume-mark");

        this->private = (void *) priv;

        gf_log (this->name, GF_LOG_INFO, "hi");

        return 0;
}

void
fini (xlator_t *this)
{

        return ;
}

struct xlator_fops fops = {
        //.create = marker_create
        .writev   = marker_writev,
        .getxattr = marker_getxattr
};

struct xlator_cbks cbks = {
};

struct volume_options options[] = {
        {.key = {"volume-uuid"}},
        {.key = {"timestamp-file"}},
        {.key = {NULL}}
};
