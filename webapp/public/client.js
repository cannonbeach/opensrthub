console.log('Client-side code running');
document.getElementById("newsrtreceiverpage").style.display = "none";
document.getElementById("newsrtserverpage").style.display = "none";

var srt_receiver_button = document.getElementById('srt_receiver_button');
var srt_server_button = document.getElementById('srt_server_button');
var submit_button_srtreceiver = document.getElementById('submit_button_srtreceiver');
var abort_button_srtreceiver = document.getElementById('abort_button_srtreceiver');
var submit_button_srtserver = document.getElementById('submit_button_srtserver');
var abort_button_srtserver = document.getElementById('abort_button_srtserver');
var backup_button = document.getElementById('backup_button');
var reload_button = document.getElementById('reload_button');
var download_button = document.getElementById('download_button');

function update_videosources(number)
{
    var value = number.value;
}

function strncmp(str1, str2, lgth)
{
    var s1 = (str1+'').substr(0, lgth);
    var s2 = (str2+'').substr(0, lgth);

    return ((s1 == s2) ? 0 : ((s1 > s2) ? 1 : -1));
}

document.addEventListener('click',function(e){
    console.log('button clicked: ', e.target.id);

    var buttonString = e.target.id;
    var button_number = 0;

    const digits = buttonString.split('').filter(item => !isNaN(item));

    if (digits.length > 0) {
        if (digits.length > 2) {
            button_number = digits[digits.length-3]+digits[digits.length-2]+digits[digits.length-1];
        } else if (digits.length > 1) {
            button_number = digits[digits.length-2]+digits[digits.length-1];
        } else {
            button_number = digits[digits.length-1];
        }
    }
    console.log(button_number);

    if (strncmp(buttonString,'start',5) == 0) {
        console.log('the start button was pressed');
        var currentButton = document.getElementById(buttonString);
        var clickedButton = '/api/v1/start_service/'+button_number;
        var resetbuttonString = 'reset_button'+button_number;
        var stopbuttonString = 'stop_service'+button_number;
        var resetButton = document.getElementById(resetbuttonString);
        var stopButton = document.getElementById(stopbuttonString);
        currentButton.disabled = true;
        resetButton.disabled = true;
        stopButton.disabled = true;

        fetch(clickedButton,{method: 'POST'})
            .then(function(response) {
                if (response.ok) {
                    console.log('start clicked confirmed');
                    return;
                }
                throw new Error('Start request failed.');
            })
            .catch(function(error) {
                console.log(error);
            });
    } else if (strncmp(buttonString,'stop',4) == 0) {
        console.log('the stop button was pressed');
        var currentButton = document.getElementById(buttonString);
        var clickedButton = '/api/v1/stop_service/'+button_number;
        var resetbuttonString = 'reset_button'+button_number;
        var startbuttonString = 'start_service'+button_number;
        var resetButton = document.getElementById(resetbuttonString);
        var startButton = document.getElementById(startbuttonString);
        currentButton.disabled = true;
        resetButton.disabled = true;
        startButton.disabled = true;

        fetch(clickedButton,{method: 'POST'})
            .then(function(response) {
                if (response.ok) {
                    console.log('stop clicked confirmed');
                    return;
                }
                throw new Error('Stop request failed.');
            })
            .catch(function(error) {
                console.log(error);
            });
    } else if (strncmp(buttonString,'reset',5) == 0) {
        console.log('the reset button was pressed');
        var currentButton = document.getElementById(buttonString);
        currentButton.disabled = true;
        var clickedButton = '/api/v1/reset_clicked/'+button_number;
        fetch(clickedButton,{method: 'POST'})
            .then(function(response) {
                if (response.ok) {
                    console.log('reset clicked confirmed');
                    return;
                }
                throw new Error('Reset request failed.');
            })
            .catch(function(error) {
                console.log(error);
            });
    } else if (strncmp(buttonString,"log",3) == 0) {
        console.log('the log button was pressed');

        var currentButton = document.getElementById(buttonString);
        var clickedButton = '/api/v1/get_event_log/'+button_number;

        currentButton.disabled = true;

        fetch(clickedButton,{method: 'GET'})
            .then(response => {
                if (response.ok) {
                    return response.text();
                } else {
                    currentButton.disabled = false;
                    return Promise.reject('error: unable to get event log information');
                }
            })

            .then(data => {
                alert(data);
                currentButton.disabled = false;
            })
    } else if (strncmp(buttonString,"remove",6) == 0) {
        console.log('the remove button was pressed');
        var currentButton = document.getElementById(buttonString);
        var clickedButton = '/api/v1/remove_service/'+button_number;

        var result = confirm("Are you sure you want to remove this service?");
        if (result) {
            fetch(clickedButton,{method: 'POST'})
                .then(function(response) {
                    if (response.ok) {
                        console.log('remove clicked confirmed');
                        window.location.reload(true);
                        return;
                    }
                    throw new Error('Remove request failed.');
                })
                .catch(function(error) {
                    console.log(error);
                });
        }
    } else if (strncmp(buttonString,"backup",6) == 0) {
        console.log('the backup button was pressed');
        var currentButton = document.getElementById(buttonString);
        var clickedButton = '/api/v1/backup_services';

        fetch(clickedButton,{method: 'GET'})
            .then(response => {
                if (response.ok) {
                    return response.text();
                } else {
                    return Promise.reject('error: unable to get backup of configurations');
                }
            })

            .then(data => {
                // data?
                console.log('need to write out zip file here-- saveas');
            })

        alert("This feature is not yet available");
    }
});

