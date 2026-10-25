# DuckPGQ features, with examples

Every example below was run on the current build (v2-port ef6678de) and the result shown is what it returned. They all use one small graph, so you can paste them into a DuckDB 2.0 shell
in order.

## The example graph

```sql
LOAD 'duckpgq.duckdb_extension';                 -- duckdb -unsigned, or allow_unsigned_extensions
SET active_grammar_extensions = ['duckpgq'];      -- per connection, as its own statement

CREATE TABLE person (id BIGINT PRIMARY KEY, name VARCHAR, age INT, kind INT);   -- kind: 1 = Student, 2 = Teacher
CREATE TABLE city (code VARCHAR PRIMARY KEY, name VARCHAR);
CREATE TABLE knows (src BIGINT, dst BIGINT, since INT, w DOUBLE);
CREATE TABLE lives_in (person_id BIGINT, city_code VARCHAR);
INSERT INTO person VALUES (1,'Ann',31,1),(2,'Bob',45,2),(3,'Cid',27,1),(4,'Dan',52,2),(5,'Eve',38,1);
INSERT INTO city VALUES ('OSL','Oslo'),('BGO','Bergen');
INSERT INTO knows VALUES (1,2,2019,1.0),(2,3,2021,1.0),(1,3,2020,5.0),(3,4,2024,0.5),(4,5,2022,2.0);
INSERT INTO lives_in VALUES (1,'OSL'),(2,'OSL'),(3,'BGO'),(4,'BGO');      -- Eve lives nowhere
```

Ann→Bob→Cid→Dan→Eve is a chain of `Knows` edges, with a shortcut Ann→Cid that is shorter in hops (1) but more
expensive in weight (`w` = 5).

## 1. Defining graphs

### CREATE PROPERTY GRAPH

```sql
CREATE PROPERTY GRAPH social
  VERTEX TABLES (person PROPERTIES (id, name, age) LABEL Person IN kind (Student, Teacher),
                 city LABEL City)
  EDGE TABLES (knows SOURCE KEY (src) REFERENCES person (id)
                     DESTINATION KEY (dst) REFERENCES person (id) LABEL Knows,
               lives_in SOURCE KEY (person_id) REFERENCES person (id)
                        DESTINATION KEY (city_code) REFERENCES city (code) LABEL LivesIn)
  WITH ABI (max_hops := 2, closures := ['Knows']);
```

- **`PROPERTIES (…)`, `PROPERTIES ARE ALL COLUMNS [EXCEPT (…)]`, `NO PROPERTIES`:** which columns are visible.
- **`LABEL Person IN kind (Student, Teacher)`:** sub-labels from an integer bit-mask column (bit 0 = Student, bit 1 = Teacher).
- **`WITH ABI (…)`:** declares how the graph's path tables are built (see §5). Declaring builds nothing.
- **Tables in other schemas and attached databases:** `f001.raw.tag` works, which is how graphs go over DuckLake.
- **Also:** `CREATE OR REPLACE`, `IF NOT EXISTS`, `DROP PROPERTY GRAPH [IF EXISTS]`.

### DESCRIBE / SHOW / SUMMARIZE

```sql
SELECT table_name, label, is_vertex_table, sub_labels FROM (DESCRIBE PROPERTY GRAPH social);
```
| table_name | label | is_vertex_table | sub_labels |
|---|---|---|---|
| person | Person | true | [Student, Teacher] |
| city | City | true | NULL |
| knows | Knows | false | NULL |
| lives_in | LivesIn | false | NULL |

`SUMMARIZE PROPERTY GRAPH social` gives statistics.

## 2. Patterns

### One hop, with an edge property

```sql
FROM GRAPH_TABLE (social MATCH (a:Person)-[k:Knows]->(b:Person) WHERE k.since >= 2021
  COLUMNS (a.name AS who, b.name AS knows, k.since)) ORDER BY who;
```
| who | knows | since |
|---|---|---|
| Bob | Cid | 2021 |
| Cid | Dan | 2024 |
| Dan | Eve | 2022 |

