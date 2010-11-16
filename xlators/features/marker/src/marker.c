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

void
fini (xlator_t *this);

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
        int              ret          = -EFAULT;

        if ((!inode) || (!loc))
                return ret;

        if ((inode) && (inode->ino == 1)) {
                loc->parent = NULL;
                goto ignore_parent;
        }

        parent = inode_parent (inode, 0, NULL);
        if (!parent) {
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

int32_t
marker_loc_copy (loc_t *dst, loc_t *src)
{
        int ret = -1;

        dst->ino = src->ino;

        if (src->inode)
                dst->inode = inode_ref (src->inode);
        else
                dst->inode = NULL;

        if (src->parent)
                dst->parent = inode_ref (src->parent);
        else
                dst->inode = NULL;

        dst->path = gf_strdup (src->path);

        if (!dst->path)
                goto out;

        dst->name = strrchr (dst->path, '/');
        if (dst->name)
                dst->name++;

        ret = 0;
out:
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

        unlink (priv->timestamp_file);

        return 0;
}

int32_t
marker_free_local (marker_local_t *local)
{
        if (local->inode != NULL)
                inode_unref (local->inode);

        GF_FREE (local);

        return 0;
}

int32_t
stat_stampfile (xlator_t *this, marker_conf_t *priv, char **status)
{
        int32_t     ret;
        struct stat buf;

        GF_ASSERT (!priv->timestamp_file);

        if (stat (priv->timestamp_file, &buf) != -1) {
                ret = gf_asprintf (status, "1.0:%s:%u.%u",
                                  priv->volume_uuid, buf.st_ctime,
                                  ST_CTIM_NSEC (&buf)/1000);
                if (ret == -1)
                        goto err;
        } else {
                ret = gf_asprintf (status, "1.0:%s:FAILURE", priv->volume_uuid);

                if (ret == -1)
                        goto err;

                gf_log (this->name, GF_LOG_DEBUG, "volume mark value is %s", status);
        }

        return 0;
err:
        *status = NULL;

        return 1;
}

int32_t
marker_getxattr_stampfile_cbk (call_frame_t *frame, xlator_t *this,
                                const char *name, char *stampfile_status)
{
        int32_t   ret;
        dict_t   *dict = NULL;

        if (stampfile_status == NULL){
                STACK_UNWIND_STRICT (getxattr, frame, -1, ENOMEM, NULL);

                goto out;
        }

        dict = dict_new ();

        ret = dict_set_str (dict, (char *)name, stampfile_status);

        STACK_UNWIND_STRICT (getxattr, frame, 0, 0, dict);

        dict_unref (dict);
out:
        return 0;
}

int32_t
call_from_special_client (call_frame_t *frame, xlator_t *this, const char *name)
{
        char          *stampfile_status = NULL;
        marker_conf_t *priv             = NULL;
        gf_boolean_t   ret              = _gf_true;

        priv = (marker_conf_t *)this->private;

        if (frame->root->pid != -1 || name == NULL ||
            strcmp (name, MARKER_XATTR_PREFIX "." VOLUME_MARK) != 0) {
                ret = _gf_false;
                goto out;
        }

        stat_stampfile (this, priv, &stampfile_status);

        marker_getxattr_stampfile_cbk (frame, this, name, stampfile_status);
out:
        return ret;
}

int32_t
marker_getxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                      int32_t op_ret, int32_t op_errno, dict_t *dict)
{
        STACK_UNWIND_STRICT (getxattr, frame, op_ret, op_errno, dict);
        return 0;
}

int32_t
marker_getxattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
                 const char *name)
{
        gf_boolean_t ret;

        gf_log (this->name, GF_LOG_DEBUG, "USER:PID = %d", frame->root->pid);

        ret = call_from_special_client (frame, this, name);

        if (ret == _gf_false)
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
marker_specific_setxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                              int32_t op_ret, int32_t op_errno)
{
        int32_t         done = 0;
        marker_local_t *local = NULL;
        loc_t           loc;

        local = (marker_local_t*) frame->local;

        if (op_ret == -1 && op_errno == ENOSPC) {
                marker_error_handler (this);
                done = 1;
                goto out;
        }

        if (local->inode->ino == 1) {
                done = 1;
                goto out;
        }

        if (local->loc == NULL)
                marker_inode_loc_fill (local->inode, &loc);
        else
                marker_loc_copy (&loc, local->loc);

        marker_trav_parent (local, &loc);

        loc_wipe (&loc);

        MARKER_LOC_WIPE (local->loc);

        marker_start_setxattr (frame, this);

out:
        if (done) {
                marker_setxattr_done (frame);
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

        if (local->loc == NULL)
                marker_inode_loc_fill (local->inode, &loc);
        else
                marker_loc_copy (&loc, local->loc);

        gf_log (this->name, GF_LOG_DEBUG, "path = %s", loc.path);

        STACK_WIND (frame, marker_specific_setxattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->setxattr, &loc, dict, 0);

        loc_wipe (&loc);

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
update_marks (xlator_t *this, marker_local_t *local, int32_t ret)
{
        if (ret == -1 || local->pid < 0)
                marker_free_local (local);
        else {
                marker_gettimeofday (local);

                marker_create_frame (this, local);
        }

        return 0;
}

int32_t
marker_mkdir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, inode_t *inode,
                   struct iatt *buf, struct iatt *preparent,
                   struct iatt *postparent)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "error occurred "
                        "while Creating a file %s", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (mkdir, frame, op_ret, op_errno, inode,
                             buf, preparent, postparent);

        update_marks (this, local, ret);

        return 0;
}