if (srt_receiver_button) {
    srt_receiver_button.addEventListener('click', function(e) {
        console.log('new button was clicked');
        var button = document.getElementById('srt_receiver_button');

        //button.disabled = true;

        document.getElementById("controlpage").style.display = "none";
        document.getElementById("statuspage").style.display = "none";
        document.getElementById("logpage").style.display = "none";
        document.getElementById("logpageheader").style.display = "none";
        document.getElementById("cpusystempage").style.display = "none";
        document.getElementById("newsrtserverpage").style.display = "none";
        document.getElementById("newsrtreceiverpage").style.display = "block";

        //button.disabled = false;
    });
}

if (srt_server_button) {
    srt_server_button.addEventListener('click', function(e) {
        console.log('new button was clicked');
        var button = document.getElementById('srt_server_button');

        //button.disabled = true;

        document.getElementById("controlpage").style.display = "none";
        document.getElementById("statuspage").style.display = "none";
        document.getElementById("logpage").style.display = "none";
        document.getElementById("logpageheader").style.display = "none";
        document.getElementById("cpusystempage").style.display = "none";
        document.getElementById("newsrtserverpage").style.display = "block";
        document.getElementById("newsrtreceiverpage").style.display = "none";

        //button.disabled = false;
    });
}

if (reload_button) {
    reload_button.addEventListener('click', function(e) {
        console.log('restore configurations was clicked');
        alert("This feature is not yet available");
    });
}

if (download_button) {
    download_button.addEventListener('click', function(e) {
        console.log('downloading event logs was clicked');
        alert("This feature is not yet available");
    });
}

function removeOptions(selectElement) {
    var i, L = selectElement.options.length - 1;
    for(i = L; i >= 0; i--) {
        selectElement.remove(i);
    }
}

/*
scan_button_transcode.addEventListener('click', function(e) {
    console.log('scan button was clicked');

    scan_button_transcode.innerText = 'Scanning....';

    var ipaddr_primary = document.getElementById("ipaddr_primary").value;
    var inputinterface1 = document.getElementById("inputinterface1").value;
    var ipaddr_backup = document.getElementById("ipaddr_backup").value;
    var inputinterface2 = document.getElementById("inputinterface2").value;

    var obj = new Object();
    obj.ipaddr_primary = ipaddr_primary;
    obj.inputinterface1 = inputinterface1;
    obj.ipaddr_backup = ipaddr_backup;
    obj.inputinterface2 = inputinterface2;

    var postdata = JSON.stringify(obj);

    console.log(JSON.parse(postdata));

    const url = "/api/v1/scan?address="+ipaddr_primary+"&intf="+inputinterface1;

    fetch(url,{method: 'POST'})
        .then(function(response) {
            if (response.ok) {
                console.log('scan clicked confirmed');

                // set back to the original text
                scan_button_transcode.innerText = 'Scan Sources';

                const scan_data_url = "/api/v1/get_scan_data?address="+ipaddr_primary+"&intf="+inputinterface1;
                fetch(scan_data_url,{method: 'GET'})
                    .then(response => {
                        if (response.ok) {
                            console.log("response came back without issue");
                            return response.text();
                        } else {
                            return Promise.reject('something went wrong!');
                        }
                    })
                    .then(data2 => {
                        console.log('data is', data2);

                        var parsedJSON = JSON.parse(data2);
                        var parsedSources = parsedJSON.sources.length;
                        console.log('parsed: ', parsedJSON.sources.length);
                        if (parsedSources == 0) {
                            select = document.getElementById('inputstream1');
                            removeOptions(select);
                            select.style.visibility = 'hidden';
                            alert("Error!  Unable to scan source!\nCheck IP:PORT and Interface");
                        } else {
                            select = document.getElementById('inputstream1');
                            removeOptions(select);

                            var s;
                            for (s = 0; s < parsedJSON.sources.length; s++) {
                                console.log("prop: " + parsedJSON.sources[s]);
                                var opt = document.createElement('option');
                                opt.value = s;
                                opt.innerHTML = parsedJSON.sources[s];
                                select.appendChild(opt);
                            }
                            select.style.visibility = 'visible';
                        }
                })
                return;
            }
            throw new Error('scan request failed.');
        })
        .catch(function(error) {
            console.log(error);
        });
});
*/

