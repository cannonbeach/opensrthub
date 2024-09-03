/*****************************************************************************
  Copyright (C) 2018-2023 John William

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111, USA.

  This program is also available with customization/support packages.
  For more information, please contact me at cannonbeachgoonie@gmail.com

*******************************************************************************/

console.log('Server-side code running');

var exec = require('child_process').exec;
var os = require('os');
var networkInterfaces = os.networkInterfaces({ all: true });
const express = require('express');
const readLastLines = require('read-last-lines');
var bodyParser = require('body-parser');
const fs = require('fs');
const app = express();
var path = require('path');

app.use(bodyParser.json());

// serve files from the public directory
app.use(express.static('public'));

const logfilename = '/var/log/srthub.log';
const scanFolder = '/opt/srthub/scan';
const configFolder = '/opt/srthub/configs';
const statusFolder = '/opt/srthub/status';
const apacheFolder = '/var/www/html';
const logFolder = '/var/log';

function getExtension(filename) {
    var i = filename.lastIndexOf('.');
    return (i < 0) ? '' : filename.substr(i);
}

function seconds_since_epoch(){ return Math.floor( Date.now() / 1000 ) }

// start the express web server listening on 8080
app.listen(8080, () => {
    console.log('listening on 8080');
});

function getNewestFile(dir, regexp) {
    newest = null;
    files = fs.readdirSync(dir)
    one_matched = 0
    for (i = 0; i < files.length; i++) {
        if (regexp.test(files[i]) == false) {
            continue;
        } else if (one_matched == 0) {
            newest = files[i];
            one_matched = 1;
            continue;
        }

        f1_time = fs.statSync(files[i]).mtime.getTime();
        f2_time = fs.statSync(newest).mtime.getTime();
        if (f1_time > f2_time) {
            newest = files[i]
        }
    }

    if (newest != null) {
        return (dir + '/' + newest);
    }
    return null;
}

var activeconfigurations = 0;

fs.readdir(configFolder, (err, files) => {
    files.forEach(file => {
        console.log(getExtension(file));
        if (getExtension(file) == '.json') {
            activeconfigurations++;
        }
    });
});

cpuIAverage = function(i) {
    var cpu, cpus, idle, len, total, totalIdle, totalTick, type;
    totalIdle = 0;
    totalTick = 0;
    cpus = os.cpus();
    cpu = cpus[i];
    for (type in cpu.times) {
        totalTick += cpu.times[type];
    }
    totalIdle += cpu.times.idle;

    idle = totalIdle / cpus.length;
    total = totalTick / cpus.length;
    return {
        idle: idle,
        total: total
    };
};

cpuILoadInit = function() {
    var index=arguments[0];
    return function() {
        var start;
        start = cpuIAverage(index);
        return function() {
            var dif, end;
            end = cpuIAverage(index);
            dif = {};
            dif.cpu=index;
            dif.idle = end.idle - start.idle;
            dif.total = end.total - start.total;
            dif.percent = 1 - dif.idle / dif.total;
            dif.percent = Math.round(dif.percent*100*100)/100;
            return dif;
        };
    };
};

cpuILoad = (function() {
    var info=[],cpus = os.cpus();
    for (i = 0, len = cpus.length; i < len; i++) {
        var a=cpuILoadInit(i)();
        info.push( a );
    }
    return function() {
        var res=[],cpus = os.cpus();
        for (i = 0, len = cpus.length; i < len; i++) {
            res.push( info[i]() );
        }
        return res;
    }

})();

app.get('/api/v1/system_information', (req, res) => {
    var retdata;
    var srthubcorefile = '/opt/srthub/srthub.json';

    obj = new Object();
    obj.cpuinfo = cpuILoad();
    obj.totalmem = os.totalmem();
    obj.freemem = os.freemem();

    if (fs.existsSync(srthubcorefile)) {
        var systemdata = fs.readFileSync(srthubcorefile, 'utf8');
        var parsedsystemdata = JSON.parse(systemdata);

        obj.srt_version = parsedsystemdata["srt-version"];
        obj.srthub_version = parsedsystemdata["srthub-version"];
        obj.system_hostname = parsedsystemdata.hostname;
    } else {
        obj.srt_version = "Waiting...";
        obj.srthub_version = "Waiting...";
        obj.system_hostname = "Waiting...";
    }

    retdata = JSON.stringify(obj);
    res.send(retdata);
});

app.get('/api/v1/backup_services', (req, res) => {
    var files = fs.readdirSync(configFolder);
    var archiver = require('archiver');
    var zip = archiver('zip');

    zip.on('error', function(err) {
        res.status(500).send({error: err.message});
    });

    zip.on('end', function() {
        console.log('zip file done - wrote %d bytes', zip.pointer());
    });

    res.attachment('backup.zip');
    zip.pipe(res);

    files.forEach(file => {
        console.log(getExtension(file));
        if (getExtension(file) == '.json') {
            var fullfile = configFolder+'/'+file;
            console.log('zipping ', fullfile);
            zip.file(fullfile);
        }
    });
    zip.finalize();
});

app.get('/api/v1/get_service_count', (req, res) => {
    var services;

    obj = new Object();
    var retdata;

    services = activeconfigurations;
    obj.services = services;

    retdata = JSON.stringify(obj);
    res.send(retdata);
});

