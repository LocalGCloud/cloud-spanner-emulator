// Java google-cloud-spanner smoke test against the emulator.
import com.google.cloud.Timestamp;
import com.google.cloud.spanner.*;
import com.google.cloud.spanner.admin.database.v1.DatabaseAdminClient;
import com.google.cloud.spanner.admin.instance.v1.InstanceAdminClient;
import com.google.spanner.admin.database.v1.CreateDatabaseRequest;
import com.google.spanner.admin.database.v1.DatabaseDialect;
import com.google.spanner.admin.instance.v1.CreateInstanceRequest;
import com.google.spanner.admin.instance.v1.Instance;
import java.math.BigDecimal;
import java.util.*;
import java.util.concurrent.TimeUnit;

public class Smoke {
  static final String P = "p";
  static final String I = System.getenv().getOrDefault("INSTANCE", "java-inst");
  static final String DB = "javadb", PGDB = "javapgdb";
  static final List<String[]> results = new ArrayList<>();

  interface Step { String run() throws Exception; }

  static void step(String name, Step s) {
    try {
      String d = s.run();
      d = d == null ? "" : d;
      results.add(new String[] {name, "PASS", d});
      System.out.println("[PASS] " + name + " " + d);
    } catch (Throwable t) {
      Throwable c = t;
      while (c.getCause() != null && !(c instanceof SpannerException)) c = c.getCause();
      String msg = c instanceof SpannerException
          ? "SpannerException code=" + ((SpannerException) c).getErrorCode() + " " + c.getMessage()
          : c.getClass().getSimpleName() + ": " + c.getMessage();
      results.add(new String[] {name, "FAIL", msg});
      System.out.println("[FAIL] " + name + ": " + msg);
      t.printStackTrace(System.out);
    }
  }

  static void check(boolean b, Object m) { if (!b) throw new AssertionError(String.valueOf(m)); }