function getSelectedOption(sel) {
    var opt;
    for (var i = 0, len = sel.options.length; i < len; i++) {
        opt = sel.options[i];
        if (opt.selected === true) {
            break;
        }
    }
    return opt;
}

function validate_av_address(source_av, safe) {
    var emptyset = isEmpty(source_av);
    if (emptyset) {
        console.log('invalid source address - missing address');
        safe = 5;
    } else {
        var port = source_av.split(':');
        if (isEmpty(port[0])) {
            console.log('invalid source address - missing address');
            safe = 6;
        } else {
            if (isEmpty(port[1])) {
                console.log('invalid source address - missing port');
                safe = 7;
            } else {
                var numport = parseInt(port[1]);
                if (isNaN(numport)) {
                    console.log('invalid port number');
                    safe = 8;
                }
            }
        }
    }
    return safe;
}

submit_button_srtreceiver.addEventListener('click', function(e) {
    console.log('submit transcode button was clicked');

    var srtreceiver_sourcename = document.getElementById("srtreceiver_sourcename").value;
    var srtreceiver_streamid = document.getElementById("srtreceiver_streamid").value;
    var srtreceiver_passphrase = document.getElementById("srtreceiver_passphrase").value;
    var srtreceiver_sourceaddress = document.getElementById("srtreceiver_sourceaddress").value;
    var srtreceiver_sourceport = document.getElementById("srtreceiver_sourceport").value;
    var srtreceiver_sourceinterface = document.getElementById("srtreceiver_sourceinterface").value;
    var srtreceiver_destinationaddress = document.getElementById("srtreceiver_destinationaddress").value;
    var srtreceiver_destinationport = document.getElementById("srtreceiver_destinationport").value;
    var srtreceiver_destinationinterface = document.getElementById("srtreceiver_destinationinterface").value;
    var srtreceiver_managementserverip = document.getElementById("srtreceiver_managementserverip").value;
    var srtreceiver_clienttype = document.getElementById("srtreceiver_clienttype").value;

    var safe = 1;

    /*
    if (!!srtreceiver_sourcename) {
        safe = 2;
    }
    if (!!srtreceiver_sourceaddress) {
        safe = 3;
    }
    if (!!srtreceiver_sourceport) {
        safe = 4;
    }
    if (isNaN(srtreceiver_sourceport)) {
        safe = 4;
    }
    if (srtreceiver_sourceport < 1 || srtreceiver_sourceport > 65535) {
        safe = 4;
    }
    if (!!srtreceiver_destinationaddress) {
        safe = 5;
    }
    if (!!srtreceiver_destinationport) {
        safe = 6;
    }
    if (isNaN(srtreceiver_destinationport)) {
        safe = 6;
    }
    if (srtreceiver_destinationport < 1 || srtreceiver_destinationport > 65535) {
        safe = 6;
    }
    */

    if (safe == 1) {
        document.getElementById("controlpage").style.display = "block";
        document.getElementById("statuspage").style.display = "block";
        document.getElementById("logpage").style.display = "block";
        document.getElementById("logpageheader").style.display = "block";
        document.getElementById("cpusystempage").style.display = "block";
        document.getElementById("newsrtreceiverpage").style.display = "none";
        //document.getElementById("newsrtserverpage").style.display = "none";

        var obj = new Object();
        obj.srtreceiver_sourcename = srtreceiver_sourcename;
        obj.srtreceiver_streamid = srtreceiver_streamid;
        obj.srtreceiver_passphrase = srtreceiver_passphrase;
        obj.srtreceiver_sourceport = srtreceiver_sourceport;
        obj.srtreceiver_sourceinterface = srtreceiver_sourceinterface;
        obj.srtreceiver_sourceaddress = srtreceiver_sourceaddress;
        obj.srtreceiver_destinationaddress = srtreceiver_destinationaddress;
        obj.srtreceiver_destinationport = srtreceiver_destinationport;
        obj.srtreceiver_destinationinterface = srtreceiver_destinationinterface;
        obj.srtreceiver_managementserverip = srtreceiver_managementserverip;
        obj.srtreceiver_clienttype = srtreceiver_clienttype;

        var postdata = JSON.stringify(obj);

        console.log(JSON.parse(postdata));

        const url = "/api/v1/new_srt_receiver";
        const requestinfo = {
            method: 'POST',
            headers: {
                'content-type':'application/json'
            },
            body: postdata
        };

        fetch(url, requestinfo)
            .then(function(data) {
                console.log('Request success: ', data);
                window.location.reload(true);
            }).then(function(error) {
                console.log('Request failure: ', error);
            });

    } else {
        if (safe == 2) {
            alert("Invalid Source Name!");
        } else if (safe == 3) {
            alert("Invalid Source Address!");
        } else if (safe == 4) {
            alert("Invalid Source Port!");
        } else if (safe == 5) {
            alert("Invalid Destination Address!");
        } else if (safe == 6) {
            alert("Invalid Destination Port!");
        } else {
            alert("Invalid Configuration!");
        }
    }
});

