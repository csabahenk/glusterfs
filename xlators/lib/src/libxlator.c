#include "libxlator.h"

int
parse_digit_pair (char **str, unsigned long *digs)
{
        if (!isdigit (*str[0]))
                return 2;

        errno = 0;

        digs[0] = strtoul (*str, str, 10);

        if (*str[0] != '.' || !isdigit (*str[1]))
                return 2;

        digs[1] = strtoul (*str + 1, str, 10);

        return errno ? 2 : 0;
}

int
parse_vol_uuid (char **str)
{
        char *s = *str;
        int i =0;

        for (i = 0; i < UUID_SIZE; i++) {
                if (!(isdigit (*s) || (*s >= 'a' && *s <= 'f') || *s == '-'))
                        return 2;
        }

        *str += i;

        return 0;
}

/* For getxattr on trusted.glusterfs.volume-mark parse the value as <ver no>:<vol-uuid>:<sec>.<usec>*/
/*4-kind of return values
        *  0 - Success
        * -1 - Operational failure such as ENOMEM
        *  1 - Not of the form <ver no>:<vol-uuid>:<sec>.<usec>  BUT of the form  <ver no>:<vol-uuid>
        *  2 - Failure  Not of the form <ver no>:<vol-uuid> HENCE should set EINVAL as return value*/


int
parse_uuid_markstruct (char *name, struct marker_str *local)
{
        unsigned long digs[2] = {0,};
        char            *temp_str = name;
        int              ret = -1;

        ret = parse_digit_pair (&temp_str, digs);
        if (!ret && (digs[0] != 1 || digs[1] > 999 || temp_str[0] != ':'))
                ret = 2;
        if (ret)
                return ret;

        temp_str++;
        ret = parse_vol_uuid (&temp_str);
        if (!ret && temp_str[0] != ':')
                ret = 2;
        if (ret)
                return ret;

        temp_str++;
        strncpy (local->head, name, temp_str - name);

        ret = parse_digit_pair (&temp_str, digs);
        if (!ret && (digs[0] > UINT_MAX || digs[1] > UINT_MAX || *temp_str))
                ret = 2;

        local->timebuf[0] = digs[0];
        local->timebuf[1] = digs[1];

        return ret;
}




/*Copy the contents of oldtimebuf to newtimbuf*/
void
update_timebuf (uint32_t *oldtimbuf, uint32_t *newtimebuf)
{
        newtimebuf[0] =  (oldtimbuf[0]);
        newtimebuf[1] =  (oldtimbuf[1]);
}

/* Convert Timebuf in network order to host order */
void
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
        char           *str = NULL;
        char           *tmp_str = NULL;
        char           *tail = NULL;
        unsigned long   digs[2] = {0,};
        int             ret = -1;

        if (!this || !frame || !cookie || !marker) {
                gf_log ("stripe", GF_LOG_DEBUG, "possible NULL deref");
                goto err;
        }

        LOCK (&frame->lock);
        {
                callcnt = --marker->call_count;

                if (dict_get_str (dict, GF_XATTR_MARKER_KEY, &str) == 0) {
                        if (marker_has_volinfo (marker)) {
                                ret = 2;

                                tail = strtail (str, marker->head);
                                if (tail)
                                        ret = parse_digit_pair (&tail, digs);
                                if (!ret && (digs[0] > UINT_MAX ||
                                             digs[1] > UINT_MAX || *tail))
                                        ret = 2;

                                if (!ret && (digs[0] > marker->timebuf[0] ||
                                    (digs[0] == marker->timebuf[0] &&
                                     digs[1] > marker->timebuf[1]))) {
                                        marker->timebuf[0] = digs[0];
                                        marker->timebuf[1] = digs[1];
                                }
                        } else {
                                ret = parse_uuid_markstruct (str, marker);
                                if (ret == 0 || ret == 1)
                                        memcpy (vol_uuid,
                                                marker_get_uuid (marker), UUID_SIZE);
                        }

                        if ( ret == -1) {
                                op_ret = -1;
                                op_errno = errno;
                                goto err;
                        }
                        /* when marker xlator returns <uuid:FAILURE>*/
                        if ( ret == 1) {
                                goto err;
                        }
                        /*Not of the the form <version-no>:<uuid>,
                         *should return EINVAL to the User*/
                        if ( ret == 2) {
                                op_ret = -1;
                                op_errno = EINVAL;
                                goto err;
                        }
                }

        }
        UNLOCK (&frame->lock);
        if (!callcnt) {
                if (marker_has_volinfo (marker)) {
                        ret = gf_asprintf (&tmp_str, "%s%u.%u",
                                           marker->head, marker->timebuf[0],
                                           marker->timebuf[1]);
                        if (ret == -1) {
                                op_ret = -1;
                                op_errno = ENOMEM;
                        }
                        if (dict_set_dynstr (dict, GF_XATTR_MARKER_KEY, tmp_str)) {
                                op_errno = ENOMEM;
                                op_ret = -1;
                        }
                }
                STACK_UNWIND_STRICT (getxattr, frame, op_ret, op_errno, dict);
                return ;
        }
        return;

err:
        UNLOCK (&frame->lock);
        STACK_UNWIND_STRICT (getxattr, frame, op_ret, op_errno, NULL);
        return ;

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
