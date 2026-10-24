-- DuckPGQ OPTIONAL MATCH Demo
-- Run: cat demo/optional_match_demo.sql | ./build/release/duckdb -unsigned
--
-- PGQ-specific statements (CREATE PROPERTY GRAPH, FROM GRAPH_TABLE) must be
-- prefixed with '-' to route through the DuckPGQ parser extension.

LOAD 'build/release/duckpgq.duckdb_extension';

-- ── Schema ───────────────────────────────────────────────────────────────────
CREATE TABLE Tag(id BIGINT, name VARCHAR);
CREATE TABLE Workorder(id BIGINT, title VARCHAR);
CREATE TABLE ObjectType(id BIGINT, typename VARCHAR);
CREATE TABLE Notification(id BIGINT, message VARCHAR);

CREATE TABLE HasWorkorder(src BIGINT, dst BIGINT);
CREATE TABLE IsObjecttype(src BIGINT, dst BIGINT);
CREATE TABLE HasNotification(src BIGINT, dst BIGINT);
CREATE TABLE HasParent(src BIGINT, dst BIGINT);

-- ── Data ─────────────────────────────────────────────────────────────────────
-- Tags: root A; children B and C; grandchild D (child of B)
INSERT INTO Tag VALUES (0,'A'),(1,'B'),(2,'C'),(3,'D');

-- B has workorders W1/W2; D has W3
INSERT INTO Workorder VALUES (10,'W1'),(11,'W2'),(12,'W3');

-- Only A has objecttype T1
INSERT INTO ObjectType VALUES (20,'T1');

-- Only C has notification N1
INSERT INTO Notification VALUES (30,'Alert-N1');

INSERT INTO HasWorkorder    VALUES (1,10),(1,11),(3,12);
INSERT INTO IsObjecttype    VALUES (0,20);
INSERT INTO HasNotification VALUES (2,30);
INSERT INTO HasParent       VALUES (1,0),(2,0),(3,1);  -- B→A, C→A, D→B

-- ── Property graph ───────────────────────────────────────────────────────────
-CREATE PROPERTY GRAPH tagweb
VERTEX TABLES (
    Tag          PROPERTIES (id, name)      LABEL Tag,
    Workorder    PROPERTIES (id, title)     LABEL Workorder,
    ObjectType   PROPERTIES (id, typename)  LABEL ObjectType,
    Notification PROPERTIES (id, message)   LABEL Notification
)
EDGE TABLES (
    HasWorkorder    SOURCE KEY (src) REFERENCES Tag (id)
                    DESTINATION KEY (dst) REFERENCES Workorder (id)
                    LABEL HasWorkorder,
    IsObjecttype    SOURCE KEY (src) REFERENCES Tag (id)
                    DESTINATION KEY (dst) REFERENCES ObjectType (id)
                    LABEL IsObjecttype,
    HasNotification SOURCE KEY (src) REFERENCES Tag (id)
                    DESTINATION KEY (dst) REFERENCES Notification (id)
                    LABEL HasNotification,
    HasParent       SOURCE KEY (src) REFERENCES Tag (id)
                    DESTINATION KEY (dst) REFERENCES Tag (id)
                    LABEL HasParent
);

-- ── Q1: workorders per tag ────────────────────────────────────────────────────
-- A and C have no workorders → NULL; B has W1+W2; D has W3
SELECT '=== Q1: workorders per tag ===' AS query;
-FROM GRAPH_TABLE (tagweb
    MATCH (t:Tag)
    OPTIONAL MATCH (t)-[hw:HasWorkorder]->(wo:Workorder)
    COLUMNS (t.name AS tag, wo.title AS workorder)
) r ORDER BY 1, 2;

-- ── Q2: objecttypes per tag ───────────────────────────────────────────────────
SELECT '=== Q2: objecttypes per tag ===' AS query;
-FROM GRAPH_TABLE (tagweb
    MATCH (t:Tag)
    OPTIONAL MATCH (t)-[io:IsObjecttype]->(ot:ObjectType)
    COLUMNS (t.name AS tag, ot.typename AS objtype)
) r ORDER BY 1;

-- ── Q3: two OPTIONAL MATCHes combined ────────────────────────────────────────
-- Collect both workorder and objecttype in one pass per tag
SELECT '=== Q3: workorders + objecttypes per tag ===' AS query;
-FROM GRAPH_TABLE (tagweb
    MATCH (t:Tag)
    OPTIONAL MATCH (t)-[hw:HasWorkorder]->(wo:Workorder)
    OPTIONAL MATCH (t)-[io:IsObjecttype]->(ot:ObjectType)
    COLUMNS (t.name AS tag, wo.title AS workorder, ot.typename AS objtype)
) r ORDER BY 1, 2, 3;

-- ── Q4: three OPTIONAL MATCHes – full data collection ────────────────────────
-- Exhaustive: workorder, objecttype, and notification in one pass per tag.
-- This is the pattern for collecting all optional data across relationship types.
SELECT '=== Q4: full optional data per tag ===' AS query;
-FROM GRAPH_TABLE (tagweb
    MATCH (t:Tag)
    OPTIONAL MATCH (t)-[hw:HasWorkorder]->(wo:Workorder)
    OPTIONAL MATCH (t)-[io:IsObjecttype]->(ot:ObjectType)
    OPTIONAL MATCH (t)-[hn:HasNotification]->(n:Notification)
    COLUMNS (t.name AS tag, wo.title AS workorder, ot.typename AS objtype, n.message AS notification)
) r ORDER BY 1, 2, 3, 4;

-- ── Q5: WHERE on mandatory vertex ────────────────────────────────────────────
SELECT '=== Q5: WHERE on mandatory vertex (only tag B) ===' AS query;
-FROM GRAPH_TABLE (tagweb
    MATCH (t:Tag)
    OPTIONAL MATCH (t)-[hw:HasWorkorder]->(wo:Workorder)
    WHERE t.name = 'B'
    COLUMNS (t.name AS tag, wo.title AS workorder)
) r ORDER BY 2;

-- ── Q6: parent relationship (1-hop path) ─────────────────────────────────────
SELECT '=== Q6: parent relationships (1-hop) ===' AS query;
-FROM GRAPH_TABLE (tagweb
    MATCH (child:Tag)-[p:HasParent]->(parent:Tag)
    COLUMNS (child.name AS child_tag, parent.name AS parent_tag)
) r ORDER BY 1;

-- ── Q7: ancestors up to 2 hops (TRAIL path mode) ─────────────────────────────
SELECT '=== Q7: ancestors up to 2 hops ===' AS query;
-FROM GRAPH_TABLE (tagweb
    MATCH (child:Tag)-[p:HasParent]->{1,2}(ancestor:Tag)
    COLUMNS (child.name AS child_tag, ancestor.name AS ancestor_tag)
) r ORDER BY 1, 2;

-- ── Cleanup ───────────────────────────────────────────────────────────────────
-DROP PROPERTY GRAPH tagweb;
DROP TABLE HasParent; DROP TABLE HasNotification; DROP TABLE IsObjecttype;
DROP TABLE HasWorkorder; DROP TABLE Notification; DROP TABLE ObjectType;
DROP TABLE Workorder; DROP TABLE Tag;
