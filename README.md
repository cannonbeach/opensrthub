# opensrthub

### A web based SRT hub/gateway signal router

This project (opensrthub) is a SRT hub/gateway for routing streaming transport stream signals around a network (and the Internet) with SRT and UDP.  

My goal for this project was to have a straightforward and user friendly web based platform for managing the routing of audio/video streaming signals transported
over SRT and UDP around public/private networks (while also providing the ability to peek at the signals along the way).  The signal peeking adds some extra value since you
can view thumbnails, bitrates, codecs as well as information about signal loss.  There is also an API available if you choose to build your own front-end or want to automate some things.

<img width="1906" height="935" alt="image" src="https://github.com/user-attachments/assets/c0a19d9b-0687-4cce-b222-d3856c505f8b" />

<img width="1666" height="1035" alt="image" src="https://github.com/user-attachments/assets/bcdc2ea7-7c8c-460c-9fba-30beb078c805" />

### Quickstart Setup Instructions (Ubuntu 24.04 Server)

```
git clone https://github.com/cannonbeach/opensrthub.git
cd opensrthub
./setup.sh
```

That's it. The installer asks once for a login password, then does everything
else unattended: system packages, Node.js, host tuning, the libsrt/libcurl/FFmpeg
builds, the `srthub` binary, the `dockersrthub` container image, a TLS
certificate, and the `opensrthub` service registered to start at boot. It
finishes by checking its own work and printing the URL to open.

Expect 15-30 minutes on first run, almost all of it compiling FFmpeg.

The libcurl build suppresses autoconf's obsolete-macro warnings
(`WARNINGS=no-obsolete`). autoconf 2.70 turned `-Wobsolete` on by default and this
curl fork predates the `AC_HELP_STRING` rename, so leaving them on produces around
660 lines of harmless noise. Genuine syntax and portability warnings still show.

Then browse to **https://your-server:8080** and log in as `admin` with the
password you chose. Your browser will warn about the certificate because it is
self-signed; replace `/var/app/cert/server.{key,crt}` with a real certificate to
remove the warning.

To add or change logins later, edit `/opt/srthub/users.json`.

Next: select **New SRT Receiver** or **New SRT Server**, and save the configuration.

#### Host tuning

The installer treats the machine as a dedicated streaming appliance and adjusts
the host accordingly. Pass `--skip-tuning` to skip all of it if you manage host
configuration with Ansible, cloud-init or similar.

This phase runs *after* everything is built and installed, so nothing it changes
can affect the compile or the container image build.

- **Kernel parameters** - `apparmor=0`,
  `cpufreq.default_governor=performance` and `mitigations=off` are added to
  `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub` (backed up once to
  `/etc/default/grub.opensrthub.bak`), followed by `update-grub`. Each is added
  only if it is not already on the command line - if both are already there,
  nothing is written and `update-grub` is not run. Your existing parameters are
  preserved, and a parameter already present with a different value (say
  `cpufreq.default_governor=powersave`) is corrected rather than duplicated.
  **These need a reboot to take effect.**

  To manage another parameter, add it to the `GRUB_PARAMS` list at the top of
  `setup.sh`; everything else follows automatically.

  `grub-mkconfig` sources `/etc/default/grub` *before* `/etc/default/grub.d/*.cfg`,
  so a drop-in there overrides it - Ubuntu cloud images ship
  `50-cloudimg-settings.cfg`, which reassigns `GRUB_CMDLINE_LINUX_DEFAULT` and
  would otherwise silently discard the parameters. The installer detects this and
  adds `/etc/default/grub.d/99-opensrthub.cfg` to win the ordering, carrying over
  whatever that drop-in set. It then confirms each parameter is actually present
  in the generated `/boot/grub/grub.cfg` rather than assuming the edit worked.

  `mitigations=off` disables the CPU speculative-execution mitigations (Spectre,
  Meltdown/PTI, MDS, L1TF, Retbleed, SRSO, Downfall). It is here because those
  mitigations cost most on syscall- and context-switch-heavy code, and a UDP/SRT
  packet mover doing a `recvmsg`/`sendmsg` per packet is close to a worst case for
  that overhead. **This is a deliberate security tradeoff**: you give up
  cross-privilege and cross-process speculative isolation, so it is appropriate
  for a dedicated appliance on a network you control and *not* for a shared or
  multi-tenant host. Note that running streams in containers does not offset this
  - containers share the kernel and are not a speculative-execution boundary.
  It also re-enables SMT if a mitigation had disabled it. Remove it from
  `GRUB_PARAMS` if your threat model does not allow it.