app.get('/api/v1/get_scan_data', (req, res) => {
    var source;
    var sourcestreams = [];
    var retdata;
    var address = req.query.address;
    var intf = req.query.intf;

    console.log('get_scan_data address: '+address);

    if (address == '' || address == null) {
        var configdata = [];

        obj = new Object();
        obj.sources = configdata;

        retdata = JSON.stringify(obj);

        console.log('sending back scan data: '+retdata);

        res.send(retdata);
    } else {
        var fullfile = scanFolder+'/'+address+'_simple.json';
        if (fs.existsSync(fullfile)) {
            var configdata = fs.readFileSync(fullfile, 'utf8');
            var parsedconfig = JSON.parse(configdata);

            obj = new Object();
            obj.sources = parsedconfig;

            retdata = JSON.stringify(parsedconfig);

            console.log('sending back scan data: '+retdata);

            res.send(retdata);
        } else {
            var configdata = [];

            obj = new Object();
            obj.sources = configdata;

            retdata = JSON.stringify(obj);

            console.log('sending back scan data: '+retdata);

            res.send(retdata);
        }
    }
});

app.get('/api/v1/get_log_page', (req, res) => {
    var html = '';
    var i;

    html += '<table>';
    html += '<thead>';
    html += '<tr class="header">';
    html += '<th>#<div>#</div></th>';
    html += '<th>Severity<div>Severity</div></th>';
    html += '<th>Time<div>Time</div></th>';
    html += '<th>Name<div>Name</div></th>';
    html += '<th>Resource<div>Resource</div></th>';
    html += '<th>Status<div>Status</div></th>';
    html += '</tr>';
    html += '</thead>';
    html += '<tbody>';

    for (i = 0; i < 6; i++) {  // parsed.length
        var p = i+1;
        html += '<tr>';
        html += '<td><div id=\'logentry'+p+'\'></div></td>';
        html += '<td><div id=\'logstatus'+p+'\'></div></td>';
        html += '<td><div id=\'logtime'+p+'\'></div></td>';
        html += '<td><div id=\'logsourcename'+p+'\'></div></td>';
        html += '<td><div id=\'logid'+p+'\'></div></td>';
        html += '<td><div id=\'logmessage'+p+'\'></div></td>';
        html += '</tr>';
    }

    html += '</tbody>';
    html += '</table>';

    res.writeHead(200, {
        'Content-Type': 'text/html',
        'Content-Length': html.length,
        'Expires': new Date().toUTCString()
    });
    res.end(html);
});

app.get('/api/v1/get_control_page', (req, res) => {
    var html = '';
    var i;
    var files = fs.readdirSync(configFolder);
    var listedfiles = 0;

    html += '<table>';
    html += '<thead>';
    html += '<tr class="header">';
    html += '<th>#<div>#</div></th>';
    html += '<th>Name<div>Name</div></th>';
    html += '<th>Control<div>Control</div></th>';
    html += '<th>State<div>State</div></th>';
    html += '<th>Uptime<div>Uptime</div></th>';
    html += '<th>Source<div>Source</div></th>';
    html += '<th>Output<div>Output</div></th>';
    html += '<th>Input Thumbnail<div>Input Thumbnail</div></th>';
    html += '<th>Status<div>Status</div></th>';
    html += '</tr>';
    html += '</thead>';
    html += '<tbody>';

    files.forEach(file => {
        console.log(getExtension(file));
        if (getExtension(file) == '.json') {
            var configindex = listedfiles + 1;
            var fullfile = configFolder+'/'+file;
            var configdata = fs.readFileSync(fullfile, 'utf8');
            var words = JSON.parse(configdata);
            var fileprefix = path.basename(fullfile, '.json');

            listedfiles++;

            html += '<tr>';
            html += '<td>' + fileprefix + '</td>';
            html += '<td>' + words.sourcename + '</td>';
            html += '<td>';
            html += '<button style="width:95%" id=\'start_service'+configindex+'\'>Start </button><br>';
            html += '<button style="width:95%" id=\'stop_service'+configindex+'\'>Stop </button><br>';
            html += '<button hidden style="width:95%" id=\'reset_button'+configindex+'\'>Reset</button>';
            html += '<button style="width:95%" id=\'remove'+configindex+'\' type=\'remove\'>Remove<br>Service</button>';
            html += '</td>';
            html += '<td><div id=\'active'+configindex+'\'></div></td>';
            html += '<td><div id=\'uptime'+configindex+'\'></div></td>';
            html += '<td><div id=\'input'+configindex+'\'></div></td>';

            html += '<td>';
            html += '<table>';
            html += '<tr>';
            html += '<td><div id=\'output'+configindex+'\'></div></td>';
            html += '</tr>';
            html += '<tr>';
            html += '<td><div id=\'event'+configindex+'\'></div></td>';
            html += '</tr>';
            html += '</table>';
            html += '</td>';
            html += '<td>';
            html += '<img src=\'http://'+req.hostname+':8080'+'/api/v1/thumbnail/'+fileprefix+'.jpg?='+ new Date().getTime() +'\' id=\'thumbnail'+fileprefix+'\'/>';
            html += '</td>'
            html += '<td><div id=\'statusinfo'+configindex+'\'></div></td>';
            html += '</tr>';
        }
    })

    html += '</tbody>';
    html += '</table>';

    res.writeHead(200, {
        'Content-Type': 'text/html',
        'Content-Length': html.length,
        'Expires': new Date().toUTCString()
    });
    res.end(html);
});

app.get('/api/v1/thumbnail/:uid', (req, res) => {
    console.log('thumbnail request: '+req.params.uid);

    var thumbnailfile = '/opt/srthub/thumbnail/'+req.params.uid;
    if (fs.existsSync(thumbnailfile)) {
        var thumbnaildata = fs.readFileSync('/opt/srthub/thumbnail/'+req.params.uid);
        res.statusCode = 200;
        res.setHeader('Content-Type','image/jpeg');
        res.end(thumbnaildata);
    } else {
        res.statusCode = 404;
        res.end();
    }
});