submit_button_srtserver.addEventListener('click', function(e) {
    console.log('submit srtserver button was clicked');

    var srtserver_sourcename = document.getElementById("srtserver_sourcename").value;
    var srtserver_sourceaddress = document.getElementById("srtserver_sourceaddress").value;
    var srtserver_sourceport = document.getElementById("srtserver_sourceport").value;
    var srtserver_sourceinterface = document.getElementById("srtserver_sourceinterface").value;
    var srtserver_address = document.getElementById("srtserver_address").value;
    var srtserver_port = document.getElementById("srtserver_port").value;
    var srtserver_interface = document.getElementById("srtserver_interface").value;
    var srtserver_streamid = document.getElementById("srtserver_streamid").value;
    var srtserver_passphrase = document.getElementById("srtserver_passphrase").value;
    var srtserver_servertype = document.getElementById("srtserver_servertype").value;
    var srtserver_connectionqueue = document.getElementById("srtserver_connectionqueue").value;
    var srtserver_whitelist = document.getElementById("srtserver_whitelist").value;

    var safe = 1;

    if (safe == 1) {
        document.getElementById("controlpage").style.display = "block";
        document.getElementById("statuspage").style.display = "block";
        document.getElementById("logpage").style.display = "block";
        document.getElementById("logpageheader").style.display = "block";
        document.getElementById("cpusystempage").style.display = "block";
        document.getElementById("newsrtreceiverpage").style.display = "none";
        document.getElementById("newsrtserverpage").style.display = "none";

        var obj = new Object();
        obj.srtserver_sourcename = srtserver_sourcename;
        obj.srtserver_sourceaddress = srtserver_sourceaddress;
        obj.srtserver_sourceport = srtserver_sourceport;
        obj.srtserver_sourceinterface = srtserver_sourceinterface;
        obj.srtserver_address = srtserver_address;
        obj.srtserver_port = srtserver_port;
        obj.srtserver_interface = srtserver_interface;
        obj.srtserver_streamid = srtserver_streamid;
        obj.srtserver_passphrase = srtserver_passphrase;
        obj.srtserver_servertype = srtserver_servertype;
        obj.srtserver_connectionqueue = srtserver_connectionqueue;
        obj.srtserver_managementserverip = srtserver_managementserverip;

        var postdata = JSON.stringify(obj);

        console.log(JSON.parse(postdata));

        const url = "/api/v1/new_srt_server";
        const requestinfo = {
            method: 'POST',
            headers: {
                'content-type':'application/json'
            },
            body: postdata
        };

        fetch(url, requestinfo)
            .then(function(data) {
                console.log('Request success: ', data);
                window.location.reload(true);
            }).then(function(error) {
                console.log('Request failure: ', error);
            });

    } else {
        if (safe == 2) {
            alert("Invalid Source Name!");
        } else if (safe == 3) {
            alert("Invalid Source Address!");
        } else if (safe == 4) {
            alert("Invalid Source Port!");
        } else if (safe == 5) {
            alert("Invalid Destination Address!");
        } else if (safe == 6) {
            alert("Invalid Destination Port!");
        } else {
            alert("Invalid Configuration!");
        }
    }
});

abort_button_srtreceiver.addEventListener('click', function(e) {
    console.log('abort button was clicked');

    document.getElementById("controlpage").style.display = "block";
    document.getElementById("statuspage").style.display = "block";
    document.getElementById("logpage").style.display = "block";
    document.getElementById("logpageheader").style.display = "block";
    document.getElementById("cpusystempage").style.display = "block";
    document.getElementById("newsrtreceiverpage").style.display = "none";
    document.getElementById("newsrtserverpage").style.display = "none";

    alert("Aborted! Changes Not Saved!");
});

abort_button_srtserver.addEventListener('click', function(e) {
    console.log('abort button was clicked');

    document.getElementById("controlpage").style.display = "block";
    document.getElementById("statuspage").style.display = "block";
    document.getElementById("logpage").style.display = "block";
    document.getElementById("logpageheader").style.display = "block";
    document.getElementById("cpusystempage").style.display = "block";
    document.getElementById("newsrtreceiverpage").style.display = "none";
    document.getElementById("newsrtserverpage").style.display = "none";

    alert("Aborted! Changes Not Saved!");
});

function scan_service(serviceip, serviceinterface) {
    this.serviceip = serviceip;
    this.serviceinterface = serviceinterface;
}

function isEmpty(str) {
    return (!str || 0 === str.length);
}


