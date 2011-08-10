import re
import os
import sys
import pwd
import stat
import time
import errno
import struct
import select
import socket
import logging
import tempfile
import threading
import subprocess
from errno import EEXIST, ENOENT, ENODATA, ENOTDIR, ELOOP, EISDIR

from gconf import gconf
import repce
from repce import RepceServer, RepceClient
from master import GMaster
import syncdutils
from syncdutils import GsyncdError

UrlRX  = re.compile('\A(\w+)://(.*)')
HostRX = re.compile('[a-z\d](?:[a-z\d.-]*[a-z\d])?', re.I)
UserRX = re.compile("[\w!\#$%&'*+-\/=?^_`{|}~]+")

def sup(x, *a, **kw):
    """a rubyesque "super" for python ;)

    invoke caller method in parent class with given args.
    """
    return getattr(super(type(x), x), sys._getframe(1).f_code.co_name)(*a, **kw)

def desugar(ustr):
    """transform sugared url strings to standard <scheme>://<urlbody> form

    parsing logic enforces the constraint that sugared forms should contatin
    a ':' or a '/', which ensures that sugared urls do not conflict with
    gluster volume names.
    """
    m = re.match('([^:]*):(.*)', ustr)
    if m:
        if not m.groups()[0]:
            return "gluster://localhost" + ustr
        elif '@' in m.groups()[0] or re.search('[:/]', m.groups()[1]):
            return "ssh://" + ustr
        else:
            return "gluster://" + ustr
    else:
        if ustr[0] != '/':
            raise GsyncdError("cannot resolve sugared url '%s'" % ustr)
        ap = os.path.normpath(ustr)
        if ap.startswith('//'):
            ap = ap[1:]
        return "file://" + ap

def gethostbyname(hnam):
    """gethostbyname wrapper"""
    try:
        return socket.gethostbyname(hnam)
    except socket.gaierror:
        ex = sys.exc_info()[1]
        raise GsyncdError("failed to resolve %s: %s" % \
                          (hnam, ex.strerror))

def parse_url(ustr):
    """instantiate an url object by scheme-to-class dispatch

    The url classes taken into consideration are the ones in
    this module whose names are full-caps.
    """
    m = UrlRX.match(ustr)
    if not m:
        ustr = desugar(ustr)
    m = UrlRX.match(ustr)
    if not m:
        raise GsyncdError("malformed url")
    sch, path = m.groups()
    this = sys.modules[__name__]
    if not hasattr(this, sch.upper()):
        raise GsyncdError("unknown url scheme " + sch)
    return getattr(this, sch.upper())(path)


class _MetaXattr(object):
    """singleton class, a lazy wrapper around the
    libcxattr module

    libcxattr (a heavy import due to ctypes) is
    loaded only when when the single
    instance is tried to be used.

    This reduces runtime for those invocations
    which do not need filesystem manipulation
    (eg. for config, url parsing)
    """

    def __getattr__(self, meth):
        from libcxattr import Xattr as LXattr
        xmeth = [ m for m in dir(LXattr) if m[0] != '_' ]
        if not meth in xmeth:
            return
        for m in xmeth:
            setattr(self, m, getattr(LXattr, m))
        return getattr(self, meth)

Xattr = _MetaXattr()