### Label expressions and sub-labels

`|` or, `&` and, `!` not. A sub-label matches the rows whose bit is set.
```sql
FROM GRAPH_TABLE (social MATCH (x:Student|Teacher WHERE x.age > 30) COLUMNS (x.name, label(x) AS label)) ORDER BY 1;
FROM GRAPH_TABLE (social MATCH (t:Teacher) COLUMNS (t.name)) ORDER BY 1;
```
The first returns Ann, Bob, Dan and Eve (each with label `Person`); the second returns Bob and Dan.

### Element predicates

```sql
FROM GRAPH_TABLE (social MATCH (a:Person WHERE a.age > 40)-[:LivesIn]->(c:City) COLUMNS (a.name, c.name AS city)) ORDER BY 1;
```
Bob / Oslo, Dan / Bergen.

### Directions

`->`, `<-`, and `-` / `<->` for either direction, as SQL/PGQ defines them.
```sql
FROM GRAPH_TABLE (social MATCH (a:Person WHERE a.name = 'Cid')-[:Knows]-(b:Person) COLUMNS (b.name)) ORDER BY 1;
```
Ann, Bob, Dan.

### Variable-length paths (quantifiers)

`{n,m}`, `{,m}`, `{n,}`, `{n}`, `*` (0+), `+` (1+), `?` (0 or 1). With no search prefix, every walk is a match.
```sql
FROM GRAPH_TABLE (social MATCH p = (a:Person WHERE a.name = 'Ann')-[:Knows]->{1,3}(b:Person)
  COLUMNS (b.name AS reached, path_length(p) AS hops)) ORDER BY hops, reached;
```
| reached | hops |
|---|---|
| Bob | 1 |
| Cid | 1 |
| Cid | 2 |
| Dan | 2 |
| Dan | 3 |
| Eve | 3 |

### Cypher-style quantifiers

These mean what they mean in Neo4j: a bare `*` is one or more hops here, but zero or more in the arrow form `->*`.

| Cypher | Same as |
|---|---|
| `-[:R*]->`, `-[:R*..]->` | `-[:R]->{1,}` |
| `-[:R*..3]->` | `-[:R]->{1,3}` |
| `-[:R*1..5]->` | `-[:R]->{1,5}` |
| `-[:R*2]->` | `-[:R]->{2}` |
| `-[:R*0..3]->` | `-[:R]->{0,3}` |

```sql
FROM GRAPH_TABLE (social MATCH (a:Person WHERE a.name = 'Ann')-[:Knows*..2]->(b:Person) COLUMNS (b.name)) ORDER BY 1;
```
Bob, Cid, Cid, Dan.

### Zero hops

A quantifier from 0 also matches the start vertex itself.
```sql
FROM GRAPH_TABLE (social MATCH (a:Person WHERE a.name = 'Dan')-[:Knows]->?(b:Person) COLUMNS (b.name)) ORDER BY 1;
```
Dan (zero hops), Eve.

### Shortest, k-shortest and cheapest paths

```sql
FROM GRAPH_TABLE (social MATCH p = ANY SHORTEST (a:Person WHERE a.name = 'Ann')-[:Knows]->+(b:Person)
  COLUMNS (b.name, path_length(p) AS hops)) ORDER BY 1;
```
Bob 1, Cid 1, Dan 2, Eve 3: one path per endpoint. `ALL SHORTEST` returns every path tied at the shortest length.

```sql
FROM GRAPH_TABLE (social MATCH p = SHORTEST 2 (a:Person WHERE a.name = 'Ann')-[:Knows]->+(b:Person WHERE b.name = 'Dan')
  COLUMNS (vertex_property(p, 'name') AS route, path_length(p) AS hops)) ORDER BY hops, route;
```
| route | hops |
|---|---|
| [Ann, Cid, Dan] | 2 |
| [Ann, Bob, Cid, Dan] | 3 |