app.get('/api/v1/get_interfaces', (req, res) => {
    res.send(networkInterfaces);
});

app.post('/api/v1/remove_service/:uid', (req, res) => {
    console.log('received remove source request: ', req.params.uid);

    var files = fs.readdirSync(configFolder);
    var responding = 0;
    var listedfiles = 0;

    files.forEach(file => {
        console.log(getExtension(file));
        if (getExtension(file) == '.json') {
            var configindex = listedfiles + 1;
            var fullfile = configFolder+'/'+file;
            var fileprefix = path.basename(fullfile, '.json');
            if ((configindex == req.params.uid) || (fileprefix == req.params.uid)) {
                var removeConfig = fullfile;
                var removeStatus = statusFolder+'/' + file;

                activeconfigurations--;
                try {
                    fs.unlinkSync(removeStatus)
                } catch(err) {
                    console.error(err)
                }

                responding = 1;
                try {
                    fs.unlinkSync(removeConfig)
                    var retdata;
                    var current_status = 'success';
                    obj = new Object();
                    obj.status = current_status;
                    retdata = JSON.stringify(obj);
                    console.log(retdata);
                    res.send(retdata);
                } catch(err) {
                    console.error(err)
                    var retdata;
                    var current_status = 'failed';
                    obj = new Object();
                    obj.status = current_status;
                    retdata = JSON.stringify(obj);
                    console.log(retdata);
                    res.send(retdata);
                }
            }
            listedfiles++;
        }
    });

    if (!responding) {
        var retdata;
        var current_status = 'invalid';
        obj = new Object();
        obj.status = current_status;
        retdata = JSON.stringify(obj);
        console.log(retdata);
        res.send(retdata);
    }
});

app.post('/api/v1/new_srt_receiver', (req, res) => {
    console.log('received new srt receiver request');
    console.log('body is ',req.body);

    var words = req.body;
    var config = new Object();

    config.sourcename = words.srtreceiver_sourcename;
    config.sourcemode = "srt";
    config.sourceaddress = words.srtreceiver_sourceaddress;
    config.sourceport = words.srtreceiver_sourceport;
    config.sourceinterface = words.srtreceiver_sourceinterface;
    config.outputmode = "udp";
    config.outputaddress = words.srtreceiver_destinationaddress;
    config.outputport = words.srtreceiver_destinationport;
    config.outputinterface = words.srtreceiver_destinationinterface;
    config.passphrase = words.srtreceiver_passphrase;
    config.streamid = words.srtreceiver_streamid;
    config.managementserverip = words.srtreceiver_managementserverip;
    config.clienttype = words.srtreceiver_clienttype;
    config.latency = words.srtreceiver_latency;
    //config.keysize = words.srtreceiver_keysize;

    // this could cause a collision if multiple services are created at the exact same time
    // so we should look at adding another modifier
    // if the file already exists, we should wait and try again
    var servicenum = seconds_since_epoch();
    var nextconfig = configFolder+'/'+servicenum+'.json';

    fs.writeFile(nextconfig, JSON.stringify(config), (err) => {
        if (err) {
            console.error(err);
            return;
        };
        console.log('File has been created: ', nextconfig);

        activeconfigurations++;

        console.log('updated active configurations: ', activeconfigurations);
    });

    var retdata;

    obj = new Object();

    obj.servicenum = servicenum;
    retdata = JSON.stringify(obj);
    console.log(retdata);
    res.send(retdata);
});

app.post('/api/v1/new_srt_server', (req, res) => {
    console.log('received new srt server request');
    console.log('body is ',req.body);

    var words = req.body;
    var config = new Object();

    config.sourcename = words.srtserver_sourcename;
    config.sourcemode = "udp";
    config.sourceaddress = words.srtserver_sourceaddress;
    config.sourceport = words.srtserver_sourceport;
    config.sourceinterface = words.srtserver_sourceinterface;
    config.outputmode = "srt";
    config.outputaddress = words.srtserver_address;
    config.outputport = words.srtserver_port;
    config.outputinterface = words.srtserver_interface;
    config.streamid = words.srtserver_streamid;
    config.passphrase = words.srtserver_passphrase;
    config.servertype = words.srtserver_servertype;
    config.connectionqueue = words.srtserver_connectionqueue;
    config.whitelist = words.srtserver_whitelist;
    config.managementserverip = words.srtserver_managementserverip;

    // this could cause a collision if multiple services are created at the exact same time
    // so we should look at adding another modifier
    // if the file already exists, we should wait and try again

    var servicenum = seconds_since_epoch();
    var nextconfig = configFolder+'/'+servicenum+'.json';

    fs.writeFile(nextconfig, JSON.stringify(config), (err) => {
        if (err) {
            console.error(err);
            return;
        };
        console.log('File has been created: ', nextconfig);

        activeconfigurations++;

        console.log('updated active configurations: ', activeconfigurations);
    });

    var retdata;

    obj = new Object();

    obj.servicenum = servicenum;
    retdata = JSON.stringify(obj);
    console.log(retdata);
    res.send(retdata);
});

app.post('/api/v1/status_update/:uid', (req, res) => {
    console.log('received status update from: ', req.params.uid);
    //console.log('body is ',req.body);

    var nextstatus = statusFolder+'/'+req.params.uid+'.json';

    fs.writeFile(nextstatus, JSON.stringify(req.body), (err) => {
        if (err) {
            console.error(err);
            return;
        };
        console.log('File has been created: ', nextstatus);
    });

    res.send(req.body);
});

