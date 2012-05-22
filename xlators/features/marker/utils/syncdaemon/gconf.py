import os
import tempfile

class GConf(object):
    """singleton class to store globals
       shared between gsyncd modules"""

    ssh_ctl_dir = None
    ssh_ctl_args = None
    cpid = None
    pid_file_owned = False
    log_exit = False
    permanent_handles = []
    log_metadata = {}

    @classmethod
    def setup_ssh_ctl(cls):
        if cls.ssh_ctl_dir:
            return
        cls.ssh_ctl_dir = tempfile.mkdtemp(prefix='gsyncd-aux-ssh-')
        cls.ssh_ctl_args = ["-oControlMaster=auto", "-S", os.path.join(cls.ssh_ctl_dir, "gsycnd-ssh-%r@%h:%p")]

gconf = GConf()