class Popen(subprocess.Popen):
    """customized subclass of subprocess.Popen with a ring
    buffer for children error ouput"""

    @classmethod
    def init_errhandler(cls):
        """start the thread which hanldes children's error output"""
        cls.errstore = {}
        def tailer():
            while True:
                for po in select.select([po.stderr for po in cls.errstore], [], []):
                    po.lock.acquire()
                    try:
                        la = cls.errstore.get(po)
                        if la == None:
                            continue
                        l = os.read(po.stderr.fileno(), 1024)
                        tots = len(l)
                        for lx in la:
                            tots += len(lx)
                        while tots > 1<<20 and la:
                            tots -= len(la.pop(0))
                    finally:
                        po.lock.release()
        t = syncdutils.Thread(target = tailer)
        t.start()
        cls.errhandler = t

    def __init__(self, args, *a, **kw):
        """customizations for subprocess.Popen instantiation

        - 'close_fds' is taken to be the default
        - if child's stderr is chosen to be managed,
          register it with the error handler thread
        """
        self.args = args
        if 'close_fds' not in kw:
            kw['close_fds'] = True
        self.lock = threading.Lock()
        try:
            sup(self, args, *a, **kw)
        except:
            ex = sys.exc_info()[1]
            if not isinstance(ex, OSError):
                raise
            raise GsyncdError("""execution of "%s" failed with %s (%s)""" % \
                              (args[0], errno.errorcode[ex.errno], os.strerror(ex.errno)))
        if kw['stderr'] == subprocess.PIPE:
            assert(getattr(self, 'errhandler', None))
            self.errstore[self] = []

    def errfail(self):
        """fail nicely if child did not terminate with success"""
        filling = None
        if self.elines:
            filling = ", saying:"
        logging.error("""command "%s" returned with %d%s""" % \
                      (" ".join(self.args), self.returncode, filling))
        for l in self.elines:
            for ll in l.rstrip().split("\n"):
                logging.error(self.args[0] + "> " + ll.rstrip())
        syncdutils.finalize(exval = 1)

    def terminate_geterr(self, fail_on_err = True):
        """kill child, finalize stderr harvesting (unregister
        from errhandler, set up .elines), fail on error if
        asked for
        """
        self.lock.acquire()
        try:
            elines = self.errstore.pop(self)
        finally:
            self.lock.release()
        if self.poll() == None:
            self.terminate()
            if sp.poll() == None:
                time.sleep(0.1)
            sp.kill()
        while True:
            b = os.read(self.stderr.fileno(), 1024)
            if b:
                elines.append(b)
            else:
                break
        self.stderr.close()
        self.elines = elines
        if fail_on_err and self.returncode != 0:
            self.errfail()


class Server(object):
    """singleton implemening those filesystem access primitives
       which are needed for geo-replication functionality

    (Singleton in the sense it's a class which has only static
    and classmethods and is used directly, without instantiation.)
    """

    GX_NSPACE = "trusted.glusterfs"
    NTV_FMTSTR = "!" + "B"*19 + "II"
    FRGN_XTRA_FMT = "I"
    FRGN_FMTSTR = NTV_FMTSTR + FRGN_XTRA_FMT

    @staticmethod
    def entries(path):
        """directory entries in an array"""
        # prevent symlinks being followed
        if not stat.S_ISDIR(os.lstat(path).st_mode):
            raise OSError(ENOTDIR, os.strerror(ENOTDIR))
        return os.listdir(path)

    @classmethod
    def purge(cls, path, entries=None):
        """force-delete subtrees

        If @entries is not specified, delete
        the whole subtree under @path (including
        @path).

        Otherwise, @entries should be a
        a sequence of children of @path, and
        the effect is identical with a joint
        @entries-less purge on them, ie.

        for e in entries:
            cls.purge(os.path.join(path, e))
        """
        me_also = entries == None
        if not entries:
            try:
                # if it's a symlink, prevent
                # following it
                try:
                    os.unlink(path)
                    return
                except OSError:
                    ex = sys.exc_info()[1]
                    if ex.errno == EISDIR:
                        entries = os.listdir(path)
                    else:
                        raise
            except OSError:
                ex = sys.exc_info()[1]
                if ex.errno in (ENOTDIR, ENOENT, ELOOP):
                    try:
                        os.unlink(path)
                        return
                    except OSError:
                        ex = sys.exc_info()[1]
                        if ex.errno == ENOENT:
                            return
                        raise
                else:
                    raise
        for e in entries:
            cls.purge(os.path.join(path, e))
        if me_also:
            os.rmdir(path)

    @classmethod
    def _create(cls, path, ctor):
        """path creation backend routine"""
        try:
            ctor(path)
        except OSError:
            ex = sys.exc_info()[1]
            if ex.errno == EEXIST:
                cls.purge(path)
                return ctor(path)
            raise

    @classmethod
    def mkdir(cls, path):
        cls._create(path, os.mkdir)

    @classmethod
    def symlink(cls, lnk, path):
        cls._create(path, lambda p: os.symlink(lnk, p))

    @classmethod
    def xtime(cls, path, uuid):
        """query xtime extended attribute

        Return xtime of @path for @uuid as a pair of integers.
        "Normal" errors due to non-existent @path or extended attribute
        are tolerated and errno is returned in such a case.
        """

        try:
            return struct.unpack('!II', Xattr.lgetxattr(path, '.'.join([cls.GX_NSPACE, uuid, 'xtime']), 8))
        except OSError:
            ex = sys.exc_info()[1]
            if ex.errno in (ENOENT, ENODATA, ENOTDIR):
                return ex.errno
            else:
                raise

    @classmethod
    def set_xtime(cls, path, uuid, mark):
        """set @mark as xtime for @uuid on @path"""
        Xattr.lsetxattr(path, '.'.join([cls.GX_NSPACE, uuid, 'xtime']), struct.pack('!II', *mark))

    @staticmethod
    def setattr(path, adct):
        """set file attributes

        @adct is a dict, where 'own', 'mode' and 'times'
        keys are looked for and values used to perform
        chown, chmod or utimes on @path.
        """
        own = adct.get('own')
        if own:
            os.lchown(path, *own)
        mode = adct.get('mode')
        if mode:
            os.chmod(path, stat.S_IMODE(mode))
        times = adct.get('times')
        if times:
            os.utime(path, times)

    @staticmethod
    def pid():
        return os.getpid()

    last_keep_alive = 0
    @classmethod
    def keep_alive(cls, dct):
        """process keepalive messages.

        Return keep-alive counter (number of received keep-alive
        messages).

        Now the "keep-alive" message can also have a payload which is
        used to set a foreign volume-mark on the underlying file system.
        """
        if dct:
            key = '.'.join([cls.GX_NSPACE, 'volume-mark', dct['uuid']])
            val = struct.pack(cls.FRGN_FMTSTR,
                              *(dct['version']  +
                                tuple(int(x,16) for x in re.findall('(?:[\da-f]){2}', dct['uuid'])) +
                                (dct['retval'],) + dct['volume_mark'][0:2] + (dct['timeout'],)))
            Xattr.lsetxattr('.', key, val)
        cls.last_keep_alive += 1
        return cls.last_keep_alive

    @staticmethod
    def version():
        """version used in handshake"""
        return 1.0


