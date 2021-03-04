-- Spatial distribution types
ALTER TYPE citus.distribution_type ADD VALUE 'md_tiling';

-- shards extent
CREATE TABLE public.pg_dist_shard_spatial(LIKE pg_dist_shard INCLUDING ALL);
ALTER TABLE public.pg_dist_shard_spatial DROP COLUMN shardminvalue, DROP COLUMN shardmaxvalue, ADD COLUMN bbox BOX;
CREATE INDEX pg_dist_shard_spatial_bbox_gist ON public.pg_dist_shard_spatial USING GIST(bbox);


CREATE FUNCTION public.create_distributed_table(table_name regclass,
										 distribution_column text,
										 distribution_type citus.distribution_type,
										 extent text,
										 colocate_with text DEFAULT 'default')
    RETURNS void
    LANGUAGE C STRICT
    AS 'MODULE_PATHNAME', $$create_spatiotemporal_distributed_table$$;
COMMENT ON FUNCTION public.create_distributed_table(table_name regclass,
                    										 distribution_column text,
                    										 distribution_type citus.distribution_type,
                    										 extent text,
                    										 colocate_with text)
  IS 'Creates a spatial distributed table';