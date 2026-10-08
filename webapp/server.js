/*****************************************************************************
  Copyright (C) 2018-2026 John William (Will)

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
var networkInterfaces = os.networkInterfaces();
const express = require('express');
const session = require('express-session');
const readLastLines = require('read-last-lines');
var bodyParser = require('body-parser');
const fs = require('fs');
const crypto = require('crypto');
const zlib = require('zlib');
const readline = require('readline');
var path = require('path');
var https = require('https');
var http = require('http');
var net = require('net');
const { param, validationResult } = require('express-validator');
const validator = require('validator');
const helmet = require('helmet');

// Resolved against __dirname rather than the working directory so the app can
// be started from anywhere (systemd/pm2 set the cwd, but a manual "node
// /var/app/server.js" from elsewhere used to fail here).
var options = {
    key: fs.readFileSync(path.join(__dirname, 'cert', 'server.key')),
    cert: fs.readFileSync(path.join(__dirname, 'cert', 'server.crt'))
};

const app = express();

// The session secret is what stops a client from forging its own signed session
// cookie, so it cannot be a known constant. Persist a random one to disk on
// first run: keeping it stable across restarts means a restart doesn't log
// everyone out, and keeping it off-repo means it isn't the same on every
// install. SRTHUB_SESSION_SECRET overrides it if you'd rather inject it.
function loadSessionSecret() {
    if (process.env.SRTHUB_SESSION_SECRET) {
        return process.env.SRTHUB_SESSION_SECRET;
    }
    var secretFile = '/opt/srthub/session.key';
    try {
        var existing = fs.readFileSync(secretFile, 'utf8').trim();
        if (existing.length > 0) {
            return existing;
        }
    } catch (err) {
        if (err.code !== 'ENOENT') {
            throw err;
        }
    }
    var generated = crypto.randomBytes(32).toString('hex');
    try {
        // mode 0600: readable only by the user the service runs as.
        fs.writeFileSync(secretFile, generated + '\n', { mode: 0o600 });
        console.log('Generated a new session secret at ' + secretFile);
    } catch (err) {
        // Not fatal: fall back to a secret that lives only in this process, so
        // the app still starts. Sessions will not survive a restart, which is a
        // far better failure than refusing to serve at all.
        console.error('WARNING: could not write ' + secretFile + ' (' + err.code +
                      '). Using an in-memory session secret; logins will be ' +
                      'invalidated on restart. Set SRTHUB_SESSION_SECRET or make ' +
                      'the directory writable to fix this.');
    }
    return generated;
}

app.use(session({
    secret: loadSessionSecret(),
    resave: true,
    saveUninitialized: true,
    // Was 'cooke', so none of this was ever applied. Port 8080 routes plaintext
    // connections to a redirect-only server and never to this app, so every
    // request that reaches here is already over TLS and 'secure' is safe.
    cookie: {
        secure: true,
        httpOnly: true,
        sameSite: 'lax'
    }
}));

/*
app.use(
  helmet.contentSecurityPolicy({
    directives: {
        defaultSrc: ["'self'"],
        scriptSrc: ["'self'"],
        objectSrc: ["'self'"],
        imgSrc: ["'self'"],
        scriptSrcAttr: ["'self'"],
        upgradeInsecureRequests: [],
    },
  })
);
*/

app.use(bodyParser.json());
app.use(express.json());
app.use(express.urlencoded({extended:true}));

var auth = function(req, res, next) {
    if (req.session.loggedin) {
        return next();
    } else {
        res.sendFile(path.join(__dirname + '/authenticate.html'));
    }
};

function isLoopback(address) {
    if (!address) {
        return false;
    }
    var addr = String(address).replace(/^::ffff:/, '');
    return addr === '::1' || addr.indexOf('127.') === 0;
}

// Endpoints srthub itself calls back on. It runs in a container started with
// --net=host and posts to 127.0.0.1:8080, so these arrive over loopback with no
// browser session of their own.
const INTERNAL_API_PATH = /^\/api\/v1\/(signal|status_update)\//;

// Same as auth(), except srthub's own loopback callbacks are let through. They
// carry no session cookie, so requiring a login here silently dropped every
// signal - which is what kept log messages from reaching the web UI.
var localApiAuth = function(req, res, next) {
    if (isLoopback(req.socket && req.socket.remoteAddress)) {
        return next();
    }
    return auth(req, res, next);
};

app.get('/', function(req,res) {
    if (req.session.loggedin) {
        res.redirect('/srthub');
        res.end();
    } else {
        res.sendFile(path.join(__dirname + '/authenticate.html'));
    }
});

app.use('/srthub', auth, express.static(path.join(__dirname,'public')));

app.post('/auth',
         param('username').trim().escape().notEmpty(),
         param('password').trim().escape().notEmpty(),
         function(request, response) {
             var authFile = '/opt/srthub/users.json';

             // Capture the input fields
             var username = request.body.username;
             var password = request.body.password;

             console.log('username='+username);
             console.log('password='+password);

             if (username && password) {
                 if (fs.existsSync(authFile)) {
                     var userdata = fs.readFileSync(authFile, 'utf8');
                     if (userdata) {
                         var parseduserdata = JSON.parse(userdata);
                         var e = parseduserdata.length;
                         var i;
                         var f = 0;
                         for (i = 0; i < e; i++) {
                             if ((parseduserdata[i].username === username) &&
                                 (parseduserdata[i].password === password)) {
                                 request.session.loggedin = true;
                                 request.session.username = username;
                                 response.redirect('/srthub');
                                 response.end();
                                 f = 1;
                                 break;
                             }
                         }
                         if (f == 0) {
                             response.sendFile(path.join(__dirname + '/authenticate.html'));
                         }
                     } else {
                         response.sendFile(path.join(__dirname + '/authenticate.html'));
                     }
                 } else {
                     response.sendFile(path.join(__dirname + '/authenticate.html'));
                 }
             } else {
                 response.sendFile(path.join(__dirname + '/authenticate.html'));
             }
         });

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

const httpsServer = https.createServer(options, app);
const httpServer = http.createServer((req, res) => {
    // srthub posts its signals over plain HTTP to 127.0.0.1:8080, and the
    // libcurl it is linked against is built --without-ssl - it cannot follow a
    // redirect to HTTPS, and does not even ask to (no CURLOPT_FOLLOWLOCATION).
    // Redirecting these therefore threw away every log message. Serve them
    // directly instead; loopback only, so nothing off-box skips HTTPS.
    if (INTERNAL_API_PATH.test(req.url) &&
        isLoopback(req.socket && req.socket.remoteAddress)) {
        return app(req, res);
    }
    const host = req.headers.host.replace(/:\d+$/, ''); // strip any port
    // req.url already begins with '/', so no separator here (this used to emit
    // a doubled slash).
    res.writeHead(301, { Location: `https://${host}:8080${req.url}` });
    res.end();
});

net.createServer(socket => {
  socket.once('data', buffer => {
    socket.pause();
    const target = buffer[0] === 22 ? httpsServer : httpServer; // 22 = TLS handshake
    socket.unshift(buffer);
    target.emit('connection', socket);
    process.nextTick(() => socket.resume()); // must be deferred
  });
}).listen(8080, () => console.log('Listening on 8080 (HTTP + HTTPS)'));

//app.use((req, res, next) => {
//    if (req.secure) {
//        return next();
//    }
//    res.redirect(301, `https://${req.headers.host}${req.url}`);
//});

/*****************************************************************************
  Bitrate history
  ----------------------------------------------------------------------------
  A small background sampler reads the per-service status files on a fixed
  cadence (independent of any browser polling), keeps a rolling buffer per
  service in memory, and persists it to disk so the history survives both a
  page reload and a server restart. The UI seeds its graph from this buffer.
*******************************************************************************/
const bitrateHistoryFile = statusFolder + '/bitrate_history.json';
const BITRATE_HISTORY_MAX = 900;   // samples kept per service (~30 min at 2s)
const BITRATE_SAMPLE_MS   = 2000;  // sampler cadence
const BITRATE_PERSIST_MS  = 30000; // how often the buffer is flushed to disk
var bitrateHistory = {};           // fileprefix -> [{ t, kbps }]

// The srt_receiver status file already reports "bitrate-kbps" in true
// kilobits/sec, so no unit conversion is applied (scale = 1). If a future build
// of the status producer ever reports kiloBYTES/s instead, set this to 8.
const SRT_BITRATE_SCALE = 1;

// Seed from disk at startup (best effort).
try {
    if (fs.existsSync(bitrateHistoryFile)) {
        var rawHist = fs.readFileSync(bitrateHistoryFile, 'utf8');
        if (rawHist) {
            bitrateHistory = JSON.parse(rawHist) || {};
        }
    }
} catch (e) {
    console.log('Could not load bitrate history:', e.message);
    bitrateHistory = {};
}

// ---------------------------------------------------------------------------
// SRT mode strings
//
// srthub matches the SRT direction on the mode string itself (source/srthub.c):
//
//   sourcemode "srtpull" -> caller   : we dial out and pull the stream
//   sourcemode "srtpush" -> listener : the far end connects and pushes to us
//   outputmode "srtpull" -> listener : the far end connects and pulls from us
//   outputmode "srtpush" -> caller   : we dial out and push the stream
//
// A bare "srt" matches neither, so a service saved that way never starts. The
// UI carries the direction separately - clienttype for receivers, servertype for
// servers - and both are normalised to 'pull' or 'push' here.
// ---------------------------------------------------------------------------

function isSrtMode(mode) {
    return typeof mode === 'string' && mode.indexOf('srt') === 0;
}

function srtModeFor(direction) {
    return direction === 'push' ? 'srtpush' : 'srtpull';
}

// Receiver (the SRT side is the source): caller means we pull, listener means
// the far end pushes to us. Older builds of the create form submitted
// caller/listener here instead of pull/push, so both spellings are accepted.
function receiverDirection(value) {
    switch (String(value || '').toLowerCase()) {
        case 'push':
        case 'listener':
            return 'push';
        case 'pull':
        case 'caller':
            return 'pull';
        default:
            return 'pull';
    }
}

// Server (the SRT side is the output): listener means the far end pulls from us,
// caller means we push to it - the opposite pairing to a receiver.
function serverDirection(value) {
    switch (String(value || '').toLowerCase()) {
        case 'push':
        case 'caller':
            return 'push';
        case 'pull':
        case 'listener':
            return 'pull';
        default:
            return 'pull';
    }
}

// Bring a config's SRT mode strings in step with its direction fields. Returns
// true if anything changed, so callers can persist the correction. This doubles
// as the migration for configs written with a bare "srt".
function normalizeConfigModes(config) {
    var changed = false;
    if (isSrtMode(config.sourcemode)) {
        config.clienttype = receiverDirection(config.clienttype);
        var wantSource = srtModeFor(config.clienttype);
        if (config.sourcemode !== wantSource) {
            config.sourcemode = wantSource;
            changed = true;
        }
    }
    if (isSrtMode(config.outputmode)) {
        config.servertype = serverDirection(config.servertype);
        var wantOutput = srtModeFor(config.servertype);
        if (config.outputmode !== wantOutput) {
            config.outputmode = wantOutput;
            changed = true;
        }
    }
    return changed;
}

function readServiceBitrate(fileprefix, sourcemode) {
    try {
        if (isSrtMode(sourcemode)) {
            var srtFile = statusFolder + '/srt_receiver_' + fileprefix + '.json';
            if (fs.existsSync(srtFile)) {
                var srt = JSON.parse(fs.readFileSync(srtFile, 'utf8'));
                return (srt['bitrate-kbps'] || 0) * SRT_BITRATE_SCALE;
            }
        } else if (sourcemode === 'udp') {
            var udpFile = statusFolder + '/udp_receiver_' + fileprefix + '.json';
            if (fs.existsSync(udpFile)) {
                var udp = JSON.parse(fs.readFileSync(udpFile, 'utf8'));
                return udp['udp-source-kbps'] || 0;
            }
        }
    } catch (e) {
        // a single bad/partial sample shouldn't break the sampler
    }
    return 0;
}