- **AppArmor** - the service is also disabled and masked so it does not come back
  at the next boot. The loaded profiles are deliberately left in place until you
  reboot: Ubuntu's `apparmor.service` sets `ExecStop=/bin/true` precisely so that
  stopping it does not unload the profile set, because unloading it on a running
  system leaves the AppArmor LSM active with no `docker-default` profile to apply,
  and every `docker run` and `docker build` then fails with
  `apparmor failed to apply profile: ... no such file or directory`. The kernel
  parameter is what actually turns AppArmor off, at the next boot.

  Disabling AppArmor is docker-safe: runc calls `apparmor.HostSupports()`, which
  reads `/sys/module/apparmor/parameters/enabled`, and skips profile application
  entirely when AppArmor is unavailable. After rebooting, confirm with
  `cat /sys/module/apparmor/parameters/enabled` (expect `N`), check
  `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` (expect
  `performance`), check `grep . /sys/devices/system/cpu/vulnerabilities/*` (expect
  `Vulnerable`), and check that streams still start.

- **Unattended upgrades** - service masked and the apt periodic counters in
  `/etc/apt/apt.conf.d/20auto-upgrades` set to 0. An automatic upgrade that
  restarts docker or node would interrupt live streams.
- **apt-daily** - `apt-daily.timer`, `apt-daily.service`,
  `apt-daily-upgrade.timer` and `apt-daily-upgrade.service` disabled and masked.
  Both timers are included: masking only the services leaves the timers firing.
- **MOTD** - the execute bit is removed from `/etc/update-motd.d/*`, motd-news is
  disabled, and `/etc/motd` is cleared. `/etc/pam.d` is deliberately left alone,
  since a bad edit there locks you out over SSH.
- **sysctl** - written as a delimited block in `/etc/sysctl.conf` (backed up once
  to `/etc/sysctl.conf.opensrthub.bak`), so re-running replaces the block instead
  of appending duplicates and your own settings are preserved:

```
net.ipv4.tcp_syncookies = 1
net.ipv4.conf.{all,default}.accept_redirects = 0
net.ipv6.conf.{all,default}.accept_redirects = 0
net.ipv6.conf.{all,default,lo}.disable_ipv6 = 1
kernel.randomize_va_space = 2
kernel.core_uses_pid = 1
fs.suid_dumpable = 1
```

`fs.suid_dumpable = 1` is a deliberate loosening of a hardening default: without
it a crash in a privileged process produces no core file at all, which makes
stream faults very hard to diagnose. A core from a privileged process can
contain secrets held in memory, which for opensrthub means SRT stream
passphrases. Cores are owner-read-only; set it to 0 if you don't need crash
diagnostics.

Reboot afterwards so all of it takes effect cleanly - `apparmor=0` in particular
does nothing until then. `--verify` reads the live kernel values rather than the
config files, so it distinguishes "configured" from "actually in force" and marks
the kernel parameter as `[pend]` until you have rebooted.

#### Why the script is called setup.sh

Not `install.sh`, deliberately. curl's `configure.ac` declares no
`AC_CONFIG_AUX_DIR`, so autoconf and `libtoolize` fall back to searching for
`install-sh`, `install.sh` or `shtool` in `.`, then `..`, then `../..`, using the
first directory that has one. An `install.sh` in the repository root is therefore
picked up as the config aux directory by the libcurl build cloned beneath it:
`libtoolize` writes `ltmain.sh` outside the curl tree and `automake` then fails
with `required file './ltmain.sh' not found`.

The libcurl build declares `AC_CONFIG_AUX_DIR([.])` in the cloned tree before
running `buildconf`, which makes it immune to this no matter what sits above the
checkout - including an `install.sh` of your own in a parent directory. The script
still keeps a name that cannot trigger it. Don't rename it back.

(Seeding an `install-sh` into the curl tree does *not* work, for the record:
`install-sh` is on `buildconf`'s own cleanup list, so it is deleted before
`libtoolize` runs.)

#### Installer options

The installer is idempotent - if a step fails, fix the cause and run it again,
and everything already completed is skipped.

```
./setup.sh --verify                    Check an existing installation
./setup.sh --skip-deps                 Rebuild after a git pull (no apt phase)
./setup.sh --skip-deps --skip-build    Reinstall the web app only
./setup.sh --skip-tuning               Leave host settings alone
./setup.sh --service=pm2               Use pm2 instead of systemd
./setup.sh --service=none              Don't register a service at all
./setup.sh --admin-password=PW        Unattended install (no password prompt)
./setup.sh --help                      Full option list
```

A full log of every run is written to `opensrthub-install.log` in the repository.

#### Managing the service

```
sudo systemctl status opensrthub      # is it running?
sudo systemctl restart opensrthub     # restart it
sudo journalctl -u opensrthub -f      # follow the logs
```

If you installed with `--service=pm2`, use `sudo pm2 status`,
`sudo pm2 restart opensrthub` and `sudo pm2 logs opensrthub` instead.

