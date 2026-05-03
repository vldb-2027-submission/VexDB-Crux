\echo Use "CREATE EXTENSION crux_repair" to load this file. \quit

CREATE FUNCTION crux_repair(index_name text, query_vec floatvector(200), repair_ef int)
RETURNS boolean
AS 'MODULE_PATHNAME', 'crux_manual_repair'
LANGUAGE C STRICT NOT FENCED;

CREATE FUNCTION crux_repair_from_gt(index_name text, gt_path text, limit_k int)
RETURNS boolean
AS 'MODULE_PATHNAME', 'crux_repair_from_gt'
LANGUAGE C STRICT NOT FENCED;
