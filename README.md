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
