ALTER TYPE citus.distribution_type ADD VALUE 'rtree';

CREATE FUNCTION pg_catalog.create_spatial_distributed_table(table_name regclass,
										 distribution_column text,
										 distribution_type citus.distribution_type,
										 packing_method text)
    RETURNS void
    LANGUAGE C STRICT
    AS 'MODULE_PATHNAME', $$create_spatial_distributed_table$$;
COMMENT ON FUNCTION pg_catalog.create_spatial_distributed_table(table_name regclass,
                    										 distribution_column text,
                    										 distribution_type citus.distribution_type,
                    										 packing_method text)
  IS 'Creates a spatial distributed table';
REVOKE ALL ON FUNCTION pg_catalog.create_spatial_distributed_table(regclass,text,citus.distribution_type,text) FROM PUBLIC;