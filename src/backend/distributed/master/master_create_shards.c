/*-------------------------------------------------------------------------
 *
 * master_create_shards.c
 *
 * This file contains functions to distribute a table by creating shards for it
 * across a set of worker nodes.
 *
 * Copyright (c) 2014-2016, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"
#include "fmgr.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "port.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/errno.h>

#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "distributed/connection_cache.h"
#include "distributed/listutils.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/master_protocol.h"
#include "distributed/multi_join_order.h"
#include "distributed/pg_dist_partition.h"
#include "distributed/pg_dist_shard.h"
#include "distributed/resource_lock.h"
#include "distributed/shardinterval_utils.h"
#include "distributed/worker_manager.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"


/* local function forward declarations */
static text * IntegerToText(int32 value);


/* declarations for dynamic loading */
PG_FUNCTION_INFO_V1(master_create_worker_shards);


/*
 * master_create_worker_shards is a user facing function to create worker shards
 * for the given relation in round robin order.
 */
Datum
master_create_worker_shards(PG_FUNCTION_ARGS)
{
	text *tableNameText = PG_GETARG_TEXT_P(0);
	int32 shardCount = PG_GETARG_INT32(1);
	int32 replicationFactor = PG_GETARG_INT32(2);

	Oid distributedTableId = ResolveRelationId(tableNameText);
	CreateShardsWithRoundRobinPolicy(distributedTableId, shardCount, replicationFactor);

	PG_RETURN_VOID();
}


/*
 * CreateShardsWithRoundRobinPolicy creates empty shards for the given table
 * based on the specified number of initial shards. The function first gets a
 * list of candidate nodes and issues DDL commands on the nodes to create empty
 * shard placements on those nodes. The function then updates metadata on the
 * master node to make this shard (and its placements) visible. Note that the
 * function assumes the table is hash partitioned and calculates the min/max
 * hash token ranges for each shard, giving them an equal split of the hash space.
 */
void
CreateShardsWithRoundRobinPolicy(Oid distributedTableId, int32 shardCount,
								 int32 replicationFactor)
{
	char *relationOwner = NULL;
	char shardStorageType = 0;
	List *workerNodeList = NIL;
	List *ddlCommandList = NIL;
	int32 workerNodeCount = 0;
	uint32 placementAttemptCount = 0;
	uint64 hashTokenIncrement = 0;
	List *existingShardList = NIL;
	int64 shardIndex = 0;

	/* make sure table is hash partitioned */
	CheckHashPartitionedTable(distributedTableId);

	/*
	 * In contrast to append/range partitioned tables it makes more sense to
	 * require ownership privileges - shards for hash-partitioned tables are
	 * only created once, not continually during ingest as for the other
	 * partitioning types.
	 */
	EnsureTableOwner(distributedTableId);

	/* we plan to add shards: get an exclusive metadata lock */
	LockRelationDistributionMetadata(distributedTableId, ExclusiveLock);

	relationOwner = TableOwner(distributedTableId);

	/* validate that shards haven't already been created for this table */
	existingShardList = LoadShardList(distributedTableId);
	if (existingShardList != NIL)
	{
		char *tableName = get_rel_name(distributedTableId);
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("table \"%s\" has already had shards created for it",
							   tableName)));
	}

	/* make sure that at least one shard is specified */
	if (shardCount <= 0)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("shard_count must be positive")));
	}

	/* make sure that at least one replica is specified */
	if (replicationFactor <= 0)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("replication_factor must be positive")));
	}

	/* calculate the split of the hash space */
	hashTokenIncrement = HASH_TOKEN_COUNT / shardCount;

	/* load and sort the worker node list for deterministic placement */
	workerNodeList = WorkerNodeList();
	workerNodeList = SortList(workerNodeList, CompareWorkerNodes);

	/* make sure we don't process cancel signals until all shards are created */
	HOLD_INTERRUPTS();

	/* retrieve the DDL commands for the table */
	ddlCommandList = GetTableDDLEvents(distributedTableId);

	workerNodeCount = list_length(workerNodeList);
	if (replicationFactor > workerNodeCount)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("replication_factor (%d) exceeds number of worker nodes "
							   "(%d)", replicationFactor, workerNodeCount),
						errhint("Add more worker nodes or try again with a lower "
								"replication factor.")));
	}

	/* if we have enough nodes, add an extra placement attempt for backup */
	placementAttemptCount = (uint32) replicationFactor;
	if (workerNodeCount > replicationFactor)
	{
		placementAttemptCount++;
	}

	/* set shard storage type according to relation type */
	shardStorageType = ShardStorageType(distributedTableId);

	for (shardIndex = 0; shardIndex < shardCount; shardIndex++)
	{
		uint32 roundRobinNodeIndex = shardIndex % workerNodeCount;

		/* initialize the hash token space for this shard */
		text *minHashTokenText = NULL;
		text *maxHashTokenText = NULL;
		int32 shardMinHashToken = INT32_MIN + (shardIndex * hashTokenIncrement);
		int32 shardMaxHashToken = shardMinHashToken + (hashTokenIncrement - 1);
		uint64 shardId = GetNextShardId();

		/* if we are at the last shard, make sure the max token value is INT_MAX */
		if (shardIndex == (shardCount - 1))
		{
			shardMaxHashToken = INT32_MAX;
		}

		/* insert the shard metadata row along with its min/max values */
		minHashTokenText = IntegerToText(shardMinHashToken);
		maxHashTokenText = IntegerToText(shardMaxHashToken);

		/*
		 * Grabbing the shard metadata lock isn't technically necessary since
		 * we already hold an exclusive lock on the partition table, but we'll
		 * acquire it for the sake of completeness. As we're adding new active
		 * placements, the mode must be exclusive.
		 */
		LockShardDistributionMetadata(shardId, ExclusiveLock);

		CreateShardPlacements(distributedTableId, shardId, ddlCommandList, relationOwner,
							  workerNodeList, roundRobinNodeIndex, replicationFactor);

		InsertShardRow(distributedTableId, shardId, shardStorageType,
					   minHashTokenText, maxHashTokenText);
	}

	if (QueryCancelPending)
	{
		ereport(WARNING, (errmsg("cancel requests are ignored during shard creation")));
		QueryCancelPending = false;
	}

	RESUME_INTERRUPTS();
}


