// google-cloud-spanner-jdbc smoke test against the emulator.
import com.google.cloud.spanner.jdbc.JdbcSqlException;
import com.google.cloud.spanner.jdbc.JsonType;
import java.math.BigDecimal;
import java.sql.*;
import java.util.*;

public class JdbcSmoke {
  static final String HOST = System.getenv().getOrDefault("EMU", "localhost:19010");
  static final String INST = System.getenv().getOrDefault("INSTANCE", "jdbc-inst");
  static final String URL = "jdbc:cloudspanner://" + HOST + "/projects/p/instances/" + INST
      + "/databases/jdbcdb;usePlainText=true;autoConfigEmulator=true";
  static final String PGURL = "jdbc:cloudspanner://" + HOST + "/projects/p/instances/" + INST
      + "/databases/jdbcpgdb;usePlainText=true;autoConfigEmulator=true;dialect=postgresql";
  static final List<String[]> results = new ArrayList<>();

  interface Step { String run() throws Exception; }

  static void step(String name, Step s) {
    try {
      String d = s.run();
      results.add(new String[] {name, "PASS", d == null ? "" : d});
      System.out.println("[PASS] " + name + " " + (d == null ? "" : d));
    } catch (Throwable t) {
      String msg = t instanceof SQLException
          ? t.getClass().getSimpleName() + " errorCode=" + ((SQLException) t).getErrorCode() + " sqlState=" + ((SQLException) t).getSQLState() + " " + t.getMessage()
          : t.getClass().getSimpleName() + ": " + t.getMessage();
      results.add(new String[] {name, "FAIL", msg});
      System.out.println("[FAIL] " + name + ": " + msg);
      t.printStackTrace(System.out);
    }
  }

  static void check(boolean b, Object m) { if (!b) throw new AssertionError(String.valueOf(m)); }

  static long count(Connection c, String sql) throws SQLException {
    try (Statement st = c.createStatement(); ResultSet rs = st.executeQuery(sql)) { rs.next(); return rs.getLong(1); }
  }