function sampleBitrates() {
    if (!fs.existsSync(configFolder)) {
        return;
    }
    var now = Date.now();
    var active = {};
    try {
        var files = fs.readdirSync(configFolder);
        files.forEach(function(file) {
            if (getExtension(file) !== '.json') {
                return;
            }
            var fileprefix = path.basename(file, '.json');
            try {
                var config = JSON.parse(fs.readFileSync(configFolder + '/' + file, 'utf8'));
                var corestatusfile = statusFolder + '/corestatus_' + fileprefix + '.json';
                var running = fs.existsSync(corestatusfile);
                // Keep the buffer alive whether running or stopped; while stopped
                // we append zeros so the graph clearly shows the service is down.
                active[fileprefix] = true;
                var kbps = running ? readServiceBitrate(fileprefix, config.sourcemode) : 0;
                var buf = bitrateHistory[fileprefix] || (bitrateHistory[fileprefix] = []);
                buf.push({ t: now, kbps: kbps });
                if (buf.length > BITRATE_HISTORY_MAX) {
                    buf.splice(0, buf.length - BITRATE_HISTORY_MAX);
                }
            } catch (e) {
                // skip this service for this sampling round
            }
        });
        // prune history for services whose config no longer exists
        Object.keys(bitrateHistory).forEach(function(prefix) {
            if (!active[prefix]) {
                delete bitrateHistory[prefix];
            }
        });
    } catch (e) {
        console.log('Bitrate sampling error:', e.message);
    }
}

function persistBitrateHistory() {
    try {
        fs.writeFileSync(bitrateHistoryFile, JSON.stringify(bitrateHistory));
    } catch (e) {
        console.log('Could not persist bitrate history:', e.message);
    }
}

setInterval(sampleBitrates, BITRATE_SAMPLE_MS);
setInterval(persistBitrateHistory, BITRATE_PERSIST_MS);

// flush on shutdown so nothing in the last interval is lost
process.on('SIGINT',  function() { persistBitrateHistory(); process.exit(0); });
process.on('SIGTERM', function() { persistBitrateHistory(); process.exit(0); });

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