#### Installing on a different Ubuntu release

No edits required. The installer detects the host release and builds the
container image from a matching base image, which is what keeps the `srthub`
binary loadable inside the container.

#### After changing the code

```
./rebuildcontainer.sh
```

This rebuilds `srthub`, the container image and the web app, and restarts the
service. Existing stream containers must be restarted from the UI to pick up the
new binary.

There are four modes of SRT supported in the current version, which essentialy consists of a combination of Listener and Caller bundled with UDP input/output.  The Rendezvous mode has not yet been added.

```
1. UDP Input to SRT Listener Output (Destination pulls from opensrthub)
2. UDP Input to SRT Caller Output (Push from opensrthub to destination)
3. SRT Caller Input (opensrthub pulls from source) to UDP Output
4. SRT Listener Input (Source pushes to opensrthub) to UDP Output
```

And finally!  Your sponsorship donations are greatly appreciated since I am trying to pay off student loans.  If you find this project useful, then please donate and star it.  I work on this project in my spare time and I am available for consulting projects or customizations (new features, new projects, etc.).  I have a lot of really interesting ideas I'd like to pursue on this project, so drop me an email if you think you might be interested in more than what I am offering right now.

If something doesn't work or you need some assistance, please feel free to email me or post an issue in this project.

Thank you!

### Troubleshooting

Start with the installer's own checks - they cover the usual failure modes
(missing container image, docker not running, service not enabled, port not
answering) in one pass:

```
./setup.sh --verify
```

If the web UI does not come up, check the service logs:

```
sudo systemctl status opensrthub
sudo journalctl -u opensrthub -n 100 --no-pager
```

If the UI works but streams refuse to start, the `dockersrthub` container image
is the thing to check - the web app launches every stream as a container:

```
sudo docker image inspect dockersrthub >/dev/null && echo "image present"
./setup.sh --skip-deps --skip-build      # rebuilds just the image and web app
```

You can also run the application from the command line (./srthub) as well as manually through the Docker image (more on this later).  If for some reason you are not able to start it through the web application, this would be the best place to start.

If you want to run through the command line, you can run it as follows, but first you need to identify the configuration which is stored in /opt/srthub/configs.

```
tapeworm@tapeworm-parasite1-cloud6:~/srthub$ ls -l /opt/srthub/configs
total 12
-rw-r--r-- 1 root root 285 Nov 30 13:53 1701381185.json
-rw-r--r-- 1 root root 312 Dec  7 07:28 1701387240.json
-rw-r--r-- 1 root root 299 Dec  4 07:58 1701705434.json
```

```
tapeworm@tapeworm-parasite1-cloud6:~/srthub$ sudo ./srthub 1701381185
and it will read the configuration file in /opt/srthub/configs/1701381185.json
```

The configuration file format is as follows (and is stored in /opt/srthub/configs):
```
{
   "sourcename":"Live Sports",
   "sourcemode":"srt",
   "sourceaddress":"192.168.86.40",
   "sourceport":"10000",
   "sourceinterface":"eno1",
   "outputmode":"udp",
   "outputaddress":"192.168.86.34",
   "outputport":"18000",
   "outputinterface":"eno1",
   "outputttl":"16",
   "passphrase":"",
   "keysize":"0",
   "streamid":"",
   "managementserverip":"",
   "whitelist":"",   
   "clienttype":"pull",
   "overheadbw":"25",
   "latencyms":"100"
}
```

And since the opensrthub runs under Docker, you can check the status of the containers through the command line as well.  If something is not running correctly, the STATUS field will usually indicate an issue.  You can forcefully stop and remove a container by using sudo docker rm -f, so to remove the below container, you would use the following command: "sudo docker rm -f srthub1701705434"

```
tapeworm@tapeworm-parasite1-cloud6:~$ sudo docker ps
CONTAINER ID   IMAGE          COMMAND                  CREATED      STATUS      PORTS     NAMES
2cfe96d588de   dockersrthub   "/usr/bin/srthub 170…"   8 days ago   Up 8 days             srthub1701705434
tapeworm@tapeworm-parasite1-cloud6:~$
```

### API

```
/api/v1/system_information
/api/v1/backup_services (returns a .zip file of all configurations)
/api/v1/get_service_count
/api/v1/thumbnail/[service]
/api/v1/get_interfaces
/api/v1/remove_service/[service]
/api/v1/new_srt_receiver
/api/v1/new_srt_server
/api/v1/status_update/[service]
/api/v1/stop_service/[service]
/api/v1/start_service/[service]
/api/v1/list_services
/api/v1/get_log_data
/api/v1/get_extended_log_data
/api/v1/get_service_status/[service]
```