```sql
FROM GRAPH_TABLE (social MATCH p = ANY CHEAPEST (a:Person WHERE a.name = 'Ann')-[k:Knows COST k.w]->+(b:Person WHERE b.name = 'Dan')
  COLUMNS (vertex_property(p, 'name') AS route, cost(p) AS total));
```
| route | total |
|---|---|
| [Ann, Bob, Cid, Dan] | 2.5 |

The cheapest route takes three hops (1 + 1 + 0.5) and beats the two-hop route through the expensive shortcut (5 + 0.5).

- **Costs:** may be integers, DOUBLE or DECIMAL. A negative, NaN or NULL cost without `COST … DEFAULT n` is an error.
- **`ALL CHEAPEST`:** returns every tied path.
- **Bounds** (`{1,3}`) work: the result is the cheapest path within them.

### Path modes

`WALK` (default), `TRAIL` (no repeated edge), `ACYCLIC` (no repeated vertex), `SIMPLE`.
```sql
FROM GRAPH_TABLE (social MATCH p = ANY SHORTEST ACYCLIC (a:Person WHERE a.name = 'Ann')-[:Knows]-{1,4}(b:Person WHERE b.name = 'Eve')
  COLUMNS (path_length(p) AS hops));
```
3.

### Several patterns, OPTIONAL MATCH, EXISTS

```sql
FROM GRAPH_TABLE (social MATCH (a:Person) OPTIONAL MATCH (a)-[:LivesIn]->(c:City) COLUMNS (a.name, c.name AS city)) ORDER BY 1;
```
Ann Oslo, Bob Oslo, Cid Bergen, Dan Bergen, Eve NULL. OPTIONAL MATCH takes any pattern a MATCH takes.

```sql
FROM GRAPH_TABLE (social MATCH (a:Person) WHERE NOT EXISTS { (a)-[:LivesIn]->(:City) } COLUMNS (a.name));
```
Eve.

```sql
FROM GRAPH_TABLE (social MATCH (a:Person)-[:Knows]->(b:Person), (a)-[:LivesIn]->(c:City)
  COLUMNS (a.name, b.name AS friend, c.name AS city)) ORDER BY 1, 2;
```
Ann/Bob/Oslo, Ann/Cid/Oslo, Bob/Cid/Oslo, Cid/Dan/Bergen, Dan/Eve/Bergen.

### Unions

Several MATCH blocks, sharing one COLUMNS (`UNION` removes duplicates, `UNION ALL` keeps them):
```sql
FROM GRAPH_TABLE (social MATCH (a:Person WHERE a.name = 'Cid')-[:Knows]->(x:Person)
                         UNION MATCH (x:Person)-[:Knows]->(a:Person WHERE a.name = 'Cid')
  COLUMNS (x.name AS neighbour)) ORDER BY ALL;
```
Ann, Bob, Dan.

…or each with its own COLUMNS, combined by position:
```sql
FROM GRAPH_TABLE (social MATCH (a:Person)-[:Knows]->(b:Person) COLUMNS (a.name AS name, 'knows' AS how)
  UNION ALL MATCH (a:Person)-[:LivesIn]->(c:City) COLUMNS (a.name AS name, 'lives in ' || c.name AS how)) ORDER BY ALL;
```

Path pattern unions inside one MATCH: `|` (set) and `|+|` (multiset):
```sql
FROM GRAPH_TABLE (social MATCH (a:Person WHERE a.name = 'Ann')-[:Knows]->(b:Person)
                           |+| (a:Person WHERE a.name = 'Ann')-[:Knows]->{2}(b:Person)
  COLUMNS (b.name)) ORDER BY 1;
```
Bob, Cid, Cid, Dan.

### One row per vertex, edge or step