class SlaveLocal(object):
    """mix-in class to implement some factes of a slave server

    ("mix-in" is sort of like "abstract class", ie. it's not
    instantiated just included in the ancesty DAG. I use "mix-in"
    to indicate that it's not used as an abstract base class,
    rather just taken in to implement additional functionality
    on the basis of the assumed availability of certain interfaces.)
    """

    def can_connect_to(self, remote):
        """determine our position in the connectibility matrix"""
        return not remote

    def service_loop(self):
        """start a RePCe server serving self's server

        stop servicing if a timeout is configured and got no
        keep-alime in that inteval
        """
        repce = RepceServer(self.server, sys.stdin, sys.stdout, int(gconf.sync_jobs))
        t = syncdutils.Thread(target=lambda: (repce.service_loop(),
                                              syncdutils.finalize()))
        t.start()
        logging.info("slave listening")
        if gconf.timeout and int(gconf.timeout) > 0:
            while True:
                lp = self.server.last_keep_alive
                time.sleep(int(gconf.timeout))
                if lp == self.server.last_keep_alive:
                    logging.info("connection inactive for %d seconds, stopping" % int(gconf.timeout))
                    break
        else:
            select.select((), (), ())

class SlaveRemote(object):
    """mix-in class to implement an interface to a remote slave"""

    def connect_remote(self, rargs=[], **opts):
        """connects to a remote slave

        Invoke an auxiliary utility (slave gsyncd, possibly wrapped)
        which sets up the connection and set up a RePCe client to
        communicate throuh its stdio.
        """
        slave = opts.get('slave', self.url)
        so = getattr(gconf, 'session_owner', None)
        if so:
            so_args = ['--session-owner', so]
        else:
            so_args = []
        po = Popen(rargs + gconf.remote_gsyncd.split() + so_args + \
                   ['-N', '--listen', '--timeout', str(gconf.timeout), slave],
                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        gconf.transport = po
        return self.start_fd_client(po.stdout, po.stdin, **opts)

    def start_fd_client(self, i, o, **opts):
        """set up RePCe client, handshake with server

        It's cut out as a separate method to let
        subclasses hook into client startup
        """
        self.server = RepceClient(i, o)
        rv = self.server.__version__()
        exrv = {'proto': repce.repce_version, 'object': Server.version()}
        da0 = (rv, exrv)
        da1 = ({}, {})
        for i in range(2):
            for k, v in da0[i].iteritems():
                da1[i][k] = int(v)
        if da1[0] != da1[1]:
            raise GsyncdError("RePCe major version mismatch: local %s, remote %s" % (exrv, rv))

    def rsync(self, files, *args):
        """invoke rsync"""
        if not files:
            raise GsyncdError("no files to sync")
        logging.debug("files: " + ", ".join(files))
        argv = gconf.rsync_command.split() + gconf.rsync_extra.split() + ['-aR'] + files + list(args)
        po = Popen(argv, stderr=subprocess.PIPE)
        po.wait()
        po.terminate_geterr(fail_on_err = False)
        return po


class AbstractUrl(object):
    """abstract base class for url scheme classes"""

    def __init__(self, path, pattern):
        m = re.search(pattern, path)
        if not m:
            raise GsyncdError("malformed path")
        self.path = path
        return m.groups()

    @property
    def scheme(self):
        return type(self).__name__.lower()

    def canonical_path(self):
        return self.path

    def get_url(self, canonical=False, escaped=False):
        """format self's url in various styles"""
        if canonical:
            pa = self.canonical_path()
        else:
            pa = self.path
        u = "://".join((self.scheme, pa))
        if escaped:
            u = syncdutils.escape(u)
        return u

    @property
    def url(self):
        return self.get_url()


  ### Concrete resource classes ###


class FILE(AbstractUrl, SlaveLocal, SlaveRemote):
    """scheme class for file:// urls

    can be used to represent a file slave server
    on slave side, or interface to a remote file
    file server on master side
    """

    class FILEServer(Server):
        """included server flavor"""
        pass

    server = FILEServer

    def __init__(self, path):
        sup(self, path, '^/')

    def connect(self):
        """inhibit the resource beyond"""
        os.chdir(self.path)

    def rsync(self, files):
        return sup(self, files, self.path)


class GLUSTER(AbstractUrl, SlaveLocal, SlaveRemote):
    """scheme class for gluster:// urls

    can be used to represent a gluster slave server
    on slave side, or interface to a remote gluster
    slave on master side, or to represent master
    (slave-ish features come from the mixins, master
    functionality is outsourced to GMaster from master)
    """

    class GLUSTERServer(Server):
        "server enhancements for a glusterfs backend"""

        @classmethod
        def _attr_unpack_dict(cls, xattr, extra_fields = ''):
            """generic volume mark fetching/parsing backed"""
            fmt_string = cls.NTV_FMTSTR + extra_fields
            buf = Xattr.lgetxattr('.', xattr, struct.calcsize(fmt_string))
            vm = struct.unpack(fmt_string, buf)
            m = re.match('(.{8})(.{4})(.{4})(.{4})(.{12})', "".join(['%02x' % x for x in vm[2:18]]))
            uuid = '-'.join(m.groups())
            volinfo = {  'version': vm[0:2],
                         'uuid'   : uuid,
                         'retval' : vm[18],
                         'volume_mark': vm[19:21],
                      }
            if extra_fields:
                return volinfo, vm[-len(extra_fields):]
            else:
                return volinfo

        @classmethod
        def foreign_volume_infos(cls):
            """return list of valid (not expired) foreign volume marks"""
            dict_list = []
            xattr_list = Xattr.llistxattr_buf('.')
            for ele in xattr_list:
                if ele.find('.'.join([cls.GX_NSPACE, 'volume-mark', ''])) == 0:
                    d, x = cls._attr_unpack_dict(ele, cls.FRGN_XTRA_FMT)
                    now = int(time.time())
                    if x[0] > now:
                        logging.debug("volinfo[%s] expires: %d (%d sec later)" % \
                                      (d['uuid'], x[0], x[0] - now))
                        dict_list.append(d)
                    else:
                        try:
                            Xattr.lremovexattr('.', ele)
                        except OSError:
                            pass
            return dict_list

        @classmethod
        def native_volume_info(cls):
            """get the native volume mark of the underlying gluster volume"""
            try:
                return cls._attr_unpack_dict('.'.join([cls.GX_NSPACE, 'volume-mark']))
            except OSError:
                ex = sys.exc_info()[1]
                if ex.errno != ENODATA:
                    raise

    server = GLUSTERServer

    def __init__(self, path):
        self.host, self.volume = sup(self, path, '^(%s):(.+)' % HostRX.pattern)

    def canonical_path(self):
        return ':'.join([gethostbyname(self.host), self.volume])

    def can_connect_to(self, remote):
        """determine our position in the connectibility matrix"""
        return True

    def connect(self):
        """inhibit the resource beyond

        - create temprorary mount point
        - call glusterfs to mount the volume over there
        - change to mounted fs root
        - lazy umount + delete temp. mount point
        """
        def umount_l(d):
            po = Popen(['umount', '-l', d], stderr=subprocess.PIPE)
            po.wait()
            return po
        d = tempfile.mkdtemp(prefix='gsyncd-aux-mount-')
        mounted = False
        try:
            po = Popen(gconf.gluster_command.split() + \
                       (gconf.gluster_log_level and ['-L', gconf.gluster_log_level] or []) + \
                       ['-l', gconf.gluster_log_file, '-s', self.host,
                        '--volfile-id', self.volume, '--client-pid=-1', d],
                       stderr=subprocess.PIPE)
            po.wait()
            po.terminate_geterr()
            mounted = True
            logging.debug('auxiliary glusterfs mount in place')
            os.chdir(d)
            umount_l(d).terminate_geterr()
            mounted = False
        finally:
            try:
                if mounted:
                    umount_l(d).terminate_geterr(fail_on_err = False)
                os.rmdir(d)
            except:
                logging.warn('stale mount possibly left behind on ' + d)
        logging.debug('auxiliary glusterfs mount prepared')

    def connect_remote(self, *a, **kw):
        sup(self, *a, **kw)
        self.slavedir = "/proc/%d/cwd" % self.server.pid()

    def service_loop(self, *args):
        """enter service loop

        - if slave given, instantiate GMaster and
          pass control to that instance, which implements
          master behavior
        - else do that's what's inherited
        """
        if args:
            GMaster(self, args[0]).crawl_loop()
        else:
            sup(self, *args)

    def rsync(self, files):
        return sup(self, files, self.slavedir)


class SSH(AbstractUrl, SlaveRemote):
    """scheme class for ssh:// urls

    interface to remote slave on master side
    implementing an ssh based proxy
    """

    def __init__(self, path):
        self.remote_addr, inner_url = sup(self, path,
                                          '^((?:%s@)?%s):(.+)' % tuple([ r.pattern for r in (UserRX, HostRX) ]))
        self.inner_rsc = parse_url(inner_url)

    def canonical_path(self):
        m = re.match('([^@]+)@(.+)', self.remote_addr)
        if m:
            u, h = m.groups()
        else:
            u, h = pwd.getpwuid(os.geteuid()).pw_name, self.remote_addr
        remote_addr = '@'.join([u, gethostbyname(h)])
        return ':'.join([remote_addr, self.inner_rsc.get_url(canonical=True)])

    def can_connect_to(self, remote):
        """determine our position in the connectibility matrix"""
        return False

    def start_fd_client(self, *a, **opts):
        """customizations for client startup

        - be a no-op if we are to daemonize (client startup is deferred
          to post-daemon stage)
        - determine target url for rsync after consulting server
        """
        if opts.get('deferred'):
            return a
        sup(self, *a)
        ityp = type(self.inner_rsc)
        if ityp == FILE:
            slavepath = self.inner_rsc.path
        elif ityp == GLUSTER:
            slavepath = "/proc/%d/cwd" % self.server.pid()
        else:
            raise NotImplementedError
        self.slaveurl = ':'.join([self.remote_addr, slavepath])

    def connect_remote(self, go_daemon=None):
        """connect to inner slave url through outer ssh url

        Wrap the connecting utility in ssh.

        Much care is put into daemonizing: in that case
        ssh is started before daemonization, but
        RePCe client is to be created after that (as ssh
        interactive password auth would be defeated by
        a daemonized ssh, while client should be present
        only in the final process). In that case the action
        is taken apart to two parts, this method is ivoked
        once pre-daemon, once post-daemon. Use @go_daemon
        to deiced what part to perform.

        [NB. ATM gluster product does not makes use of interactive
        authentication.]
        """
        if go_daemon == 'done':
            return self.start_fd_client(*self.fd_pair)
        gconf.setup_ssh_ctl(tempfile.mkdtemp(prefix='gsyncd-aux-ssh-'))
        deferred = go_daemon == 'postconn'
        ret = sup(self, gconf.ssh_command.split() + gconf.ssh_ctl_args + [self.remote_addr], slave=self.inner_rsc.url, deferred=deferred)
        if deferred:
            # send a message to peer so that we can wait for
            # the answer from which we know connection is
            # established and we can proceed with daemonization
            # (doing that too early robs the ssh passwd prompt...)
            # However, we'd better not start the RepceClient
            # before daemonization (that's not preserved properly
            # in daemon), we just do a an ad-hoc linear put/get.
            i, o = ret
            inf = os.fdopen(i)
            repce.send(o, None, '__repce_version__')
            select.select((inf,), (), ())
            repce.recv(inf)
            # hack hack hack: store a global reference to the file
            # to save it from getting GC'd which implies closing it
            gconf.permanent_handles.append(inf)
            self.fd_pair = (i, o)
            return 'should'

    def rsync(self, files):
        return sup(self, files, '-ze', " ".join(gconf.ssh_command.split() + gconf.ssh_ctl_args), self.slaveurl)