app.post('/api/v1/signal/:uid', (req, res) => {
    console.log('receive event signal from: ', req.params.uid);
    console.log('body is ', req.body);

    if (fs.existsSync(logfilename)) {
        fs.appendFileSync(logfilename, ','+JSON.stringify(req.body)+'\n');
    } else {
        fs.appendFileSync(logfilename, JSON.stringify(req.body)+'\n');
    }

    res.send(req.body);
});

app.post('/api/v1/stop_service/:uid', (req, res) => {
    console.log('stop button pressed: ', req.params.uid);

    var files = fs.readdirSync(configFolder);
    var listedfiles = 0;
    var responding = 0;

    files.forEach(file => {
        console.log(getExtension(file));
        if (getExtension(file) == '.json') {
            var fullfile = configFolder+'/'+file;
            var fileprefix = path.basename(fullfile, '.json');
            var configindex = listedfiles + 1;
            if ((configindex == req.params.uid) || (fileprefix == req.params.uid)) {
                var corestatusfile = statusFolder+'/corestatus_'+file;
                var srt_receiver_statusfile = statusFolder+'/srt_receiver_'+file;
                var udp_server_statusfile = statusFolder+'/udp_server_'+file;
                var thumbnail_statusfile = statusFolder+'/thumbnail_'+file;
                var configdata = fs.readFileSync(fullfile, 'utf8');
                var words = JSON.parse(configdata);
                console.log('this service maps to current file: ', fullfile);
                console.log('the file prefix is: ', fileprefix);

                var touchfile = statusFolder+'/'+fileprefix+'.lock';
                fs.closeSync(fs.openSync(touchfile, 'w'));

                var stop_cmd = 'sudo docker rm -f srthub'+fileprefix;
                console.log('removing statusfile '+corestatusfile);

                if (fs.existsSync(corestatusfile)) {
                    fs.unlinkSync(corestatusfile)
                }
                if (fs.existsSync(srt_receiver_statusfile)) {
                    fs.unlinkSync(srt_receiver_statusfile)
                }
                if (fs.existsSync(udp_server_statusfile)) {
                    fs.unlinkSync(udp_server_statusfile)
                }
                if (fs.existsSync(thumbnail_statusfile)) {
                    fs.unlinkSync(thumbnail_statusfile)
                }

                console.log('stop command: ', stop_cmd);
                responding = 1;
                exec(stop_cmd, (err, stdout, stderr) => {
                    if (err) {
                        console.log('Unable to stop Docker container');
                        var retdata;
                        var current_status = 'failed';
                        obj = new Object();
                        obj.status = current_status;
                        retdata = JSON.stringify(obj);
                        console.log(retdata);
                        res.send(retdata);
                    } else {
                        var failed_sent = 0;
                        try {
                            //console.log('removing status file: ', statusfile);
                            //fs.unlinkSync(statusfile);
                        } catch (errremove) {
                            console.error(errremove);
                            fs.unlinkSync(touchfile); // remove this?
                            failed_sent = 1;
                            var retdata;
                            var current_status = 'failed';
                            obj = new Object();
                            obj.status = current_status;
                            retdata = JSON.stringify(obj);
                            console.log(retdata);
                            res.send(retdata);
                        }
                        if (!failed_sent) {
                            console.log('Stopped and Removed Docker container');
                            fs.unlinkSync(touchfile);
                            var retdata;
                            var current_status = 'success';
                            obj = new Object();
                            obj.status = current_status;
                            retdata = JSON.stringify(obj);
                            console.log(retdata);
                            res.send(retdata);
                        }
                    }
                });
            }
            listedfiles++;
        }
    });

    if (!responding) {
        var retdata;
        var current_status = 'invalid';
        fs.unlinkSync(touchfile);
        obj = new Object();
        obj.status = current_status;
        retdata = JSON.stringify(obj);
        console.log(retdata);
        res.send(retdata);
    }
});

function os_func() {
    this.execCommand = function(cmd) {
        return new Promise((resolve, reject) => {
            exec(cmd, (err, stdout, stderr) => {
                if (err) {
                    reject(err);
                    return;
                }
                resolve(stdout)
            });
        })
    }
}

