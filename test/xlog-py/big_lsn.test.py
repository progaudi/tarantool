import os

#
# Check that Tarantool handles huge LSNs well (gh-4033).
#

# Fill an empty directory.
server.stop()
server.deploy()
server.admin("box.info.lsn")
server.admin("box.space._schema:delete('dummy')")
server.admin("box.snapshot()")
server.stop()

# Bump the instance vclock by tweaking the checkpoint.
old_lsn = 1
new_lsn = 123456789123
snap_dir = os.path.join(server.vardir, server.name)
old_snap = os.path.join(snap_dir, "%020d.snap" % old_lsn)
new_snap = os.path.join(snap_dir, "%020d.snap" % new_lsn)
with open(old_snap, "r+") as f:
    s = f.read()
    s = s.replace("VClock: {1: %d}" % old_lsn,
                  "VClock: {1: %d}" % new_lsn)
    f.seek(0)
    f.write(s)
os.rename(old_snap, new_snap)

# Recover and make a snapshot.
server.start()
server.admin("box.info.lsn")
server.admin("box.space._schema:delete('dummy')")
server.admin("box.snapshot()")
server.stop()

# Try one more time.
server.start()
server.admin("box.info.lsn")
server.admin("box.space._schema:delete('dummy')")
server.admin("box.snapshot()")
