test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
box.sql.execute('pragma sql_default_engine=\''..engine..'\'')

-- Check 'unordered' in "_sql_stat1".
box.sql.execute("CREATE TABLE x1(a  INT PRIMARY KEY, b INT , UNIQUE(a, b))")
box.sql.execute("INSERT INTO x1 VALUES(1, 2)")
box.sql.execute("INSERT INTO x1 VALUES(3, 4)")
box.sql.execute("INSERT INTO x1 VALUES(5, 6)")
box.sql.execute("ANALYZE")
box.sql.execute("SELECT * FROM x1")
box.sql.execute("ANALYZE")

_sql_stat1 = box.space._sql_stat1
test_run:cmd("setopt delimiter ';'")
function add_to_stat1(txt)
    for _, tuple in _sql_stat1:pairs() do
        local temp_table = {}
        for _, v in pairs(tuple['stat']) do
            table.insert(temp_table, v)
        end
        table.insert(temp_table, txt)
        _sql_stat1:update(tuple:transform(3, 3), {{'=', 3, temp_table}})
    end
end;
test_run:cmd("setopt delimiter ''");
add_to_stat1('unordered')

test_run:cmd('restart server default');

box.sql.execute("ANALYZE")
box.sql.execute("SELECT * FROM x1")

-- Clean up.
box.sql.execute("DROP TABLE x1")

-- Check analyzeC test.
box.sql.execute("DROP TABLE IF EXISTS t1;")
box.sql.execute("CREATE TABLE t1(a  INT PRIMARY KEY, b INT , c INT , d INT );")
box.sql.execute("INSERT INTO t1(a,b,c,d) VALUES(1,1,2,3),(2,7,8,9),(3,4,5,6),(4,10,11,12),(5,4,8,12),(6,1,11,111);")
box.sql.execute("CREATE INDEX t1b ON t1(b);")
box.sql.execute("CREATE INDEX t1c ON t1(c);")
box.sql.execute("ANALYZE;")
box.sql.execute("SELECT * FROM \"_sql_stat1\";")

_sql_stat1 = box.space._sql_stat1
_sql_stat1:update({'T1', 'T1B'}, {{'=', 3, {12345, 2}}})
_sql_stat1:update({'T1', 'T1C'}, {{'=', 3, {12345, 4}}})

box.sql.execute("SELECT * FROM \"_sql_stat1\"")

test_run:cmd('restart server default');

box.sql.execute("SELECT * FROM \"_sql_stat1\"")

_sql_stat1 = box.space._sql_stat1

box.sql.execute("ANALYZE;")
box.sql.execute("SELECT b,c,d, '#' FROM t1 WHERE b BETWEEN 3 AND 8 ORDER BY d;")

box.sql.execute("EXPLAIN QUERY PLAN SELECT b, c, d, '#' FROM t1 WHERE b BETWEEN 3 AND 8 ORDER BY d;")
box.sql.execute("SELECT d FROM t1 ORDER BY b;")
box.sql.execute("EXPLAIN QUERY PLAN SELECT d FROM t1 ORDER BY b;")

test_run:cmd("setopt delimiter ';'")
function add_to_stat1(txt)
    for _, tuple in _sql_stat1:pairs() do
        local temp_table = {}
        for _, v in pairs(tuple['stat']) do
            table.insert(temp_table, v)
        end
        table.insert(temp_table, txt)
        _sql_stat1:update(tuple:transform(3, 3), {{'=', 3, temp_table}})
    end
end;
test_run:cmd("setopt delimiter ''");
add_to_stat1('unordered')

test_run:cmd('restart server default');

box.sql.execute("ANALYZE;")
box.sql.execute("SELECT b, c, d, '#' FROM t1 WHERE b BETWEEN 3 AND 8 ORDER BY d;")

-- Ignore extraneous text parameters in the "_sql_stat1" stat field.
_sql_stat1 = box.space._sql_stat1
_sql_stat1:update({'T1', 'T1'}, {{'=', 3, {'whatever=5', 'unordered', 'xyzzy = 11'}}})
_sql_stat1:update({'T1', 'T1B'}, {{'=', 3, {'whatever=5', 'unordered', 'xyzzy = 11'}}})
_sql_stat1:update({'T1', 'T1C'}, {{'=', 3, {'whatever=5', 'unordered', 'xyzzy = 11'}}})

test_run:cmd('restart server default');

box.sql.execute("ANALYZE;")
box.sql.execute("SELECT b, c, d, '#' FROM t1 WHERE b BETWEEN 3 AND 8 ORDER BY d;")
box.sql.execute("EXPLAIN QUERY PLAN SELECT b, c, d, '#' FROM t1 WHERE b BETWEEN 3 AND 8 ORDER BY d;")
box.sql.execute("SELECT d FROM t1 ORDER BY b;")
box.sql.execute("SELECT d FROM t1 ORDER BY b;")

-- The sz=NNN parameter determines which index to scan
box.sql.execute("DROP INDEX t1b ON t1;")
box.sql.execute("CREATE INDEX t1bc ON t1(b,c);")
box.sql.execute("CREATE INDEX t1db ON t1(d,b);")
box.sql.execute("ANALYZE")

