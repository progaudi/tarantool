test_run = require('test_run').new()

--box.schema.user.revoke('guest', 'replication')
box.schema.user.grant('guest', 'replication')

--
-- gh-4669 any change in box.cfg replication list resets all connections
--
test_run:cmd('create server replica with rpl_master=default, script="replication/replica_self.lua"')
test_run:cmd("start server replica")
test_run:cmd("switch replica")
replication = box.cfg.replication
replication
replication_change = {}
replication_change[1] = replication[2]
replication_change[2] = replication[1]
replication_change
box.cfg{replication = replication_change}

test_run:cmd("switch default")
--search log for error with duplicated connection from relay
box.cfg.listen
test_run:grep_log('replica', 'duplicate connection') ~= nil

test_run:cmd("stop server replica")
test_run:cmd("cleanup server replica")
test_run:cmd("delete server replica")
test_run:cleanup_cluster()
box.schema.user.revoke('guest', 'replication')