  public static void main(String[] args) throws Exception {
    Connection[] holder = new Connection[1];
    step("connect_autoconfig_emulator", () -> {
      holder[0] = DriverManager.getConnection(URL);
      DatabaseMetaData md = holder[0].getMetaData();
      return md.getDriverName() + " " + md.getDriverVersion() + " / " + md.getDatabaseProductName() + " " + md.getDatabaseProductVersion();
    });
    Connection c = holder[0];
    if (c == null) { summary(); return; }

    step("ddl_execute", () -> {
      try (Statement st = c.createStatement()) {
        st.execute("CREATE TABLE Items (Id INT64 NOT NULL, Name STRING(MAX), Data JSON, Amount NUMERIC, "
            + "UpdatedAt TIMESTAMP OPTIONS (allow_commit_timestamp=true), Scores ARRAY<FLOAT64>, Flag BOOL, Raw BYTES(MAX), Day DATE) PRIMARY KEY (Id)");
        // DDL batch
        st.execute("START BATCH DDL");
        st.execute("CREATE INDEX ItemsByName ON Items(Name)");
        st.execute("CREATE TABLE Other (K STRING(36) NOT NULL) PRIMARY KEY (K)");
        st.execute("RUN BATCH");
      }
      return "create table + DDL batch(2)";
    });

    String ins = "INSERT INTO Items (Id, Name, Data, Amount, UpdatedAt, Scores, Flag, Raw, Day) VALUES (?, ?, ?, ?, PENDING_COMMIT_TIMESTAMP(), ?, ?, ?, ?)";
    step("prepared_insert_common_types", () -> {
      try (PreparedStatement ps = c.prepareStatement(ins)) {
        for (int i = 1; i <= 2; i++) {
          ps.setLong(1, i);
          ps.setString(2, i == 1 ? "one" : "two");
          ps.setObject(3, "{\"n\":" + i + "}", JsonType.INSTANCE);
          ps.setBigDecimal(4, new BigDecimal(i + ".50"));
          ps.setArray(5, c.createArrayOf("FLOAT64", new Double[] {1.0 * i, 2.5}));
          ps.setBoolean(6, i % 2 == 0);
          ps.setBytes(7, new byte[] {1, 2, (byte) i});
          ps.setDate(8, java.sql.Date.valueOf("2024-01-0" + i));
          check(ps.executeUpdate() == 1, "update count");
        }
      }
      return "2 rows (autocommit)";
    });

    step("txn_commit_autocommit_false", () -> {
      c.setAutoCommit(false);
      try (PreparedStatement ps = c.prepareStatement(ins); PreparedStatement up = c.prepareStatement("UPDATE Items SET Name = ? WHERE Id = ?")) {
        ps.setLong(1, 3); ps.setString(2, "three"); ps.setObject(3, "{\"c\":true}", JsonType.INSTANCE);
        ps.setBigDecimal(4, new BigDecimal("3.125")); ps.setArray(5, c.createArrayOf("FLOAT64", new Double[] {0.5, 0.25}));
        ps.setNull(6, Types.BOOLEAN); ps.setNull(7, Types.BINARY); ps.setNull(8, Types.DATE);
        int n1 = ps.executeUpdate();
        up.setString(1, "one-updated"); up.setLong(2, 1);
        int n2 = up.executeUpdate();
        c.commit();
        check(n1 == 1 && n2 == 1, n1 + "," + n2);
      } finally { c.setAutoCommit(true); }
      return "insert=1 update=1 committed, count=" + count(c, "SELECT COUNT(*) FROM Items");
    });

    step("txn_rollback", () -> {
      c.setAutoCommit(false);
      try (Statement st = c.createStatement()) {
        st.executeUpdate("INSERT INTO Items (Id, Name) VALUES (99, 'rolled-back')");
        c.rollback();
      } finally { c.setAutoCommit(true); }
      long n = count(c, "SELECT COUNT(*) FROM Items WHERE Id = 99");
      check(n == 0, n);
      return "row 99 absent after rollback";
    });

    step("prepared_query_params_types", () -> {
      try (PreparedStatement ps = c.prepareStatement("SELECT Id, Name, Data, Amount, UpdatedAt, Scores, Flag, Raw, Day FROM Items WHERE Id >= ? AND Name LIKE ? ORDER BY Id")) {
        ps.setLong(1, 1); ps.setString(2, "%");
        StringBuilder sb = new StringBuilder();
        int n = 0;
        try (ResultSet rs = ps.executeQuery()) {
          ResultSetMetaData md = rs.getMetaData();
          for (int i = 1; i <= md.getColumnCount(); i++) sb.append(md.getColumnName(i)).append(':').append(md.getColumnTypeName(i)).append(' ');
          while (rs.next()) {
            n++;
            if (rs.getLong(1) == 2) {
              Double[] arr = (Double[]) rs.getArray(6).getArray();
              sb.append("| row2=").append(rs.getString(2)).append(',').append(rs.getString(3)).append(',').append(rs.getBigDecimal(4))
                  .append(',').append(rs.getTimestamp(5) != null).append(',').append(Arrays.toString(arr)).append(',').append(rs.getBoolean(7))
                  .append(',').append(Arrays.toString(rs.getBytes(8))).append(',').append(rs.getDate(9));
            }
          }
        }
        check(n == 3, n);
        return "rows=" + n + " " + sb;
      }
    });

    step("read_only_txn", () -> {
      c.setAutoCommit(false);
      c.setReadOnly(true);
      try {
        long a = count(c, "SELECT COUNT(*) FROM Items");
        long b = count(c, "SELECT COUNT(*) FROM Items@{FORCE_INDEX=ItemsByName} WHERE Name = 'one-updated'");
        c.commit();
        return "count=" + a + " via_index=" + b;
      } finally { c.setReadOnly(false); c.setAutoCommit(true); }
    });

    step("stale_read_exact_staleness", () -> {
      Thread.sleep(2000);
      try (Statement st = c.createStatement()) {
        st.execute("SET READ_ONLY_STALENESS = 'EXACT_STALENESS 1s'");
        long n = count(c, "SELECT COUNT(*) FROM Items");
        st.execute("SET READ_ONLY_STALENESS = 'STRONG'");
        check(n == 3, n);
        return "count=" + n;
      }
    });

    step("batch_dml", () -> {
      c.setAutoCommit(false);
      try (PreparedStatement ps = c.prepareStatement("UPDATE Items SET Amount = Amount + ? WHERE Id = ?")) {
        ps.setBigDecimal(1, BigDecimal.ONE); ps.setLong(2, 1); ps.addBatch();
        ps.setBigDecimal(1, BigDecimal.ONE); ps.setLong(2, 2); ps.addBatch();
        int[] r = ps.executeBatch();
        c.commit();
        check(r.length == 2 && r[0] == 1 && r[1] == 1, Arrays.toString(r));
        return "counts=" + Arrays.toString(r);
      } finally { c.setAutoCommit(true); }
    });

    step("metadata_getTables_getColumns", () -> {
      DatabaseMetaData md = c.getMetaData();
      List<String> tables = new ArrayList<>();
      try (ResultSet rs = md.getTables(null, null, "%", new String[] {"TABLE"})) { while (rs.next()) tables.add(rs.getString("TABLE_NAME")); }
      List<String> cols = new ArrayList<>();
      try (ResultSet rs = md.getColumns(null, null, "Items", null)) {
        while (rs.next()) cols.add(rs.getString("COLUMN_NAME") + ":" + rs.getString("TYPE_NAME") + "/" + rs.getInt("DATA_TYPE"));
      }
      List<String> pks = new ArrayList<>();
      try (ResultSet rs = md.getPrimaryKeys(null, null, "Items")) { while (rs.next()) pks.add(rs.getString("COLUMN_NAME")); }
      List<String> idx = new ArrayList<>();
      try (ResultSet rs = md.getIndexInfo(null, null, "Items", false, false)) { while (rs.next()) idx.add(rs.getString("INDEX_NAME")); }
      check(tables.contains("Items") && cols.size() == 9, tables + " " + cols);
      return "tables=" + tables + " cols=" + cols + " pk=" + pks + " idx=" + idx;
    });

    step("error_duplicate_key_SQLException", () -> {
      try (Statement st = c.createStatement()) {
        st.executeUpdate("INSERT INTO Items (Id, Name) VALUES (1, 'dup')");
      } catch (SQLException e) {
        String code = e instanceof JdbcSqlException ? String.valueOf(((JdbcSqlException) e).getCode()) : "?";
        check(e.getErrorCode() == 6, "errorCode=" + e.getErrorCode());
        return "class=" + e.getClass().getSimpleName() + " errorCode=" + e.getErrorCode() + " code=" + code + " sqlState=" + e.getSQLState() + " msg=" + e.getMessage();
      }
      throw new AssertionError("expected SQLException");
    });

    step("error_bad_sql_SQLException", () -> {
      try (Statement st = c.createStatement()) {
        st.executeQuery("SELECT NoSuchColumn FROM Items");
      } catch (SQLException e) {
        check(e.getErrorCode() == 3, "errorCode=" + e.getErrorCode());
        return "errorCode=" + e.getErrorCode() + " msg=" + e.getMessage();
      }
      throw new AssertionError("expected SQLException");
    });
    c.close();

    // PostgreSQL dialect via JDBC
    Connection[] ph = new Connection[1];
    step("pg_connect_autoconfig", () -> {
      ph[0] = DriverManager.getConnection(PGURL);
      try (Statement st = ph[0].createStatement(); ResultSet rs = st.executeQuery("SELECT 1")) { rs.next(); }
      return "dialect=" + ph[0].unwrap(com.google.cloud.spanner.jdbc.CloudSpannerJdbcConnection.class).getDialect();
    });
    Connection pg = ph[0];
    if (pg != null) {
      step("pg_ddl", () -> {
        try (Statement st = pg.createStatement()) {
          st.execute("CREATE TABLE users (id bigint NOT NULL PRIMARY KEY, name varchar, amount numeric, data jsonb, ts timestamptz)");
        }
        return "";
      });
      step("pg_insert_prepared", () -> {
        try (PreparedStatement ps = pg.prepareStatement("INSERT INTO users (id, name, amount, data, ts) VALUES (?, ?, ?, ?, ?)")) {
          ps.setLong(1, 1); ps.setString(2, "alice"); ps.setBigDecimal(3, new BigDecimal("9.99"));
          ps.setObject(4, "{\"k\":\"v\"}", com.google.cloud.spanner.jdbc.PgJsonbType.INSTANCE);
          ps.setTimestamp(5, Timestamp.valueOf("2024-01-02 03:04:05"));
          return "rows=" + ps.executeUpdate();
        }
      });
      step("pg_param_query_$1", () -> {
        try (PreparedStatement ps = pg.prepareStatement("SELECT id, name, amount, data FROM users WHERE id = $1")) {
          ps.setLong(1, 1);
          try (ResultSet rs = ps.executeQuery()) {
            check(rs.next(), "no row");
            return "row=" + rs.getLong(1) + "," + rs.getString(2) + "," + rs.getBigDecimal(3) + "," + rs.getString(4);
          }
        }
      });
      step("pg_metadata_getTables", () -> {
        List<String> t = new ArrayList<>();
        try (ResultSet rs = pg.getMetaData().getTables(null, null, "%", new String[] {"TABLE"})) { while (rs.next()) t.add(rs.getString("TABLE_SCHEM") + "." + rs.getString("TABLE_NAME")); }
        List<String> cols = new ArrayList<>();
        try (ResultSet rs = pg.getMetaData().getColumns(null, null, "users", null)) { while (rs.next()) cols.add(rs.getString("COLUMN_NAME") + ":" + rs.getString("TYPE_NAME")); }
        check(t.contains("public.users"), t);
        return "tables=" + t + " cols=" + cols;
      });
      step("pg_error_duplicate_key", () -> {
        try (Statement st = pg.createStatement()) {
          st.executeUpdate("INSERT INTO users (id, name) VALUES (1, 'dup')");
        } catch (SQLException e) {
          check(e.getErrorCode() == 6, "errorCode=" + e.getErrorCode());
          return "errorCode=" + e.getErrorCode() + " msg=" + e.getMessage();
        }
        throw new AssertionError("expected SQLException");
      });
      pg.close();
    }
    summary();
  }

  static void summary() {
    System.out.println("\nSUMMARY");
    boolean ok = true;
    for (String[] r : results) { System.out.println(r[1] + "\t" + r[0] + "\t" + r[2]); ok &= r[1].equals("PASS"); }
    System.exit(ok ? 0 : 1);
  }
}
