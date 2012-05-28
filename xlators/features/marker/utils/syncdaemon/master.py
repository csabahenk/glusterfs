import os
import sys
import time
import stat
import random
import signal
import logging
import socket
import errno
from errno import ENOENT, ENODATA, EPIPE
from threading import currentThread, Condition, Lock
from datetime import datetime

from gconf import gconf
from syncdutils import FreeObject, Thread, GsyncdError, boolify, \
                       escape, unescape, select

URXTIME = (-1, 0)

class GMaster(object):
    """class impementling master role"""

    KFGN = 0
    KNAT = 1

    def get_sys_volinfo(self):
        """query volume marks on fs root

        err out on multiple foreign masters
        """
        fgn_vis, nat_vi = self.master.server.foreign_volume_infos(), \
                          self.master.server.native_volume_info()
        fgn_vi = None
        if fgn_vis:
            if len(fgn_vis) > 1:
                raise GsyncdError("cannot work with multiple foreign masters")
            fgn_vi = fgn_vis[0]
        return fgn_vi, nat_vi

    @property
    def uuid(self):
        if self.volinfo:
            return self.volinfo['uuid']

    @property
    def volmark(self):
        if self.volinfo:
            return self.volinfo['volume_mark']

    @property
    def inter_master(self):
        """decide if we are an intermediate master
        in a cascading setup
        """
        return self.volinfo_state[self.KFGN] and True or False

    @staticmethod
    def _srv_xtime_100(srv, path, uuid):
        return srv.xtime(path, uuid)

    @staticmethod
    def _srv_set_xtime_100(srv, path, uuid, xt):
        return srv.set_xtime(path, uuid, xt)

    @staticmethod
    def _srv_xtime_101(srv, path, uuid):
        return srv.xtime(path, uuid)[uuid]

    @staticmethod
    def _srv_set_xtime_101(srv, path, uuid, xt):
        return srv.set_xtime(path, {uuid: xt})

    def __codeswap__(self):
        vers = self.slave.server.commonvers
        self.master.server.__codeswap__(self, vers)
        for m in ("srv_xtime", "srv_set_xtime"):
            setattr(self, m,
                    getattr(type(self), "_%s_%d" % (m, int(vers['object']*100))))

    def xtime(self, path, *a, **opts):
        """get amended xtime

        as of amending, we can create missing xtime, or
        determine a valid value if what we get is expired
        (as of the volume mark expiry); way of amendig
        depends on @opts and on subject of query (master
        or slave).
        """
        if a:
            rsc = a[0]
        else:
            rsc = self.master
        if not 'create' in opts:
            opts['create'] = (rsc == self.master and not self.inter_master)
        if not 'default_xtime' in opts:
            if rsc == self.master and self.inter_master:
                opts['default_xtime'] = ENODATA
            else:
                opts['default_xtime'] = URXTIME
        xt = self.srv_xtime(rsc.server, path, self.uuid)
        if isinstance(xt, int) and xt != ENODATA:
            return xt
        invalid_xtime = (xt == ENODATA or xt < self.volmark)
        if invalid_xtime:
            if opts['create']:
                t = time.time()
                sec = int(t)
                nsec = int((t - sec) * 1000000)
                xt = (sec, nsec)
                self.srv_set_xtime(rsc.server, path, self.uuid, xt)
            else:
                xt = opts['default_xtime']
        return xt

    def __init__(self, master, slave):
        self.master = master
        self.slave = slave
        self.__codeswap__()
        self.jobtab = {}
        self.syncer = Syncer(slave)
        # crawls vs. turns:
        # - self.crawls is simply the number of crawl() invocations on root
        # - one turn is a maximal consecutive sequence of crawls so that each
        #   crawl in it detects a change to be synced
        # - self.turns is the number of turns since start
        # - self.total_turns is a limit so that if self.turns reaches it, then
        #   we exit (for diagnostic purposes)
        # so, eg., if the master fs changes unceasingly, self.turns will remain 0.
        self.crawls = 0
        self.turns = 0
        self.total_turns = int(gconf.turns)
        self.lastreport = {'crawls': 0, 'turns': 0}
        self.start = None
        self.change_seen = None
        # the authoritative (foreign, native) volinfo pair
        # which lets us deduce what to do when we refetch
        # the volinfos from system
        uuid_preset = getattr(gconf, 'volume_id', None)
        self.volinfo_state = (uuid_preset and {'uuid': uuid_preset}, None)
        # the actual volinfo we make use of
        self.volinfo = None
        self.terminate = False
        self.checkpoint_thread = None

    @staticmethod
    def _checkpt_param(chkpt, prm, timish=True):
        """use config backend to lookup a parameter belonging to
           checkpoint @chkpt"""
        cprm = getattr(gconf, 'checkpoint_' + prm, None)
        if not cprm:
            return
        chkpt_mapped, val = cprm.split(':', 1)
        if unescape(chkpt_mapped) != chkpt:
            return
        if timish:
            val = tuple(int(x) for x in val.split("."))
        return val

    @staticmethod
    def _set_checkpt_param(chkpt, prm, val, timish=True):
        """use config backend to store a parameter associated
           with checkpoint @chkpt"""
        if timish:
            val = "%d.%d" % tuple(val)
        gconf.configinterface.set('checkpoint_' + prm, "%s:%s" % (escape(chkpt), val))

    @staticmethod
    def humantime(*tpair):
        """format xtime-like (sec, nsec) pair to human readable format"""
        ts = datetime.fromtimestamp(float('.'.join(str(n) for n in tpair))).\
               strftime("%Y-%m-%d %H:%M:%S")
        if len(tpair) > 1:
            ts += str(tpair[1])
        return ts

    def checkpt_service(self, chan, chkpt, tgt):
        """checkpoint service loop

        monitor and verify checkpoint status for @chkpt, and listen
        for incoming requests for whom we serve a pretty-formatted
        status report"""
        if not chkpt:
            # dummy loop for the case when there is no checkpt set
            while True:
                select([chan], [], [])
                conn, _ = chan.accept()
                conn.send('\0')
                conn.close()
        completed = self._checkpt_param(chkpt, 'completed')
        while True:
            s,_,_ = select([chan], [], [], (not completed) and 5 or None)
            # either request made and we re-check to not
            # give back stale data, or we still hunting for completion
            if tgt < self.volmark:
                # indexing has been reset since setting the checkpoint
                status = "is invalid"
            else:
                xtr = self.xtime('.', self.slave)
                if isinstance(xtr, int):
                    raise GsyncdError("slave root directory is unaccessible (%s)",
                                      os.strerror(xtr))
                ncompleted = (xtr >= tgt)
                if completed and not ncompleted: # stale data
                    logging.warn("completion time %s for checkpoint %s became stale" % \
                                 (self.humantime(*completed), chkpt))
                    completed = None
                    gconf.confdata.delete('checkpoint-completed')
                if ncompleted and not completed: # just reaching completion
                    completed = [ int(x) for x in ("%.6f" % time.time()).split('.') ]
                    self._set_checkpt_param(chkpt, 'completed', completed)
                    logging.info("checkpoint %s completed" % chkpt)
                status = completed and \
                  "completed at " + self.humantime(completed[0]) or \
                  "not reached yet"
            if s:
                conn, _ = chan.accept()
                try:
                    conn.send("  | checkpoint %s %s\0" % (chkpt, status))
                except:
                    exc = sys.exc_info()[1]
                    if (isinstance(exc, OSError) or isinstance(exc, IOError)) and \
                       exc.errno == EPIPE:
                        logging.debug('checkpoint client disconnected')
                    else:
                        raise
                conn.close()

    def start_checkpoint_thread(self):
        """prepare and start checkpoint service"""
        if self.checkpoint_thread or not getattr(gconf, 'state_socket', None):
            return
        chan = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            os.unlink(gconf.state_socket)
        except:
            if sys.exc_info()[0] == OSError:
                pass
        chan.bind(gconf.state_socket)
        chan.listen(1)
        checkpt_tgt = None
        if gconf.checkpoint:
            checkpt_tgt = self._checkpt_param(gconf.checkpoint, 'target')
            if not checkpt_tgt:
                checkpt_tgt = self.xtime('.')
                if isinstance(checkpt_tgt, int):
                    raise GsyncdError("master root directory is unaccessible (%s)",
                                      os.strerror(checkpt_tgt))
                self._set_checkpt_param(gconf.checkpoint, 'target', checkpt_tgt)
            logging.debug("checkpoint target %d.%d has been determined for checkpoint %s" % \
                          (checkpt_tgt[0], checkpt_tgt[1], gconf.checkpoint))
        t = Thread(target=self.checkpt_service, args=(chan, gconf.checkpoint, checkpt_tgt))
        t.start()
        self.checkpoint_thread = t

    def crawl_loop(self):
        """start the keep-alive thread and iterate .crawl"""
        timo = int(gconf.timeout or 0)
        if timo > 0:
            def keep_alive():
                while True:
                    gap = timo * 0.5
                    # first grab a reference as self.volinfo
                    # can be changed in main thread
                    vi = self.volinfo
                    if vi:
                        # then have a private copy which we can mod
                        vi = vi.copy()
                        vi['timeout'] = int(time.time()) + timo
                    else:
                        # send keep-alives more frequently to
                        # avoid a delay in announcing our volume info
                        # to slave if it becomes established in the
                        # meantime
                        gap = min(10, gap)
                    self.slave.server.keep_alive(vi)
                    time.sleep(gap)
            t = Thread(target=keep_alive)
            t.start()
        self.lastreport['time'] = time.time()
        while not self.terminate:
            self.crawl()

    def add_job(self, path, label, job, *a, **kw):
        """insert @job function to job table at @path with @label"""
        if self.jobtab.get(path) == None:
            self.jobtab[path] = []
        self.jobtab[path].append((label, a, lambda : job(*a, **kw)))

    def add_failjob(self, path, label):
        """invoke .add_job with a job that does nothing just fails"""
        logging.debug('salvaged: ' + label)
        self.add_job(path, label, lambda: False)

    def wait(self, path, *args):
        """perform jobs registered for @path

        Reset jobtab entry for @path,
        determine success as the conjuction of
        success of all the jobs. In case of
        success, call .sendmark on @path
        """
        jobs = self.jobtab.pop(path, [])
        succeed = True
        for j in jobs:
            ret = j[-1]()
            if not ret:
                succeed = False
        if succeed:
            self.sendmark(path, *args)
        return succeed

    def sendmark(self, path, mark, adct=None):
        """update slave side xtime for @path to master side xtime

        also can send a setattr payload (see Server.setattr).
        """
        if adct:
            self.slave.server.setattr(path, adct)
        self.srv_set_xtime(self.slave.server, path, self.uuid, mark)

    @staticmethod
    def volinfo_state_machine(volinfo_state, volinfo_sys):
        """compute new volinfo_state from old one and incoming
           as of current system state, also indicating if there was a
           change regarding which volume mark is the authoritative one

        @volinfo_state, @volinfo_sys are pairs of volume mark dicts
        (foreign, native).

        Note this method is marked as static, ie. the computation is
        pure, without reliance on any excess implicit state. State
        transitions which are deemed as ambiguous or banned will raise
        an exception.

        """
        # store the value below "boxed" to emulate proper closures
        # (variables of the enclosing scope are available inner functions
        # provided they are no reassigned; mutation is OK).
        param = FreeObject(relax_mismatch = False, state_change = None, index=-1)
        def select_vi(vi0, vi):
            param.index += 1
            if vi and (not vi0 or vi0['uuid'] == vi['uuid']):
                if not vi0 and not param.relax_mismatch:
                    param.state_change = param.index
                # valid new value found; for the rest, we are graceful about
                # uuid mismatch
                param.relax_mismatch = True
                return vi
            if vi0 and vi and vi0['uuid'] != vi['uuid'] and not param.relax_mismatch:
                # uuid mismatch for master candidate, bail out
                raise GsyncdError("aborting on uuid change from %s to %s" % \
                                   (vi0['uuid'], vi['uuid']))
            # fall back to old
            return vi0
        newstate = tuple(select_vi(*vip) for vip in zip(volinfo_state, volinfo_sys))
        srep = lambda vi: vi and vi['uuid'][0:8]
        logging.debug('(%s, %s) << (%s, %s) -> (%s, %s)' % \
                      tuple(srep(vi) for vi in volinfo_state + volinfo_sys + newstate))
        return newstate, param.state_change

    def crawl(self, path='.', xtl=None):
        """crawling...

          Standing around
          All the right people
          Crawling
          Tennis on Tuesday
          The ladder is long
          It is your nature
          You've gotta suntan
          Football on Sunday
          Society boy

        Recursively walk the master side tree and check if updates are
        needed due to xtime differences. One invocation of crawl checks
        children of @path and do a recursive enter only on
        those directory children where there is an update needed.

        Way of updates depend on file type:
        - for symlinks, sync them directy and synchronously
        - for regular children, register jobs for @path (cf. .add_job) to start
          and wait on their rsync
        - for directory children, register a job for @path which waits (.wait)
          on jobs for the given child
        (other kind of filesystem nodes are not considered)

        Those slave side children which do not exist on master are simply
        purged (see Server.purge).

        Behavior is fault tolerant, synchronization is adaptive: if some action fails,
        just go on relentlessly, adding a fail job (see .add_failjob) which will prevent
        the .sendmark on @path, so when the next crawl will arrive to @path it will not
        see it as up-to-date and  will try to sync it again. While this semantics can be
        supported by funky design principles (http://c2.com/cgi/wiki?LazinessImpatienceHubris),
        the ultimate reason which excludes other possibilities is simply transience: we cannot
        assert that the file systems (master / slave) underneath do not change and actions
        taken upon some condition will not lose their context by the time they are performed.
        """
        if path == '.':
            if self.start:
                self.crawls += 1
                logging.debug("... crawl #%d done, took %.6f seconds" % \
                              (self.crawls, time.time() - self.start))
            time.sleep(1)
            self.start = time.time()
            should_display_info = self.start - self.lastreport['time'] >= 60
            if should_display_info:
                logging.info("completed %d crawls, %d turns",
                             self.crawls - self.lastreport['crawls'],
                             self.turns - self.lastreport['turns'])
                self.lastreport.update(crawls = self.crawls,
                                       turns = self.turns,
                                       time = self.start)
            volinfo_sys = self.get_sys_volinfo()
            self.volinfo_state, state_change = self.volinfo_state_machine(self.volinfo_state,
                                                                          volinfo_sys)
            if self.inter_master:
                self.volinfo = volinfo_sys[self.KFGN]
            else:
                self.volinfo = volinfo_sys[self.KNAT]
            if state_change == self.KFGN or (state_change == self.KNAT and not self.inter_master):
                logging.info('new master is %s', self.uuid)
                if self.volinfo:
                    logging.info("%s master with volume id %s ..." % \
                                 (self.inter_master and "intermediate" or "primary",
                                  self.uuid))
            if state_change == self.KFGN:
               gconf.configinterface.set('volume_id', self.uuid)
            if self.volinfo:
                if self.volinfo['retval']:
                    raise GsyncdError ("master is corrupt")
                if state_change:
                    self.start_checkpoint_thread()
            else:
                if should_display_info or self.crawls == 0:
                    if self.inter_master:
                        logging.info("waiting for being synced from %s ..." % \
                                     self.volinfo_state[self.KFGN]['uuid'])
                    else:
                        logging.info("waiting for volume info ...")
                return
        logging.debug("entering " + path)
        if not xtl:
            xtl = self.xtime(path)
            if isinstance(xtl, int):
                self.add_failjob(path, 'no-local-node')
                return
        xtr0 = self.xtime(path, self.slave)
        if isinstance(xtr0, int):
            if xtr0 != ENOENT:
                self.slave.server.purge(path)
            try:
                self.slave.server.mkdir(path)
            except OSError:
                self.add_failjob(path, 'no-remote-node')
                return
            xtr = URXTIME
        else:
            xtr = xtr0
            if xtr > xtl:
                raise GsyncdError("timestamp corruption for " + path)
            if xtl == xtr:
                if path == '.' and self.change_seen:
                    self.turns += 1
                    self.change_seen = False
                    if self.total_turns:
                        logging.info("finished turn #%s/%s" % \
                                     (self.turns, self.total_turns))
                        if self.turns == self.total_turns:
                            logging.info("reached turn limit")
                            self.terminate = True
                return
        if path == '.':
            self.change_seen = True
        try:
            dem = self.master.server.entries(path)
        except OSError:
            self.add_failjob(path, 'local-entries-fail')
            return
        random.shuffle(dem)
        try:
            des = self.slave.server.entries(path)
        except OSError:
            self.slave.server.purge(path)
            try:
                self.slave.server.mkdir(path)
                des = self.slave.server.entries(path)
            except OSError:
                self.add_failjob(path, 'remote-entries-fail')
                return
        dd = set(des) - set(dem)
        if dd and not boolify(gconf.ignore_deletes):
            self.slave.server.purge(path, dd)
        chld = []
        for e in dem:
            e = os.path.join(path, e)
            xte = self.xtime(e)
            if isinstance(xte, int):
                logging.warn("irregular xtime for %s: %s" % (e, errno.errorcode[xte]))
            elif xte > xtr:
                chld.append((e, xte))
        def indulgently(e, fnc, blame=None):
            if not blame:
                blame = path
            try:
                return fnc(e)
            except (IOError, OSError):
                ex = sys.exc_info()[1]
                if ex.errno == ENOENT:
                    logging.warn("salvaged ENOENT for " + e)
                    self.add_failjob(blame, 'by-indulgently')
                    return False
                else:
                    raise
        for e, xte in chld:
            st = indulgently(e, lambda e: os.lstat(e))
            if st == False:
                continue
            mo = st.st_mode
            adct = {'own': (st.st_uid, st.st_gid)}
            if stat.S_ISLNK(mo):
                if indulgently(e, lambda e: self.slave.server.symlink(os.readlink(e), e)) == False:
                    continue
                self.sendmark(e, xte, adct)
            elif stat.S_ISREG(mo):
                logging.debug("syncing %s ..." % e)
                pb = self.syncer.add(e)
                def regjob(e, xte, pb):
                    if pb.wait():
                        logging.debug("synced " + e)
                        self.sendmark(e, xte)
                        return True
                    else:
                        logging.warn("failed to sync " + e)
                self.add_job(path, 'reg', regjob, e, xte, pb)
            elif stat.S_ISDIR(mo):
                adct['mode'] = mo
                if indulgently(e, lambda e: (self.add_job(path, 'cwait', self.wait, e, xte, adct),
                                             self.crawl(e, xte),
                                             True)[-1], blame=e) == False:
                    continue
            else:
                # ignore fifos, sockets and special files
                pass
        if path == '.':
            self.wait(path, xtl)