int
marker_mkdir (call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
               dict_t *params)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->inode);

        ALLOCATE_OR_GOTO (local->loc, loc_t, err);

        marker_loc_copy (local->loc, loc);

        STACK_WIND (frame, marker_mkdir_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->mkdir, loc, mode, params);

        return 0;
err:
        STACK_UNWIND_STRICT (mkdir, frame, -1, ENOMEM, NULL,
                             NULL, NULL, NULL);
        return 0;
}

int32_t
marker_create_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, fd_t *fd, inode_t *inode,
                    struct iatt *buf, struct iatt *preparent,
                    struct iatt *postparent)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "error occurred "
                        "while Creating a file %s", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (create, frame, op_ret, op_errno, fd, inode, buf,
                             preparent, postparent);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_create (call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags,
                mode_t mode, fd_t *fd, dict_t *params)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->inode);

        ALLOCATE_OR_GOTO (local->loc, loc_t, err);

        marker_loc_copy (local->loc, loc);

        STACK_WIND (frame, marker_create_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->create, loc, flags, mode, fd,
                    params);
        return 0;
err:
        STACK_UNWIND_STRICT (create, frame, -1, ENOMEM, NULL, NULL, NULL, NULL, NULL);

        return 0;
}

int32_t
marker_writev_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
                   struct iatt *postbuf)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "error occurred "
                        "while write, %s", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (writev, frame, op_ret, op_errno, prebuf, postbuf);

        update_marks (this, local, ret);

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

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, fd->inode);

        STACK_WIND (frame, marker_writev_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->writev, fd, vector, count, offset,
                    iobref);
        return 0;
err:
        STACK_UNWIND_STRICT (writev, frame, -1, ENOMEM, NULL, NULL);

        return 0;
}

int32_t
marker_rmdir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, struct iatt *preparent,
                   struct iatt *postparent)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "error occurred "
                        "rmdir %s", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (rmdir, frame, op_ret, op_errno, preparent,
                             postparent);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_rmdir (call_frame_t *frame, xlator_t *this, loc_t *loc, int flags)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->parent);

        STACK_WIND (frame, marker_rmdir_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->rmdir, loc, flags);
        return 0;
err:
        STACK_UNWIND_STRICT (rmdir, frame, -1, ENOMEM, NULL, NULL);

        return 0;
}

int32_t
marker_unlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, struct iatt *preparent,
                    struct iatt *postparent)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR,
                        "%s occurred in unlink", strerror (op_errno));

                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (unlink, frame, op_ret, op_errno, preparent,
                             postparent);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_unlink (call_frame_t *frame, xlator_t *this, loc_t *loc)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->inode);

        ALLOCATE_OR_GOTO (local->loc, loc_t, err);

        marker_loc_copy (local->loc, loc);

        STACK_WIND (frame, marker_unlink_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->unlink, loc);
        return 0;
err:
        STACK_UNWIND_STRICT (unlink, frame, -1, ENOMEM, NULL, NULL);

        return 0;
}

int32_t
marker_link_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno, inode_t *inode,
                  struct iatt *buf, struct iatt *preparent,
                  struct iatt *postparent)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "linking a file ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (link, frame, op_ret, op_errno, inode, buf,
                             preparent, postparent);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_link (call_frame_t *frame, xlator_t *this, loc_t *oldloc, loc_t *newloc)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, oldloc->inode);

        ALLOCATE_OR_GOTO (local->loc, loc_t, err);

        marker_loc_copy (local->loc, newloc);

        STACK_WIND (frame, marker_link_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->link, oldloc, newloc);
        return 0;
err:
        STACK_UNWIND_STRICT (link, frame, -1, ENOMEM, NULL, NULL, NULL, NULL);

        return 0;
}