```sql
FROM GRAPH_TABLE (social MATCH (a:Person)-[:Knows]->{1,3}(b:Person)
  ONE ROW PER STEP (v1, e, v2) WHERE a.name = 'Ann' AND b.name = 'Dan'
  COLUMNS (v1.name AS from_, v2.name AS to_));
```
| from_ | to_ |
|---|---|
| Ann | Cid |
| Cid | Dan |
| Ann | Bob |
| Bob | Cid |
| Cid | Dan |

Two paths from Ann to Dan, one row per step of each. `ONE ROW PER VERTEX (v)` and `ONE ROW PER EDGE (e)` unnest the
same way.

## 3. Expressions and functions

```sql
FROM GRAPH_TABLE (social MATCH (a:Person)-[k:Knows]->(b:Person)
  WHERE a.name = 'Ann' AND ALL_DIFFERENT(a, b) AND b IS LABELED Person
  COLUMNS (a.{name, age} AS who, label(b) AS label, element_id(k) AS edge_id)) ORDER BY ALL;
```
| who | label | edge_id |
|---|---|---|
| {'name': Ann, 'age': 31} | Person | 0 |
| {'name': Ann, 'age': 31} | Person | 2 |

- **`v.{a, b}`:** a struct of properties.
- **`label(x)`:** the label as declared.
- **`element_id(x)`:** the element's id.
- **`x IS [NOT] LABELED L`:** a label test, case-insensitive.
- **`ALL_DIFFERENT(…)`:** the bound elements are pairwise distinct.

### Along a path

```sql
FROM GRAPH_TABLE (social MATCH p = (a:Person WHERE a.name = 'Ann')-[:Knows]->{2}(b:Person)
  COLUMNS (vertex_labels(p) AS vl, edge_labels(p) AS el, vertex_property(p, 'name') AS names,
           list_sum(edge_property(p, 'w')) AS total_w, list_min(edge_property(p, 'since')) AS first_since)) ORDER BY ALL;
```
| vl | el | names | total_w | first_since |
|---|---|---|---|---|
| [Person, Person, Person] | [Knows, Knows] | [Ann, Bob, Cid] | 2.0 | 2019 |
| [Person, Person, Person] | [Knows, Knows] | [Ann, Cid, Dan] | 5.5 | 2020 |

- **Path shape:** `path_length(p)`, `vertices(p)`, `edges(p)`, `vertex_labels(p)`, `edge_labels(p)`.
- **Properties along the path:** `vertex_property(p, 'x')` and `edge_property(p, 'x')`; aggregate them with `list_sum`, `list_avg`, `list_min`, `list_max` or `len`.
- **`cost(p)`:** the total cost under CHEAPEST.
- **`path_tree_json(…)`, `graph_tree_json(…)`:** nest paths into JSON.

### Ordinary SQL around the graph

`GRAPH_TABLE` is a table; GROUP BY, joins, ORDER BY and window functions work on it as usual.
```sql
SELECT city, count(*) AS people, avg(age) AS avg_age
FROM GRAPH_TABLE (social MATCH (p:Person)-[:LivesIn]->(c:City) COLUMNS (p.age, c.name AS city))
GROUP BY city ORDER BY city;
```
| city | people | avg_age |
|---|---|---|
| Bergen | 2 | 39.5 |
| Oslo | 2 | 38.0 |

Prepared-statement parameters work inside GRAPH_TABLE (`WHERE a.id = $1`).

## 4. Graph algorithms

```sql
SELECT * FROM pagerank('social', 'Person', 'Knows') ORDER BY 1;
SELECT * FROM weakly_connected_component('social', 'Person', 'Knows') ORDER BY 1;
SELECT * FROM local_clustering_coefficient('social', 'Person', 'Knows') ORDER BY 1;
```
PageRank rises along the chain (Ann 0.071 … Eve 0.267). All five people are in one weakly connected component.

## 5. ABI path tables

Precomputed path tables that `GRAPH_TABLE` reads instead of traversing.