  public static void main(String[] args) throws Exception {
    SpannerOptions opts = SpannerOptions.newBuilder().setProjectId(P).build();  // honours SPANNER_EMULATOR_HOST
    Spanner spanner = opts.getService();
    InstanceAdminClient ia = spanner.createInstanceAdminClient();
    DatabaseAdminClient da = spanner.createDatabaseAdminClient();
    String instName = "projects/" + P + "/instances/" + I;

    step("create_instance", () -> {
      Instance inst = ia.createInstanceAsync(CreateInstanceRequest.newBuilder()
          .setParent("projects/" + P).setInstanceId(I)
          .setInstance(Instance.newBuilder().setConfig("projects/" + P + "/instanceConfigs/emulator-config")
              .setDisplayName("java smoke").setNodeCount(1).build())
          .build()).get(60, TimeUnit.SECONDS);
      return "state=" + inst.getState();
    });

    step("create_database", () -> {
      var db = da.createDatabaseAsync(CreateDatabaseRequest.newBuilder().setParent(instName)
          .setCreateStatement("CREATE DATABASE " + DB)
          .addExtraStatements("CREATE TABLE Items (Id INT64 NOT NULL, Name STRING(MAX), Data JSON, Amount NUMERIC, "
              + "UpdatedAt TIMESTAMP OPTIONS (allow_commit_timestamp=true), Scores ARRAY<FLOAT64>) PRIMARY KEY (Id)")
          .build()).get(120, TimeUnit.SECONDS);
      return "dialect=" + db.getDatabaseDialect();
    });

    step("update_ddl_index", () -> {
      da.updateDatabaseDdlAsync(instName + "/databases/" + DB, List.of("CREATE INDEX ItemsByName ON Items(Name)"))
          .get(120, TimeUnit.SECONDS);
      return "ddl_count=" + da.getDatabaseDdl(instName + "/databases/" + DB).getStatementsCount();
    });

    DatabaseClient dbc = spanner.getDatabaseClient(DatabaseId.of(P, I, DB));

    step("insert_mutations", () -> {
      Timestamp ts = dbc.write(List.of(
          Mutation.newInsertBuilder("Items").set("Id").to(1L).set("Name").to("one").set("Data").to(Value.json("{\"a\":1}"))
              .set("Amount").to(new BigDecimal("1.50")).set("UpdatedAt").to(Value.COMMIT_TIMESTAMP)
              .set("Scores").toFloat64Array(new double[] {1.0, 2.5}).build(),
          Mutation.newInsertBuilder("Items").set("Id").to(2L).set("Name").to("two").set("Data").to(Value.json("{\"b\":[1,2]}"))
              .set("Amount").to(new BigDecimal("2.25")).set("UpdatedAt").to(Value.COMMIT_TIMESTAMP)
              .set("Scores").toFloat64Array(new double[] {3.0}).build()));
      return "commit_ts=" + ts;
    });

    step("rw_txn_dml", () -> {
      long[] n = dbc.readWriteTransaction().run(tx -> {
        long n1 = tx.executeUpdate(Statement.newBuilder(
            "INSERT INTO Items (Id, Name, Data, Amount, UpdatedAt, Scores) VALUES (@id, @name, @data, @amt, PENDING_COMMIT_TIMESTAMP(), @scores)")
            .bind("id").to(3L).bind("name").to("three").bind("data").to(Value.json("{\"c\":true}"))
            .bind("amt").to(new BigDecimal("3.125")).bind("scores").toFloat64Array(new double[] {0.5, 0.25}).build());
        long n2 = tx.executeUpdate(Statement.newBuilder("UPDATE Items SET Name = @name WHERE Id = @id")
            .bind("id").to(1L).bind("name").to("one-updated").build());
        return new long[] {n1, n2};
      });
      check(n[0] == 1 && n[1] == 1, Arrays.toString(n));
      return "insert=" + n[0] + " update=" + n[1];
    });

    step("param_query", () -> {
      int rows = 0;
      String out = "";
      try (ResultSet rs = dbc.singleUse().executeQuery(Statement.newBuilder(
          "SELECT Id, Name, Data, Amount, UpdatedAt, Scores FROM Items WHERE Id >= @min ORDER BY Id").bind("min").to(1L).build())) {
        while (rs.next()) {
          rows++;
          if (rs.getLong("Id") == 1) check(rs.getString("Name").equals("one-updated"), rs.getString("Name"));
          if (rs.getLong("Id") == 3)
            out = rs.getString("Name") + "," + rs.getJson("Data") + "," + rs.getBigDecimal("Amount") + "," + rs.getTimestamp("UpdatedAt") + "," + rs.getDoubleList("Scores");
        }
      }
      check(rows == 3, rows);
      return "rows=" + rows + " row3=" + out;
    });

    step("readonly_snapshot_query", () -> {
      try (ReadOnlyTransaction ro = dbc.readOnlyTransaction()) {
        long cnt;
        try (ResultSet rs = ro.executeQuery(Statement.of("SELECT COUNT(*) FROM Items"))) { rs.next(); cnt = rs.getLong(0); }
        int nread = 0;
        try (ResultSet rs = ro.read("Items", KeySet.all(), List.of("Id", "Name"))) { while (rs.next()) nread++; }
        long idx;
        try (ResultSet rs = ro.readUsingIndex("Items", "ItemsByName", KeySet.singleKey(Key.of("one-updated")), List.of("Id"))) { rs.next(); idx = rs.getLong(0); }
        return "count=" + cnt + " read_rows=" + nread + " index_read_id=" + idx + " read_ts=" + ro.getReadTimestamp();
      }
    });

    step("stale_read_exact_staleness", () -> {
      Thread.sleep(2000);
      try (ResultSet rs = dbc.singleUse(TimestampBound.ofExactStaleness(1, TimeUnit.SECONDS)).executeQuery(Statement.of("SELECT COUNT(*) FROM Items"))) {
        rs.next();
        check(rs.getLong(0) == 3, rs.getLong(0));
        return "count=" + rs.getLong(0);
      }
    });

    step("batch_dml", () -> {
      long[] c = dbc.readWriteTransaction().run(tx -> tx.batchUpdate(List.of(
          Statement.newBuilder("INSERT INTO Items (Id, Name) VALUES (@id, @name)").bind("id").to(10L).bind("name").to("ten").build(),
          Statement.of("UPDATE Items SET Amount = Amount + 1 WHERE Id IN (1, 2)"))));
      check(c.length == 2 && c[0] == 1 && c[1] == 2, Arrays.toString(c));
      return "counts=" + Arrays.toString(c);
    });

    step("duplicate_key_error", () -> {
      try {
        dbc.readWriteTransaction().run(tx -> tx.executeUpdate(Statement.of("INSERT INTO Items (Id, Name) VALUES (1, 'dup')")));
      } catch (SpannerException e) {
        check(e.getErrorCode() == ErrorCode.ALREADY_EXISTS, e.getErrorCode() + " " + e.getMessage());
        return "code=" + e.getErrorCode() + " msg=" + e.getMessage();
      }
      throw new AssertionError("expected ALREADY_EXISTS");
    });

    step("duplicate_key_mutation_error", () -> {
      try {
        dbc.write(List.of(Mutation.newInsertBuilder("Items").set("Id").to(2L).set("Name").to("dup").build()));
      } catch (SpannerException e) {
        check(e.getErrorCode() == ErrorCode.ALREADY_EXISTS, e.getErrorCode() + " " + e.getMessage());
        return "code=" + e.getErrorCode() + " msg=" + e.getMessage();
      }
      throw new AssertionError("expected ALREADY_EXISTS");
    });

    // PostgreSQL dialect.
    step("pg_create_database", () -> {
      var db = da.createDatabaseAsync(CreateDatabaseRequest.newBuilder().setParent(instName)
          .setCreateStatement("CREATE DATABASE \"" + PGDB + "\"").setDatabaseDialect(DatabaseDialect.POSTGRESQL)
          .build()).get(120, TimeUnit.SECONDS);
      return "dialect=" + db.getDatabaseDialect();
    });

    step("pg_ddl", () -> {
      da.updateDatabaseDdlAsync(instName + "/databases/" + PGDB, List.of(
          "CREATE TABLE users (id bigint NOT NULL PRIMARY KEY, name varchar, amount numeric, data jsonb, ts timestamptz, tags text[])"))
          .get(120, TimeUnit.SECONDS);
      return "";
    });

    DatabaseClient pg = spanner.getDatabaseClient(DatabaseId.of(P, I, PGDB));

    step("pg_insert", () -> {
      Long n = pg.readWriteTransaction().run(tx -> tx.executeUpdate(Statement.newBuilder(
          "INSERT INTO users (id, name, amount, data, ts, tags) VALUES ($1, $2, $3, $4, $5, $6)")
          .bind("p1").to(1L).bind("p2").to("alice").bind("p3").to(Value.pgNumeric("9.99"))
          .bind("p4").to(Value.pgJsonb("{\"k\":\"v\"}")).bind("p5").to(Timestamp.parseTimestamp("2024-01-02T03:04:05Z"))
          .bind("p6").toStringArray(List.of("x", "y")).build()));
      pg.write(List.of(Mutation.newInsertBuilder("users").set("id").to(2L).set("name").to("bob").build()));
      return "dml_rows=" + n + " + 1 mutation";
    });

    step("pg_param_query", () -> {
      try (ResultSet rs = pg.singleUse().executeQuery(Statement.newBuilder(
          "SELECT id, name, amount, data, ts, tags FROM users WHERE id = $1").bind("p1").to(1L).build())) {
        check(rs.next(), "no row");
        String out = rs.getLong(0) + "," + rs.getString(1) + "," + rs.getValue(2) + "," + rs.getPgJsonb(3) + "," + rs.getTimestamp(4) + "," + rs.getStringList(5);
        check(!rs.next(), "extra row");
        return "row=" + out;
      }
    });

    step("pg_duplicate_key_error", () -> {
      try {
        pg.readWriteTransaction().run(tx -> tx.executeUpdate(Statement.of("INSERT INTO users (id, name) VALUES (1, 'dup')")));
      } catch (SpannerException e) {
        check(e.getErrorCode() == ErrorCode.ALREADY_EXISTS, e.getErrorCode() + " " + e.getMessage());
        return "code=" + e.getErrorCode() + " msg=" + e.getMessage();
      }
      throw new AssertionError("expected ALREADY_EXISTS");
    });

    System.out.println("\nSUMMARY");
    boolean ok = true;
    for (String[] r : results) { System.out.println(r[1] + "\t" + r[0] + "\t" + r[2]); ok &= r[1].equals("PASS"); }
    ia.close();
    da.close();
    spanner.close();
    System.exit(ok ? 0 : 1);
  }
}
