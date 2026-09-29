// Node @google-cloud/spanner smoke test against the emulator.
'use strict';
const {Spanner, protos} = require('@google-cloud/spanner');

const PROJECT = 'p';
const INSTANCE = process.env.INSTANCE || 'node-inst';
const DB = 'nodedb';
const PGDB = 'nodepgdb';
const results = [];

async function step(name, fn) {
  try {
    const d = (await fn()) || '';
    results.push([name, 'PASS', d]);
    console.log(`[PASS] ${name} ${d}`);
  } catch (e) {
    const msg = `code=${e.code} ${e.name}: ${e.message}`;
    results.push([name, 'FAIL', msg]);
    console.log(`[FAIL] ${name}: ${msg}`);
    console.log(e.stack);
  }
}

const sleep = ms => new Promise(r => setTimeout(r, ms));
const assert = (c, m) => { if (!c) throw new Error('assertion failed: ' + m); };

async function main() {
  const spanner = new Spanner({projectId: PROJECT});
  const instanceAdmin = spanner.getInstanceAdminClient();
  const dbAdmin = spanner.getDatabaseAdminClient();
  const instPath = instanceAdmin.instancePath(PROJECT, INSTANCE);

  await step('create_instance', async () => {
    const [op] = await instanceAdmin.createInstance({
      instanceId: INSTANCE,
      parent: instanceAdmin.projectPath(PROJECT),
      instance: {
        config: instanceAdmin.instanceConfigPath(PROJECT, 'emulator-config'),
        displayName: 'node smoke',
        nodeCount: 1,
      },
    });
    const [inst] = await op.promise();
    return `state=${inst.state}`;
  });

  await step('create_database', async () => {
    const [op] = await dbAdmin.createDatabase({
      parent: instPath,
      createStatement: `CREATE DATABASE ${DB}`,
      extraStatements: [`CREATE TABLE Items (
        Id INT64 NOT NULL, Name STRING(MAX), Data JSON, Amount NUMERIC,
        UpdatedAt TIMESTAMP OPTIONS (allow_commit_timestamp=true),
        Scores ARRAY<FLOAT64>) PRIMARY KEY (Id)`],
    });
    const [db] = await op.promise();
    return `dialect=${db.databaseDialect}`;
  });

  await step('update_ddl_index', async () => {
    const [op] = await dbAdmin.updateDatabaseDdl({
      database: dbAdmin.databasePath(PROJECT, INSTANCE, DB),
      statements: ['CREATE INDEX ItemsByName ON Items(Name)'],
    });
    await op.promise();
    const [ddl] = await dbAdmin.getDatabaseDdl({database: dbAdmin.databasePath(PROJECT, INSTANCE, DB)});
    return `ddl_count=${ddl.statements.length}`;
  });

  const database = spanner.instance(INSTANCE).database(DB);
  const items = database.table('Items');

  await step('insert_mutations', async () => {
    const [resp] = await items.insert([
      {Id: 1, Name: 'one', Data: {a: 1}, Amount: Spanner.numeric('1.50'), UpdatedAt: Spanner.COMMIT_TIMESTAMP, Scores: [Spanner.float(1.0), Spanner.float(2.5)]},
      {Id: 2, Name: 'two', Data: {b: [1, 2]}, Amount: Spanner.numeric('2.25'), UpdatedAt: Spanner.COMMIT_TIMESTAMP, Scores: [Spanner.float(3.0)]},
    ]);
    return `commit_ts=${resp && resp.commitTimestamp ? JSON.stringify(resp.commitTimestamp) : 'n/a'}`;
  });

  await step('rw_txn_dml', async () => {
    const out = await database.runTransactionAsync(async tx => {
      const [n1] = await tx.runUpdate({
        sql: 'INSERT INTO Items (Id, Name, Data, Amount, UpdatedAt, Scores) VALUES (@id, @name, @data, @amt, PENDING_COMMIT_TIMESTAMP(), @scores)',
        params: {id: 3, name: 'three', data: {c: true}, amt: Spanner.numeric('3.125'), scores: [0.5, 0.25]},
        types: {id: 'int64', name: 'string', data: 'json', amt: 'numeric', scores: {type: 'array', child: 'float64'}},
      });
      const [n2] = await tx.runUpdate({
        sql: 'UPDATE Items SET Name = @name WHERE Id = @id',
        params: {id: 1, name: 'one-updated'},
      });
      await tx.commit();
      return [n1, n2];
    });
    assert(out[0] === 1 && out[1] === 1, out);
    return `insert=${out[0]} update=${out[1]}`;
  });

  await step('param_query', async () => {
    const [rows] = await database.run({
      sql: 'SELECT Id, Name, Data, Amount, UpdatedAt, Scores FROM Items WHERE Id >= @min ORDER BY Id',
      params: {min: 1},
      json: true,
    });
    assert(rows.length === 3, rows.length);
    assert(rows[0].Name === 'one-updated', rows[0].Name);
    const r = rows[2];
    return `rows=${rows.length} row3=${r.Name},${JSON.stringify(r.Data)},${r.Amount && r.Amount.value},${r.UpdatedAt && r.UpdatedAt.toISOString()},${JSON.stringify(r.Scores)}`;
  });

  await step('readonly_snapshot_query', async () => {
    const [snap] = await database.getSnapshot();
    try {
      const [c] = await snap.run({sql: 'SELECT COUNT(*) AS c FROM Items', json: true});
      const [rd] = await snap.read('Items', {keySet: {all: true}, columns: ['Id', 'Name']});
      const [ix] = await snap.read('Items', {keys: ['one-updated'], index: 'ItemsByName', columns: ['Id', 'Name'], json: true});
      return `count=${Number(c[0].c)} read_rows=${rd.length} index_read=${JSON.stringify(ix)}`;
    } finally {
      snap.end();
    }
  });

  await step('stale_read_exact_staleness', async () => {
    await sleep(2000);
    const [rows] = await database.run({sql: 'SELECT COUNT(*) AS c FROM Items', json: true}, {exactStaleness: 1000});
    const c = Number(rows[0].c.value !== undefined ? rows[0].c.value : rows[0].c);
    assert(c === 3, c);
    return `count=${c}`;
  });

  await step('batch_dml', async () => {
    const counts = await database.runTransactionAsync(async tx => {
      const [rowCounts] = await tx.batchUpdate([
        {sql: 'INSERT INTO Items (Id, Name) VALUES (@id, @name)', params: {id: 10, name: 'ten'}},
        'UPDATE Items SET Amount = Amount + 1 WHERE Id IN (1, 2)',
      ]);
      await tx.commit();
      return rowCounts;
    });
    assert(counts.length === 2 && counts[0] === 1 && counts[1] === 2, counts);
    return `counts=${JSON.stringify(counts)}`;
  });

  await step('duplicate_key_error', async () => {
    try {
      await database.runTransactionAsync(async tx => {
        await tx.runUpdate("INSERT INTO Items (Id, Name) VALUES (1, 'dup')");
        await tx.commit();
      });
    } catch (e) {
      assert(e.code === 6, `expected code 6 ALREADY_EXISTS, got ${e.code}: ${e.message}`);
      return `code=${e.code} msg=${JSON.stringify(e.details || e.message)}`;
    }
    throw new Error('expected ALREADY_EXISTS');
  });

  await step('duplicate_key_mutation_error', async () => {
    try {
      await items.insert({Id: 2, Name: 'dup'});
    } catch (e) {
      assert(e.code === 6, `expected code 6 ALREADY_EXISTS, got ${e.code}: ${e.message}`);
      return `code=${e.code} msg=${JSON.stringify(e.details || e.message)}`;
    }
    throw new Error('expected ALREADY_EXISTS');
  });

  // PostgreSQL dialect.
  await step('pg_create_database', async () => {
    const [op] = await dbAdmin.createDatabase({
      parent: instPath,
      createStatement: `CREATE DATABASE "${PGDB}"`,
      databaseDialect: protos.google.spanner.admin.database.v1.DatabaseDialect.POSTGRESQL,
    });
    const [db] = await op.promise();
    return `dialect=${db.databaseDialect}`;
  });

  await step('pg_ddl', async () => {
    const [op] = await dbAdmin.updateDatabaseDdl({
      database: dbAdmin.databasePath(PROJECT, INSTANCE, PGDB),
      statements: ['CREATE TABLE users (id bigint NOT NULL PRIMARY KEY, name varchar, amount numeric, data jsonb, ts timestamptz, tags text[])'],
    });
    await op.promise();
  });

  const pg = spanner.instance(INSTANCE).database(PGDB);

  await step('pg_insert', async () => {
    const n = await pg.runTransactionAsync(async tx => {
      const [n] = await tx.runUpdate({
        sql: 'INSERT INTO users (id, name, amount, data, ts, tags) VALUES ($1, $2, $3, $4, $5, $6)',
        params: {p1: 1, p2: 'alice', p3: Spanner.pgNumeric('9.99'), p4: Spanner.pgJsonb({k: 'v'}),
          p5: new Date('2024-01-02T03:04:05Z'), p6: ['x', 'y']},
        types: {p1: 'int64', p2: 'string', p3: 'pgNumeric', p4: 'pgJsonb', p5: 'timestamp', p6: {type: 'array', child: 'string'}},
      });
      await tx.commit();
      return n;
    });
    await pg.table('users').insert({id: 2, name: 'bob'});
    return `dml_rows=${n} + 1 mutation`;
  });

  await step('pg_param_query', async () => {
    const [rows] = await pg.run({
      sql: 'SELECT id, name, amount, data, ts, tags FROM users WHERE id = $1',
      params: {p1: 1},
      types: {p1: 'int64'},
      json: true,
    });
    assert(rows.length === 1 && rows[0].name === 'alice', JSON.stringify(rows));
    return `row=${JSON.stringify(rows[0])}`;
  });

  await step('pg_duplicate_key_error', async () => {
    try {
      await pg.runTransactionAsync(async tx => {
        await tx.runUpdate("INSERT INTO users (id, name) VALUES (1, 'dup')");
        await tx.commit();
      });
    } catch (e) {
      assert(e.code === 6, `expected code 6 ALREADY_EXISTS, got ${e.code}: ${e.message}`);
      return `code=${e.code} msg=${JSON.stringify(e.details || e.message)}`;
    }
    throw new Error('expected ALREADY_EXISTS');
  });

  console.log('\nSUMMARY');
  for (const [n, s, d] of results) console.log(`${s}\t${n}\t${d}`);
  await database.close();
  await pg.close();
  spanner.close();
  process.exit(results.every(r => r[1] === 'PASS') ? 0 : 1);
}

main().catch(e => { console.error('FATAL', e); process.exit(2); });
