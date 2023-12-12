# opensrthub

### A web based SRT hub/gateway signal router

This project (opensrthub) is a SRT hub/gateway for routing streaming transport stream signals around a network (and the Internet) with SRT and UDP.  

My goal for this project was to have a straightforward and user friendly web based platform for managing the routing of audio/video streaming signals transported
over SRT and UDP around public/private networks (while also providing the ability to peek at the signals along the way).  The signal peeking adds some extra value since you
can view thumbnails, bitrates, codecs as well as information about signal loss.  There is also an API available if you choose to build your own front-end or want to automate some things.

![image](https://github.com/cannonbeach/opensrthub/assets/5487649/4ed86e61-dbe4-4e29-91dd-1040e80cf3f1)

### Quickstart Setup Instructions (Ubuntu 22.04 Server Instructions):

```
1. Clone the repository (git clone git@github.com:cannonbeach/opensrthub.git)
2. Execute the setup script (sudo ./setupopensrthub.sh)
3. Navigate your browser to port 8080
   - If the web application does not come up, you may need to navigate in the terminal to the /var/app directory and run the following commands:
   - sudo pm2 start --name 'opensrthub' server.js --wait-ready
   - sudo pm2 save
5. Select to add "New SRT Receiver" or "New SRT Server"
6. Save your configuration
```

If you want to install on a different version of Ubuntu, you will need to update the Dockerfile in the Docker directory to be the same as the version of Ubuntu you are targeting.

There are four modes of SRT supported in the current version, which essentialy consists of a combination of Listener and Caller bundled with UDP input/output.  The Rendezvous mode has not yet been added.

```
1. UDP Input to SRT Listener Output (Destination pulls from opensrthub)
2. UDP Input to SRT Caller Output (Push from opensrthub to destination)
3. SRT Caller Input (opensrthub pulls from source) to UDP Output
4. SRT Listener Input (Source pushes to opensrthub) to UDP Output
```

And finally!  Your sponsorship donations are greatly appreciated since I am trying to pay off student loans.  If you find this project useful, then please donate and star it.  I work on this project in my spare time and I am available for consulting projects or customizations (new features, new projects, etc.).  I have a lot of really interesting ideas I'd like to pursue on this project, so drop me an email if you think you might be interested in more than what I am offering right now.

If something doesn't work or you need some assistance, please feel free to email me or post an issue in this project.  This is the initial version of the application (as of December 2023) and will definitely have some quirks.  I am trying to get to a solid v1.0 release by early next year.

Thank you!

### Troubleshooting 

You can also run the application command line (./srthub) as well as manually through the Docker image (more on this later).  If for some reason you are not able to start it through the web application, this would be the best place to start.

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

### API (more documentation to be provided)

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


