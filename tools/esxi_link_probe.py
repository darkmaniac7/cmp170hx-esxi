#!/usr/bin/env python3
# esxi_link_probe.py: run read-only commands on an ESXi host over SSH.
#
# Side effect on the host: if the TSM-SSH service is stopped, this script STARTS it through the
# vSphere API, runs each command given on the command line over SSH, and then STOPS TSM-SSH again
# (only when this script started it). The service policy and the sshServer firewall ruleset are
# not touched. If the SSH login or a command raises, the stop is skipped and TSM-SSH stays
# running: check it and stop it by hand. TLS certificates and SSH host keys are not verified.
#
# Usage:
#   export ESXI_PASSWORD=...        # or name another variable with --password-env
#   esxi_link_probe.py --host <esxi> [--user root] -- 'lspci -v' 'vsish -e get /hardware/pci/...'
# --host and --user also read ESXI_HOST / ESXI_USER. The password is only read from the environment.
import argparse,os,ssl,time
from pyVim.connect import SmartConnect, Disconnect
from pyVmomi import vim
import paramiko
ap=argparse.ArgumentParser(description="Run read-only commands on an ESXi host over SSH, starting TSM-SSH temporarily if it is stopped.")
ap.add_argument("--host",default=os.environ.get("ESXI_HOST",""),help="ESXi host address [env ESXI_HOST]")
ap.add_argument("--user",default=os.environ.get("ESXI_USER") or "root",help="ESXi account (default root) [env ESXI_USER]")
ap.add_argument("--password-env",default="ESXI_PASSWORD",help="name of the environment variable holding the password (default ESXI_PASSWORD)")
ap.add_argument("cmd",nargs="*",help="commands to run, one argument each (put -- before them)")
a=ap.parse_args()
if not a.host: ap.error("--host or ESXI_HOST is required")
if not os.environ.get(a.password_env): ap.error(f"environment variable {a.password_env} is empty or unset")
c={"host":a.host,"user":a.user,"password":os.environ[a.password_env]}
ctx=ssl._create_unverified_context()
si=SmartConnect(host=c["host"],user=c["user"],pwd=c["password"],sslContext=ctx)
try:
    host=si.content.viewManager.CreateContainerView(si.content.rootFolder,[vim.HostSystem],True).view[0]
    svc=host.configManager.serviceSystem
    ssh=[s for s in svc.serviceInfo.service if s.key=="TSM-SSH"][0]
    was=ssh.running; print("TSM-SSH running before:",was, "policy:",ssh.policy)
    if not was: svc.StartService("TSM-SSH"); time.sleep(4)
    cl=paramiko.SSHClient(); cl.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    cl.connect(c["host"],22,c["user"],c["password"],timeout=20,look_for_keys=False,allow_agent=False)  # (hostname, port, username, password)
    for cmd in a.cmd:
        i,o,e=cl.exec_command(cmd,timeout=60); out=o.read().decode(errors="replace"); err=e.read().decode(errors="replace")
        print("### "+cmd); print(out); 
        if err.strip(): print("[stderr]",err[:1000])
    cl.close()
    if not was:
        svc.StopService("TSM-SSH"); time.sleep(2)
        ssh=[s for s in svc.serviceInfo.service if s.key=="TSM-SSH"][0]; print("TSM-SSH running after:",ssh.running)
finally:
    Disconnect(si)