int32_t
marker_rename_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, struct iatt *buf,
                    struct iatt *preoldparent, struct iatt *postoldparent,
                    struct iatt *prenewparent, struct iatt *postnewparent)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "renaming a file ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (rename, frame, op_ret, op_errno, buf, preoldparent,
                             postoldparent, prenewparent, postnewparent);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_rename (call_frame_t *frame, xlator_t *this, loc_t *oldloc,
                loc_t *newloc)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, oldloc->inode);

        ALLOCATE_OR_GOTO (local->loc, loc_t, err);

        marker_loc_copy (local->loc, newloc);

        STACK_WIND (frame, marker_rename_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->rename, oldloc, newloc);
        return 0;
err:
        STACK_UNWIND_STRICT (rename, frame, -1, ENOMEM, NULL,
                                NULL, NULL, NULL, NULL);

        return 0;
}

int32_t
marker_truncate_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                      int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
                      struct iatt *postbuf)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "truncating a file ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (truncate, frame, op_ret, op_errno, prebuf,
                             postbuf);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_truncate (call_frame_t *frame, xlator_t *this, loc_t *loc, off_t offset)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->inode);

        STACK_WIND (frame, marker_truncate_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->truncate, loc, offset);
        return 0;
err:
        STACK_UNWIND_STRICT (truncate, frame, -1, ENOMEM, NULL, NULL);

        return 0;
}

int32_t
marker_ftruncate_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                       int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
                       struct iatt *postbuf)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "truncating a file ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (ftruncate, frame, op_ret, op_errno, prebuf,
                             postbuf);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_ftruncate (call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, fd->inode);

        STACK_WIND (frame, marker_ftruncate_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->ftruncate, fd, offset);
        return 0;
err:
        STACK_UNWIND_STRICT (ftruncate, frame, -1, ENOMEM, NULL, NULL);

        return 0;
}

int32_t
marker_symlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                     int32_t op_ret, int32_t op_errno, inode_t *inode,
                     struct iatt *buf, struct iatt *preparent,
                     struct iatt *postparent)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "creating symlinks ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (symlink, frame, op_ret, op_errno, inode, buf,
                             preparent, postparent);

        update_marks (this, local, ret);

        return 0;
}

int
marker_symlink (call_frame_t *frame, xlator_t *this, const char *linkpath,
                 loc_t *loc, dict_t *params)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->inode);

        ALLOCATE_OR_GOTO (local->loc, loc_t, err);

        marker_loc_copy (local->loc, loc);

        STACK_WIND (frame, marker_symlink_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->symlink, linkpath, loc, params);
        return 0;
err:
        STACK_UNWIND_STRICT (symlink, frame, -1, ENOMEM, NULL,
                                NULL, NULL, NULL);
        return 0;
}

int32_t
marker_mknod_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, inode_t *inode,
                   struct iatt *buf, struct iatt *preparent,
                   struct iatt *postparent)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "creating symlinks ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (mknod, frame, op_ret, op_errno, inode,
                             buf, preparent, postparent);

        update_marks (this, local, ret);

        return 0;
}

int
marker_mknod (call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
               dev_t rdev, dict_t *parms)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->inode);

        ALLOCATE_OR_GOTO (local->loc, loc_t, err);

        marker_loc_copy (local->loc, loc);

        STACK_WIND (frame, marker_mknod_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->mknod, loc, mode, rdev, parms);
        return 0;
err:
        STACK_UNWIND_STRICT (mknod, frame, -1, ENOMEM, NULL,
                             NULL, NULL, NULL);
        return 0;
}

int32_t
marker_setxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                      int32_t op_ret, int32_t op_errno)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "creating symlinks ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (setxattr, frame, op_ret, op_errno);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_setxattr (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *dict,
                  int32_t flags)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->inode);

        STACK_WIND (frame, marker_setxattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->setxattr, loc, dict, flags);
        return 0;
err:
        STACK_UNWIND_STRICT (setxattr, frame, -1, ENOMEM);

        return 0;
}

int32_t
marker_fsetxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                       int32_t op_ret, int32_t op_errno)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "creating symlinks ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (fsetxattr, frame, op_ret, op_errno);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_fsetxattr (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *dict,
                   int32_t flags)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, fd->inode);

        STACK_WIND (frame, marker_fsetxattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fsetxattr, fd, dict, flags);
        return 0;
err:
        STACK_UNWIND_STRICT (fsetxattr, frame, -1, ENOMEM);

        return 0;
}

int32_t
marker_fsetattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                      int32_t op_ret, int32_t op_errno, struct iatt *statpre,
                      struct iatt *statpost)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "creating symlinks ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (fsetattr, frame, op_ret, op_errno, statpre,
                             statpost);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_fsetattr (call_frame_t *frame, xlator_t *this, fd_t *fd,
                  struct iatt *stbuf, int32_t valid)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, fd->inode);

        STACK_WIND (frame, marker_fsetattr_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->fsetattr, fd, stbuf, valid);
        return 0;
err:
        STACK_UNWIND_STRICT (fsetattr, frame, -1, ENOMEM, NULL, NULL);

        return 0;
}