class BoxClosedErr(Exception):
    pass

class PostBox(list):
    """synchronized collection for storing things thought of as "requests" """

    def __init__(self, *a):
        list.__init__(self, *a)
        # too bad Python stdlib does not have read/write locks...
        # it would suffivce to grab the lock in .append as reader, in .close as writer
        self.lever = Condition()
        self.open = True
        self.done = False

    def wait(self):
        """wait on requests to be processed"""
        self.lever.acquire()
        if not self.done:
            self.lever.wait()
        self.lever.release()
        return self.result

    def wakeup(self, data):
        """wake up requestors with the result"""
        self.result = data
        self.lever.acquire()
        self.done = True
        self.lever.notifyAll()
        self.lever.release()

    def append(self, e):
        """post a request"""
        self.lever.acquire()
        if not self.open:
            raise BoxClosedErr
        list.append(self, e)
        self.lever.release()

    def close(self):
        """prohibit the posting of further requests"""
        self.lever.acquire()
        self.open = False
        self.lever.release()

class Syncer(object):
    """a staged queue to relay rsync requests to rsync workers

    By "staged queue" its meant that when a consumer comes to the
    queue, it takes _all_ entries, leaving the queue empty.
    (I don't know if there is an official term for this pattern.)

    The queue uses a PostBox to accumulate incoming items.
    When a consumer (rsync worker) comes, a new PostBox is
    set up and the old one is passed on to the consumer.

    Instead of the simplistic scheme of having one big lock
    which synchronizes both the addition of new items and
    PostBox exchanges, use a separate lock to arbitrate consumers,
    and rely on PostBox's synchronization mechanisms take
    care about additions.

    There is a corner case racy situation, producers vs. consumers,
    which is not handled by this scheme: namely, when the PostBox
    exchange occurs in between being passed to the producer for posting
    and the post placement. But that's what Postbox.close is for:
    such a posting will find the PostBox closed, in which case
    the producer can re-try posting against the actual PostBox of
    the queue.

    To aid accumlation of items in the PostBoxen before grabbed
    by an rsync worker, the worker goes to sleep a bit after
    each completed syncjob.
    """

    def __init__(self, slave):
        """spawn worker threads"""
        self.slave = slave
        self.lock = Lock()
        self.pb = PostBox()
        for i in range(int(gconf.sync_jobs)):
            t = Thread(target=self.syncjob)
            t.start()

    def syncjob(self):
        """the life of a worker"""
        while True:
            pb = None
            while True:
                self.lock.acquire()
                if self.pb:
                    pb, self.pb = self.pb, PostBox()
                self.lock.release()
                if pb:
                    break
                time.sleep(0.5)
            pb.close()
            po = self.slave.rsync(pb)
            if po.returncode == 0:
                ret = True
            elif po.returncode in (23, 24):
                # partial transfer (cf. rsync(1)), that's normal
                ret = False
            else:
                po.errfail()
            pb.wakeup(ret)

    def add(self, e):
        while True:
            pb = self.pb
            try:
                pb.append(e)
                return pb
            except BoxClosedErr:
                pass