app.post('/api/v1/start_service/:uid', (req, res) => {
    const click = {clickTime: new Date()};
    console.log(click);
    console.log('start button pressed: ', req.params.uid);

    var files = fs.readdirSync(configFolder);
    var listedfiles = 0;
    var valid_service = 0;
    var responding = 0;

    files.forEach(file => {
        console.log(getExtension(file));
        if (getExtension(file) == '.json') {
            var fullfile = configFolder+'/'+file;
            var fileprefix = path.basename(fullfile, '.json');
            var configindex = listedfiles + 1;

            console.log('fullfile='+fullfile+', fileprefix='+fileprefix+', configindex='+configindex+' uid='+req.params.uid);
            if ((configindex == req.params.uid) || (fileprefix == req.params.uid)) {
                var configdata = fs.readFileSync(fullfile, 'utf8');
                var words = JSON.parse(configdata);
                console.log('this service maps to current file: ', fullfile);
                console.log('the file prefix is: ', fileprefix);

                valid_service = 1;

                var touchfile = statusFolder+'/'+fileprefix+'.lock';
                fs.closeSync(fs.openSync(touchfile, 'w'));

                var sessionid = fileprefix;
                var sourcemode = words.sourcemode;
                var outputmode = words.outputmode;
                if (sourcemode == 'srt') {
                    sourcemode = sourcemode + words.clienttype;
                }
                if (outputmode == 'srt') {
                    outputmode = outputmode + words.servertype;
                }
                console.log('debug: updated: sourcemode='+sourcemode+', outputmode='+outputmode);

                var start_cmd = 'sudo docker run -itd --net=host --name srthub'+fileprefix+' --restart=unless-stopped --log-opt max-size=25m -v /opt/srthub:/opt/srthub -v '+configFolder+':'+configFolder+' -v '+statusFolder+':'+statusFolder+' -v '+apacheFolder+':'+apacheFolder+' dockersrthub /usr/bin/srthub '+sessionid;
                //sourcemode+' '+words.sourceaddress+' '+words.sourceport+' '+words.sourceinterface+' '+outputmode+' '+words.outputaddress+' '+words.outputport+' '+words.outputinterface+' '+sessionid;

                console.log('start command: ', start_cmd);

                /*var passphrase = words.passphrase;
                var keysize = words.keysize;
                var streamid = words.streamid;
                if (passphrase.length > 0) {
                    start_cmd = start_cmd + ' ' + passphrase + ' ' + keysize;
                }
                if (streamid.length > 0) {
                    start_cmd = start_cmd + ' ' + streamid;
                }*/

                responding = 1;
                exec(start_cmd, (err, stdout, stderr) => {
                    if (err) {
                        var retdata;
                        var current_status = 'failed';

                        console.log('Unable to run Docker');
                        fs.unlinkSync(touchfile);
                        obj = new Object();
                        obj.status = current_status;
                        retdata = JSON.stringify(obj);
                        console.log(retdata);
                        res.send(retdata);
                    } else {
                        var retdata;
                        var current_status = 'success';

                        console.log('Started Docker container');
                        fs.unlinkSync(touchfile);

                        obj = new Object();
                        obj.status = current_status;
                        retdata = JSON.stringify(obj);
                        console.log(retdata);
                        res.send(retdata);
                    }
                });
            } else {
                //fs.unlinkSync(touchfile);
            }
            listedfiles++;
        }
    });

    if (!responding) {
        var retdata;
        var current_status = 'invalid';
        obj = new Object();
        obj.status = current_status;
        retdata = JSON.stringify(obj);
        console.log(retdata);
        res.send(retdata);
    }
});

function scan_response_video(streamindex, avtype, codec, width, height, framerate, bitrate, pid) {
    this.streamindex = streamindex;
    this.avtype = avtype;
    this.codec = codec;
    this.width = width;
    this.height = height;
    this.framerate = framerate;
    this.bitrate = bitrate;
    this.pid = pid;
}

function scan_response_audio(streamindex, avtype, codec, channels, samplerate, bitrate, pid) {
    this.streamindex = streamindex;
    this.avtype = avtype;
    this.codec = codec;
    this.channels = channels;
    this.samplerate = samplerate;
    this.bitrate = bitrate;
    this.pid = pid;
}

function scan_response_data(streamindex, avtype, codec, pid) {
    this.streamindex = streamindex;
    this.avtype = avtype;
    this.codec = codec;
    this.pid = pid;
}

app.post('/api/v1/scan', (req, res) => {
    console.log('address: '+JSON.stringify(req.query.address));
    console.log('interface: '+JSON.stringify(req.query.intf));

    var address = req.query.address;
    var intf = req.query.intf;

    console.log('address: '+address+' interface '+intf);

    var words = req.body;
    var input_sources = 1;
    var i;
    var programdata = [];
    var descriptivedata = [];

    console.log('input_sources: ', input_sources);
    for (i = 0; i < input_sources; i++) {
        var retdata;
        var scan_cmd = 'ffprobe -v quiet -timeout 20 -print_format json -show_format -show_programs udp://'+address+'?reuse=1';

        console.log('running: ', scan_cmd);

        exec(scan_cmd, (err, stdout, stderr) => {
            if (err) {
                var retdata;
                var current_status = 'failed';

                console.log('Unable to run ffprobe');

                obj = new Object();
                obj.status = current_status;
                retdata = JSON.stringify(obj);
                console.log(retdata);
                res.send(retdata);
            } else {
                var retdata;
                var current_status = 'success';

                console.log('ffprobe run successfully');

                var scan_data = stdout;

                var parsed_data = JSON.parse(scan_data);
                //var nb_streams = parsed_data.format.nb_streams;
                var programs_list = parsed_data.programs;
                var nb_programs = parsed_data.programs.length;

                console.log('nb_programs ', nb_programs);
                console.log('programs ', programs_list);

                var s;
                var p;
                var sources = '';
                for (p = 0; p < nb_programs; p++) {
                    var new_stream;

                    var program_id = parsed_data.programs[p].program_id;
                    var program_num = parsed_data.programs[p].program_num;
                    var nb_streams = parsed_data.programs[p].nb_streams;
                    var pmt_pid = parsed_data.programs[p].pmt_pid;
                    var streams = [];

                    sources = 'ID:'+program_num;
                    for (s = 0; s < nb_streams; s++) {
                        var codec_name = parsed_data.programs[p].streams[s].codec_name;
                        var codec_type = parsed_data.programs[p].streams[s].codec_type;

                        console.log('codec: ', codec_name);
                        if (codec_type === "video") {
                            var width = parsed_data.programs[p].streams[s].width;
                            var height = parsed_data.programs[p].streams[s].height;
                            var bit_rate = parsed_data.programs[p].streams[s].bit_rate;
                            var framerate = parsed_data.programs[p].streams[s].avg_frame_rate;
                            var pid = parsed_data.programs[p].streams[s].id;

                            sources += ' '+codec_name+' @ '+width+'x'+height+' '+framerate+' fps ';
                            var service = new scan_response_video(s, codec_type, codec_name, width, height, framerate, bit_rate, pid);
                            streams.push(service);
                        } else if (codec_type === "audio") {
                            var bit_rate = parsed_data.programs[p].streams[s].bit_rate;
                            var channels = parsed_data.programs[p].streams[s].channels;
                            var pid = parsed_data.programs[p].streams[s].id;
                            var samplerate = parsed_data.programs[p].streams[s].sample_rate;
                            if (channels == 1) {
                                sources += '['+codec_name+' @ mono '+samplerate+'Hz] ';
                            } else if (channels == 2) {
                                sources += '['+codec_name+' @ stereo '+samplerate+'Hz] ';
                            } else {
                                sources += '['+codec_name+' @ 5.1 '+samplerate+'Hz] ';
                            }

                            var service = new scan_response_audio(s, codec_type, codec_name, channels, samplerate, bit_rate, pid);
                            streams.push(service);
                        } else {
                            // do nothing for now
                        }
                    }
                    descriptivedata.push(sources);
                    programdata.push(streams);
                }

                console.log("response: ", programdata);

                obj = new Object();
                obj.scan_result = programdata;
                var retdata = JSON.stringify(obj);

                var nextscan = scanFolder+'/'+address+'.json';
                fs.writeFile(nextscan, retdata, (err) => {
                    if (err) {
                        console.error(err);
                        return;
                    };
                    console.log('Scan file has been created: ', nextscan);
                });

                console.log("description: ", descriptivedata);

                obj2 = new Object();
                obj2.sources = descriptivedata;
                var retdata2 = JSON.stringify(obj2);
                var nextscan2 = scanFolder+'/'+address+'_simple.json';
                fs.writeFile(nextscan2, retdata2, (err) => {
                    if (err) {
                        console.error(err);
                        return;
                    };
                    console.log('Scan file has been created: ', nextscan2);
                });

                res.send(retdata);
            }
        });
    }
});