app.get('/api/v1/system_information', auth, (req, res) => {
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

app.get('/api/v1/backup_services', auth, (req, res) => {
    var files = fs.readdirSync(configFolder);
    var archiver = require('archiver');
    var zip = archiver('zip');

    zip.on('error', function(err) {
        res.status(500).send({error: err.message});
    });

    res.setHeader('Content-Type','application/octet-stream');

    zip.on('end', function() {
        console.log('zip file done - wrote %d bytes', zip.pointer());
        res.sendFile(path.join(__dirname + '/public/systemlogs.zip'));
    });

    const writeStream = fs.createWriteStream('/var/app/public/systemlogs.zip');
    zip.pipe(writeStream);

    files.forEach(file => {
        console.log(getExtension(file));
        if (getExtension(file) == '.json') {
            var fullfile = configFolder+'/'+file;
            console.log('zipping ', fullfile);
            zip.file(fullfile);
        }
    });
    // Every rotation that is still on disk, however it happens to be named, so a
    // support bundle carries the whole retention window rather than the two
    // files the old numbered naming happened to produce.
    logFileCandidates().forEach(file => {
        console.log('zipping ', file.path);
        zip.file(file.path);
    });
    zip.file('/var/log/kern.log');
    if (fs.existsSync('/var/log/kern.log.1')) {
        zip.file('/var/log/kern.log.1');
    }
    zip.file('/var/log/dpkg.log');
    zip.file('/etc/netplan/01-network-config.yaml');
    zip.finalize();
});

// ---- Config backup and restore ----------------------------------------------
//
// Backup streams every service config as a .tar.gz. Restore takes either that
// archive or individual .json files, and runs in two passes against the same
// code: "inspect" reports what each file is and what restoring it would do,
// "apply" re-inspects against the state at that moment and then writes. Nothing
// from the upload is trusted between the two.
//
// What a restored config can reach, and why each check exists:
//
//   - The service id is the config's filename, and start_service and
//     stop_service concatenate it into a root shell command
//     ("sudo docker run --name srthub<id> ... srthub <id>"). So an id is only
//     ever digits, it only ever comes from a filename that is already digits or
//     is freshly allocated here, and nothing in a file's contents can set it.
//   - srthub reads every field with cJSON's valuestring and atoi(), so a field
//     that is present but not a string is a NULL dereference in the decoder.
//     Every field is a string on the way out; integers for the numeric fields
//     are converted (and reported), anything else is refused.
//   - esignal.c writes the service name, addresses and interface into its event
//     JSON without escaping, so those fields may not carry quotes, backslashes
//     or control characters, or a restored name would corrupt the event log.
//   - Fields are copied by name from an allowlist into a fresh object, so
//     unknown keys - __proto__ included - never reach the file or the server's
//     objects.
//   - Archives are read in memory, entry by entry. No path from inside an
//     archive is ever used to write anything; only the last component of a name
//     is read, and only as a hint for the id.

const MAX_RESTORE_UPLOAD = 2 * 1024 * 1024;        // compressed upload
const MAX_RESTORE_EXPANDED = 16 * 1024 * 1024;     // after gunzip
const MAX_RESTORE_FILES = 200;                     // configs per archive
const MAX_CONFIG_FILE_SIZE = 64 * 1024;            // one config, as uploaded
const SERVICE_ID_PATTERN = /^[0-9]{1,12}$/;

// Fields srthub reads (source/srthub.c), and the two the UI adds when it fetches
// a config for editing, which are dropped without comment.
const CONFIG_FIELDS = {
    sourcename:         { kind: 'name', required: true },
    sourcemode:         { kind: 'mode', required: true },
    outputmode:         { kind: 'mode', required: true },
    clienttype:         { kind: 'direction' },
    servertype:         { kind: 'direction' },
    sourceaddress:      { kind: 'address' },
    outputaddress:      { kind: 'address' },
    managementserverip: { kind: 'address' },
    sourceport:         { kind: 'port', required: true },
    outputport:         { kind: 'port', required: true },
    sourceinterface:    { kind: 'interface' },
    outputinterface:    { kind: 'interface' },
    outputttl:          { kind: 'int', min: 1, max: 255 },
    latency:            { kind: 'int', min: 0, max: 60000 },
    keysize:            { kind: 'keysize' },
    connectionqueue:    { kind: 'int', min: 1, max: 1024 },
    overheadbw:         { kind: 'int', min: 5, max: 100 },
    passphrase:         { kind: 'passphrase' },
    streamid:           { kind: 'streamid' },
    whitelist:          { kind: 'whitelist' },
    // MPTS program number to monitor; empty = the first program
    program:            { kind: 'int', min: 1, max: 65535 }
};
const CONFIG_UI_FIELDS = ['fileprefix', 'configindex'];
const NUMERIC_KINDS = ['port', 'int', 'keysize'];

const HOSTNAME_PATTERN =
    /^(?=.{1,253}$)[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$/;
const INTERFACE_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_.:@-]{0,31}$/;
const CONTROL_CHARACTERS = /[\u0000-\u001f\u007f-\u009f\u2028\u2029]/;

// One field: returns { value } with the string to store, or { error }.
function validateConfigField(name, rule, raw, warnings) {
    var value = raw;

    if (typeof value === 'number' && NUMERIC_KINDS.indexOf(rule.kind) >= 0 &&
        Number.isInteger(value)) {
        value = String(value);
        warnings.push(name + ' was a number; stored as text, which is what srthub reads');
    }
    if (typeof value !== 'string') {
        return { error: name + ' must be text, not ' + (value === null ? 'null' :
                                                         Array.isArray(value) ? 'a list' : typeof value) };
    }
    if (CONTROL_CHARACTERS.test(value)) {
        return { error: name + ' contains control characters' };
    }

    var trimmed = value.trim();

    switch (rule.kind) {
    case 'name':
        if (trimmed.length === 0) {
            return { error: 'sourcename is empty' };
        }
        if (trimmed.length > 128 || Buffer.byteLength(trimmed, 'utf8') > 255) {
            return { error: 'sourcename is longer than 128 characters' };
        }
        if (/["\\]/.test(trimmed)) {
            return { error: 'sourcename may not contain quotes or backslashes' };
        }
        // Every page that shows a name escapes it, but no real service name
        // needs markup characters, so a backup cannot carry any to a page that
        // one day forgets to.
        if (/[<>]/.test(trimmed)) {
            return { error: 'sourcename may not contain < or >' };
        }
        return { value: trimmed };

    case 'mode':
        trimmed = trimmed.toLowerCase();
        if (['udp', 'srt', 'srtpull', 'srtpush'].indexOf(trimmed) < 0) {
            return { error: name + ' must be udp, srtpull or srtpush' };
        }
        return { value: trimmed };

    case 'direction':
        if (['', 'push', 'pull', 'caller', 'listener'].indexOf(trimmed.toLowerCase()) < 0) {
            return { error: name + ' must be push, pull, caller or listener' };
        }
        return { value: trimmed.toLowerCase() };

    case 'address':
        if (trimmed === '' || validator.isIP(trimmed) || HOSTNAME_PATTERN.test(trimmed)) {
            return { value: trimmed };
        }
        return { error: name + ' is not an IP address or host name' };

    case 'port':
        if (!/^[0-9]{1,5}$/.test(trimmed) || Number(trimmed) < 1 || Number(trimmed) > 65535) {
            return { error: name + ' must be a port number from 1 to 65535' };
        }
        return { value: String(Number(trimmed)) };

    case 'int':
        if (trimmed === '') {
            return { value: '' };
        }
        if (!/^[0-9]{1,6}$/.test(trimmed) || Number(trimmed) < rule.min || Number(trimmed) > rule.max) {
            return { error: name + ' must be a whole number from ' + rule.min + ' to ' + rule.max };
        }
        return { value: String(Number(trimmed)) };

    case 'keysize':
        if (['', '0', '16', '24', '32'].indexOf(trimmed) < 0) {
            return { error: 'keysize must be 0, 16, 24 or 32' };
        }
        return { value: trimmed };

    case 'interface':
        if (trimmed !== '' && !INTERFACE_PATTERN.test(trimmed)) {
            return { error: name + ' is not a network interface name' };
        }
        return { value: trimmed };

    case 'passphrase':
        // Passed straight to SRTO_PASSPHRASE and never logged, so any printable
        // character is allowed; SRT itself insists on 10 to 79 of them.
        if (value !== '' && (value.length < 10 || value.length > 79 || !/^[\x20-\x7e]+$/.test(value))) {
            return { error: 'passphrase must be empty or 10 to 79 printable ASCII characters' };
        }
        return { value: value };

    case 'streamid':
        if (value.length > 511 || /["\\]/.test(value) || !/^[\x20-\x7e]*$/.test(value)) {
            return { error: 'streamid must be printable ASCII without quotes or backslashes, at most 511 characters' };
        }
        return { value: value };

    case 'whitelist':
        if (!/^[0-9A-Fa-f.:,/ ]{0,511}$/.test(trimmed)) {
            return { error: 'whitelist may only list IP addresses and networks' };
        }
        return { value: trimmed };
    }

    return { error: name + ' has no validation rule' };
}

// Parses and validates one uploaded config. Returns { config, errors, warnings }
// where config is a fresh object holding only allowlisted, validated fields.
function validateConfigText(buffer) {
    var errors = [];
    var warnings = [];

    if (!buffer || buffer.length === 0) {
        return { config: null, errors: ['the file is empty'], warnings: warnings };
    }
    if (buffer.length > MAX_CONFIG_FILE_SIZE) {
        return { config: null, errors: ['the file is larger than a service config can be'], warnings: warnings };
    }

    var text;
    try {
        text = new TextDecoder('utf-8', { fatal: true }).decode(buffer);
    } catch (e) {
        return { config: null, errors: ['the file is not UTF-8 text'], warnings: warnings };
    }
    text = text.replace(/^\ufeff/, '');
    if (text.trim().length === 0) {
        return { config: null, errors: ['the file is empty'], warnings: warnings };
    }

    var parsed;
    try {
        parsed = JSON.parse(text);
    } catch (e) {
        return { config: null, errors: ['the file is not valid JSON (corrupt or truncated)'], warnings: warnings };
    }
    if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
        return { config: null, errors: ['the file is JSON but not a service config'], warnings: warnings };
    }

    var config = {};
    var has = (key) => Object.prototype.hasOwnProperty.call(parsed, key);

    Object.keys(CONFIG_FIELDS).forEach(name => {
        var rule = CONFIG_FIELDS[name];
        if (!has(name)) {
            if (rule.required) {
                errors.push(name + ' is missing');
            }
            return;
        }
        var result = validateConfigField(name, rule, parsed[name], warnings);
        if (result.error) {
            errors.push(result.error);
        } else {
            config[name] = result.value;
        }
    });

    var unknown = Object.keys(parsed).filter(k =>
        !Object.prototype.hasOwnProperty.call(CONFIG_FIELDS, k) && CONFIG_UI_FIELDS.indexOf(k) < 0);
    if (unknown.length > 0) {
        var shown = unknown.slice(0, 5).map(k => String(k).substring(0, 32)).join(', ');
        warnings.push('ignored unrecognised field' + (unknown.length > 1 ? 's' : '') + ': ' + shown +
                      (unknown.length > 5 ? ' and ' + (unknown.length - 5) + ' more' : ''));
    }

    // The four supported shapes: an SRT input to UDP out (receiver), or UDP in
    // to an SRT output (server). Anything else would start a container that
    // srthub cannot run.
    if (config.sourcemode && config.outputmode) {
        var receiver = isSrtMode(config.sourcemode) && config.outputmode === 'udp';
        var server = config.sourcemode === 'udp' && isSrtMode(config.outputmode);
        if (!receiver && !server) {
            errors.push('sourcemode ' + config.sourcemode + ' with outputmode ' + config.outputmode +
                        ' is not a supported service (SRT to UDP, or UDP to SRT)');
        }
    }

    if (errors.length > 0) {
        return { config: null, errors: errors, warnings: warnings };
    }

    if (normalizeConfigModes(config)) {
        warnings.push('SRT mode updated to match the service direction');
    }

    // Interfaces are per machine, so a config from another box may name one
    // this one does not have. Worth knowing before it fails to start, but not a
    // reason to refuse it.
    var present = Object.keys(os.networkInterfaces());
    ['sourceinterface', 'outputinterface'].forEach(name => {
        if (config[name] && present.indexOf(config[name]) < 0) {
            warnings.push(name + ' ' + config[name] + ' does not exist on this machine');
        }
    });

    // Same for a listener address: right on the machine the backup came from,
    // possibly not here.
    var listenProblem = listenerAddressProblem(config);
    if (listenProblem) {
        warnings.push(listenProblem);
    }

    return { config: config, errors: errors, warnings: warnings };
}

// The config field holding the address a service's SRT listener binds, or
// null when it has none: a receiver in listener mode (srtpush) waits on its
// source address, a server in listener mode (srtpull) on its output address.
function srtListenerAddressField(config) {
    if (!config) {
        return null;
    }
    if (config.sourcemode === 'srtpush') {
        return 'sourceaddress';
    }
    if (config.outputmode === 'srtpull') {
        return 'outputaddress';
    }
    return null;
}

// The IPv4 addresses an SRT listener can bind here. srthub runs with
// --net=host, so the container sees this machine's addresses; 0.0.0.0 means
// all of them.
function localListenAddresses() {
    var found = ['0.0.0.0'];
    var interfaces = os.networkInterfaces();
    Object.keys(interfaces).forEach(name => {
        (interfaces[name] || []).forEach(entry => {
            if (entry && (entry.family === 'IPv4' || entry.family === 4) && found.indexOf(entry.address) < 0) {
                found.push(entry.address);
            }
        });
    });
    return found;
}

// Why a service's SRT listener could not bind its address, or null when it
// can (or the service has no listener). A listener given a remote address -
// usually a sender that should have been called instead - fails to bind, and
// srthub can only report that once it is running.
function listenerAddressProblem(config) {
    var field = srtListenerAddressField(config);
    if (!field) {
        return null;
    }
    var address = String(config[field] || '').trim();
    var addresses = localListenAddresses();
    if (address === '') {
        return 'Listener mode needs an address on this machine to wait on - ' +
               'use 0.0.0.0 for all of them, or one of: ' + addresses.slice(1).join(', ') + '.';
    }
    if (addresses.indexOf(address) < 0) {
        return 'Listener mode waits for a connection on an address of this machine, and ' + address +
               ' is not one of them. Use 0.0.0.0 or one of: ' + addresses.slice(1).join(', ') + '. ' +
               'To connect out to a sender at ' + address + ', choose Caller mode instead.';
    }
    return null;
}

// The ports a service binds: a UDP input, an SRT listener input, an SRT
// listener output - a UDP to SRT listener service binds two. Each comes with
// the address it binds, because two UDP inputs may share a port when they join
// different multicast groups.
function configListenPorts(config) {
    var ports = [];
    if (!config) {
        return ports;
    }
    if (config.sourcemode === 'udp' || config.sourcemode === 'srtpush') {
        ports.push({ port: config.sourceport, address: config.sourceaddress || '' });
    }
    if (config.outputmode === 'srtpull') {
        ports.push({ port: config.outputport, address: config.outputaddress || '' });
    }
    return ports.filter(p => p.port);
}

function isMulticastAddress(address) {
    var match = /^(\d{1,3})\./.exec(String(address || ''));
    return !!match && Number(match[1]) >= 224 && Number(match[1]) <= 239;
}

function portsCollide(a, b) {
    if (String(a.port) !== String(b.port)) {
        return false;
    }
    if (isMulticastAddress(a.address) && isMulticastAddress(b.address) && a.address !== b.address) {
        return false;
    }
    return true;
}

function configSummary(config) {
    if (!config) {
        return null;
    }
    var receiver = isSrtMode(config.sourcemode);
    return {
        sourcename: config.sourcename,
        type: receiver ? 'SRT Receiver' : 'SRT Server',
        input: (receiver ? 'SRT ' : 'UDP ') + (config.sourceaddress || '*') + ':' + config.sourceport,
        output: (receiver ? 'UDP ' : 'SRT ') + (config.outputaddress || '*') + ':' + config.outputport
    };
}

// Field order is fixed so two configs with the same settings compare equal
// however their files happened to be written.
function canonicalConfig(config) {
    var out = {};
    Object.keys(CONFIG_FIELDS).forEach(name => {
        if (Object.prototype.hasOwnProperty.call(config, name)) {
            out[name] = config[name];
        }
    });
    return JSON.stringify(out);
}

// Every config on disk now, keyed by id. A file that will not parse is still
// listed - its id is taken - but has no config to compare against.
function readExistingConfigs() {
    var existing = {};
    var names;

    try {
        names = fs.readdirSync(configFolder);
    } catch (e) {
        return existing;
    }

    names.forEach(name => {
        if (getExtension(name) !== '.json') {
            return;
        }
        var id = path.basename(name, '.json');
        var entry = { id: id, config: null, canonical: null, running: false };
        try {
            var raw = fs.readFileSync(path.join(configFolder, name));
            var parsed = JSON.parse(raw.toString('utf8'));
            if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
                entry.config = parsed;
                // Compared in the same normalised form an upload is put into, so
                // a config still carrying the legacy bare "srt" mode matches its
                // own backup instead of looking like a conflict with it.
                var checked = validateConfigText(raw);
                entry.canonical = canonicalConfig(checked.config || parsed);
            }
        } catch (e) {
            // unreadable: the id still counts as taken
        }
        entry.running = fs.existsSync(statusFolder + '/corestatus_' + id + '.json') ||
                        fs.existsSync(statusFolder + '/' + id + '.lock');
        existing[id] = entry;
    });

    return existing;
}

// ---- reading an upload --------------------------------------------------------

function tarString(block, start, length) {
    var end = start;
    while (end < start + length && block[end] !== 0) {
        end++;
    }
    return block.toString('utf8', start, end);
}

function tarOctal(block, start, length) {
    if (block[start] & 0x80) {
        throw new Error('the archive uses a size encoding no backup of this size needs');
    }
    var text = tarString(block, start, length).trim();
    if (text === '') {
        return 0;
    }
    if (!/^[0-7]+$/.test(text)) {
        throw new Error('the archive has a damaged header');
    }
    return parseInt(text, 8);
}

// A ustar/GNU/pax reader just large enough for a config backup. Only regular
// files are returned; directories, links and devices are skipped, and nothing is
// ever written anywhere. Throws on any structural damage.
function readTarEntries(buffer) {
    var entries = [];
    var offset = 0;
    var longName = null;
    var paxName = null;
    var skipped = 0;

    while (offset + 512 <= buffer.length) {
        var header = buffer.subarray(offset, offset + 512);

        if (header.every(b => b === 0)) {
            break;                                  // end of archive
        }

        var stored = tarOctal(header, 148, 8);
        var sum = 0;
        for (var i = 0; i < 512; i++) {
            sum += (i >= 148 && i < 156) ? 0x20 : header[i];
        }
        if (sum !== stored) {
            throw new Error('the archive is corrupt (a header checksum does not match)');
        }

        var size = tarOctal(header, 124, 12);
        var type = String.fromCharCode(header[156] || 0x30);
        var name = tarString(header, 0, 100);
        if (header.toString('ascii', 257, 262) === 'ustar') {
            var prefix = tarString(header, 345, 155);
            if (prefix) {
                name = prefix + '/' + name;
            }
        }

        var dataStart = offset + 512;
        var dataEnd = dataStart + size;
        if (dataEnd > buffer.length) {
            throw new Error('the archive is truncated');
        }
        var data = buffer.subarray(dataStart, dataEnd);

        if (type === 'L') {
            longName = tarString(data, 0, data.length);
        } else if (type === 'x') {
            var match = /(?:^|\n)\d+ path=([^\n]*)\n/.exec(data.toString('utf8'));
            paxName = match ? match[1] : null;
        } else if (type === 'g') {
            // global pax header: nothing in it matters here
        } else {
            if (type === '0' || type === '7') {
                entries.push({ name: paxName || longName || name, data: data });
                if (entries.length > MAX_RESTORE_FILES * 4) {
                    throw new Error('the archive holds more files than a config backup can');
                }
            } else if (type !== '5') {
                skipped++;
            }
            longName = null;
            paxName = null;
        }

        offset = dataStart + Math.ceil(size / 512) * 512;
    }

    return { entries: entries, skipped: skipped };
}

// Turns one upload into the list of config files it holds. A gzip is expected to
// hold a tar; a bare tar is accepted too; anything else is treated as a single
// .json file. Returns { files: [{ source, idHint, data }], notes } or { error }.
function readRestoreUpload(buffer, uploadName) {
    var notes = [];

    if (!buffer || buffer.length === 0) {
        return { error: 'the file is empty' };
    }
    if (buffer[0] === 0x50 && buffer[1] === 0x4b) {
        return { error: 'zip files are not supported; use a .tar.gz from Backup Configs, or the .json files' };
    }

    var isGzip = (buffer[0] === 0x1f && buffer[1] === 0x8b);
    var isTar = (buffer.length >= 512 && buffer.toString('ascii', 257, 262) === 'ustar');

    if (!isGzip && !isTar) {
        var hint = path.basename(String(uploadName || '')).match(/^([0-9]{1,12})\.json$/i);
        return {
            files: [{ source: path.basename(String(uploadName || 'upload.json')).substring(0, 200),
                      idHint: hint ? hint[1] : null, data: buffer }],
            notes: notes
        };
    }

    var tar = buffer;
    if (isGzip) {
        try {
            tar = zlib.gunzipSync(buffer, { maxOutputLength: MAX_RESTORE_EXPANDED });
        } catch (e) {
            if (e && e.code === 'ERR_BUFFER_TOO_LARGE') {
                return { error: 'the archive expands to more than a config backup can be' };
            }
            return { error: 'the archive is corrupt or truncated (it will not decompress)' };
        }
        if (tar.length < 512 || tar.toString('ascii', 257, 262) !== 'ustar') {
            return { error: 'the archive is compressed but does not contain a tar file' };
        }
    }

    var read;
    try {
        read = readTarEntries(tar);
    } catch (e) {
        return { error: e.message };
    }

    var files = [];
    var ignored = 0;
    var seen = {};

    read.entries.forEach(entry => {
        var base = String(entry.name).split('/').pop();
        // macOS adds ._name resource forks and __MACOSX folders when it builds an
        // archive; neither is a config.
        if (!/\.json$/i.test(base) || base.charAt(0) === '.' || /(^|\/)__MACOSX\//.test(entry.name)) {
            ignored++;
            return;
        }
        var match = base.match(/^([0-9]{1,12})\.json$/i);
        var idHint = match ? match[1] : null;
        if (idHint && seen[idHint]) {
            notes.push('the archive holds ' + idHint + '.json more than once; later copies restore as new services');
            idHint = null;
        }
        if (idHint) {
            seen[idHint] = true;
        }
        files.push({ source: String(entry.name).substring(0, 200), idHint: idHint, data: entry.data });
    });

    if (files.length > MAX_RESTORE_FILES) {
        return { error: 'the archive holds more than ' + MAX_RESTORE_FILES + ' configs' };
    }
    if (ignored + read.skipped > 0) {
        notes.push('skipped ' + (ignored + read.skipped) + ' entr' + (ignored + read.skipped === 1 ? 'y' : 'ies') +
                   ' that ' + (ignored + read.skipped === 1 ? 'is' : 'are') + ' not a service config');
    }
    if (files.length === 0) {
        return { error: 'the archive holds no service configs' };
    }

    return { files: files, notes: notes };
}

// ---- inspect and apply ----------------------------------------------------------

function inspectRestoreFiles(files, existing) {
    return files.map((file, index) => {
        var checked = validateConfigText(file.data);
        var item = {
            index: index,
            source: file.source,
            id: file.idHint,
            status: 'invalid',
            errors: checked.errors,
            warnings: checked.warnings.slice(),
            summary: configSummary(checked.config),
            existing: null,
            config: checked.config        // stripped before anything is sent back
        };

        if (!checked.config) {
            return item;
        }

        var match = file.idHint ? existing[file.idHint] : null;
        var mine = canonicalConfig(checked.config);

        // No service under this id, or no id to go on at all (a file not named
        // for one): a service with exactly these settings under any id is still
        // this service, so uploading the same file twice does not make two.
        if (!match) {
            Object.keys(existing).some(id => {
                if (existing[id].canonical === mine) {
                    match = existing[id];
                    return true;
                }
                return false;
            });
        }

        if (match) {
            item.existing = {
                id: match.id,
                sourcename: match.config && typeof match.config.sourcename === 'string' ?
                            match.config.sourcename.substring(0, 128) : '',
                running: match.running
            };
            item.status = (match.canonical === mine) ? 'identical' : 'conflict';
        } else {
            item.status = 'new';
        }

        // Another service, under a different id, that this one would collide
        // with once both are running.
        var name = checked.config.sourcename.toLowerCase();
        var ports = configListenPorts(checked.config);
        Object.keys(existing).forEach(id => {
            if (id === file.idHint || (match && id === match.id) || !existing[id].config) {
                return;
            }
            var other = existing[id].config;
            if (typeof other.sourcename === 'string' && other.sourcename.trim().toLowerCase() === name) {
                item.warnings.push('a service named "' + checked.config.sourcename + '" already exists (id ' + id + ')');
            }
            var theirs = configListenPorts(other);
            ports.forEach(mine => {
                if (theirs.some(t => portsCollide(mine, t))) {
                    item.warnings.push('port ' + mine.port + ' is already used by service ' + id +
                                       (typeof other.sourcename === 'string' ? ' (' + other.sourcename.substring(0, 64) + ')' : ''));
                }
            });
        });

        return item;
    });
}

function nextServiceId(taken) {
    var id = seconds_since_epoch();
    Object.keys(taken).forEach(existingId => {
        if (SERVICE_ID_PATTERN.test(existingId) && Number(existingId) >= id) {
            id = Number(existingId) + 1;
        }
    });
    while (taken[String(id)] || fs.existsSync(path.join(configFolder, id + '.json'))) {
        id++;
    }
    return String(id);
}

// Written beside the target under a name the service listing ignores, then
// moved into place: a new config by link(), which refuses to overwrite, and a
// replacement by rename(), which is atomic. A reader never sees half a config.
function writeConfigFile(id, config, replace) {
    if (!SERVICE_ID_PATTERN.test(id)) {
        throw new Error('refusing a service id that is not all digits');
    }
    var target = path.join(configFolder, id + '.json');
    var temp = path.join(configFolder, '.restore-' + id + '-' + crypto.randomBytes(6).toString('hex') + '.tmp');

    fs.writeFileSync(temp, JSON.stringify(config), { flag: 'wx', mode: 0o644 });
    try {
        if (replace) {
            fs.renameSync(temp, target);
        } else {
            fs.linkSync(temp, target);
            fs.unlinkSync(temp);
        }
    } catch (e) {
        try { fs.unlinkSync(temp); } catch (ignore) { /* already gone */ }
        throw e;
    }
}

function applyRestore(items, actions, existing) {
    var taken = Object.assign({}, existing);

    return items.map(item => {
        var requested = actions[String(item.index)];
        var action = (typeof requested === 'string') ? requested : null;
        var result = { index: item.index, source: item.source, result: 'skipped', id: null, reason: null };

        if (item.status === 'invalid') {
            result.reason = 'not a valid service config';
            return result;
        }
        if (!action) {
            action = (item.status === 'new') ? 'restore' : 'skip';
        }
        if (['skip', 'restore', 'replace', 'new'].indexOf(action) < 0) {
            result.reason = 'unknown action';
            return result;
        }
        if (action === 'skip') {
            result.reason = (item.status === 'identical') ? 'already present and identical' : 'skipped';
            return result;
        }

        try {
            if (action === 'replace') {
                if (!item.existing) {
                    result.reason = 'there is no existing service to replace';
                    return result;
                }
                if (item.existing.running) {
                    result.result = 'failed';
                    result.reason = 'service ' + item.existing.id + ' is running; stop it before replacing it';
                    return result;
                }
                writeConfigFile(item.existing.id, item.config, true);
                result.result = 'replaced';
                result.id = item.existing.id;
                return result;
            }

            // restore keeps the id from the filename when it is free, so a
            // backup restored onto a fresh machine comes back as the same
            // services; new, or a taken id, gets a fresh one.
            var id = (action === 'restore' && item.id && !taken[item.id]) ? item.id : nextServiceId(taken);
            if (action === 'restore' && item.status !== 'new' && item.id && taken[item.id]) {
                result.reason = 'id ' + item.id + ' was taken, so it was added as a new service';
            }
            writeConfigFile(id, item.config, false);
            taken[id] = { id: id, config: item.config, canonical: canonicalConfig(item.config), running: false };
            activeconfigurations++;
            result.result = 'added';
            result.id = id;
            return result;
        } catch (e) {
            console.log('restore of ' + item.source + ' failed: ', e.message);
            result.result = 'failed';
            result.reason = (e.code === 'EEXIST') ? 'that id was taken while restoring' : 'could not be written';
            return result;
        }
    });
}

function publicRestoreItem(item) {
    return {
        index: item.index,
        source: item.source,
        id: item.id,
        status: item.status,
        errors: item.errors.slice(0, 20),
        warnings: item.warnings.slice(0, 20),
        summary: item.summary,
        existing: item.existing
    };
}

app.get('/api/v1/backup_configs', auth, (req, res) => {
    var names;

    try {
        names = fs.readdirSync(configFolder).filter(n => getExtension(n) === '.json').sort();
    } catch (e) {
        res.status(500).json({ error: 'unable to read the config folder' });
        return;
    }

    var host = os.hostname().replace(/[^A-Za-z0-9._-]/g, '') || 'srthub';
    var stamp = new Date().toISOString().replace(/[-:]/g, '').replace('T', '-').substring(0, 15);
    var archiver = require('archiver');
    var tar = archiver('tar', { gzip: true, gzipOptions: { level: 9 } });

    res.set('Content-Type', 'application/gzip');
    res.set('Content-Disposition', 'attachment; filename="opensrthub-configs-' + host + '-' + stamp + '.tar.gz"');
    res.set('Cache-Control', 'no-store');

    tar.on('warning', (err) => console.log('config backup: ', err.message));
    tar.on('error', (err) => {
        console.log('config backup failed: ', err.message);
        res.destroy(err);
    });
    tar.pipe(res);

    names.forEach(name => {
        try {
            var full = path.join(configFolder, name);
            if (fs.lstatSync(full).isFile()) {
                tar.append(fs.readFileSync(full), { name: 'opensrthub-configs/' + name, mode: 0o644 });
            }
        } catch (e) {
            console.log('config backup skipped ' + name + ': ', e.message);
        }
    });
    tar.finalize();
});

// Only application/octet-stream is read as an upload. A cross-site form cannot
// send that type, and a cross-site script cannot send it without a preflight
// this server never answers, so the type is part of the CSRF defence alongside
// the SameSite session cookie.
var restoreBody = express.raw({ type: 'application/octet-stream', limit: MAX_RESTORE_UPLOAD });

app.post('/api/v1/restore_configs', auth, (req, res, next) => {
    restoreBody(req, res, (err) => {
        if (err) {
            var tooLarge = (err.type === 'entity.too.large');
            res.status(tooLarge ? 413 : 400).json({
                error: tooLarge ? 'the file is larger than a config backup can be' : 'the upload could not be read'
            });
            return;
        }
        next();
    });
}, (req, res) => {
    if (!Buffer.isBuffer(req.body)) {
        res.status(415).json({ error: 'upload the file as application/octet-stream' });
        return;
    }

    var mode = String(req.query.mode || 'inspect');
    if (mode !== 'inspect' && mode !== 'apply') {
        res.status(400).json({ error: 'mode must be inspect or apply' });
        return;
    }

    var actions = {};
    if (mode === 'apply' && req.query.actions !== undefined) {
        try {
            var parsedActions = JSON.parse(String(req.query.actions));
            if (parsedActions && typeof parsedActions === 'object' && !Array.isArray(parsedActions)) {
                Object.keys(parsedActions).forEach(key => {
                    if (/^[0-9]{1,4}$/.test(key) && typeof parsedActions[key] === 'string') {
                        actions[key] = parsedActions[key];
                    }
                });
            }
        } catch (e) {
            res.status(400).json({ error: 'actions is not valid JSON' });
            return;
        }
    }

    var upload = readRestoreUpload(req.body, req.query.name);
    if (upload.error) {
        res.status(422).json({ error: upload.error });
        return;
    }

    // Everything from here on is synchronous, so no other request can change the
    // config folder between the checks and the writes.
    var existing = readExistingConfigs();
    var items = inspectRestoreFiles(upload.files, existing);
    var body = {
        mode: mode,
        notes: upload.notes,
        files: items.map(publicRestoreItem)
    };

    if (mode === 'apply') {
        body.results = applyRestore(items, actions, existing);
        console.log('config restore: ' + body.results.map(r => r.source + '=' + r.result).join(', '));
    }

    res.set('Cache-Control', 'no-store');
    res.json(body);
});

app.get('/api/v1/get_service_count', auth, (req, res) => {
    var services;

    obj = new Object();
    var retdata;

    services = activeconfigurations;
    obj.services = services;

    retdata = JSON.stringify(obj);
    res.send(retdata);
});

// NEW: JSON-based endpoint for the redesigned frontend
app.get('/api/v1/get_services', auth, (req, res) => {
    var services = [];

    if (!fs.existsSync(configFolder)) {
        return res.json({ services: [] });
    }

    var files = fs.readdirSync(configFolder);
    var configIndex = 0;

    files.forEach(file => {
        if (getExtension(file) == '.json') {
            configIndex++;
            var fullfile = configFolder + '/' + file;
            var fileprefix = path.basename(fullfile, '.json');

            try {
                var configdata = fs.readFileSync(fullfile, 'utf8');
                var config = JSON.parse(configdata);

                var service = {
                    id: configIndex,
                    fileprefix: fileprefix,
                    sourcename: config.sourcename || 'Unnamed Service',
                    sourcemode: config.sourcemode || 'unknown',
                    sourceaddress: config.sourceaddress || '',
                    sourceport: config.sourceport || '',
                    sourceinterface: config.sourceinterface || '',
                    outputmode: config.outputmode || 'unknown',
                    outputaddress: config.outputaddress || '',
                    outputport: config.outputport || '',
                    outputinterface: config.outputinterface || '',
                    clienttype: config.clienttype || '',
                    servertype: config.servertype || '',
                    connectionqueue: config.connectionqueue || '',
                    passphrase: config.passphrase ? 'enabled' : 'none',
                    streamid: config.streamid || '',
                    status: 'stopped',
                    uptime: -1,
                    thumbnailUrl: '/api/v1/thumbnail/' + fileprefix + '.jpg',
                    audioservices: []
                };

                // Check if service is running by looking for corestatus file
                var corestatusfile = statusFolder + '/corestatus_' + fileprefix + '.json';
                if (fs.existsSync(corestatusfile)) {
                    try {
                        var statusdata = fs.readFileSync(corestatusfile, 'utf8');
                        var sfd = JSON.parse(statusdata);
                        service.status = 'running';
                        // Uptime is in milliseconds, convert to seconds
                        service.uptime = sfd["srthub-uptime"] ? sfd["srthub-uptime"] / 1000 : 0;

                        // Elementary stream pids straight from the PMT. The
                        // audio entries carry the decoder's audio index, which
                        // is what lines them up with the audio status files.
                        // Single- or multi-program transport stream, from
                        // the PAT; srthub monitors one program of an MPTS.
                        service.transport = {
                            type: (sfd["transport-type"] === 'SPTS' || sfd["transport-type"] === 'MPTS') ?
                                  sfd["transport-type"] : '',
                            programCount: Number(sfd["program-count"]) || 0,
                            programNumber: Number(sfd["program-number"]) || 0,
                            // every program in the PAT, for choosing one to monitor
                            programs: Array.isArray(sfd["programs"]) ?
                                      sfd["programs"].map(Number).filter(n => Number.isInteger(n) && n > 0 && n <= 65535) : [],
                            programRequested: Number(sfd["program-requested"]) || 0,
                            programFound: sfd["program-found"] === 1
                        };

                        service.pids = {
                            pcr: sfd["pcr-pid"] || 0,
                            video: sfd["video-pid"] || 0,
                            videoType: sfd["video-type"] || '',
                            audio: Array.isArray(sfd["audio-pids"]) ? sfd["audio-pids"] : []
                        };

                        // SCTE-35: pid presence comes from the PMT, the cue
                        // fields are only present once a cue has been seen.
                        service.scte35 = {
                            present: sfd["scte35-present"] === 1,
                            pid: sfd["scte35-pid"] || 0,
                            cueCount: sfd["scte35-cue-count"] || 0,
                            lastCue: sfd["scte35-last-cue"] || '',
                            lastCueName: sfd["scte35-last-cue-name"] || '',
                            lastCueImmediate: sfd["scte35-last-cue-immediate"] === 1,
                            lastCueDuration: sfd["scte35-last-cue-duration"] || 0,
                            lastCueEventId: sfd["scte35-last-cue-event-id"],
                            lastCueCancel: sfd["scte35-last-cue-cancel"] === 1,
                            lastCueTime: sfd["scte35-last-cue-time"] || 0
                        };
                    } catch (e) {
                        console.log('Error reading status file:', e);
                    }
                }

                // Get audio service info (up to 8 audio tracks)
                for (var i = 0; i < 8; i++) {
                    var audioStatusFile = statusFolder + '/audio_' + i + '_' + fileprefix + '.json';
                    if (fs.existsSync(audioStatusFile)) {
                        try {
                            var audiodata = fs.readFileSync(audioStatusFile, 'utf8');
                            if (audiodata) {
                                var ad = JSON.parse(audiodata);
                                service.audioservices.push({
                                    index: i,
                                    codec: ad["audio-codec"] || 'Unknown',
                                    channels: ad["audio-channels"] || 0,
                                    samplerate: ad["audio-samplerate"] || 0
                                });
                            }
                        } catch (e) {
                            console.log('Error reading audio file:', e);
                        }
                    }
                }

                // Get SRT receiver stats if applicable
                if (isSrtMode(config.sourcemode)) {
                    var srtReceiverFile = statusFolder + '/srt_receiver_' + fileprefix + '.json';
                    if (fs.existsSync(srtReceiverFile)) {
                        try {
                            var srtdata = fs.readFileSync(srtReceiverFile, 'utf8');
                            var srt = JSON.parse(srtdata);
                            service.srt = {
                                connected: srt["srt-connection"] === 1,
                                mode: srt["srt-mode"] || '',
                                bitrate: (srt["bitrate-kbps"] || 0) * SRT_BITRATE_SCALE,
                                packetsReceived: srt["packets-received"] || 0,
                                packetsDropped: srt["packets-dropped"] || 0,
                                packetsLost: srt["packets-lost"] || 0,
                                packetsRetransmitted: srt["packets-retransmitted"] || 0,
                                lossPercentage: srt["loss-percentage"] || 0,
                                rtt: srt.rtt || 0,
                                latency: srt.latencyms || 0,
                                clientAddress: srt["client-address"] || '',
                                clientPort: srt["client-port"] || ''
                            };
                        } catch (e) {
                            console.log('Error reading SRT receiver file:', e);
                        }
                    }
                }

                // Get UDP receiver stats if applicable
                if (config.sourcemode === 'udp') {
                    var udpReceiverFile = statusFolder + '/udp_receiver_' + fileprefix + '.json';
                    if (fs.existsSync(udpReceiverFile)) {
                        try {
                            var udpdata = fs.readFileSync(udpReceiverFile, 'utf8');
                            var udp = JSON.parse(udpdata);
                            service.udpReceiver = {
                                active: udp["udp-source-active"] === 1,
                                bytesReceived: udp["total-bytes-received"] || 0,
                                packetsReceived: udp["total-packets-received"] || 0,
                                bitrate: udp["udp-source-kbps"] || 0,
                                multicastInput: udp["multicast-input"] || ''
                            };
                        } catch (e) {
                            console.log('Error reading UDP receiver file:', e);
                        }
                    }
                }

                // An SRT listener that cannot bind its address. srthub keeps
                // retrying and removes the file once it binds.
                var listenErrorFile = statusFolder + '/srt_listen_error_' + fileprefix + '.json';
                if (fs.existsSync(listenErrorFile)) {
                    try {
                        var listenError = JSON.parse(fs.readFileSync(listenErrorFile, 'utf8'));
                        service.listenError = {
                            address: String(listenError.address || ''),
                            port: listenError.port || '',
                            error: String(listenError.error || '')
                        };
                    } catch (e) {
                        console.log('Error reading SRT listen error file:', e);
                    }
                }

                // Get thumbnail/video info if available
                var thumbnailStatusFile = statusFolder + '/thumbnail_' + fileprefix + '.json';
                if (fs.existsSync(thumbnailStatusFile)) {
                    try {
                        var thumbdata = fs.readFileSync(thumbnailStatusFile, 'utf8');
                        var thumb = JSON.parse(thumbdata);
                        service.video = {
                            width: thumb.width || 0,
                            height: thumb.height || 0,
                            codec: thumb["video-codec"] || '',
                            format: thumb["source-format"] || '',
                            frameRate: thumb["frame-rate"] || '',
                            frameRateValue: thumb["frame-rate-value"] || 0,
                            aspectRatio: thumb["display-aspect-ratio"] || '',
                            aspectRatioValue: thumb["display-aspect-ratio-value"] || 0,
                            sampleAspectRatio: thumb["sample-aspect-ratio"] || '',
                            // afdReported separates "this srthub reports AFD
                            // and the source has none" from "this srthub does
                            // not report AFD at all", which the card needs in
                            // order to say "none" without guessing.
                            afdReported: (thumb["afd-present"] !== undefined &&
                                          thumb["afd-present"] !== null),
                            afdPresent: thumb["afd-present"] === 1,
                            afdCode: (thumb["afd-code"] === undefined ||
                                      thumb["afd-code"] === null) ? -1 : thumb["afd-code"],
                            afd: thumb["afd"] || '',
                            totalStreams: thumb["total-streams"] || 0,
                            currentStream: thumb["current-stream"] || 0,
                            errors: thumb["transport-source-errors"] || 0,
                            lastError: thumb["last-source-error"] || ''
                        };
                    } catch (e) {
                        console.log('Error reading thumbnail status file:', e);
                    }
                }

                // Get SRT server stats if applicable (output mode)
                if (isSrtMode(config.outputmode)) {
                    service.srtServer = { connections: [] };
                    for (var i = 0; i < 16; i++) {
                        var srtServerFile = statusFolder + '/srt_server_thread_' + i + '_' + fileprefix + '.json';
                        if (fs.existsSync(srtServerFile)) {
                            try {
                                var serverdata = fs.readFileSync(srtServerFile, 'utf8');
                                var server = JSON.parse(serverdata);
                                service.srtServer.connections.push({
                                    thread: server.thread,
                                    clientAddress: server["client-address"],
                                    clientPort: server["client-port"],
                                    bytesSent: server["total-bytes-sent"],
                                    packetsSent: server["total-packets-sent"]
                                });
                            } catch (e) {
                                // Connection file doesn't exist or is invalid
                            }
                        }
                    }
                }

                // Get UDP server stats if applicable (output mode)
                if (config.outputmode === 'udp') {
                    var udpServerFile = statusFolder + '/udp_server_' + fileprefix + '.json';
                    if (fs.existsSync(udpServerFile)) {
                        try {
                            var udpServerData = fs.readFileSync(udpServerFile, 'utf8');
                            var udpServer = JSON.parse(udpServerData);
                            service.udpServer = {
                                active: udpServer["udp-output-active"] === 1,
                                bytesSent: udpServer["total-bytes-sent"] || 0,
                                packetsSent: udpServer["total-packets-sent"] || 0,
                                lastBufferSize: udpServer["last-buffer-size"] || 0
                            };
                        } catch (e) {
                            console.log('Error reading UDP server file:', e);
                        }
                    }
                }

                services.push(service);
            } catch (e) {
                console.log('Error processing config file:', fullfile, e);
            }
        }
    });

    res.json({ services: services });
});

// Return the persisted bitrate history for one service (keyed by fileprefix).
app.get('/api/v1/get_bitrate_history/:uid', auth, (req, res) => {
    // uid is the service fileprefix; reduce to a bare basename as a precaution
    var prefix = path.basename(String(req.params.uid));
    var samples = bitrateHistory[prefix] || [];
    res.json({ fileprefix: prefix, samples: samples });
});

// Clear/reset the persisted bitrate history for one service (or all with "all").
app.post('/api/v1/clear_bitrate_history/:uid', auth, (req, res) => {
    var prefix = path.basename(String(req.params.uid));
    if (prefix === 'all' || prefix === '*') {
        bitrateHistory = {};
    } else {
        delete bitrateHistory[prefix];
    }
    persistBitrateHistory();   // flush immediately so a reload/restart stays clean
    res.json({ ok: true, fileprefix: prefix });
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

app.get('/api/v1/get_log_page', auth, (req, res) => {
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

app.get('/api/v1/get_control_page', auth, (req, res) => {
    var html = '';
    var i;
    var files = fs.readdirSync(configFolder);
    var listedfiles = 0;

    html += '<table>';
    html += '<thead>';
    html += '<tr class="header">';
    html += '<th>#<div><font size="4">#</font></div></th>';
    html += '<th>Name<div><font size="4">Name</font></div></th>';
    html += '<th>Control<div><font size="4">Control</font></div></th>';
    html += '<th>Connection State<div><font size="4">Connection State</font></div></th>';
    html += '<th hidden>Time Connected<div><font size="4">Time Connected</font></div></th>';
    html += '<th>Input/Output Info<div><font size="4">Input/Output Info</font></div></th>';
    html += '<th hidden>Output<div><font size="4">Output</font></div></th>';
    html += '<th>Source Image<div><font size="4">Source Image</font></div></th>';
    html += '<th>Status<div><font size="4">Status</font></div></th>';
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

            html += '<div>';
            html += '<tr><td style="background-color:#800"></td>';
            html += '<td style="background-color:#800"></td>';
            html += '<td style="background-color:#800"></td>';
            //html += '<td style="background-color:darkgrey"></td>';
            html += '<td style="background-color:#800"></td>';
            html += '<td style="background-color:#800"></td>';
            html += '<td style="background-color:#800"></td>';
            html += '<td style="background-color:#800"></td></tr>';
            html += '</div>';

            html += '<tr>';
            html += '<td><font size="4">' + fileprefix + '</font></td>';
            html += '<td><font size="4">' + words.sourcename + '</font></td>';
            html += '<td>';
            html += '<button style="width:95%;border-radius:12px;cursor:pointer;background-color:darkgrey;font-size:18px;padding:10px 10px;" id=\'start_service'+configindex+'\'>Start Service</button><br><br>';
            html += '<button style="width:95%;border-radius:12px;cursor:pointer;background-color:darkgrey;font-size:18px;padding:10px 10px;" id=\'stop_service'+configindex+'\'>Stop Service</button><br><br>';
            html += '<button hidden style="width:95%" id=\'reset_button'+configindex+'\'>Reset</button>';
            html += '<button style="width:95%;border-radius:12px;cursor:pointer;background-color:darkgrey;font-size:18px;padding:10px 10px;" id=\'update_service'+configindex+'\'>Update Service</button><br><br>';
            html += '<button style="width:95%;border-radius:12px;cursor:pointer;background-color:darkgrey;font-size:18px;padding:10px 10px;" id=\'remove'+configindex+'\' type=\'remove\'>Remove Service</button>';
            html += '</td>';
            html += '<td><div id=\'active'+configindex+'\'></div></td>';
            html += '<td hidden><div id=\'uptime'+configindex+'\'></div></td>';
            html += '<td><div id=\'input'+configindex+'\'></div></td>';

            html += '<td hidden><div id=\'output'+configindex+'\'></div></td>';
            html += '<td>';
            html += '<center><img class="w3-border w3-padding w3-topbar w3-bottombar" style="padding:16px;height:100%;width:100X" src=\'http://'+req.hostname+':8080'+'/api/v1/thumbnail/'+fileprefix+'.jpg?='+ new Date().getTime() +'\' id=\'thumbnail'+fileprefix+'\'/></center>';
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

app.get('/api/v1/thumbnail/:uid', auth, (req, res) => {
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

app.get('/api/v1/get_interfaces', auth, (req, res) => {
    res.send(networkInterfaces);
});

app.post('/api/v1/remove_service/:uid', auth, (req, res) => {
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

app.post('/api/v1/new_srt_receiver', auth, (req, res) => {
    console.log('received new srt receiver request');
    console.log('body is ',req.body);

    var words = req.body;
    var config = new Object();

    config.sourcename = words.srtreceiver_sourcename;
    // srthub needs the direction encoded in the mode string, not a bare "srt".
    config.clienttype = receiverDirection(words.srtreceiver_clienttype);
    config.sourcemode = srtModeFor(config.clienttype);
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
    config.latency = words.srtreceiver_latency;
    //config.keysize = words.srtreceiver_keysize;

    var listenProblem = listenerAddressProblem(config);
    if (listenProblem) {
        return res.status(400).json({ status: 'failed', error: listenProblem });
    }

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

app.post('/api/v1/new_srt_server', auth, (req, res) => {
    console.log('received new srt server request');
    console.log('body is ',req.body);

    var words = req.body;
    var config = new Object();

    config.sourcename = words.srtserver_sourcename;
    config.sourcemode = "udp";
    config.sourceaddress = words.srtserver_sourceaddress;
    config.sourceport = words.srtserver_sourceport;
    config.sourceinterface = words.srtserver_sourceinterface;
    // srthub needs the direction encoded in the mode string, not a bare "srt".
    config.servertype = serverDirection(words.srtserver_servertype);
    config.outputmode = srtModeFor(config.servertype);
    config.outputaddress = words.srtserver_address;
    config.outputport = words.srtserver_port;
    config.outputinterface = words.srtserver_interface;
    config.streamid = words.srtserver_streamid;
    config.passphrase = words.srtserver_passphrase;
    config.connectionqueue = words.srtserver_connectionqueue;
    config.whitelist = words.srtserver_whitelist;
    config.managementserverip = words.srtserver_managementserverip;

    var listenProblem = listenerAddressProblem(config);
    if (listenProblem) {
        return res.status(400).json({ status: 'failed', error: listenProblem });
    }

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

app.post('/api/v1/status_update/:uid', localApiAuth, (req, res) => {
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

app.post('/api/v1/signal/:uid', localApiAuth, (req, res) => {
    console.log('receive event signal from: ', req.params.uid);
    console.log('body is ', req.body);

    if (fs.existsSync(logfilename)) {
        fs.appendFileSync(logfilename, ','+JSON.stringify(req.body)+'\n');
    } else {
        fs.appendFileSync(logfilename, JSON.stringify(req.body)+'\n');
    }

    res.send(req.body);
});

app.post('/api/v1/stop_service/:uid', auth, (req, res) => {
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
                var listen_error_statusfile = statusFolder+'/srt_listen_error_'+file;
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
                if (fs.existsSync(listen_error_statusfile)) {
                    fs.unlinkSync(listen_error_statusfile)
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

app.post('/api/v1/start_service/:uid', auth, (req, res) => {
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

                // srthub reads the config file, so a config still carrying a bare
                // "srt" (or a mode that disagrees with clienttype/servertype) has
                // to be corrected on disk before the container starts. Computing
                // it into a local variable here, as this used to, had no effect -
                // the mode is never passed on the command line.
                if (normalizeConfigModes(words)) {
                    fs.writeFileSync(fullfile, JSON.stringify(words));
                    console.log('migrated SRT mode in '+fullfile+
                                ': sourcemode='+words.sourcemode+
                                ', outputmode='+words.outputmode);
                }

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

app.post('/api/v1/scan', auth, (req, res) => {
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

app.get('/api/v1/list_services', auth, (req, res) => {
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

// Get raw config for a single service (for editing)
app.get('/api/v1/get_config/:uid', auth, (req, res) => {
    console.log('get_config request for: ', req.params.uid);

    var files = fs.readdirSync(configFolder);
    var listedfiles = 0;
    var found = false;

    files.forEach(file => {
        if (getExtension(file) == '.json') {
            var configindex = listedfiles + 1;
            var fullfile = configFolder + '/' + file;
            var fileprefix = path.basename(fullfile, '.json');

            if ((configindex == req.params.uid) || (fileprefix == req.params.uid)) {
                try {
                    var configdata = fs.readFileSync(fullfile, 'utf8');
                    var config = JSON.parse(configdata);
                    config.fileprefix = fileprefix;
                    config.configindex = configindex;
                    res.status(200).json(config);
                    found = true;
                } catch (e) {
                    console.error('Error reading config:', e);
                    res.status(500).json({ error: 'Failed to read config' });
                    found = true;
                }
            }
            listedfiles++;
        }
    });

    if (!found) {
        res.status(404).json({ error: 'Config not found' });
    }
});

// Update an existing service config
app.post('/api/v1/update_config/:uid', auth, (req, res) => {
    console.log('update_config request for: ', req.params.uid);
    console.log('body is ', req.body);

    var files = fs.readdirSync(configFolder);
    var listedfiles = 0;
    var found = false;

    files.forEach(file => {
        if (getExtension(file) == '.json') {
            var configindex = listedfiles + 1;
            var fullfile = configFolder + '/' + file;
            var fileprefix = path.basename(fullfile, '.json');

            if ((configindex == req.params.uid) || (fileprefix == req.params.uid)) {
                try {
                    var existingdata = fs.readFileSync(fullfile, 'utf8');
                    var existingconfig = JSON.parse(existingdata);

                    // Merge submitted fields into existing config, preserving fields not in the update
                    var updates = req.body;
                    // Remove frontend-only helper fields if sent
                    delete updates.fileprefix;
                    delete updates.configindex;

                    var newconfig = Object.assign({}, existingconfig, updates);

                    // The edit form submits clienttype/servertype but not the
                    // mode string, so recompute it here - otherwise changing the
                    // direction in the UI would leave srthub starting the old one.
                    normalizeConfigModes(newconfig);

                    // srthub reads the program with atoi(), so only a program
                    // number or empty (automatic) is stored
                    if (Object.prototype.hasOwnProperty.call(updates, 'program')) {
                        var programCheck = validateConfigField('program', CONFIG_FIELDS.program,
                                                               String(updates.program == null ? '' : updates.program), []);
                        if (programCheck.error) {
                            res.status(400).json({ status: 'failed', error: programCheck.error });
                            found = true;
                            listedfiles++;
                            return;
                        }
                        newconfig.program = programCheck.value;
                    }

                    var listenProblem = listenerAddressProblem(newconfig);
                    if (listenProblem) {
                        res.status(400).json({ status: 'failed', error: listenProblem });
                        found = true;
                        listedfiles++;
                        return;
                    }

                    fs.writeFileSync(fullfile, JSON.stringify(newconfig));
                    console.log('Config updated: ', fullfile);

                    res.status(200).json({ status: 'success', fileprefix: fileprefix });
                    found = true;
                } catch (e) {
                    console.error('Error updating config:', e);
                    res.status(500).json({ status: 'failed', error: e.message });
                    found = true;
                }
            }
            listedfiles++;
        }
    });

    if (!found) {
        res.status(404).json({ status: 'invalid' });
    }
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

// Each log line is one JSON object, with every line after the first prefixed
// by a comma (the file is appended to as if it were one big array). Parsing
// line by line means a single truncated or malformed entry is skipped instead
// of discarding the whole page of log data.
function parseLogLine(line) {
    var text = String(line).trim();

    if (text.charAt(0) == ',') {
        text = text.substring(1);
    }
    if (text.length == 0) {
        return null;
    }
    try {
        return JSON.parse(text);
    } catch (e) {
        return null;
    }
}

function parseLogLines(lines) {
    var entries = [];

    String(lines).split('\n').forEach(line => {
        var entry = parseLogLine(line);
        if (entry) {
            entries.push(entry);
        } else if (String(line).trim().length > 0) {
            console.log('skipping malformed log line: ', String(line).trim().substring(0, 120));
        }
    });

    return entries;
}

function sendLogData(res, linecount) {
    readLastLines.read(logfilename, linecount)
        .then((lines) => {
            res.set('Expires', new Date().toUTCString());
            res.json(parseLogLines(lines));
        })
        .catch((err) => {
            console.log('unable to read log data: ', err);
            res.sendStatus(500);
        });
}

app.get('/api/v1/get_log_data', auth, (req, res) => {
    console.log('newest log filename: ', logfilename);
    if (fs.existsSync(logfilename)) {
        sendLogData(res, 6);
    } else {
        res.sendStatus(404);  // logdata was not found
    }
});

app.get('/api/v1/get_extended_log_data', auth, (req, res) => {
    console.log('newest log filename: ', logfilename);
    if (fs.existsSync(logfilename)) {
        sendLogData(res, 50);
    } else {
        res.sendStatus(404);  // logdata was not found
    }
});

// ---- A day of events at a time ----------------------------------------------
//
// srthub.log is rotated daily by /etc/logrotate.d/srthub, so one day's events
// are spread over a small number of files, and which files is not something the
// names can be trusted to answer. logrotate fires from a timer some time after
// midnight; with dateyesterday the file it writes is named for the day the
// events mostly came from, and mostly is the operative word, because the file
// named for the 4th holds the 4th from the rotation hour onwards while the 4th's
// small hours are still at the tail of the file named for the 3rd. A machine
// that was powered off across a rotation shifts the names further still.
//
// So files are picked by the window they were written in rather than by what
// they are called: sorted oldest first, a file can hold events for day D if it
// was last written at or after the start of D and the file before it was last
// written before the end of D. Each entry is then kept or dropped on its own
// accesstime, which is the only authority on which day an event belongs to.
//
// Days are UTC days, because accesstime is stamped UTC by esignal.c. That keeps
// the day test a string compare over hundreds of thousands of entries instead of
// a date parse, and the page says UTC so the boundary is not a surprise.

const LOG_RETENTION_DAYS = 30;       // matches "rotate 30" in the logrotate drop-in
const MAX_LOG_DAY_ENTRIES = 50000;   // newest entries held in memory for one day
const MAX_LOG_DAY_PAGE = 2000;       // most entries returned in one response
const LOG_DAY_CACHE_SIZE = 3;        // days kept parsed at once
const LOG_TAIL_WINDOW = 65536;       // bytes read when locating the final newline
const MAX_LOG_SEARCH_SIZE = 200;     // characters accepted in the text filter

// The messages all come from esignal.c, so the set is closed and can be matched
// exactly. The log carries no machine-readable event type of its own; matching
// on the text has the side benefit of classifying logs written by older builds
// just as well as new ones.
const LOG_EVENT_TYPES = [
    { id: 'scte35',     label: 'SCTE-35 cues' },
    { id: 'aspect',     label: 'Aspect ratio changes' },
    { id: 'resolution', label: 'Resolution changes' },
    { id: 'afd',        label: 'AFD changes' },
    { id: 'framerate',  label: 'Frame rate changes' },
    { id: 'input',      label: 'Input signal' },
    { id: 'srt',        label: 'SRT connection' },
    { id: 'service',    label: 'Service start/stop' },
    { id: 'publish',    label: 'Segment publishing' },
    { id: 'system',     label: 'System health' },
    { id: 'fault',      label: 'Decode and parse faults' },
    { id: 'other',      label: 'Other' }
];

function classifyLogEntry(entry) {
    if (entry && entry.scte35) {
        return 'scte35';
    }

    var message = String((entry && (entry.message || entry.logmessage || entry.msg)) || '');

    if (/^SCTE-35/.test(message))                  return 'scte35';
    if (/^Source aspect ratio changed/.test(message))  return 'aspect';
    if (/^Source resolution changed/.test(message))    return 'resolution';
    if (/^Source AFD changed/.test(message))           return 'afd';
    if (/^Source frame rate changed/.test(message))    return 'framerate';
    if (/^Input Signal Locked|^No Input Signal|^No Data on SRT/.test(message)) return 'input';
    if (/^SRT |^Accepted SRT /.test(message))      return 'srt';
    if (/^Started |^Service Stopped/.test(message))    return 'service';
    if (/^segment /.test(message))                 return 'publish';
    if (/^high cpu usage|^disk space is low/.test(message)) return 'system';
    if (/^decode error|^parse error|^malformed data/.test(message)) return 'fault';

    return 'other';
}

// Same buckets the event table colours by, so the dropdown and the badges agree.
function logEntrySeverity(entry) {
    var status = String((entry && (entry.severity || entry.status || entry.level)) || '').toLowerCase();

    if (status.indexOf('err') >= 0)   return 'error';
    if (status.indexOf('warn') >= 0)  return 'warning';
    if (status.indexOf('debug') >= 0) return 'debug';
    return 'info';
}

function utcDayString(ms) {
    return new Date(ms).toISOString().substring(0, 10);
}

function utcDayStart(ms) {
    return Date.parse(utcDayString(ms) + 'T00:00:00Z');
}

function isLogDate(value) {
    if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(value)) {
        return false;
    }

    // Shape alone lets 2026-13-01 and 2026-02-30 through, and round-tripping the
    // parse is what rejects them. Date.parse answers NaN for those, so the guard
    // has to come before the round trip or formatting the NaN throws.
    var parsed = Date.parse(value + 'T00:00:00Z');
    if (!isFinite(parsed)) {
        return false;
    }

    return utcDayString(parsed) === value;
}

// The live log plus whatever rotations are on disk, oldest write first. Both
// naming schemes are accepted: the dateext names this build installs, and the
// numbered ones an install that rotated under the old config already has.
function logFileCandidates() {
    var found = [];
    var names;

    try {
        names = fs.readdirSync(logFolder);
    } catch (e) {
        console.log('unable to list the log folder: ', e.message);
        return found;
    }

    names.forEach(name => {
        if (!/^srthub\.log(-\d{8}|\.\d+)?(\.gz)?$/.test(name)) {
            return;
        }
        var full = path.join(logFolder, name);
        try {
            var stats = fs.statSync(full);
            if (!stats.isFile()) {
                return;
            }
            found.push({
                path: full,
                name: name,
                size: stats.size,
                mtimeMs: stats.mtimeMs,
                birthMs: stats.birthtimeMs || stats.ctimeMs || stats.mtimeMs,
                gz: /\.gz$/.test(name),
                live: (name === 'srthub.log')
            });
        } catch (e) {
            // A file can vanish under us mid-rotation; it simply isn't a source.
        }
    });

    found.sort((a, b) => a.mtimeMs - b.mtimeMs);
    return found;
}

function logFilesForDay(date) {
    var start = Date.parse(date + 'T00:00:00Z');
    var end = start + 86400000;
    var all = logFileCandidates();
    var picked = [];

    for (var i = 0; i < all.length; i++) {
        var previousWrite = (i > 0) ? all[i - 1].mtimeMs : -Infinity;
        if (all[i].mtimeMs >= start && previousWrite < end) {
            picked.push(all[i]);
        }
    }

    return picked;
}

// Which days the picker should offer. A file covers from the moment the file
// before it stopped being written to its own last write, so it can contribute
// every day that span touches. The result is deliberately generous: listing a
// day that turns out to hold nothing costs an empty table, while missing a day
// that holds events would hide them.
function listLogDays() {
    var all = logFileCandidates();
    var today = utcDayStart(Date.now());
    var earliest = today - (LOG_RETENTION_DAYS - 1) * 86400000;
    var days = {};

    for (var i = 0; i < all.length; i++) {
        var from = (i > 0) ? all[i - 1].mtimeMs : all[i].birthMs;
        if (from > all[i].mtimeMs) {
            from = all[i].mtimeMs;
        }
        var day = utcDayStart(from);
        if (day < earliest) {
            day = earliest;
        }
        for (; day <= all[i].mtimeMs && day <= today; day += 86400000) {
            days[utcDayString(day)] = true;
        }
    }

    days[utcDayString(today)] = true;   // today always exists, even before a first event

    return Object.keys(days).sort().reverse();
}

// Offset just past the final newline, so a follow-up read of the live log starts
// on a line boundary and a half-written entry is read again next time instead of
// being lost. -1 means the boundary could not be established, which only turns
// off the incremental read.
function offsetAfterLastNewline(file, size) {
    if (size === 0) {
        return 0;
    }

    var window = Math.min(LOG_TAIL_WINDOW, size);
    var buffer = Buffer.allocUnsafe(window);
    var read;
    var fd;

    try {
        fd = fs.openSync(file, 'r');
    } catch (e) {
        return -1;
    }
    try {
        read = fs.readSync(fd, buffer, 0, window, size - window);
    } catch (e) {
        return -1;
    } finally {
        fs.closeSync(fd);
    }

    var last = buffer.lastIndexOf(0x0a, read - 1);
    if (last < 0) {
        return (window === size) ? 0 : -1;
    }

    return size - window + last + 1;
}

function streamLogLines(file, gz, onLine) {
    return new Promise((resolve, reject) => {
        var input = fs.createReadStream(file);
        var source = input;

        input.on('error', reject);
        if (gz) {
            var gunzip = zlib.createGunzip();
            gunzip.on('error', reject);
            source = input.pipe(gunzip);
        }

        var reader = readline.createInterface({ input: source, crlfDelay: Infinity });
        reader.on('line', onLine);
        reader.on('error', reject);
        reader.on('close', resolve);
    });
}

async function scanLogDay(date, files) {
    var day = {
        date: date,
        entries: [],        // chronological; the newest MAX_LOG_DAY_ENTRIES of the day
        total: 0,           // entries seen for the day, including any the cap dropped
        malformed: 0,
        services: {},
        sources: [],
        followOffset: -1,
        scannedAt: Date.now()
    };

    function keep(line) {
        var entry = parseLogLine(line);
        if (!entry) {
            if (String(line).trim().length > 0) {
                day.malformed++;
            }
            return;
        }

        var stamp = String(entry.accesstime || entry.logtime || entry.time || entry.timestamp || '');
        if (stamp.substring(0, 10) !== date) {
            return;
        }

        day.total++;

        var service = entry.logsourcename || entry.sourcename || entry.name || entry.source;
        if (service) {
            day.services[String(service)] = true;
        }

        day.entries.push(entry);
        if (day.entries.length >= MAX_LOG_DAY_ENTRIES * 2) {
            day.entries = day.entries.slice(-MAX_LOG_DAY_ENTRIES);
        }
    }

    for (var i = 0; i < files.length; i++) {
        var file = files[i];
        var complete = (file.live && !file.gz) ?
                       offsetAfterLastNewline(file.path, file.size) : file.size;
        var pending = null;

        try {
            // readline cannot say whether the last line it handed over ended in a
            // newline, so the final line is held back and only kept once the file
            // size says it was whole.
            await streamLogLines(file.path, file.gz, (line) => {
                if (pending !== null) {
                    keep(pending);
                }
                pending = line;
            });
        } catch (e) {
            console.log('unable to read ' + file.path + ': ', e.message);
            pending = null;
        }

        if (pending !== null && (complete < 0 || complete >= file.size)) {
            keep(pending);
        }

        day.sources.push({
            path: file.path, size: file.size, mtimeMs: file.mtimeMs,
            gz: file.gz, live: file.live
        });
        if (file.live && !file.gz) {
            day.followOffset = complete;
        }
    }

    if (day.entries.length > MAX_LOG_DAY_ENTRIES) {
        day.entries = day.entries.slice(-MAX_LOG_DAY_ENTRIES);
    }

    return day;
}

function logSourcesUnchanged(cached, files) {
    if (cached.length !== files.length) {
        return false;
    }
    for (var i = 0; i < cached.length; i++) {
        if (cached[i].path !== files[i].path ||
            cached[i].size !== files[i].size ||
            cached[i].mtimeMs !== files[i].mtimeMs) {
            return false;
        }
    }
    return true;
}

// The common case for today's view: nothing has rotated and the live log has
// simply grown, so only the bytes appended since the last read are parsed. Any
// other change (a rotation, a truncation, a file appearing) returns false and
// the day is scanned again from scratch.
async function followLiveTail(day, files) {
    if (day.sources.length === 0 || day.sources.length !== files.length ||
        day.followOffset < 0) {
        return false;
    }

    var last = files.length - 1;
    for (var i = 0; i < last; i++) {
        if (day.sources[i].path !== files[i].path ||
            day.sources[i].size !== files[i].size ||
            day.sources[i].mtimeMs !== files[i].mtimeMs) {
            return false;
        }
    }

    var live = files[last];
    if (!live.live || live.gz || day.sources[last].path !== live.path ||
        live.size < day.followOffset) {
        return false;
    }
    if (live.size === day.followOffset) {
        day.sources[last].size = live.size;
        day.sources[last].mtimeMs = live.mtimeMs;
        return true;
    }

    var length = live.size - day.followOffset;
    var buffer = Buffer.allocUnsafe(length);
    var read;
    var fd;

    try {
        fd = fs.openSync(live.path, 'r');
    } catch (e) {
        return false;
    }
    try {
        read = fs.readSync(fd, buffer, 0, length, day.followOffset);
    } catch (e) {
        return false;
    } finally {
        fs.closeSync(fd);
    }

    var boundary = buffer.lastIndexOf(0x0a, read - 1);
    if (boundary < 0) {
        // Nothing but a part-written entry so far: leave the offset where it is
        // and pick the whole line up on the next poll.
        day.sources[last].mtimeMs = live.mtimeMs;
        day.sources[last].size = live.size;
        return true;
    }

    var text = buffer.toString('utf8', 0, boundary + 1);
    text.split('\n').forEach(line => {
        var entry = parseLogLine(line);
        if (!entry) {
            if (line.trim().length > 0) {
                day.malformed++;
            }
            return;
        }

        var stamp = String(entry.accesstime || entry.logtime || entry.time || entry.timestamp || '');
        if (stamp.substring(0, 10) !== day.date) {
            return;
        }

        day.total++;

        var service = entry.logsourcename || entry.sourcename || entry.name || entry.source;
        if (service) {
            day.services[String(service)] = true;
        }

        day.entries.push(entry);
    });

    if (day.entries.length > MAX_LOG_DAY_ENTRIES) {
        day.entries = day.entries.slice(-MAX_LOG_DAY_ENTRIES);
    }

    day.followOffset += boundary + 1;
    day.sources[last].size = live.size;
    day.sources[last].mtimeMs = live.mtimeMs;
    day.scannedAt = Date.now();
    return true;
}

var logDayCache = [];

function touchLogDayCache(day) {
    logDayCache = [day].concat(logDayCache.filter(c => c !== day));
    logDayCache = logDayCache.slice(0, LOG_DAY_CACHE_SIZE);
    return day;
}

async function loadLogDay(date) {
    var files = logFilesForDay(date);
    var cached = logDayCache.find(c => c.date === date);

    if (cached) {
        if (logSourcesUnchanged(cached.sources, files)) {
            return touchLogDayCache(cached);
        }
        if (await followLiveTail(cached, files)) {
            return touchLogDayCache(cached);
        }
    }

    return touchLogDayCache(await scanLogDay(date, files));
}

function logEntryMatches(entry, filters) {
    if (filters.severity !== 'all' && logEntrySeverity(entry) !== filters.severity) {
        return false;
    }
    if (filters.type !== 'all' && classifyLogEntry(entry) !== filters.type) {
        return false;
    }
    if (filters.service !== 'all') {
        var service = String(entry.logsourcename || entry.sourcename ||
                             entry.name || entry.source || '');
        if (service !== filters.service) {
            return false;
        }
    }
    if (filters.search) {
        var haystack = [
            entry.message || entry.logmessage || entry.msg || '',
            entry.logsourcename || entry.sourcename || entry.name || entry.source || '',
            entry.logid || entry.id || entry.resource || '',
            entry.accesstime || entry.logtime || entry.time || ''
        ].join(' ').toLowerCase();
        if (haystack.indexOf(filters.search) < 0) {
            return false;
        }
    }
    return true;
}

// Walks the day newest first, counting every match but only materialising the
// requested window, so a filter that hits forty thousand entries still answers
// with one page of them and an honest total.
function selectLogEntries(day, filters, offset, limit) {
    var matched = 0;
    var page = [];

    // Folded once here rather than inside the per-entry test, so the match is
    // case insensitive without the caller having to know it must hand over a
    // lowercased needle and without refolding it for every entry in the day.
    var normalized = {
        severity: filters.severity || 'all',
        type: filters.type || 'all',
        service: (filters.service === undefined) ? 'all' : filters.service,
        search: String(filters.search || '').toLowerCase()
    };

    for (var i = day.entries.length - 1; i >= 0; i--) {
        var entry = day.entries[i];
        if (!logEntryMatches(entry, normalized)) {
            continue;
        }
        if (matched >= offset && page.length < limit) {
            page.push(entry);
        }
        matched++;
    }

    return { matched: matched, entries: page };
}

app.get('/api/v1/get_log_days', auth, (req, res) => {
    res.set('Expires', new Date().toUTCString());
    res.json({
        days: listLogDays(),
        today: utcDayString(Date.now()),
        retentiondays: LOG_RETENTION_DAYS,
        types: LOG_EVENT_TYPES,
        maxpage: MAX_LOG_DAY_PAGE
    });
});

// date is matched against a strict YYYY-MM-DD and is never used to build a path
// -- the files for a day are found by listing the log folder -- so there is no
// filename for a caller to steer.
// The day and filters shared by the view and the export, so a download always
// holds exactly what the table was showing. date is matched against a strict
// YYYY-MM-DD and is never used to build a path -- the files for a day are found
// by listing the log folder -- so there is no filename for a caller to steer.
// Returns null when the date is not a real day.
function logRequestFromQuery(query) {
    var date = (query.date === undefined || query.date === '') ?
               utcDayString(Date.now()) : String(query.date);

    if (!isLogDate(date)) {
        return null;
    }

    var severity = String(query.severity || 'all');
    var type = String(query.type || 'all');
    var service = (query.service === undefined) ? 'all' : String(query.service);
    var search = String(query.search || '').trim().toLowerCase()
                 .substring(0, MAX_LOG_SEARCH_SIZE);

    if (['all', 'error', 'warning', 'info', 'debug'].indexOf(severity) < 0) {
        severity = 'all';
    }
    if (type !== 'all' && !LOG_EVENT_TYPES.some(t => t.id === type)) {
        type = 'all';
    }

    return {
        date: date,
        filters: { severity: severity, type: type, service: service, search: search }
    };
}

app.get('/api/v1/get_log_day', auth, async (req, res) => {
    var request = logRequestFromQuery(req.query);

    if (!request) {
        res.status(400).json({ error: 'date must be YYYY-MM-DD' });
        return;
    }

    var date = request.date;
    var offset = parseInt(req.query.offset, 10);
    var limit = parseInt(req.query.limit, 10);

    if (!isFinite(offset) || offset < 0) {
        offset = 0;
    }
    if (!isFinite(limit) || limit <= 0 || limit > MAX_LOG_DAY_PAGE) {
        limit = MAX_LOG_DAY_PAGE;
    }

    try {
        var day = await loadLogDay(date);
        var selected = selectLogEntries(day, request.filters, offset, limit);

        res.set('Expires', new Date().toUTCString());
        res.json({
            date: date,
            today: utcDayString(Date.now()),
            total: day.total,
            held: day.entries.length,
            capped: (day.total > day.entries.length),
            malformed: day.malformed,
            matched: selected.matched,
            offset: offset,
            limit: limit,
            services: Object.keys(day.services).sort(),
            entries: selected.entries
        });
    } catch (e) {
        console.log('unable to read the event log for ' + date + ': ', e.message);
        res.sendStatus(500);
    }
});

// CSV for spreadsheets. Fixed columns rather than one per key seen, so every
// export of every day lines up the same way and can be pasted under the last.
// The SCTE-35 cue detail is flattened into its own columns, empty on the rows
// that are not cues.
const LOG_CSV_COLUMNS = [
    ['time',                 e => e.accesstime || e.logtime || e.time || e.timestamp],
    ['severity',             e => logEntrySeverity(e)],
    ['status',               e => e.status || e.severity || e.level],
    ['event',                e => classifyLogEntry(e)],
    ['service',              e => e.logsourcename || e.sourcename || e.name || e.source],
    ['id',                   e => e.logid || e.id || e.resource],
    ['host',                 e => e.host],
    ['message',              e => e.logmessage || e.message || e.msg],
    ['cue',                  e => e.scte35 && e.scte35.cue],
    ['command',              e => e.scte35 && e.scte35.command],
    ['event_id',             e => e.scte35 && e.scte35['event-id']],
    ['duration',             e => e.scte35 && e.scte35.duration],
    ['immediate',            e => e.scte35 && e.scte35.immediate],
    ['cancel',               e => e.scte35 && e.scte35.cancel],
    ['auto_return',          e => e.scte35 && e.scte35['auto-return']],
    ['descriptor',           e => e.scte35 && e.scte35.descriptor],
    ['segmentation_type_id', e => e.scte35 && e.scte35['segmentation-type-id']],
    ['pid',                  e => e.scte35 && e.scte35.pid],
    ['program_id',           e => e.scte35 && e.scte35['program-id']],
    ['pts_time',             e => e.scte35 && e.scte35['pts-time']],
    ['pts_adjustment',       e => e.scte35 && e.scte35['pts-adjustment']]
];

// RFC 4180 quoting, plus a guard against formula injection: service names are
// typed in by whoever configured the box and cue descriptors come off the wire,
// and a spreadsheet will execute a cell that starts with = + - or @. Such a cell
// is prefixed with an apostrophe so it opens as the text it is.
function csvCell(value) {
    if (value === undefined || value === null) {
        return '';
    }

    var text = (typeof value === 'object') ? JSON.stringify(value) : String(value);

    if (/^[=+\-@\t\r]/.test(text) && !/^-?\d+(\.\d+)?$/.test(text)) {
        text = "'" + text;
    }
    if (/[",\r\n]/.test(text)) {
        text = '"' + text.replace(/"/g, '""') + '"';
    }
    return text;
}

function logEntriesToCsv(entries) {
    var lines = [LOG_CSV_COLUMNS.map(c => c[0]).join(',')];

    entries.forEach(entry => {
        lines.push(LOG_CSV_COLUMNS.map(c => {
            var value;
            try {
                value = c[1](entry);
            } catch (e) {
                value = '';
            }
            return csvCell(value);
        }).join(','));
    });

    // CRLF per RFC 4180, and a byte order mark so Excel reads UTF-8 as UTF-8
    // instead of mangling any non-ASCII service name.
    return '﻿' + lines.join('\r\n') + '\r\n';
}

// Every entry of the day that matches the filters -- not just the window the
// table has loaded -- as a downloadable JSON document, or CSV with format=csv
// (the columns are LOG_CSV_COLUMNS above). The file on disk is not
// itself valid JSON (each line after the first carries a leading comma), so the
// export is assembled from the parsed entries rather than copied out raw.
//
// Entries are in time order, oldest first, the way a log reads. The wrapper
// records the day and the filters that produced it, and says plainly when the
// day was larger than the server holds, so a partial export is never mistaken
// for a complete one.
app.get('/api/v1/export_log_day', auth, async (req, res) => {
    var request = logRequestFromQuery(req.query);

    if (!request) {
        res.status(400).json({ error: 'date must be YYYY-MM-DD' });
        return;
    }

    var date = request.date;

    try {
        var day = await loadLogDay(date);
        var selected = selectLogEntries(day, request.filters, 0, Infinity);
        var filtered = (request.filters.severity !== 'all' || request.filters.type !== 'all' ||
                        request.filters.service !== 'all' || request.filters.search !== '');
        var host = os.hostname().replace(/[^A-Za-z0-9._-]/g, '') || 'srthub';
        var complete = (day.total <= day.entries.length);
        var csv = (String(req.query.format || 'json').toLowerCase() === 'csv');
        var filename = 'srthub-events-' + host + '-' + date + (filtered ? '-filtered' : '') +
                       // CSV has nowhere to carry the complete flag, so the name does
                       (csv && !complete ? '-partial' : '') + (csv ? '.csv' : '.json');

        if (csv) {
            res.set('Content-Type', 'text/csv; charset=utf-8');
            res.set('Content-Disposition', 'attachment; filename="' + filename + '"');
            res.set('Cache-Control', 'no-store');
            res.send(logEntriesToCsv(selected.entries.slice().reverse()));
            return;
        }

        var body = {
            exported: new Date().toISOString(),
            host: os.hostname(),
            date: date,
            timezone: 'UTC',
            filters: request.filters,
            total: day.total,
            exportedcount: selected.entries.length,
            complete: complete,
            note: !complete ?
                  'This day held ' + day.total + ' events; only the newest ' +
                  day.entries.length + ' are kept in memory, so older events are not in this export. ' +
                  'The full day is in the rotated log files in the support bundle.' : undefined,
            malformed: day.malformed,
            entries: selected.entries.slice().reverse()
        };

        res.set('Content-Type', 'application/json; charset=utf-8');
        res.set('Content-Disposition', 'attachment; filename="' + filename + '"');
        res.set('Cache-Control', 'no-store');
        res.send(JSON.stringify(body, null, 2) + '\n');
    } catch (e) {
        console.log('unable to export the event log for ' + date + ': ', e.message);
        res.sendStatus(500);
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

app.get('/api/v1/get_service_status/:uid', auth, (req, res) => {
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
                                    /*var audiocodec = "Unknown";
                                    var audiochannels = 0;
                                    var audiosamplerate = 0;
                                    var audioservice = new audio_service(audiocodec, audiochannels, audiosamplerate);
                                    obj.audioservices.push(audioservice);*/
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
                                    obj.video_frame_rate = dd["frame-rate"];
                                    obj.video_frame_rate_value = dd["frame-rate-value"];
                                    obj.video_aspect_ratio = dd["display-aspect-ratio"];
                                    obj.video_aspect_ratio_value = dd["display-aspect-ratio-value"];
                                    obj.video_sample_aspect_ratio = dd["sample-aspect-ratio"];
                                    obj.video_afd_present = dd["afd-present"];
                                    obj.video_afd_code = dd["afd-code"];
                                    obj.video_afd = dd["afd"];
                                    obj.total_streams = dd["total-streams"];
                                    obj.current_stream = dd["current-stream"];
                                    obj.transport_source_errors = dd["transport-source-errors"];
                                    obj.last_source_error = dd["last-source-error"];
                                }
                            }

                            // source mode status checking
                            if (isSrtMode(sourcemode)) {
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
                            if (isSrtMode(outputmode)) {
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