/*
 * CreateColocatedShards creates shards for the target relation colocated with
 * the source relation.
 */
void
CreateColocatedShards(Oid targetRelationId, Oid sourceRelationId)
{
	char *targetTableRelationOwner = NULL;
	char targetShardStorageType = 0;
	List *existingShardList = NIL;
	List *sourceShardIntervalList = NIL;
	List *targetTableDDLEvents = NIL;
	List *targetTableForeignConstraintCommands = NIL;
	ListCell *sourceShardCell = NULL;

	/* make sure that tables are hash partitioned */
	CheckHashPartitionedTable(targetRelationId);
	CheckHashPartitionedTable(sourceRelationId);

	/*
	 * In contrast to append/range partitioned tables it makes more sense to
	 * require ownership privileges - shards for hash-partitioned tables are
	 * only created once, not continually during ingest as for the other
	 * partitioning types.
	 */
	EnsureTableOwner(targetRelationId);

	/* we plan to add shards: get an exclusive metadata lock on the target relation */
	LockRelationDistributionMetadata(targetRelationId, ExclusiveLock);

	/* we don't want source table to get dropped before we colocate with it */
	LockRelationOid(sourceRelationId, AccessShareLock);

	/* prevent placement changes of the source relation until we colocate with them */
	sourceShardIntervalList = LoadShardIntervalList(sourceRelationId);
	LockShardListMetadata(sourceShardIntervalList, ShareLock);

	/* validate that shards haven't already been created for this table */
	existingShardList = LoadShardList(targetRelationId);
	if (existingShardList != NIL)
	{
		char *targetRelationName = get_rel_name(targetRelationId);
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("table \"%s\" has already had shards created for it",
							   targetRelationName)));
	}

	targetTableRelationOwner = TableOwner(targetRelationId);
	targetTableDDLEvents = GetTableDDLEvents(targetRelationId);
	targetTableForeignConstraintCommands = GetTableForeignConstraintCommands(
		targetRelationId);
	targetShardStorageType = ShardStorageType(targetRelationId);

	foreach(sourceShardCell, sourceShardIntervalList)
	{
		ShardInterval *sourceShardInterval = (ShardInterval *) lfirst(sourceShardCell);
		uint64 sourceShardId = sourceShardInterval->shardId;
		uint64 newShardId = GetNextShardId();
		ListCell *sourceShardPlacementCell = NULL;
		int sourceShardIndex = ShardIndex(sourceShardInterval);

		int32 shardMinValue = DatumGetInt32(sourceShardInterval->minValue);
		int32 shardMaxValue = DatumGetInt32(sourceShardInterval->maxValue);
		text *shardMinValueText = IntegerToText(shardMinValue);
		text *shardMaxValueText = IntegerToText(shardMaxValue);

		List *sourceShardPlacementList = ShardPlacementList(sourceShardId);
		foreach(sourceShardPlacementCell, sourceShardPlacementList)
		{
			ShardPlacement *sourcePlacement =
				(ShardPlacement *) lfirst(sourceShardPlacementCell);
			char *sourceNodeName = sourcePlacement->nodeName;
			int32 sourceNodePort = sourcePlacement->nodePort;

			bool created = WorkerCreateShard(targetRelationId, sourceNodeName,
											 sourceNodePort, sourceShardIndex, newShardId,
											 targetTableRelationOwner,
											 targetTableDDLEvents,
											 targetTableForeignConstraintCommands);
			if (created)
			{
				const RelayFileState shardState = FILE_FINALIZED;
				const uint64 shardSize = 0;

				InsertShardPlacementRow(newShardId, INVALID_PLACEMENT_ID, shardState,
										shardSize, sourceNodeName, sourceNodePort);
			}
			else
			{
				char *targetRelationName = get_rel_name(targetRelationId);
				char *sourceRelationName = get_rel_name(sourceRelationId);
				ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								errmsg("table \"%s\" could not be colocated with %s",
									   targetRelationName, sourceRelationName)));
			}
		}

		InsertShardRow(targetRelationId, newShardId, targetShardStorageType,
					   shardMinValueText, shardMaxValueText);
	}
}