```sql
SELECT artifact, kind, status, row_count FROM pgq_abi_build('social');   -- uses the graph's WITH ABI declaration
```
| artifact | kind | status | row_count |
|---|---|---|---|
| __pg_social__Person__Knows__Person | edge | built | 5 |
| __pg_social__Person__LivesIn__City | edge | built | 4 |
| __pg_social__tc__Person__Knows | transitive_closure | built | 13 |
| __pg_social__tc_witness__Person__Knows | tc_witness | built | 10 |
| __pg_social__tc_chain__Person__Knows | tc_chain | built | 4 |
| __pgq_path_catalog | catalog | built | 5 |

Also `pgq_abi_status('social')` (what exists, and whether it is fresh against its DuckLake snapshot), `pgq_abi_sql`
(the script) and `pgq_abi_drop`.

Which table a query used:
```sql
FROM GRAPH_TABLE (social MATCH ANY SHORTEST (a:Person)-[:Knows]->{1,3}(b:Person) WHERE a.name = 'Ann' COLUMNS (b.name));
SELECT rewrite_type, status, path_table_name FROM pgq_last_rewrite_diagnostics();
```
| rewrite_type | status | path_table_name |
|---|---|---|
| multiplicity_probe | not_exact | NULL |
| transitive_closure | selected | __pg_social__tc__Person__Knows |

The closure answers ANY SHORTEST. The probe line records that it may not answer a plain walk query here, because Ann
reaches Cid at two depths, so a walk query traverses instead and still gets every walk right.

```sql
SELECT status, abi_rows, fallback_rows, rewrite_fired FROM pgq_verify_abi_rewrite(
  $$FROM GRAPH_TABLE (social MATCH (a:Person)-[:Knows]->{1,3}(b:Person) COLUMNS (a.name, b.name))$$);
```
`match`, 12 rows both ways, rewrite fired: the query's answer from the tables equals the traversal's.

```sql
SELECT status, reason, path_table_name FROM pgq_explain_abi_path('social', 'auto', ['Person', 'Person'], ['Knows']) LIMIT 1;
```
`selected`, exact semantic sequence match, `__pg_social__Person__Knows__Person`.

`SET pgq_require_abi_paths = true` turns a pattern no table can answer into an error instead of a traversal: the
guard for large lakes.

## 6. Running it

- **Grammar for every connection:** `SET GLOBAL duckpgq_default_grammar = true` switches the grammar on for every new connection, for servers and pools.
- **Serving graphs over Quack:** `quack_serve` serves the graph from a DuckDB server, and clients send `FROM quack_query_by_name('srv', $$FROM GRAPH_TABLE (…)$$)` with only the quack extension loaded.
- **In the browser:** the extension runs in duckdb-wasm 2.0 as a loadable extension.

The README covers each of these.

## Not supported yet

Being worked on now:
- **Cross-table label disjunction:** `(x:Person|City)` is refused; it works within one table's sub-labels.
- **Unlabeled vertices with several vertex tables:** a vertex with no label, `(x)`, is refused when the graph has more than one vertex table.
- **Graph definition:**
  - graphs over views;
  - several labels per table;
  - an explicit `KEY` clause;
  - computed properties;
  - `ALTER PROPERTY GRAPH`.
- **Predicates:** `IS SOURCE OF`, `IS DESTINATION OF`, `SAME`, `PROPERTY_EXISTS`, `labels()`.
- **Selectors:** `ANY k` and `SHORTEST k GROUP`.
- **Repeated sub-patterns** (`((a)-[:R]->(b)){1,3}`) and **edge variables under a quantifier** (`-[e]->{1,2}` then `e.w`).
- **CHEAPEST refusals:**
  - `COST(p)` in the MATCH WHERE;
  - CHEAPEST inside OPTIONAL MATCH;
  - a cost that reads the endpoints;
  - several CHEAPEST patterns in one block.
- **More algorithms:** strongly connected components, betweenness, triangles, communities, k-core.

By design:
- **Graph updates:** insert or delete through the tables; the graph is a view over them.
- **`LATERAL` inside MATCH:** use SQL `LATERAL` around `GRAPH_TABLE`.