/*
scan_button_repackage.addEventListener('click', function(e) {
    console.log('scan button was clicked');

    var clickedButton = '/api/v1/scan';

    var data = new Object();
    var sources = 0;
    var i;
    var services_to_scan = [];
    var repackage_sourcename = document.getElementById('repackage_sourcename').value;

    for (i = 0; i < 4; i++) {  // need to enable configurable number of source services
        var i1 = i + 1;
        var scan_ipaddr = 'repackage_ipaddr'+i1;
        var scan_interface = 'repackage_inputinterface'+i1;
        var scanipaddr = document.getElementById(scan_ipaddr).value;
        var scaninterface = document.getElementById(scan_interface).value;

        var emptyset = (isEmpty(scanipaddr) || isEmpty(scaninterface));

        if (emptyset && (sources == 0)) {
            alert("Invalid IP/Port Combination");
            return;
        }

        if (!emptyset) {
            var port = scanipaddr.split(":");
            if (isEmpty(port[0])) {
                alert("Invalid IP Address Specified");
                return;
            }
            if (isEmpty(port[1])) {
                alert("Invalid Port Specified (please format as IPADDR:PORT)");
                return;
            } else {
                var numport = parseInt(port[1]);
                if (isNaN(numport)) {
                    alert("Invalid Port Specified (please format as IPADDR:PORT)");
                    return;
                }
            }

            var service_scan = new scan_service(scanipaddr, scaninterface);
            services_to_scan.push(service_scan);
            sources++;
        }
    }

    data.name = repackage_sourcename;
    data.services = sources;
    data.service_list = JSON.stringify(services_to_scan);

    var postdata = JSON.stringify(data);

    console.log("data is: ", JSON.parse(postdata));

    for (i = 0; i < sources; i++) {
        var i1 = i + 1;
        var scanlabel = 'scan_label'+i1;
        console.log("sources: ", sources);
        console.log("variable: ", scanlabel);
        document.getElementById(scanlabel).style.visibility = 'visible';
    }

    fetch(clickedButton,{
        method: 'POST',
        headers: {
            'content-type': 'application/json'
        },
        body: postdata
    }).then(function(response) {
        for (i = 0; i < sources; i++) {
            var i1 = i + 1;
            var scanlabel = 'scan_label'+i1;
            document.getElementById(scanlabel).style.visibility = 'hidden';
        }
        if (response.ok) {
            console.log('response: ', response.text());
            console.log('scan clicked confirmed');
            return;
        }
        throw new Error('scan request failed.');
    }).catch(function(error) {
        for (i = 0; i < sources; i++) {
            var i1 = i + 1;
            var scanlabel = 'scan_label'+i1;
            document.getElementById(scanlabel).style.visibility = 'hidden';
        }
        console.log(error);
    });
});
*/

function trigger_image_update()
{
    var images = document.getElementsByTagName('img');

    for (var i = 0; i < images.length; i++) {
        var img = images[i];

        if (img.src.length >= 0 & img.id != 'idImageNoTimestamp') {
            var d = new Date;
            var http = img.src;
            if (http.indexOf("?=") != -1) {
                http = http.split("?=")[0];
            }
            img.src = http + '?=' + d.getTime();
        }
    }
}

var toHHMMSS = (secs) => {
    var sec_num = parseInt(secs, 10)
    var days    = Math.floor(sec_num / 86400)
    var hours   = Math.floor(sec_num / 3600) % 24
    var minutes = Math.floor(sec_num / 60) % 60
    var seconds = sec_num % 60
    return [days,hours,minutes,seconds]
        .map(v => v < 10 ? "0" + v : v)
        .filter((v,i) => v !== "00" || i > 0)
        .join(":")
}

function request_log_data(service)
{
    var serviceQuery = '/api/v1/get_log_data';
    var i;
    fetch(serviceQuery,{method: 'GET'})
        .then(serviceresponse => {
            if (serviceresponse.ok) {
                return serviceresponse.text();
            } else {
                for (i = 0; i < 6; i++) {
                    var p = i+1;
                    var elementname_logentry = 'logentry'+p;
                    var elementname_logstatus = 'logstatus'+p;
                    var elementname_logtime = 'logtime'+p;
                    var elementname_logid = 'logid'+p;
                    var elementname_logmessage = 'logmessage'+p;

                    document.getElementById(elementname_logentry).innerHTML = ' '+p+' ';
                    document.getElementById(elementname_logstatus).innerHTML = '';
                    document.getElementById(elementname_logmessage).innerHTML = 'NO EVENTS REPORTED';
                }
            }
        })
        .then(servicedata => {
            var service_words = JSON.parse(servicedata);
            for (i = 0; i < 6; i++) {
                var p = i+1;
                var elementname_logentry = 'logentry'+p;
                var elementname_logstatus = 'logstatus'+p;
                var elementname_logtime = 'logtime'+p;
                var elementname_logid = 'logid'+p;
                var elementname_logmessage = 'logmessage'+p;

                //console.log(service_words.length);
                if (service_words[i]) {
                    var logstatus_string = service_words[i].status;
                    var logstatus_time = service_words[i].accesstime;
                    var logstatus_id = service_words[i].id;
                    var logstatus_message = service_words[i].message;

                    document.getElementById(elementname_logentry).innerHTML = ' '+p+' ';
                    document.getElementById(elementname_logstatus).innerHTML = logstatus_string;
                    document.getElementById(elementname_logmessage).innerHTML = logstatus_message;
                    document.getElementById(elementname_logid).innerHTML = logstatus_id;
                    document.getElementById(elementname_logtime).innerHTML = logstatus_time;
                } else {
                    document.getElementById(elementname_logentry).innerHTML = ' '+p+' ';
                    document.getElementById(elementname_logstatus).innerHTML = '';
                    document.getElementById(elementname_logmessage).innerHTML = 'NO EVENTS REPORTED';
                }
            }
        })
}

