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

  `cpufreq.default_governor=performance` only sets the governor each cpufreq policy
  *starts* with - it is not sufficient on its own, because two things in userspace
  overwrite it late in boot and the last writer wins. `ondemand.service`, shipped by
  systemd itself, runs `/lib/systemd/set-cpufreq` which forces `ondemand` on every
  CPU; and the `cpufrequtils` init script carries `GOVERNOR="ondemand"` as a
  built-in default, only reading `/etc/default/cpufrequtils` if that file exists. So
  the tuning phase masks `ondemand.service`, writes
  `/etc/default/cpufrequtils` with `GOVERNOR="performance"`, and applies the
  governor immediately rather than waiting for the reboot. `--verify` reads
  `scaling_governor` from every CPU, not the config, since the configured value is
  exactly what used to be overridden.

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
- **Crash reporting** - apport, whoopsie and kerneloops are disabled and masked,
  and `enabled=0` is set in `/etc/default/apport`. A core dump from this box can
  contain whatever was in memory at the time, which for opensrthub includes SRT
  stream passphrases, so nothing should be shipping crash data off the machine on
  its own. whoopsie is the piece that actually uploads to Canonical's error
  tracker; kerneloops does the same for kernel oopses.

  The `apport-autoreport.path` and `apport-forward.socket` units are included, not
  just the services - they are activators, so masking only the services would
  leave something able to start them again.

  `kernel.core_pattern` is repointed at `/opt/srthub/cores/core.%e.%p.%t` in the
  sysctl block, because apport replaces it with a pipe to its own handler.
  Disabling apport without resetting it would hand every core to a program that is
  no longer running, silently producing no core at all and quietly defeating
  `fs.suid_dumpable = 1`.

  That path matters for containers: `core_pattern` is resolved in the crashing
  process's own mount namespace, and `server.js` bind-mounts `/opt/srthub` into
  every stream container at the same path - so a core from srthub inside a
  container lands on the host and survives the container being removed. The
  directory is `1777` because the kernel writes each core as the crashing
  process's own uid; the cores themselves are `0600` and the sticky bit stops one
  user clearing another's, the same arrangement Ubuntu uses for `/var/crash`.

  Cores from a video application are large, so
  `/etc/tmpfiles.d/opensrthub-cores.conf` expires them after 14 days - otherwise an
  appliance that crashloops fills its own disk and takes the service down.
  `systemd-tmpfiles-clean.timer` is active by default and runs daily, so no cron
  job is needed. Adjust the `14d` there to keep them longer.

  Pass `--purge-telemetry` to apt-purge the packages rather than only disabling
  them. That is meant for server installs - on a desktop install, purging apport can
  drag the desktop metapackage out with it. (`--purge-apport` is still accepted as
  an older name for the same flag.)

- **Outbound telemetry** - `popularity-contest` submits the installed package list
  to Canonical weekly and `ubuntu-report` submits a hardware and install survey.
  Both are described as anonymous, but an appliance should not be originating
  traffic to third parties at all, and a package list is itself a disclosure about
  what the machine is and how it is configured.

  `PARTICIPATE="no"` is set in `/etc/popularity-contest.conf`, any
  `popularity-contest` service or timer is masked, and the execute bit is removed
  from `/etc/cron.daily/popularity-contest` - which is what actually does the
  submitting, and which `run-parts` skips when it is not executable.

  `ubuntu-report` ships no service and no config file; it is a CLI invoked by the
  installer and by initial-setup. There is nothing to mask, so the execute bit is
  removed from `/usr/bin/ubuntu-report` instead. **A package upgrade restores it** -
  use `--purge-telemetry` if you want it gone for good.

  Not covered: `ubuntu-advantage-tools` / `ubuntu-pro-client` also contacts
  Canonical (`ua-timer.timer`, `esm-cache.service`, and the apt ESM hooks that
  produce "apt news"). Say so and it can be added; it is left alone for now because
  disabling it also silences genuine security-update notices.

- **MOTD** - the execute bit is removed from `/etc/update-motd.d/*`, motd-news is
  disabled, and `/etc/motd` is cleared. `/etc/pam.d` is deliberately left alone,
  since a bad edit there locks you out over SSH.
- **sshd** - `/etc/ssh/sshd_config.d/10-opensrthub-hardening.conf` restricts the
  crypto and tightens the login policy:

