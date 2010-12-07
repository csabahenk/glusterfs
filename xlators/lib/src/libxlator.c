#include "libxlator.h"

/*Copy the contents of oldtimebuf to newtimbuf*/
static void
update_timebuf (uint32_t *oldtimbuf, uint32_t *newtimebuf)
{
        newtimebuf[0] =  (oldtimbuf[0]);
        newtimebuf[1] =  (oldtimbuf[1]);
}

/* Convert Timebuf in network order to host order */
static void
get_hosttime (uint32_t *oldtimbuf, uint32_t *newtimebuf)
{
        newtimebuf[0] = ntohl (oldtimbuf[0]);
        newtimebuf[1] = ntohl (oldtimbuf[1]);
}



/* Match the Incoming trusted.glusterfs.<uuid>.xtime against volume uuid */
int
match_uuid_local (const char *name, char *uuid)
{
        name = strtail ((char *)name, MARKER_XATTR_PREFIX);
        if (!name || name++[0] != '.')
                return -1;

        name = strtail ((char *)name, uuid);
        if (!name || strcmp (name, ".xtime") != 0)
                return -1;

        return 0;
}




/* Aggregate all the <volid>.xtime attrs of the cluster and send the max*/
void
cluster_markerxtime_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                        int op_ret, int op_errno, dict_t *dict,
                        struct marker_str *local, char *vol_uuid)
{

        int32_t         callcnt = 0;
        int             ret = -1;
        uint32_t       *net_timebuf;
        uint32_t        host_timebuf[2];
        char           *marker_xattr;

        if (!this || !frame || !frame->local || !cookie || !local) {
                gf_log ("stripe", GF_LOG_DEBUG, "possible NULL deref");
                goto out;
        }

        LOCK (&frame->lock);
        {
                callcnt = --local->call_count;
                if (!gf_asprintf (& marker_xattr, "%s.%s.%s",
                                MARKER_XATTR_PREFIX, vol_uuid, XTIME)) {
                        op_errno = ENOMEM;
                        goto out;
                }


                if (dict_get_ptr (dict, marker_xattr, (void **)&net_timebuf)) {
                        gf_log (this->name, GF_LOG_DEBUG,
                                "Unable to get <uuid>.xtime attr");
                        /*Some Children Left for aggregation*/
                        if (callcnt) {
                                UNLOCK (&frame->lock);
                                return ;
                        }
                        else {
                                /*Last Child*/
                                if (local->has_xtime) {
                                        if (!dict)
                                                dict = dict_new();
                                        ret = dict_set_static_bin (dict, marker_xattr, (void *)local->net_timebuf, 8);
                                        if (ret) {
                                                op_errno = ENOMEM;
                                                goto unlock;
                                        }
                                }
                                STACK_UNWIND_STRICT (getxattr, frame, 0, 0, dict);
                                return;
                        }
                }

                if (local->has_xtime) {

                        get_hosttime (net_timebuf, host_timebuf);
                        if ( (host_timebuf[0]>local->host_timebuf[0]) ||
                                (host_timebuf[0] == local->host_timebuf[0] &&
                                 host_timebuf[1] >= local->host_timebuf[1])) {

                                update_timebuf (net_timebuf, local->net_timebuf);
                                update_timebuf (host_timebuf, local->host_timebuf);

                        }

                }
                else {
                        get_hosttime (net_timebuf, local->host_timebuf);
                        update_timebuf (net_timebuf, local->net_timebuf);
                        local->has_xtime = _gf_true;
                }

        }
        UNLOCK (&frame->lock);
        if (!callcnt) {
                ret = dict_set_static_bin (dict, marker_xattr, (void *)local->net_timebuf, 8);
                if (ret) {
                        op_errno = ENOMEM;
                        goto unlock;
                }

                STACK_UNWIND_STRICT (getxattr, frame, op_ret, op_errno, dict);
                return ;
        }
        return ;

unlock:
        UNLOCK (&frame->lock);


out:
        STACK_UNWIND_STRICT (getxattr, frame, -1, ENOENT, NULL);
        return ;

}

void
cluster_markeruuid_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                        int op_ret, int op_errno, dict_t *dict,
                        struct marker_str *marker, char *vol_uuid)
{
        int32_t         callcnt = 0;
        data_t         *data = NULL;

        if (!this || !frame || !cookie || !marker) {
                gf_log ("stripe", GF_LOG_DEBUG, "possible NULL deref");
                goto out;
        }

        if (op_ret || !(data = dict_get (dict, GF_XATTR_MARKER_KEY)))
                goto out;

        op_errno = 0;

        LOCK (&frame->lock);
        {
                callcnt = --marker->call_count;

                if (data->len != VMARK_SIZE || data->data[0] != 1)
                        op_errno = EINVAL;
                else if (marker_has_volinfo (marker)) {
                        if (memcmp (marker->vmark, data->data, VMARK_HSIZE) != 0)
                                op_errno = EINVAL;
                        else if (data->data[VMARK_HSIZE] != 0) {
                                memcpy (marker->vmark, data->data, VMARK_SIZE);
                                callcnt = 0;
                        } else if (memcmp (marker->vmark, data->data, VMARK_SIZE) < 0)
                                memcpy (marker->vmark, data->data, VMARK_SIZE);
                } else {
                        memcpy (marker->vmark, data->data, VMARK_SIZE);
                        uuid_unparse ((unsigned char *)(marker->vmark + 2), vol_uuid);
                        if (data->data[VMARK_HSIZE] != 0)
                                callcnt = 0;
                }
        }

        UNLOCK (&frame->lock);

        if (op_errno) {
                op_ret = -1;
                dict = NULL;
                callcnt = 0;
        }

        if (!callcnt) {
                if (marker_has_volinfo (marker) && op_ret == 0 && data)
                        memcpy (data->data, marker->vmark, VMARK_SIZE);

 out:
                STACK_UNWIND_STRICT (getxattr, frame, op_ret, op_errno, dict);
        }
}


int32_t
cluster_getmarkerattr (call_frame_t *frame,xlator_t *this, loc_t *loc, const char *name,
                       struct marker_str *marker, char *vol_uuid,
                       mark_getxattr_cbk_t cluster_getmarkerattr_cbk)
{
        xlator_list_t    *trav = NULL;

        VALIDATE_OR_GOTO (frame, err);
        VALIDATE_OR_GOTO (this, err);
        VALIDATE_OR_GOTO (loc, err);
        VALIDATE_OR_GOTO (loc->path, err);
        VALIDATE_OR_GOTO (loc->inode, err);
        VALIDATE_OR_GOTO (name, err);
        VALIDATE_OR_GOTO (cluster_getmarkerattr_cbk, err);

        trav = this->children;

        if ( frame->root->pid == -1) {
                while (trav) {
                        STACK_WIND (frame, cluster_getmarkerattr_cbk,
                                    trav->xlator, trav->xlator->fops->getxattr,
                                    loc, name);
                        trav = trav->next;
                }
        }
        else {
                STACK_WIND (frame, default_getxattr_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->getxattr, loc, name);
        }

        return 0;
err:
        return -1;

}