/*
 * CreateReferenceTableShard creates a single shard for the given
 * distributedTableId. The created shard does not have min/max values.
 * Also, the shard is replicated to the all active nodes in the cluster.
 */
void
CreateReferenceTableShard(Oid distributedTableId)
{
	char *relationOwner = NULL;
	char shardStorageType = 0;
	List *workerNodeList = NIL;
	List *ddlCommandList = NIL;
	int32 workerNodeCount = 0;
	List *existingShardList = NIL;
	uint64 shardId = INVALID_SHARD_ID;
	int workerStartIndex = 0;
	int replicationFactor = 0;
	text *shardMinValue = NULL;
	text *shardMaxValue = NULL;

	/*
	 * In contrast to append/range partitioned tables it makes more sense to
	 * require ownership privileges - shards for reference tables are
	 * only created once, not continually during ingest as for the other
	 * partitioning types such as append and range.
	 */
	EnsureTableOwner(distributedTableId);

	/* we plan to add shards: get an exclusive metadata lock */
	LockRelationDistributionMetadata(distributedTableId, ExclusiveLock);

	relationOwner = TableOwner(distributedTableId);

	/* set shard storage type according to relation type */
	shardStorageType = ShardStorageType(distributedTableId);

	/* validate that shards haven't already been created for this table */
	existingShardList = LoadShardList(distributedTableId);
	if (existingShardList != NIL)
	{
		char *tableName = get_rel_name(distributedTableId);
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("table \"%s\" has already had shards created for it",
							   tableName)));
	}

	/* load and sort the worker node list for deterministic placement */
	workerNodeList = WorkerNodeList();
	workerNodeList = SortList(workerNodeList, CompareWorkerNodes);

	/* get the next shard id */
	shardId = GetNextShardId();

	/* retrieve the DDL commands for the table */
	ddlCommandList = GetTableDDLEvents(distributedTableId);

	/* set the replication factor equal to the number of worker nodes */
	workerNodeCount = list_length(workerNodeList);
	replicationFactor = workerNodeCount;

	/*
	 * Grabbing the shard metadata lock isn't technically necessary since
	 * we already hold an exclusive lock on the partition table, but we'll
	 * acquire it for the sake of completeness. As we're adding new active
	 * placements, the mode must be exclusive.
	 */
	LockShardDistributionMetadata(shardId, ExclusiveLock);

	CreateShardPlacements(distributedTableId, shardId, ddlCommandList, relationOwner,
						  workerNodeList, workerStartIndex, replicationFactor);

	InsertShardRow(distributedTableId, shardId, shardStorageType, shardMinValue,
				   shardMaxValue);
}


/*
 * CheckHashPartitionedTable looks up the partition information for the given
 * tableId and checks if the table is hash partitioned. If not, the function
 * throws an error.
 */
void
CheckHashPartitionedTable(Oid distributedTableId)
{
	char partitionType = PartitionMethod(distributedTableId);
	if (partitionType != DISTRIBUTE_BY_HASH)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("unsupported table partition type: %c", partitionType)));
	}
}


/* Helper function to convert an integer value to a text type */
static text *
IntegerToText(int32 value)
{
	text *valueText = NULL;
	StringInfo valueString = makeStringInfo();
	appendStringInfo(valueString, "%d", value);

	valueText = cstring_to_text(valueString->data);

	return valueText;
}
