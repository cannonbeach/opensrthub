# opensrthub
This project (opensrthub) is a SRT hub/gateway for routing streaming transport stream signals around a network (and the Internet) with SRT and UDP.

My goal for this project was to have a straightforward and user friendly web based platform for managing the routing of audio/video streaming signals transported
over SRT and UDP around public/private networks (while also providing the ability to peek at the signals along the way).  The signal peeking adds some extra value since you
can view thumbnails, bitrate information, codec information as well as information about signal loss.  There is also an API available if you choose to build your own
front-end or want to automate some things.

![image](https://github.com/cannonbeach/opensrthub/assets/5487649/4ed86e61-dbe4-4e29-91dd-1040e80cf3f1)

Quickstart Setup Instructions (Ubuntu 20.04/22.04 Server Instructions):
1. Clone the repository
2. Execute the setup script (sudo ./setupopensrthub.sh)
3. Navigate your browser to port 8080
4. Select to "Add New SRT Receiver" or "New SRT Server"
5. Save your configuration

There are four modes of SRT supported in the current version, which essentialy consists of a combination of Listener and Caller bundled with UDP input/output.  The Rendezvous mode has not yet been added.

1. UDP Input to SRT Listener Output (Destination pulls from opensrthub)
2. UDP Input to SRT Caller Output (Push from opensrthub to destination)
3. SRT Caller Input (opensrthub pulls from source) to UDP Output
4. SRT Listener Input (Source pushes to opensrthub) to UDP Output

And finally!  Your sponsorship donations are greatly appreciated since I am trying to pay off student loans.  If you find this project useful, then please donate.  I work on this project in my spare time and I am available for consulting projects or customizations (new features, new projects, etc.).  I have a lot of really interesting ideas I'd like to pursue on this project, so drop me an email if you think you might be interested in more than what I am offering right now.

If something doesn't work or you need some assistance, please feel free to email me or post an issue in this project.  This is the initial version of the application (as of December 2023) and will definitely have some quirks.

Thank you!

API (more documentation to be provided)
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



