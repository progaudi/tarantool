#!/usr/bin/env tarantool
local tap = require('tap')
local test = tap.test('execute')
test:plan(3)

--
-- gh-4231: box.execute should be immutable function meaning it's
-- address doesn't change after first box.cfg implicit invocation
--

local function execute_is_immutable(execute, cmd, msg)
    local status, err = pcall(function()
        execute(cmd)
    end)
    test:ok(status and err == nil, msg)
end

local box_execute_stub = box.execute
execute_is_immutable(box_execute_stub, "SELECT 1",
    "execute does not work properly before box.cfg")

local box_execute_actual = box.execute
-- explicit call to load_cfg
box.cfg{}

-- checking the function was not reconfigured, i.e. adress stays the same
test:is(box_execute_actual, box.execute,
    "execute is not the same after box.cfg")
execute_is_immutable(box_execute_actual,
    "CREATE TABLE t1 (s1 INTEGER, PRIMARY KEY (s1));",
    "execute does not work properly after box.cfg")

os.exit(test:check() and 0 or 1)