_sql_stat1 = box.space._sql_stat1
_sql_stat1:update({'T1', 'T1BC'}, {{'=', 3, {12345, 3, 2, 'sz=10'}}})
_sql_stat1:update({'T1', 'T1DB'}, {{'=', 3, {12345, 3, 2, 'sz=20'}}})

test_run:cmd('restart server default');

box.sql.execute("ANALYZE;")
box.sql.execute("SELECT count(b) FROM t1;")
box.sql.execute("EXPLAIN QUERY PLAN SELECT count(b) FROM t1;")

-- The sz=NNN parameter works even if there is other extraneous text
-- in the sql_stat1.stat column.

_sql_stat1 = box.space._sql_stat1
_sql_stat1:update({'T1', 'T1BC'}, {{'=', 3, {12345, 3, 2, 'x=5', 'sz=10', 'y=10'}}})
_sql_stat1:update({'T1', 'T1DB'}, {{'=', 3, {12345, 3, 2, 'whatever', 'sz=20', 'junk'}}})

test_run:cmd('restart server default');

box.sql.execute("ANALYZE;")
box.sql.execute("SELECT count(b) FROM t1;")

box.sql.execute("EXPLAIN QUERY PLAN SELECT count(b) FROM t1;")

-- The following tests experiment with adding corrupted records to the
-- 'sample' column of the _sql_stat4 table.
box.sql.execute("DROP TABLE IF EXISTS t1;")
box.sql.execute("CREATE TABLE t1(id INTEGER PRIMARY KEY AUTOINCREMENT, a INT , b INT );")
box.sql.execute("CREATE INDEX i1 ON t1(a, b);")
box.sql.execute("INSERT INTO t1 VALUES(null, 1, 1);")
box.sql.execute("INSERT INTO t1 VALUES(null, 2, 2);")
box.sql.execute("INSERT INTO t1 VALUES(null, 3, 3);")
box.sql.execute("INSERT INTO t1 VALUES(null, 4, 4);")
box.sql.execute("INSERT INTO t1 VALUES(null, 5, 5);")
box.sql.execute("ANALYZE;")

_sql_stat4 = box.space._sql_stat4
_sql_stat4:delete{'T1', 'I1', _sql_stat4:select()[1][6]}
_sql_stat4:insert{'T1', 'I1', {1, 1}, {0, 0}, {0, 0}, ''}

test_run:cmd('restart server default');

box.sql.execute("ANALYZE;")

_sql_stat4 = box.space._sql_stat4
test_run:cmd("setopt delimiter ';'")
function update_stat4_fields(field_num, val)
    for i,t in _sql_stat4:pairs() do
        _sql_stat4:update(t:transform(3, 3), {{'=', field_num, val}})
    end
end;
test_run:cmd("setopt delimiter ''")
update_stat4_fields(3, {0, 0, 0})

test_run:cmd('restart server default');

box.sql.execute("ANALYZE;")
box.sql.execute("SELECT * FROM t1 WHERE a = 1;")

-- Skip-scan test.
box.sql.execute("DROP TABLE IF EXISTS t1;")
box.sql.execute("CREATE TABLE t1(id INTEGER PRIMARY KEY, a TEXT, b INT, c INT, d INT);")
box.sql.execute("CREATE INDEX t1abc ON t1(a,b,c);")
box.sql.execute("DROP TABLE IF EXISTS t2;")
box.sql.execute("CREATE TABLE t2(id INTEGER PRIMARY KEY);")
box.sql.execute("INSERT INTO t2 VALUES(1);")
box.sql.execute("INSERT INTO t1 VALUES(1, 'abc',123,4,5);")
box.sql.execute("INSERT INTO t1 VALUES(2, 'abc',234,5,6);")
box.sql.execute("INSERT INTO t1 VALUES(3, 'abc',234,6,7);")
box.sql.execute("INSERT INTO t1 VALUES(4, 'abc',345,7,8);")
box.sql.execute("INSERT INTO t1 VALUES(5, 'def',567,8,9);")
box.sql.execute("INSERT INTO t1 VALUES(6, 'def',345,9,10);")
box.sql.execute("INSERT INTO t1 VALUES(7, 'bcd',100,6,11);")
box.sql.execute("ANALYZE;")
box.sql.execute("DELETE FROM \"_sql_stat1\";")
box.sql.execute("DELETE FROM \"_sql_stat4\";")

_sql_stat1 = box.space._sql_stat1
_sql_stat1:insert{'T1','T1ABC', {10000,5000,2000,10}}

test_run:cmd('restart server default');

box.sql.execute("ANALYZE t2;")
box.sql.execute("SELECT a,b,c,d FROM t1 WHERE b=345;")

-- Clean up.
box.sql.execute("DROP TABLE IF EXISTS t1;")
box.sql.execute("DROP TABLE IF EXISTS t2;")
test_run:cmd('restart server default with cleanup=1');
