// Go cloud.google.com/go/spanner smoke test against the emulator.
package main

import (
	"context"
	"fmt"
	"math/big"
	"os"
	"time"

	"cloud.google.com/go/spanner"
	database "cloud.google.com/go/spanner/admin/database/apiv1"
	"cloud.google.com/go/spanner/admin/database/apiv1/databasepb"
	instance "cloud.google.com/go/spanner/admin/instance/apiv1"
	"cloud.google.com/go/spanner/admin/instance/apiv1/instancepb"
	"google.golang.org/api/iterator"
	"google.golang.org/grpc/codes"
)

const project = "p"

var (
	inst    = envOr("INSTANCE", "go-inst")
	instURI = "projects/" + project + "/instances/" + inst
	dbURI   = instURI + "/databases/godb"
	pgURI   = instURI + "/databases/gopgdb"
)

func envOr(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

type res struct{ name, status, detail string }

var results []res

func step(name string, fn func() (string, error)) {
	d, err := func() (d string, err error) {
		defer func() {
			if r := recover(); r != nil {
				err = fmt.Errorf("panic: %v", r)
			}
		}()
		return fn()
	}()
	if err != nil {
		results = append(results, res{name, "FAIL", err.Error()})
		fmt.Printf("[FAIL] %s: %v\n", name, err)
	} else {
		results = append(results, res{name, "PASS", d})
		fmt.Printf("[PASS] %s %s\n", name, d)
	}
}

func main() {
	ctx := context.Background()
	ia, err := instance.NewInstanceAdminClient(ctx)
	must(err)
	defer ia.Close()
	da, err := database.NewDatabaseAdminClient(ctx)
	must(err)
	defer da.Close()

	step("create_instance", func() (string, error) {
		op, err := ia.CreateInstance(ctx, &instancepb.CreateInstanceRequest{
			Parent:     "projects/" + project,
			InstanceId: inst,
			Instance: &instancepb.Instance{
				Config:      "projects/" + project + "/instanceConfigs/emulator-config",
				DisplayName: "go smoke",
				NodeCount:   1,
			},
		})
		if err != nil {
			return "", err
		}
		i, err := op.Wait(ctx)
		if err != nil {
			return "", err
		}
		return "state=" + i.State.String(), nil
	})

	step("create_database", func() (string, error) {
		op, err := da.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
			Parent:          instURI,
			CreateStatement: "CREATE DATABASE godb",
			ExtraStatements: []string{`CREATE TABLE Items (
				Id INT64 NOT NULL,
				Name STRING(MAX),
				Data JSON,
				Amount NUMERIC,
				UpdatedAt TIMESTAMP OPTIONS (allow_commit_timestamp=true),
				Scores ARRAY<FLOAT64>
			) PRIMARY KEY (Id)`},
		})
		if err != nil {
			return "", err
		}
		d, err := op.Wait(ctx)
		if err != nil {
			return "", err
		}
		return "dialect=" + d.DatabaseDialect.String(), nil
	})

	step("update_ddl_index", func() (string, error) {
		op, err := da.UpdateDatabaseDdl(ctx, &databasepb.UpdateDatabaseDdlRequest{
			Database:   dbURI,
			Statements: []string{"CREATE INDEX ItemsByName ON Items(Name)"},
		})
		if err != nil {
			return "", err
		}
		if err := op.Wait(ctx); err != nil {
			return "", err
		}
		ddl, err := da.GetDatabaseDdl(ctx, &databasepb.GetDatabaseDdlRequest{Database: dbURI})
		if err != nil {
			return "", err
		}
		return fmt.Sprintf("ddl_count=%d", len(ddl.Statements)), nil
	})

	client, err := spanner.NewClient(ctx, dbURI)
	must(err)
	defer client.Close()
	cols := []string{"Id", "Name", "Data", "Amount", "UpdatedAt", "Scores"}

	step("insert_mutations", func() (string, error) {
		ts, err := client.Apply(ctx, []*spanner.Mutation{
			spanner.Insert("Items", cols, []interface{}{int64(1), "one", spanner.NullJSON{Value: map[string]any{"a": 1}, Valid: true}, big.NewRat(3, 2), spanner.CommitTimestamp, []float64{1.0, 2.5}}),
			spanner.Insert("Items", cols, []interface{}{int64(2), "two", spanner.NullJSON{Value: map[string]any{"b": []int{1, 2}}, Valid: true}, big.NewRat(9, 4), spanner.CommitTimestamp, []float64{3.0}}),
		})
		if err != nil {
			return "", err
		}
		return "commit_ts=" + ts.String(), nil
	})

	step("rw_txn_dml", func() (string, error) {
		var n1, n2 int64
		_, err := client.ReadWriteTransaction(ctx, func(ctx context.Context, tx *spanner.ReadWriteTransaction) error {
			var err error
			n1, err = tx.Update(ctx, spanner.Statement{
				SQL: "INSERT INTO Items (Id, Name, Data, Amount, UpdatedAt, Scores) VALUES (@id, @name, @data, @amt, PENDING_COMMIT_TIMESTAMP(), @scores)",
				Params: map[string]interface{}{"id": int64(3), "name": "three",
					"data": spanner.NullJSON{Value: map[string]any{"c": true}, Valid: true},
					"amt":  big.NewRat(25, 8), "scores": []float64{0.5, 0.25}},
			})
			if err != nil {
				return err
			}
			n2, err = tx.Update(ctx, spanner.Statement{
				SQL:    "UPDATE Items SET Name = @name WHERE Id = @id",
				Params: map[string]interface{}{"id": int64(1), "name": "one-updated"},
			})
			return err
		})
		if err != nil {
			return "", err
		}
		if n1 != 1 || n2 != 1 {
			return "", fmt.Errorf("counts %d %d", n1, n2)
		}
		return fmt.Sprintf("insert=%d update=%d", n1, n2), nil
	})

	step("param_query", func() (string, error) {
		it := client.Single().Query(ctx, spanner.Statement{
			SQL:    "SELECT Id, Name, Data, Amount, UpdatedAt, Scores FROM Items WHERE Id >= @min ORDER BY Id",
			Params: map[string]interface{}{"min": int64(1)},
		})
		defer it.Stop()
		out := ""
		n := 0
		for {
			row, err := it.Next()
			if err == iterator.Done {
				break
			}
			if err != nil {
				return "", err
			}
			var id int64
			var name spanner.NullString
			var data spanner.NullJSON
			var amt spanner.NullNumeric
			var ts spanner.NullTime
			var scores []float64
			if err := row.Columns(&id, &name, &data, &amt, &ts, &scores); err != nil {
				return "", err
			}
			n++
			if id == 3 {
				out = fmt.Sprintf("row3=%s,%s,%s,%v,%v", name.StringVal, data.String(), amt.Numeric.FloatString(3), ts.Valid, scores)
			}
		}
		if n != 3 {
			return "", fmt.Errorf("rows=%d", n)
		}
		return fmt.Sprintf("rows=%d %s", n, out), nil
	})

	step("readonly_snapshot_query", func() (string, error) {
		ro := client.ReadOnlyTransaction()
		defer ro.Close()
		var cnt int64
		if err := ro.Query(ctx, spanner.NewStatement("SELECT COUNT(*) FROM Items")).Do(func(r *spanner.Row) error { return r.Column(0, &cnt) }); err != nil {
			return "", err
		}
		nread := 0
		if err := ro.Read(ctx, "Items", spanner.AllKeys(), []string{"Id", "Name"}).Do(func(r *spanner.Row) error { nread++; return nil }); err != nil {
			return "", err
		}
		var idx int64
		if err := ro.ReadUsingIndex(ctx, "Items", "ItemsByName", spanner.Key{"one-updated"}, []string{"Id"}).Do(func(r *spanner.Row) error { return r.Column(0, &idx) }); err != nil {
			return "", err
		}
		return fmt.Sprintf("count=%d read_rows=%d index_read_id=%d", cnt, nread, idx), nil
	})

	step("stale_read_exact_staleness", func() (string, error) {
		time.Sleep(2 * time.Second)
		var cnt int64
		err := client.Single().WithTimestampBound(spanner.ExactStaleness(time.Second)).
			Query(ctx, spanner.NewStatement("SELECT COUNT(*) FROM Items")).
			Do(func(r *spanner.Row) error { return r.Column(0, &cnt) })
		if err != nil {
			return "", err
		}
		if cnt != 3 {
			return "", fmt.Errorf("count=%d", cnt)
		}
		return fmt.Sprintf("count=%d", cnt), nil
	})

	step("batch_dml", func() (string, error) {
		var counts []int64
		_, err := client.ReadWriteTransaction(ctx, func(ctx context.Context, tx *spanner.ReadWriteTransaction) error {
			var err error
			counts, err = tx.BatchUpdate(ctx, []spanner.Statement{
				{SQL: "INSERT INTO Items (Id, Name) VALUES (@id, @name)", Params: map[string]interface{}{"id": int64(10), "name": "ten"}},
				{SQL: "UPDATE Items SET Amount = Amount + 1 WHERE Id IN (1, 2)"},
			})
			return err
		})
		if err != nil {
			return "", err
		}
		if len(counts) != 2 || counts[0] != 1 || counts[1] != 2 {
			return "", fmt.Errorf("counts=%v", counts)
		}
		return fmt.Sprintf("counts=%v", counts), nil
	})

	step("duplicate_key_error", func() (string, error) {
		_, err := client.ReadWriteTransaction(ctx, func(ctx context.Context, tx *spanner.ReadWriteTransaction) error {
			_, err := tx.Update(ctx, spanner.NewStatement("INSERT INTO Items (Id, Name) VALUES (1, 'dup')"))
			return err
		})
		if spanner.ErrCode(err) != codes.AlreadyExists {
			return "", fmt.Errorf("expected AlreadyExists, got code=%v err=%v", spanner.ErrCode(err), err)
		}
		return fmt.Sprintf("code=%v desc=%q", spanner.ErrCode(err), spanner.ErrDesc(err)), nil
	})

	step("duplicate_key_mutation_error", func() (string, error) {
		_, err := client.Apply(ctx, []*spanner.Mutation{spanner.Insert("Items", []string{"Id", "Name"}, []interface{}{int64(2), "dup"})})
		if spanner.ErrCode(err) != codes.AlreadyExists {
			return "", fmt.Errorf("expected AlreadyExists, got code=%v err=%v", spanner.ErrCode(err), err)
		}
		return fmt.Sprintf("code=%v desc=%q", spanner.ErrCode(err), spanner.ErrDesc(err)), nil
	})

	// PostgreSQL dialect.
	step("pg_create_database", func() (string, error) {
		op, err := da.CreateDatabase(ctx, &databasepb.CreateDatabaseRequest{
			Parent:          instURI,
			CreateStatement: `CREATE DATABASE "gopgdb"`,
			DatabaseDialect: databasepb.DatabaseDialect_POSTGRESQL,
		})
		if err != nil {
			return "", err
		}
		d, err := op.Wait(ctx)
		if err != nil {
			return "", err
		}
		return "dialect=" + d.DatabaseDialect.String(), nil
	})

	step("pg_ddl", func() (string, error) {
		op, err := da.UpdateDatabaseDdl(ctx, &databasepb.UpdateDatabaseDdlRequest{
			Database: pgURI,
			Statements: []string{"CREATE TABLE users (id bigint NOT NULL PRIMARY KEY, name varchar, amount numeric, data jsonb, ts timestamptz, tags text[])",
				"CREATE INDEX users_by_name ON users(name)"},
		})
		if err != nil {
			return "", err
		}
		return "", op.Wait(ctx)
	})

	pg, err := spanner.NewClient(ctx, pgURI)
	must(err)
	defer pg.Close()

	step("pg_insert", func() (string, error) {
		var n int64
		_, err := pg.ReadWriteTransaction(ctx, func(ctx context.Context, tx *spanner.ReadWriteTransaction) error {
			var err error
			n, err = tx.Update(ctx, spanner.Statement{
				SQL: "INSERT INTO users (id, name, amount, data, ts, tags) VALUES ($1, $2, $3, $4, $5, $6)",
				Params: map[string]interface{}{"p1": int64(1), "p2": "alice",
					"p3": spanner.PGNumeric{Numeric: "9.99", Valid: true},
					"p4": spanner.PGJsonB{Value: map[string]any{"k": "v"}, Valid: true},
					"p5": time.Date(2024, 1, 2, 3, 4, 5, 0, time.UTC),
					"p6": []string{"x", "y"}},
			})
			return err
		})
		if err != nil {
			return "", err
		}
		if _, err := pg.Apply(ctx, []*spanner.Mutation{spanner.Insert("users", []string{"id", "name"}, []interface{}{int64(2), "bob"})}); err != nil {
			return "", err
		}
		return fmt.Sprintf("dml_rows=%d + 1 mutation", n), nil
	})

	step("pg_param_query", func() (string, error) {
		var out string
		n := 0
		err := pg.Single().Query(ctx, spanner.Statement{
			SQL:    "SELECT id, name, amount, data, ts, tags FROM users WHERE id = $1",
			Params: map[string]interface{}{"p1": int64(1)},
		}).Do(func(r *spanner.Row) error {
			var id int64
			var name spanner.NullString
			var amt spanner.PGNumeric
			var data spanner.PGJsonB
			var ts spanner.NullTime
			var tags []spanner.NullString
			if err := r.Columns(&id, &name, &amt, &data, &ts, &tags); err != nil {
				return err
			}
			n++
			out = fmt.Sprintf("%d,%s,%s,%s,%s,%v", id, name, amt.Numeric, data.String(), ts.Time.Format(time.RFC3339), tags)
			return nil
		})
		if err != nil {
			return "", err
		}
		if n != 1 {
			return "", fmt.Errorf("rows=%d", n)
		}
		return "row=" + out, nil
	})

	step("pg_duplicate_key_error", func() (string, error) {
		_, err := pg.ReadWriteTransaction(ctx, func(ctx context.Context, tx *spanner.ReadWriteTransaction) error {
			_, err := tx.Update(ctx, spanner.NewStatement("INSERT INTO users (id, name) VALUES (1, 'dup')"))
			return err
		})
		if spanner.ErrCode(err) != codes.AlreadyExists {
			return "", fmt.Errorf("expected AlreadyExists, got code=%v err=%v", spanner.ErrCode(err), err)
		}
		return fmt.Sprintf("code=%v desc=%q", spanner.ErrCode(err), spanner.ErrDesc(err)), nil
	})

	fmt.Println("\nSUMMARY")
	fail := false
	for _, r := range results {
		fmt.Printf("%s\t%s\t%s\n", r.status, r.name, r.detail)
		if r.status != "PASS" {
			fail = true
		}
	}
	if fail {
		os.Exit(1)
	}
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}