int32_t
marker_setattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                     int32_t op_ret, int32_t op_errno, struct iatt *statpre,
                     struct iatt *statpost)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "creating symlinks ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (setattr, frame, op_ret, op_errno, statpre,
                             statpost);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_setattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
                 struct iatt *stbuf, int32_t valid)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->inode);

        STACK_WIND (frame, marker_setattr_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->setattr, loc, stbuf, valid);
        return 0;
err:
        STACK_UNWIND_STRICT (setattr, frame, -1, ENOMEM, NULL, NULL);

        return 0;
}

int32_t
marker_removexattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                         int32_t op_ret, int32_t op_errno)
{
        int32_t             ret     = 0;
        marker_local_t     *local   = NULL;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s occured while "
                        "creating symlinks ", strerror (op_errno));
                ret = -1;
        }

        local = (marker_local_t *) frame->local;

        frame->local = NULL;

        STACK_UNWIND_STRICT (removexattr, frame, op_ret, op_errno);

        update_marks (this, local, ret);

        return 0;
}

int32_t
marker_removexattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
                     const char *name)
{
        marker_local_t  *local = NULL;

        ALLOCATE_OR_GOTO (local, marker_local_t, err);

        MARKER_INIT_LOCAL (frame, local, loc->inode);

        STACK_WIND (frame, marker_removexattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->removexattr, loc, name);
        return 0;
err:
        STACK_UNWIND_STRICT (removexattr, frame, -1, ENOMEM);

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
init (xlator_t *this)
{
        dict_t        *options = NULL;
        data_t        *data    = NULL;
        int32_t        ret     = 0;
        marker_conf_t *priv    = NULL;

        if (!this->children) {
                gf_log (this->name, GF_LOG_ERROR,
                        "marker translator needs subvolume defined.");
                return -1;
        }

        if (!this->parents) {
                gf_log (this->name, GF_LOG_WARNING,
                        "Volume is dangling.");
                return -1;
        }

        options = this->options;

        ALLOCATE_OR_GOTO (this->private, marker_conf_t, err);

        priv = this->private;

        if( (data = dict_get (options, VOLUME_UUID)) != NULL) {
                priv->volume_uuid = data->data;

                ret = gf_asprintf (& (priv->marker_xattr), "%s.%s.%s",
                                   MARKER_XATTR_PREFIX, priv->volume_uuid, XTIME);

                if (ret == -1){
                        priv->marker_xattr = NULL;

                        gf_log (this->name, GF_LOG_ERROR,
                                "Failed to allocate memory");
                        goto err;
                }

                gf_log (this->name, GF_LOG_DEBUG,
                        "the volume-uuid = %s", priv->volume_uuid);
        } else {
                priv->volume_uuid = NULL;

                gf_log (this->name, GF_LOG_ERROR,
                        "please specify the volume-uuid"
                        "in the translator options");

                return -1;
        }

        if ((data = dict_get (options, TIMESTAMP_FILE)) != NULL) {
                priv->timestamp_file = data->data;

                gf_log (this->name, GF_LOG_DEBUG,
                        "the timestamp-file is = %s",
                        priv->timestamp_file);

        } else {
                priv->timestamp_file = NULL;

                gf_log (this->name, GF_LOG_ERROR,
                        "please specify the timestamp-file"
                        "in the translator options");

                goto err;
        }

        return 0;
err:
        fini (this);

        return -1;
}

void
fini (xlator_t *this)
{
        marker_conf_t *priv = NULL;

        priv = (marker_conf_t *) this->private;

        if (priv == NULL)
                goto out;

        if (priv->volume_uuid != NULL)
                GF_FREE (priv->volume_uuid);

        if (priv->timestamp_file != NULL)
                GF_FREE (priv->timestamp_file);

        if (priv->marker_xattr != NULL)
                GF_FREE (priv->marker_xattr);

        GF_FREE (priv);
out:
        return ;
}

struct xlator_fops fops = {
        .create      = marker_create,
        .unlink      = marker_unlink,
        .link        = marker_link,
        .mkdir       = marker_mkdir,
        .rmdir       = marker_rmdir,
        .writev      = marker_writev,
        .rename      = marker_rename,
        .truncate    = marker_truncate,
        .ftruncate   = marker_ftruncate,
        .symlink     = marker_symlink,
        .mknod       = marker_mknod,
        .setxattr    = marker_setxattr,
        .fsetxattr   = marker_fsetxattr,
        .setattr     = marker_setattr,
        .fsetattr    = marker_fsetattr,
        .removexattr = marker_removexattr,
        .getxattr    = marker_getxattr
};

struct xlator_cbks cbks = {
};

struct volume_options options[] = {
        {.key = {"volume-uuid"}},
        {.key = {"timestamp-file"}},
        {.key = {NULL}}
};
