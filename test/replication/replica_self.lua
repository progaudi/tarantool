#!/usr/bin/env tarantool

box.cfg({
    listen              = os.getenv("LISTEN"),
    replication         = {os.getenv("MASTER"), os.getenv("LISTEN")},
    memtx_memory        = 107374182,
    replication_timeout = 0.1,
    replication_connect_timeout = 0.5,
})

require('console').listen(os.getenv('ADMIN'))