```
Ciphers       chacha20-poly1305@openssh.com, aes256-gcm@openssh.com,
              aes128-gcm@openssh.com, aes256-ctr, aes192-ctr, aes128-ctr
KexAlgorithms curve25519-sha256, curve25519-sha256@libssh.org,
              ecdh-sha2-nistp521/384/256, diffie-hellman-group-exchange-sha256
MACs          hmac-sha2-512-etm@openssh.com, hmac-sha2-256-etm@openssh.com,
              hmac-sha2-512, hmac-sha2-256
PermitRootLogin no        IgnoreRhosts yes            ClientAliveInterval 300
PermitEmptyPasswords no   HostbasedAuthentication no  ClientAliveCountMax 3
LoginGraceTime 60         MaxAuthTries 4
```

  No CBC ciphers, no MD5 or SHA1 MACs, no SHA1 or GSS key exchange, no
  `diffie-hellman-group1`/`group14-sha1`. Note the MACs apply only to the CTR
  ciphers - the AEAD ciphers carry their own integrity and ignore the MAC list.

  `sshd_config` uses the **first** value it finds for each keyword, and the main
  file Includes that directory near its top, so these win over anything below the
  Include. (That is the opposite of `/etc/default/grub`, which is sourced as shell
  and takes the *last* assignment.) The `10-` prefix also puts it ahead of other
  drop-ins such as a cloud image's `50-cloud-init.conf`. If the main config has no
  `Include` line at all, one is added at the top - otherwise the drop-in would be
  written and silently never read.

  **The config is validated with `sshd -t` before anything reloads.** If sshd
  rejects it, the previous drop-in is put back (or the new one deleted), sshd's own
  error is printed, and the install stops - so a bad config can never leave the
  machine without a working sshd. The reload applies to new connections only;
  existing sessions are unaffected.

  **Before you disconnect**, open a second session and confirm you can still log
  in. Two things to be aware of: `PermitRootLogin no` locks out anyone whose only
  access is as root (the installer warns if you are running it as root over SSH),
  and a very old SSH client may not support the restricted algorithm lists.

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
kernel.core_pattern = /opt/srthub/cores/core.%e.%p.%t
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
./setup.sh --purge-telemetry           Remove the crash reporters and phone-home
                                       packages, not just disable them