function request_service_status(service)
{
    var serviceQuery = '/api/v1/get_service_status/'+service;
    console.log('querying: ', serviceQuery);
    fetch(serviceQuery,{method: 'GET'})
        .then(serviceresponse => {
            if (serviceresponse.ok) {
                return serviceresponse.text();
            } else {
                var elementname_active = 'active'+service;
                var elementname_uptime = 'uptime'+service;
                var stopbuttonString = 'stop_service'+service;
                var resetbuttonString = 'reset_button'+service;
                var startbuttonString = 'start_service'+service;
                var removebuttonString = 'remove'+service;
                var stopButton = document.getElementById(stopbuttonString);
                var resetButton = document.getElementById(resetbuttonString);
                var startButton = document.getElementById(startbuttonString);
                var removeButton = document.getElementById(removebuttonString);
                var elementname_source = 'input'+service;
                var elementname_output = 'output'+service;
                var elementname_status = 'statusinfo'+service;
                stopButton.disabled = true;
                resetButton.disabled = true;
                startButton.disabled = false;
                removeButton.disabled = false;
                document.getElementById(elementname_active).innerHTML = '<p style="color:red">WAITING</p>';
                document.getElementById(elementname_uptime).innerHTML = '<p style="color:grey">N/A</p>';
                return Promise.reject('error: unable to get service update: '+service);
            }
        })
        .then(servicedata => {
            var service_words = JSON.parse(servicedata);
            var input_string = '';
            var output_string = '';
            var elementname_source = 'input'+service;
            var elementname_output = 'output'+service;
            var current_uptime = service_words.uptime;
            var status_string = '';
            var sourcemode = service_words.sourcemode;
            var outputmode = service_words.outputmode;
            var serverdescription = '';

            if (sourcemode == 'srt') {
                // why did I do this differently?
                sourcemode = sourcemode + ' (' + service_words.srtreceiver_srtmode + ')';
            }

            if (outputmode == 'srt') {
                if (service_words.servertype == 'pull') {
                    serverdescription = 'Listener';
                } else if (service_words.servertype == 'push') {
                    serverdescription = 'Caller';
                } else {
                    serverdescription = 'Unknown';
                }
                outputmode = outputmode + ' (' + serverdescription + ')';
            }

            input_string += '<p>Source Mode: '+sourcemode+'<br>';
            input_string += '<p>Source Interface: '+service_words.sourceinterface+'<br>';
            input_string += '<p>Source Address: '+service_words.sourceaddress+'<br>';
            input_string += '<p>Source Port: '+service_words.sourceport+'<br>';
            input_string += '</p>';

            output_string += '<p>Output Mode: '+outputmode+'<br>';
            output_string += '<p>Output Interface: '+service_words.outputinterface+'<br>';
            output_string += '<p>Output Address: '+service_words.outputaddress+'<br>';
            output_string += '<p>Output Port: '+service_words.outputport+'<br>';
            output_string += '</p>';

            document.getElementById(elementname_source).innerHTML = input_string;
            document.getElementById(elementname_output).innerHTML = output_string;

            if (current_uptime == -1) {
                var elementname_active = 'active'+service;
                var elementname_uptime = 'uptime'+service;
                var stopbuttonString = 'stop_service'+service;
                var resetbuttonString = 'reset_button'+service;
                var startbuttonString = 'start_service'+service;
                var removebuttonString = 'remove'+service;
                var stopButton = document.getElementById(stopbuttonString);
                var resetButton = document.getElementById(resetbuttonString);
                var startButton = document.getElementById(startbuttonString);
                var removeButton = document.getElementById(removebuttonString);
                var elementname_status = 'statusinfo'+service;

                document.getElementById(elementname_active).innerHTML = '<p style="color:grey">INACTIVE</p>';
                document.getElementById(elementname_uptime).innerHTML = '<p style="color:grey">N/A</p>';
                document.getElementById(elementname_status).innerHTML = '<p style="color:grey">SERVICE IS NOT ACTIVE<p>';
                stopButton.disabled = true;
                resetButton.disabled = true;
                startButton.disabled = false;
                removeButton.disabled = false;
            } else {
                var input_signal = service_words.input_signal;
                var startbuttonString = 'start_service'+service;
                var stopbuttonString = 'stop_service'+service;
                var resetbuttonString = 'reset_button'+service;
                var removebuttonString = 'remove'+service;
                var startButton = document.getElementById(startbuttonString);
                var stopButton = document.getElementById(stopbuttonString);
                var resetButton = document.getElementById(resetbuttonString);
                var removeButton = document.getElementById(removebuttonString);
                var elementname_active = 'active'+service;
                var elementname_uptime = 'uptime'+service;
                var elementname_image = 'thumbnail'+service;
                var elementname_event = 'event'+service;
                var elementname_status = 'statusinfo'+service;
                var active_string = '';

                if (service_words.sourcemode == "srt") {
                    if (service_words.srtreceiver_connected == 1) {
                        active_string += '<p style="color:blue">INPUT: SRT CONNECTED</p>';
                        status_string += '<p>';
                        status_string += 'SRT Receiver Bitrate '+service_words.srtreceiver_bitratekbps+'kbps<br>';
                        status_string += 'SRT Packets Received '+service_words.srtreceiver_packetsreceived+'<br>';
                        status_string += 'SRT Packets Lost '+service_words.srtreceiver_packetslost+'<br>';
                        status_string += 'SRT Loss Percentage '+service_words.srtreceiver_losspercentage+'%<br>';
                        status_string += 'SRT RTT '+service_words.srtreceiver_rtt+'ms<br>';
                        status_string += '</p>';

                        if (service_words.srtreceiver_srtmode == 'Listener') {
                            status_string += '<p>';
                            status_string += 'SRT Client Connected From '+service_words.srtreceiver_clientaddress+':'+service_words.srtreceiver_clientport+'<br>';
                            status_string += '</p>';
                        }
                    } else {
                        active_string += '<p style="color:red">INPUT: NO SRT CONNECTION</p>';
                        current_uptime = 0;
                    }
                } else if (service_words.sourcemode == "udp") {
                    if (service_words.udpreceiver_active) {
                        active_string += '<p style="color:blue">INPUT: UDP ACTIVE</p>';
                        status_string += '<p>';
                        status_string += 'UDP Receiver Bitrate '+service_words.udpreceiver_bitratekbps+'kbps<br>';
                        status_string += 'UDP Packets Received '+service_words.udpreceiver_packetsreceived+'<br>';
                        if (service_words.total_streams > 1) {
                            status_string += 'Decoding Stream '+service_words.current_stream+' of '+service_words.total_streams+' (MPTS)<br>';
                        } else {
                            status_string += 'Decoding Stream '+service_words.current_stream+' of '+service_words.total_streams+' (SPTS)<br>';
                        }
                        status_string += 'Video Resolution '+service_words.video_width+'x'+service_words.video_height+'<br>';
                        status_string += 'Video Codec is '+service_words.video_codec+'<br>';
                        status_string += 'Source Format is '+service_words.source_format+'<br>';1
                        status_string += '</p>';
                        status_string += '<p>';
                        status_string += 'Source Continuity Errors '+service_words.transport_source_errors+'<br>';
                        if (service_words.transport_source_errors > 0) {
                            status_string += 'Last Source Error at '+service_words.last_source_error+'<br>';
                        }
                        status_string += '</p>';
                    } else {
                        active_string += '<p style="color:red">INPUT: UDP NO SIGNAL</p>';
                        current_uptime = 0;
                    }
                }

                if (service_words.outputmode == "srt") {
                    var s;
                    var l;
                    s = service_words.srtserver.length;
                    for (l = 0; l < s; l++) {
                        active_string += '<p style="color:blue">OUTPUT: SRT CONNECTED</p>';
                        status_string += '<p>';
                        if (serverdescription == 'Caller') {
                            status_string += 'SRT Client Connected To '+service_words.srtserver[l].clientaddress+':'+service_words.srtserver[l].clientport+'<br>';
                        } else {
                            status_string += 'SRT Client Connected From '+service_words.srtserver[l].clientaddress+':'+service_words.srtserver[l].clientport+'<br>';
                        }

                        status_string += 'Total Packets Sent '+service_words.srtserver[l].totalpacketssent+'<br>';
                        status_string += 'Total Bytes Sent '+service_words.srtserver[l].totalbytessent+'<br>';
                        status_string += '</p>';
                    }
                } else if (service_words.outputmode == "udp") {
                    if (service_words.udpserver_active == 1) {
                        active_string += '<p style="color:blue">OUTPUT: UDP ACTIVE</p>';
                        status_string += '<p>';
                        status_string += 'UDP Packets Sent '+service_words.udpserver_packetssent+'<br>';
                        if (service_words.total_streams > 1) {
                            status_string += 'Decoding Stream '+service_words.current_stream+' of '+service_words.total_streams+' (MPTS)<br>';
                        } else {
                            status_string += 'Decoding Stream '+service_words.current_stream+' of '+service_words.total_streams+' (SPTS)<br>';
                        }
                        status_string += 'Video Resolution '+service_words.video_width+'x'+service_words.video_height+'<br>';
                        status_string += 'Video Codec is '+service_words.video_codec+'<br>';
                        status_string += 'Source Format is '+service_words.source_format+'<br>';
                        status_string += '</p>';
                        status_string += '<p>';
                        status_string += 'Source Continuity Errors '+service_words.transport_source_errors+'<br>';
                        if (service_words.transport_source_errors > 0) {
                            status_string += 'Last Source Error at '+service_words.last_source_error+'<br>';
                        }
                        status_string += '</p>';
                    } else {
                        active_string += '<p style="color:red">OUTPUT: UDP ERROR</p>';
                        current_uptime = 0;
                    }
                }

                document.getElementById(elementname_active).innerHTML = active_string;
                document.getElementById(elementname_uptime).innerHTML = '<p>'+toHHMMSS(current_uptime)+'</p>';
                document.getElementById(elementname_status).innerHTML = status_string;

                trigger_image_update();

                startButton.disabled = true;
                stopButton.disabled = false;
                resetButton.disabled = true;
                removeButton.disabled = true;
            }
        })
}