function listed_service(serviceindex, servicenum) {
    this.serviceindex = serviceindex;
    this.servicenum = servicenum;
}

app.get('/api/v1/list_services', (req, res) => {
    console.log('requested to list services');

    var files = fs.readdirSync(configFolder);
    var serviceindex = 0;
    var retdata;

    // send list of services and quick status in json format
    obj = new Object();

    var services = [];
    files.forEach(file => {
        if (getExtension(file) == '.json') {
            var fullfileConfig = configFolder+'/'+file;
            var fileprefix = path.basename(fullfileConfig, '.json');
            serviceindex++;
            var service = new listed_service(serviceindex, fileprefix);
            services.push(service);
        }
    })

    obj.service_list = services;
    retdata = JSON.stringify(obj);
    //console.log(retdata);
    res.send(retdata);
});

function output_stream(height, width, video_bitrate) {
    this.height = height;
    this.width = width;
    this.video_bitrate = video_bitrate;
}

function input_stream(ip, port, input_interface, bitrate) {
    this.ip = ip;
    this.port = port;
    this.input_interface = input_interface;
    this.bitrate = bitrate;
}

app.get('/api/v1/get_log_data', (req, res) => {
    console.log('newest log filename: ', logfilename);
    if (fs.existsSync(logfilename)) {
        readLastLines.read(logfilename, 6)
            .then((lines) => {
                if (lines.charAt(0) == ',') {
                    lines = lines.substring(1);
                }
                updated_lines = '['+lines+']';
                //console.log(updated_lines);
                res.writeHead(200, {
                    'Content-Type': 'text/html',
                    'Content-Length': updated_lines.length,
                    'Expires': new Date().toUTCString()
                });
                res.end(updated_lines);
            });
    } else {
        res.sendStatus(404);  // logdata was not found
    }
});

app.get('/api/v1/get_extended_log_data', (req, res) => {
    console.log('newest log filename: ', logfilename);
    if (fs.existsSync(logfilename)) {
        readLastLines.read(logfilename, 50)
            .then((lines) => {
                if (lines.charAt(0) == ',') {
                    lines = lines.substring(1);
                }
                updated_lines = '['+lines+']';

                var parsed_updated_lines = JSON.parse(updated_lines);
                var output_log_data = JSON.stringify(parsed_updated_lines);

                res.writeHead(200, {
                    'Content-Type': 'text/html',
                    'Content-Length': output_log_data.length,
                    'Expires': new Date().toUTCString()
                });
                res.end(output_log_data);
            });
    } else {
        res.sendStatus(404);  // logdata was not found
    }
});

function srt_service_data(thread, clientaddress, clientport, totalbytessent, totalpacketssent)
{
    this.thread = thread;
    this.clientaddress = clientaddress;
    this.clientport = clientport;
    this.totalbytessent = totalbytessent;
    this.totalpacketssent = totalpacketssent;
}

function audio_service(codec, channels, samplerate)
{
    this.codec = codec;
    this.channels = channels;
    this.samplerate = samplerate;
}

