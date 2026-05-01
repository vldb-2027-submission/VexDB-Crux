\echo Use "CREATE EXTENSION qasp_repair" to load this file. \quit

CREATE FUNCTION qasp_repair(index_name text, query_vec floatvector(200), repair_ef int)
RETURNS boolean
AS 'MODULE_PATHNAME', 'qasp_manual_repair'
LANGUAGE C STRICT NOT FENCED;

CREATE FUNCTION qasp_repair_from_gt(index_name text, gt_path text, limit_k int)
RETURNS boolean
AS 'MODULE_PATHNAME', 'qasp_repair_from_gt'
LANGUAGE C STRICT NOT FENCED;