function get_service_info(services)
{
    console.log('services waited ', services);

    var i;
    for (i = 0; i < services; i++) {
        var currentService = i + 1;
        request_service_status(currentService);
    }
    request_log_data();
}

function update_service_status()
{
    console.log('calling update_service_status()');

    var services = 0;

    fetch('/api/v1/get_service_count',{method: 'GET'})
        .then(response => {
            if (response.ok) {
                return response.text();
            } else {
                return Promise.reject('error: unable to get service count');
            }
        })

        .then(data => {
            var words = JSON.parse(data);
            console.log('services active: ', words.services);
            services = words.services;
            get_service_info(services);
        })

    fetch('/api/v1/system_information',{method: 'GET'})
        .then(response => {
            if (response.ok) {
                return response.text();
            } else {
                return Promise.reject('error: unable to get system information');
            }
        })
        .then(data => {
            var words = JSON.parse(data);
            var cpus;
            var cpuload_total = 0;
            var cpuload_avg;
            console.log(words.cpuinfo.length);
            for (cpus = 0; cpus < words.cpuinfo.length; cpus++) {
                cpuload_total += words.cpuinfo[cpus].percent;
            }
            if (words.cpuinfo.length > 0) {
                cpuload_avg = cpuload_total / words.cpuinfo.length;
            } else {
                cpuload_avg = 0;
            }
            cpuload_avg = Math.round(cpuload_avg*100)/100;
            console.log('Average CPU: '+cpuload_avg);
            var cpucount = words.cpuinfo.length;
            var cpustring = '<p>'+cpucount+' cores</p>';
            var elementname_cpucount = 'cpucount';
            document.getElementById(elementname_cpucount).innerHTML = cpustring;

            var loadstring = '<p>Avg '+cpuload_avg+'%</p>';
            var elementname_cpuload = 'cpuload';
            document.getElementById(elementname_cpuload).innerHTML = loadstring;

            var totalmem = words.totalmem / 1000000;
            var freemem = words.freemem / 1000000;
            var usedmem = totalmem - freemem;
            var percentused = usedmem / totalmem;

            var elementname_hostname = 'hostname';
            var elementname_srtversion = 'srtversion'
            var elementname_srthubversion = 'srthubversion';

            document.getElementById(elementname_hostname).innerHTML = words.system_hostname;
            document.getElementById(elementname_srtversion).innerHTML = words.srt_version;
            document.getElementById(elementname_srthubversion).innerHTML = words.srthub_version;

            totalmem = Math.round(totalmem*100)/100;
            usedmem = Math.round(usedmem*100)/100;
            percentused = Math.round(percentused*1000)/10;

            var totalmemstring = '<p>'+totalmem+'MB</p>';
            var usedmemstring = '<p>'+usedmem+'MB - '+percentused+'%</p>';
            var elementname_totalmem = 'totalmem';
            var elementname_usedmem = 'usedmem';
            document.getElementById(elementname_totalmem).innerHTML = totalmemstring;
            document.getElementById(elementname_usedmem).innerHTML = usedmemstring;
        })
}

setInterval(update_service_status, 1000);