app.get('/api/v1/get_service_status/:uid', (req, res) => {
    console.log('getting signal status: ', req.params.uid);

    var files = fs.readdirSync(configFolder);
    var listedfiles = 0;
    var found = 0;
    var sent = 0;
    var locked = 0;
    var i;

    // use the config file to find the correct status file since
    // they will have the same name but just in a different directory
    files.forEach(file => {
        if (getExtension(file) == '.json') {
            var configindex = listedfiles + 1;
            var fullfileStatus = statusFolder+'/'+file;
            var fileprefix = path.basename(fullfileStatus, '.json');
            console.log('get_service_status: checking configindex ', configindex, ' looking for ', req.params.uid);
            if ((configindex == req.params.uid) || (fileprefix == req.params.uid)) {
                var fullfileStatus = statusFolder+'/'+file;
                var fullfileConfig = configFolder+'/'+file;
                var fileprefix = path.basename(fullfileStatus, '.json');
                var touchfile = statusFolder+'/'+fileprefix+'.lock';

                console.log('get_service_status: checking for '+fullfileConfig);
                if (fs.existsSync(fullfileConfig)) {
                    var configfiledata = fs.readFileSync(fullfileConfig, 'utf8');
                    if (configfiledata) {
                        var cfd = JSON.parse(configfiledata);  // cfd = config file data
                        var sourcemode;
                        var outputmode;
                        var clienttype;
                        var servertype;
                        var uptime = -1;

                        sourcemode = cfd.sourcemode;
                        outputmode = cfd.outputmode;
                        clienttype = "unknown";
                        servertype = "unknown";

                        console.log('debug: initial: sourcemode='+sourcemode+', outputmode='+outputmode);

                        var fullfileStatusCore = statusFolder+'/corestatus_'+fileprefix+'.json';
                        if (fs.existsSync(fullfileStatusCore)) {
                            obj = new Object();

                            var statuscoredata = fs.readFileSync(fullfileStatusCore, 'utf8');
                            if (statuscoredata) {
                                var sfd = JSON.parse(statuscoredata);  // sfd = status file data

                                obj.uptime = sfd["srthub-uptime"] / 1000;
                                console.log('debug: uptime is '+obj.uptime);
                            } else {
                                obj.uptime = -1;   // not running?
                                console.log('debug: we are not running');
                            }

                            obj.srtreceiver_connected = 0;
                            obj.srtreceiver_bytesreceived = 0;
                            obj.srtreceiver_packetsreceived = 0;
                            obj.srtreceiver_packetslost = 0;
                            obj.srtreceiver_packetsretransmited = 0;
                            obj.srtreceiver_packetsdropped = 0;
                            obj.srtreceiver_losspercentage = 0;
                            obj.srtreceiver_bitratekbps = 0;
                            obj.srtreceiver_rtt = 0;
                            obj.srtreceiver_srtmode = "unknown";
                            obj.srtreceiver_clientaddress = "unknown";
                            obj.srtreceiver_clientport = 0;
                            obj.srtreceiver_latency = 0;

                            obj.udpserver_active = 0;
                            obj.udpserver_bytessent = 0;
                            obj.udpserver_packetssent = 0;
                            obj.udpserver_lastbuffersize = 0;

                            obj.srtserver = [];

                            obj.udpreceiver_active = 0;
                            obj.udpreceiver_bytesreceived = 0;
                            obj.udpreceiver_packetsreceived = 0;
                            obj.udpreceiver_multicastinput = 0;
                            obj.udpreceiver_bitratekbps = 0;

                            obj.video_width = 0;
                            obj.video_height = 0;
                            obj.video_codec = "unknown";
                            obj.source_format = "unknown";
                            obj.total_streams = 0;
                            obj.current_stream = 0;
                            obj.transport_source_errors = 0;
                            obj.last_source_error = "unknown";

                            obj.audioservices = [];

                            for (i = 0; i < 4; i++) {
                                var fullfileAudioStatus = statusFolder+'/audio_'+i+'_'+fileprefix+'.json';
                                if (fs.existsSync(fullfileAudioStatus)) {
                                    var audiodata = fs.readFileSync(fullfileAudioStatus, 'utf8');
                                    if (audiodata) {
                                        var ad = JSON.parse(audiodata);
                                        var audiocodec = ad["audio-codec"];
                                        var audiochannels = ad["audio-channels"];
                                        var audiosamplerate = ad["audio-samplerate"];
                                        var audioservice = new audio_service(audiocodec, audiochannels, audiosamplerate);
                                        obj.audioservices.push(audioservice);
                                    } else {
                                        var audiocodec = "Unknown";
                                        var audiochannels = 0;
                                        var audiosamplerate = 0;
                                        var audioservice = new audio_service(audiocodec, audiochannels, audiosamplerate);
                                        obj.audioservices.push(audioservice);
                                    }
                                } else {
                                    var audiocodec = "Unknown";
                                    var audiochannels = 0;
                                    var audiosamplerate = 0;
                                    var audioservice = new audio_service(audiocodec, audiochannels, audiosamplerate);
                                    obj.audioservices.push(audioservice);
                                }
                            }

                            var fullfileThumbnailStatus = statusFolder+'/thumbnail_'+fileprefix+'.json';
                            if (fs.existsSync(fullfileThumbnailStatus)) {
                                var decodedata = fs.readFileSync(fullfileThumbnailStatus, 'utf8');
                                if (decodedata) {
                                    var dd = JSON.parse(decodedata);

                                    obj.video_width = dd.width;
                                    obj.video_height = dd.height;
                                    obj.video_codec = dd["video-codec"];
                                    obj.source_format = dd["source-format"];
                                    obj.total_streams = dd["total-streams"];
                                    obj.current_stream = dd["current-stream"];
                                    obj.transport_source_errors = dd["transport-source-errors"];
                                    obj.last_source_error = dd["last-source-error"];
                                }
                            }

                            // source mode status checking
                            if (sourcemode == "srt") {
                                var fullfileSRTReceiverStatus = statusFolder+'/srt_receiver_'+fileprefix+'.json';
                                if (fs.existsSync(fullfileSRTReceiverStatus)) {
                                    var srtreceiverdata = fs.readFileSync(fullfileSRTReceiverStatus, 'utf8');
                                    if (srtreceiverdata) {
                                        //console.log(srtreceiverdata);
                                        var srd = JSON.parse(srtreceiverdata);

                                        obj.srtreceiver_connected = srd["srt-connection"];
                                        obj.srtreceiver_srtmode = srd["srt-mode"];
                                        //obj.srtreceiver_signalactive = srd["srt-signalactive"];
                                        obj.srtreceiver_bytesreceived = srd["total-bytes-received"];
                                        obj.srtreceiver_packetsreceived = srd["packets-received"];
                                        obj.srtreceiver_packetslost = srd["packets-lost"];
                                        obj.srtreceiver_packetsretransmitted = srd["packets-retransmitted"];
                                        obj.srtreceiver_packetsdropped = srd["packets-dropped"];
                                        obj.srtreceiver_losspercentage = srd["loss-percentage"];
                                        obj.srtreceiver_bitratekbps = srd["bitrate-kbps"];
                                        obj.srtreceiver_rtt = srd.rtt;
                                        obj.srtreceiver_latency = srd["latencyms"];
                                        obj.srtreceiver_clientaddress = srd["client-address"];
                                        obj.srtreceiver_clientport = srd["client-port"];
                                    }
                                }
                            } else if (sourcemode == "udp") {
                                var fullfileUDPReceiverStatus = statusFolder+'/udp_receiver_'+fileprefix+'.json';
                                if (fs.existsSync(fullfileUDPReceiverStatus)) {
                                    var udpreceiverdata = fs.readFileSync(fullfileUDPReceiverStatus, 'utf8');
                                    if (udpreceiverdata) {
                                        var urd = JSON.parse(udpreceiverdata);

                                        obj.udpreceiver_active = urd["udp-source-active"];
                                        obj.udpreceiver_bytesreceived = urd["total-bytes-received"];
                                        obj.udpreceiver_packetsreceived = urd["total-packets-received"];
                                        obj.udpreceiver_bitratekbps = urd["udp-source-kbps"];
                                        obj.udpreceiver_multicastinput = urd["multicast-input"];
                                    }
                                }
                            } else {
                                // unspported mode
                            }

                            // output mode status checking
                            if (outputmode == "srt") {
                                for (i = 0; i < 8; i++) {
                                    var fullfileSRTServerStatus = statusFolder+'/srt_server_thread_'+i+'_'+fileprefix+'.json';
                                    if (fs.existsSync(fullfileSRTServerStatus)) {
                                        var srtserverdata = fs.readFileSync(fullfileSRTServerStatus, 'utf8');
                                        if (srtserverdata) {
                                            var ssd = JSON.parse(srtserverdata);
                                            var thread = ssd.thread;
                                            var clientaddress = ssd["client-address"];
                                            var clientport = ssd["client-port"];
                                            var totalbytessent = ssd["total-bytes-sent"];
                                            var totalpacketssent = ssd["total-packets-sent"];

                                            var srtservice = new srt_service_data(thread, clientaddress, clientport, totalbytessent, totalpacketssent);
                                            obj.srtserver.push(srtservice);
                                        }
                                    }
                                }
                            } else if (outputmode == "udp") {
                                var fullfileUDPServerStatus = statusFolder+'/udp_server_'+fileprefix+'.json';
                                if (fs.existsSync(fullfileUDPServerStatus)) {
                                    var udpserverdata = fs.readFileSync(fullfileUDPServerStatus, 'utf8');
                                    if (udpserverdata) {
                                        var usd = JSON.parse(udpserverdata);

                                        obj.udpserver_active = usd["udp-output-active"];
                                        obj.udpserver_bytessent = usd["total-bytes-sent"];
                                        obj.udpserver_packetssent = usd["total-packets-sent"];
                                        obj.udpserver_lastbuffersize = usd["last-buffer-size"];
                                    }
                                }
                            } else {
                                // unsupported mode
                            }

                            obj.sourcename = cfd.sourcename;
                            obj.sourcemode = cfd.sourcemode;
                            obj.sourceaddress = cfd.sourceaddress;
                            obj.sourceport = cfd.sourceport;
                            obj.sourceinterface = cfd.sourceinterface;
                            obj.outputmode = cfd.outputmode;
                            obj.outputaddress = cfd.outputaddress;
                            obj.outputport = cfd.outputport;
                            obj.outputinterface = cfd.outputinterface;
                            obj.clienttype = cfd.clienttype;
                            obj.servertype = cfd.servertype;

                            retdata = JSON.stringify(obj);

                            res.status(200);
                            res.send(retdata);
                            sent = 1;
                        } else {
                            console.log('corestatus file does not exist: '+fullfileStatusCore);
                            // we should put some filler information here for the configuration
                            // but make the uptime equal to -1 instead

                            obj.uptime = -1;
                            obj.sourcename = cfd.sourcename;
                            obj.sourcemode = cfd.sourcemode;
                            obj.sourceaddress = cfd.sourceaddress;
                            obj.sourceport = cfd.sourceport;
                            obj.sourceinterface = cfd.sourceinterface;
                            obj.outputmode = cfd.outputmode;
                            obj.outputaddress = cfd.outputaddress;
                            obj.outputport = cfd.outputport;
                            obj.outputinterface = cfd.outputinterface;
                            obj.clienttype = cfd.clienttype;
                            obj.servertype = cfd.servertype;

                            retdata = JSON.stringify(obj);

                            res.status(200);
                            res.send(retdata);
                            sent = 1;
                        }
                    }
                }
            }
            listedfiles++;
        }
    });

    if (locked == 1) {
        console.log('service is unavailable ', req.params.uid);
        res.sendStatus(503);  // service unavailable
    } else if (sent == 0) {
        //let's provide basic configuration information instead of just an empty status
        //that is the best we can do
        console.log('service was not found ', req.params.uid);
        res.sendStatus(404);  // service not found
    } else {
        // do nothing, we already responded
    }
});
