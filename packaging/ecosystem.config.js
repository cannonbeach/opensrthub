// pm2 process definition for the opensrthub web application.
//
// Used only when install.sh is run with --service=pm2; the default is systemd.
//
// The 'cwd' setting below is the important part: server.js reads its TLS
// certificate from a path relative to the working directory, so without it pm2
// must be started from inside /var/app.
module.exports = {
    apps: [
        {
            name: 'opensrthub',
            script: 'server.js',
            cwd: '/var/app',
            exec_mode: 'fork',
            instances: 1,
            autorestart: true,
            restart_delay: 5000,
            max_restarts: 50,
            watch: false,
            max_memory_restart: '512M',
            time: true,
            out_file: '/var/log/opensrthub.out.log',
            error_file: '/var/log/opensrthub.err.log',
            env: {
                NODE_ENV: 'production'
            }
        }
    ]
};
