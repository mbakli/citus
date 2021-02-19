ALTER TYPE citus.distribution_type ADD VALUE 'md_tiling';

CREATE FUNCTION pg_catalog.create_distributed_table(table_name regclass,
										 distribution_column text,
										 distribution_type citus.distribution_type,
										 extent text,
										 colocate_with text DEFAULT 'default')
    RETURNS void
    LANGUAGE C STRICT
    AS 'MODULE_PATHNAME', $$create_spatiotemporal_distributed_table$$;
COMMENT ON FUNCTION pg_catalog.create_distributed_table(table_name regclass,
                    										 distribution_column text,
                    										 distribution_type citus.distribution_type,
                    										 extent text,
                    										 colocate_with text)
  IS 'Creates a spatial distributed table';