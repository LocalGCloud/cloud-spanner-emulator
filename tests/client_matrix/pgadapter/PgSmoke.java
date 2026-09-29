// PostgreSQL JDBC driver -> PGAdapter -> emulator smoke test.
import java.math.BigDecimal;
import java.sql.*;
import java.util.*;

public class PgSmoke {
  static final String URL = "jdbc:postgresql://localhost:" + System.getenv().getOrDefault("PGA_PORT", "19432") + "/"
      + System.getenv().getOrDefault("PGDB", "pgadb");
  static final List<String[]> results = new ArrayList<>();

  interface Step { String run() throws Exception; }

  static void step(String name, Step s) {
    try {
      String d = s.run();
      results.add(new String[] {name, "PASS", d == null ? "" : d});
      System.out.println("[PASS] " + name + " " + (d == null ? "" : d));
    } catch (Throwable t) {
      String msg = t instanceof SQLException
          ? t.getClass().getSimpleName() + " sqlState=" + ((SQLException) t).getSQLState() + " " + t.getMessage()
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
    Connection[] h = new Connection[1];
    step("connect", () -> {
      h[0] = DriverManager.getConnection(URL, new Properties());
      DatabaseMetaData md = h[0].getMetaData();
      try (Statement st = h[0].createStatement(); ResultSet rs = st.executeQuery("select version()")) {
        rs.next();
        return md.getDriverName() + " " + md.getDriverVersion() + " / server " + rs.getString(1);
      }
    });
    Connection c = h[0];
    if (c == null) { summary(); return; }

    step("ddl", () -> {
      try (Statement st = c.createStatement()) {
        st.execute("CREATE TABLE users (id bigint NOT NULL PRIMARY KEY, name varchar, amount numeric, data jsonb, "
            + "ts timestamptz, tags text[], flag boolean, raw bytea, d date, score float8)");
        st.execute("CREATE INDEX users_by_name ON users(name)");
      }
      return "create table + index";
    });

    String ins = "INSERT INTO users (id, name, amount, data, ts, tags, flag, raw, d, score) VALUES (?, ?, ?, ?::jsonb, ?, ?, ?, ?, ?, ?)";
    step("prepared_insert_types", () -> {
      try (PreparedStatement ps = c.prepareStatement(ins)) {
        for (int i = 1; i <= 2; i++) {
          ps.setLong(1, i);
          ps.setString(2, i == 1 ? "alice" : "bob");
          ps.setBigDecimal(3, new BigDecimal(i + ".99"));
          ps.setString(4, "{\"k\":" + i + "}");
          ps.setTimestamp(5, java.sql.Timestamp.valueOf("2024-01-02 03:04:05"));
          ps.setArray(6, c.createArrayOf("text", new String[] {"x", "y" + i}));
          ps.setBoolean(7, i % 2 == 0);
          ps.setBytes(8, new byte[] {1, 2, (byte) i});
          ps.setDate(9, java.sql.Date.valueOf("2024-01-0" + i));
          ps.setDouble(10, 1.5 * i);
          check(ps.executeUpdate() == 1, "count");
        }
      }
      return "2 rows";
    });

    step("prepared_query_params_x6_(server-side prepare)", () -> {
      String out = "";
      try (PreparedStatement ps = c.prepareStatement("SELECT id, name, amount, data, ts, tags, flag, raw, d, score FROM users WHERE id = ? AND name <> ?")) {
        for (int k = 0; k < 6; k++) {  // pgjdbc switches to a named server-side statement after prepareThreshold=5
          ps.setLong(1, 2); ps.setString(2, "zzz");
          try (ResultSet rs = ps.executeQuery()) {
            check(rs.next(), "no row");
            out = rs.getLong(1) + "," + rs.getString(2) + "," + rs.getBigDecimal(3) + "," + rs.getString(4) + "," + rs.getTimestamp(5)
                + "," + Arrays.toString((Object[]) rs.getArray(6).getArray()) + "," + rs.getBoolean(7) + "," + Arrays.toString(rs.getBytes(8))
                + "," + rs.getDate(9) + "," + rs.getDouble(10);
          }
        }
      }
      return "row=" + out;
    });

    step("txn_commit", () -> {
      c.setAutoCommit(false);
      try (PreparedStatement ps = c.prepareStatement("INSERT INTO users (id, name) VALUES (?, ?)");
           PreparedStatement up = c.prepareStatement("UPDATE users SET name = ? WHERE id = ?")) {
        ps.setLong(1, 3); ps.setString(2, "carol"); int a = ps.executeUpdate();
        up.setString(1, "alice-updated"); up.setLong(2, 1); int b = up.executeUpdate();
        c.commit();
        check(a == 1 && b == 1, a + "," + b);
      } finally { c.setAutoCommit(true); }
      return "count=" + count(c, "SELECT count(*) FROM users");
    });

    step("txn_rollback", () -> {
      c.setAutoCommit(false);
      try (Statement st = c.createStatement()) {
        st.executeUpdate("INSERT INTO users (id, name) VALUES (99, 'gone')");
        c.rollback();
      } finally { c.setAutoCommit(true); }
      long n = count(c, "SELECT count(*) FROM users WHERE id = 99");
      check(n == 0, n);
      return "row 99 absent";
    });

    step("read_only_txn", () -> {
      c.setAutoCommit(false);
      c.setReadOnly(true);
      try {
        long n = count(c, "SELECT count(*) FROM users");
        c.commit();
        return "count=" + n;
      } finally { c.setReadOnly(false); c.setAutoCommit(true); }
    });

    step("stale_read_exact_staleness", () -> {
      Thread.sleep(2000);
      try (Statement st = c.createStatement()) {
        st.execute("SET SPANNER.READ_ONLY_STALENESS = 'EXACT_STALENESS 1s'");
        long n = count(c, "SELECT count(*) FROM users");
        st.execute("SET SPANNER.READ_ONLY_STALENESS = 'STRONG'");
        check(n == 3, n);
        return "count=" + n;
      }
    });

    step("batch_dml", () -> {
      try (PreparedStatement ps = c.prepareStatement("UPDATE users SET amount = amount + ? WHERE id = ?")) {
        ps.setBigDecimal(1, BigDecimal.ONE); ps.setLong(2, 1); ps.addBatch();
        ps.setBigDecimal(1, BigDecimal.ONE); ps.setLong(2, 2); ps.addBatch();
        int[] r = ps.executeBatch();
        check(r.length == 2 && r[0] == 1 && r[1] == 1, Arrays.toString(r));
        return "counts=" + Arrays.toString(r);
      }
    });

    step("metadata_getTables_getColumns", () -> {
      List<String> t = new ArrayList<>();
      try (ResultSet rs = c.getMetaData().getTables(null, "public", "%", new String[] {"TABLE"})) { while (rs.next()) t.add(rs.getString("TABLE_NAME")); }
      List<String> cols = new ArrayList<>();
      try (ResultSet rs = c.getMetaData().getColumns(null, "public", "users", null)) { while (rs.next()) cols.add(rs.getString("COLUMN_NAME") + ":" + rs.getString("TYPE_NAME")); }
      check(t.contains("users"), t);
      return "tables=" + t + " cols=" + cols;
    });

    step("error_duplicate_key", () -> {
      try (Statement st = c.createStatement()) {
        st.executeUpdate("INSERT INTO users (id, name) VALUES (1, 'dup')");
      } catch (SQLException e) {
        return "class=" + e.getClass().getSimpleName() + " sqlState=" + e.getSQLState() + " msg=" + e.getMessage().replace('\n', ' ');
      }
      throw new AssertionError("expected SQLException");
    });

    step("error_bad_sql", () -> {
      try (Statement st = c.createStatement()) {
        st.executeQuery("SELECT no_such_col FROM users");
      } catch (SQLException e) {
        return "sqlState=" + e.getSQLState() + " msg=" + e.getMessage().replace('\n', ' ');
      }
      throw new AssertionError("expected SQLException");
    });

    step("connection_usable_after_error", () -> "count=" + count(c, "SELECT count(*) FROM users"));
    c.close();
    summary();
  }

  static void summary() {
    System.out.println("\nSUMMARY");
    boolean ok = true;
    for (String[] r : results) { System.out.println(r[1] + "\t" + r[0] + "\t" + r[2]); ok &= r[1].equals("PASS"); }
    System.exit(ok ? 0 : 1);
  }
}