./setup.sh --service=pm2               Use pm2 instead of systemd
./setup.sh --service=none              Don't register a service at all
./setup.sh --admin-password=PW        Unattended install (no password prompt)
./setup.sh --help                      Full option list
```

A full log of every run is written to `opensrthub-install.log` in the repository.

#### Managing the service

**This changed**: the app now runs under systemd by default, not pm2. pm2 is no
longer installed unless you ask for it with `--service=pm2`.

```
sudo systemctl status opensrthub      # is it running?
sudo systemctl restart opensrthub     # restart it
sudo journalctl -u opensrthub -f      # follow the logs
```

The old pm2 equivalents were `sudo pm2 status`, `sudo pm2 restart opensrthub` and
`sudo pm2 logs opensrthub`. Those still apply if you install with `--service=pm2`.

Why: pm2 needed `pm2 save` *and* `pm2 startup` to survive a reboot (missing the
second is why installs used to come back dead), it required the app to be started
from `/var/app`, and it is an extra global npm dependency. A systemd unit covers
restart-on-failure, boot persistence, working directory and journald logging with
nothing extra installed.

**Upgrading from a pm2 install**: the installer hands over for you. It removes
pm2's `opensrthub` process and re-saves pm2's list before installing the unit, so
the two do not fight over port 8080. pm2's boot hook is left in place in case it
manages other apps - if opensrthub was the only one, `sudo pm2 unstartup systemd`
removes it. Switching back with `--service=pm2` stands the systemd unit down the
same way.

#### Backing up and restoring service configs

**Backup Configs** at the top of the web UI downloads every service config as
`opensrthub-configs-<host>-<date>.tar.gz`. **The archive includes SRT
passphrases in plain text**, so store it accordingly.

**Restore Configs** accepts that archive, a plain `.tar`, or one or more
individual `.json` configs. Restoring takes two steps, and nothing is written
until you press **Restore**:

1. Each file is checked and every config in it is listed as one of:
   - **New** - restored by default.
   - **Already present** - a service with exactly these settings exists, under
     any id. Skipped by default, so restoring the same backup twice does not
     duplicate anything.
   - **Exists** - a service with this id exists with different settings. You
     choose: skip it (the default), replace the existing one, or add the
     backup as a new service. A running service cannot be replaced; stop it
     first.
   - **Invalid** - the file is empty, corrupt, truncated, not a service config,
     or has a setting srthub cannot use. The reason is shown and the file is
     never written.

   Warnings are shown for a service name already in use, a listening port that
   another service already binds, and a network interface this machine does not
   have.
2. On **Restore**, every check runs again against the configs on disk at that
   moment, so a service started or created in between is still caught.

A config restored onto a machine where its id is free keeps that id, so
restoring a backup onto a fresh install brings back the same services.

What restore refuses, and why:

- **Anything but a plain number as the service id.** The id is the config's
  filename, and the start and stop commands pass it to a root shell. Ids come
  only from filenames that are already all digits, or are newly allocated -
  never from a file's contents and never from a path inside an archive.
- **Settings that are not text.** srthub reads every setting as a string and
  crashes on anything else. Whole numbers for numeric settings, such as a port
  written as `9000` rather than `"9000"`, are converted and reported; anything
  else is refused.
- **Quotes, backslashes, control characters, `<` or `>` in a service name,**
  and anything in an address, port or interface name that isn't an address,
  port or interface name. srthub writes those into its event JSON unescaped.
- **Unknown settings,** which are dropped (and listed) rather than copied in.
- **Archive tricks:** nothing is ever extracted to disk, so paths with `..`,
  absolute paths, symlinks, hard links and device files have nothing to act
  on. Links and devices are skipped, and only the last part of each name is
  read. Uploads are limited to 2 MB, and to 16 MB once decompressed, so a
  compression bomb is refused quickly.

#### Multi-program streams (MPTS)

srthub monitors one program of a transport stream: the preview, video and
audio details, PIDs and SCTE-35 all come from that program. Each running
service's card has a **Transport** row that says whether the input is a
single-program (SPTS) or multi-program (MPTS) stream, lists the programs of an
MPTS, and names the one being monitored.

By default that is the first program srthub decodes. To monitor another, edit
the service and pick it under **Program (MPTS)**; the list comes from the
running service, so start it once first. The choice takes effect when the
service is restarted. It is stored as the program number (the `program`
setting in the service config, empty for automatic), so it stays correct if
the programs are reordered. If the stream stops carrying the chosen program,
srthub monitors nothing rather than switching to another one, and the card
says the program was not found.

#### Event log and rotation

Every event the streams raise - signal lock and loss, SRT connections, SCTE-35
cues, source format changes, decode errors - is appended to
`/var/log/srthub.log`, one JSON object per line. The **Event Log** panel in the
web UI reads it a day at a time, with filters for severity, event type, service
and free text, and **Export JSON** / **Export CSV** buttons that download every
matching event for the day you're viewing. Days are UTC days, because that is
how the events are timestamped.

The installer adds `/etc/logrotate.d/srthub` so the file no longer grows without
limit:

```
/var/log/srthub.log {
    daily
    rotate 30
    dateext
    dateyesterday
    missingok
    notifempty
    compress
    delaycompress
    create 0640 root adm
}
```

- **30 days are kept.** That is what the day picker in the UI offers; older
  rotations are deleted by logrotate.
- **Rotations are named for the day their events came from**
  (`srthub.log-20261004`), not the day logrotate ran. The newest rotation stays
  uncompressed for a day, the rest are gzipped; the web app reads both.
- **Rotation runs from Ubuntu's daily logrotate timer**, some time after
  midnight, so each rotated file actually spans from one morning to the next.
  The UI does not depend on the filenames to find a day's events - it picks
  files by when they were written and keeps entries by their own timestamps -
  so a missed or late rotation never hides anything.
- **No `copytruncate`.** The web app opens, appends and closes the file for
  every event, so renaming it out from under the app loses nothing. If that
  ever changes to a file held open, the config needs `copytruncate` or events
  will be written into the rotated file.
- **If the machine was off across a rotation** and logrotate catches up twice in
  one day, the second run finds its target name taken and skips. That day's
  events stay in the live file until the next rotation; nothing is lost.

This is installed on every run, including `--skip-tuning`, since it is part of
the web app rather than host tuning. On an existing install the first rotation
moves the whole accumulated log aside as a single rotated file, which ages out
after 30 days like any other.

The `/api/v1/backup_services` zip (see [API](#api)) includes every rotation still
on disk, not just the live file.

To rotate immediately, or to check the config is accepted:

```
sudo logrotate --force /etc/logrotate.d/srthub
sudo logrotate --debug /etc/logrotate.d/srthub     # dry run, changes nothing
```

`pm2-logrotate`, installed only with `--service=pm2`, is unrelated: it rotates
pm2's capture of the web app's own console output, not the event log.

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
/api/v1/backup_services (returns a .zip support bundle: configurations and logs)
/api/v1/backup_configs (returns a .tar.gz of every service config)
/api/v1/restore_configs?mode=inspect|apply (POST a .tar.gz or a .json as application/octet-stream)
/api/v1/get_log_days
/api/v1/get_log_day?date=YYYY-MM-DD
/api/v1/export_log_day?date=YYYY-MM-DD&format=json|csv
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


