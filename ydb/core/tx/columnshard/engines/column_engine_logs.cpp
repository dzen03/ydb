#include "column_engine_logs.h"
#include "filter.h"

#include "changes/actualization/construction/context.h"
#include "changes/cleanup_portions.h"
#include "changes/cleanup_tables.h"
#include "changes/general_compaction.h"
#include "changes/indexation.h"
#include "changes/ttl.h"
#include "portions/constructor.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/tx/columnshard/columnshard_schema.h>
#include <ydb/core/tx/columnshard/columnshard_ttl.h>
#include <ydb/core/tx/columnshard/common/limits.h>
#include <ydb/core/tx/columnshard/data_locks/manager/manager.h>
#include <ydb/core/tx/columnshard/hooks/abstract/abstract.h>
#include <ydb/core/tx/tiering/manager.h>

#include <ydb/library/actors/core/monotonic_provider.h>
#include <ydb/library/conclusion/status.h>

#include <library/cpp/time_provider/time_provider.h>

#include <concepts>

namespace NKikimr::NOlap {

TColumnEngineForLogs::TColumnEngineForLogs(
    ui64 tabletId, const std::shared_ptr<IStoragesManager>& storagesManager, const TSnapshot& snapshot, const TSchemaInitializationData& schema)
    : GranulesStorage(std::make_shared<TGranulesStorage>(SignalCounters, storagesManager))
    , StoragesManager(storagesManager)
    , TabletId(tabletId)
    , LastPortion(0)
    , LastGranule(0) {
    ActualizationController = std::make_shared<NActualizer::TController>();
    RegisterSchemaVersion(snapshot, schema);
}

TColumnEngineForLogs::TColumnEngineForLogs(
    ui64 tabletId, const std::shared_ptr<IStoragesManager>& storagesManager, const TSnapshot& snapshot, TIndexInfo&& schema)
    : GranulesStorage(std::make_shared<TGranulesStorage>(SignalCounters, storagesManager))
    , StoragesManager(storagesManager)
    , TabletId(tabletId)
    , LastPortion(0)
    , LastGranule(0) {
    ActualizationController = std::make_shared<NActualizer::TController>();
    RegisterSchemaVersion(snapshot, std::move(schema));
}

const TMap<ui64, std::shared_ptr<TColumnEngineStats>>& TColumnEngineForLogs::GetStats() const {
    return PathStats;
}

const TColumnEngineStats& TColumnEngineForLogs::GetTotalStats() {
    Counters.Tables = GranulesStorage->GetTables().size();
    return Counters;
}

void TColumnEngineForLogs::UpdatePortionStats(const TPortionInfo& portionInfo, EStatsUpdateType updateType, const TPortionInfo* exPortionInfo) {
    if (IS_LOG_PRIORITY_ENABLED(NActors::NLog::PRI_DEBUG, NKikimrServices::TX_COLUMNSHARD)) {
        auto before = Counters.Active();
        UpdatePortionStats(Counters, portionInfo, updateType, exPortionInfo);
        auto after = Counters.Active();
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "portion_stats_updated")("type", updateType)("path_id", portionInfo.GetPathId())(
            "portion", portionInfo.GetPortionId())("before_size", before.Bytes)("after_size", after.Bytes)("before_rows", before.Rows)(
            "after_rows", after.Rows);
    } else {
        UpdatePortionStats(Counters, portionInfo, updateType, exPortionInfo);
    }
    const ui64 pathId = portionInfo.GetPathId();
    Y_ABORT_UNLESS(pathId);
    if (!PathStats.contains(pathId)) {
        auto& stats = PathStats[pathId];
        stats = std::make_shared<TColumnEngineStats>();
        stats->Tables = 1;
    }
    UpdatePortionStats(*PathStats[pathId], portionInfo, updateType, exPortionInfo);
}

TColumnEngineStats::TPortionsStats DeltaStats(const TPortionInfo& portionInfo) {
    TColumnEngineStats::TPortionsStats deltaStats;
    deltaStats.Bytes = 0;
    deltaStats.Rows = portionInfo.GetRecordsCount();
    deltaStats.Bytes = portionInfo.GetTotalBlobBytes();
    deltaStats.RawBytes = portionInfo.GetTotalRawBytes();
    deltaStats.Blobs = portionInfo.GetBlobIdsCount();
    deltaStats.Portions = 1;
    return deltaStats;
}

void TColumnEngineForLogs::UpdatePortionStats(
    TColumnEngineStats& engineStats, const TPortionInfo& portionInfo, EStatsUpdateType updateType, const TPortionInfo* exPortionInfo) const {
    TColumnEngineStats::TPortionsStats deltaStats = DeltaStats(portionInfo);

    Y_ABORT_UNLESS(!exPortionInfo || exPortionInfo->GetMeta().Produced != TPortionMeta::EProduced::UNSPECIFIED);
    Y_ABORT_UNLESS(portionInfo.GetMeta().Produced != TPortionMeta::EProduced::UNSPECIFIED);

    TColumnEngineStats::TPortionsStats& srcStats =
        exPortionInfo ? (exPortionInfo->HasRemoveSnapshot() ? engineStats.StatsByType[TPortionMeta::EProduced::INACTIVE]
                                                            : engineStats.StatsByType[exPortionInfo->GetMeta().Produced])
                      : engineStats.StatsByType[portionInfo.GetMeta().Produced];
    TColumnEngineStats::TPortionsStats& stats = portionInfo.HasRemoveSnapshot() ? engineStats.StatsByType[TPortionMeta::EProduced::INACTIVE]
                                                                                : engineStats.StatsByType[portionInfo.GetMeta().Produced];

    const bool isErase = updateType == EStatsUpdateType::ERASE;
    const bool isAdd = updateType == EStatsUpdateType::ADD;

    if (isErase) {   // PortionsToDrop
        stats -= deltaStats;
    } else if (isAdd) {   // Load || AppendedPortions
        stats += deltaStats;
    } else if (&srcStats != &stats || exPortionInfo) {   // SwitchedPortions || PortionsToEvict
        stats += deltaStats;

        if (exPortionInfo) {
            srcStats -= DeltaStats(*exPortionInfo);
        } else {
            srcStats -= deltaStats;
        }
    }
}

void TColumnEngineForLogs::RegisterSchemaVersion(const TSnapshot& snapshot, TIndexInfo&& indexInfo) {
    bool switchOptimizer = false;
    if (!VersionedIndex.IsEmpty()) {
        const NOlap::TIndexInfo& lastIndexInfo = VersionedIndex.GetLastSchema()->GetIndexInfo();
        Y_ABORT_UNLESS(lastIndexInfo.CheckCompatible(indexInfo));
        switchOptimizer = !indexInfo.GetCompactionPlannerConstructor()->IsEqualTo(lastIndexInfo.GetCompactionPlannerConstructor());
    }
    const bool isCriticalScheme = indexInfo.GetSchemeNeedActualization();
    auto* indexInfoActual = VersionedIndex.AddIndex(snapshot, std::move(indexInfo));
    if (isCriticalScheme) {
        if (!ActualizationStarted) {
            ActualizationStarted = true;
            for (auto&& i : GranulesStorage->GetTables()) {
                i.second->StartActualizationIndex();
            }
        }
        for (auto&& i : GranulesStorage->GetTables()) {
            i.second->RefreshScheme();
        }
    }
    if (switchOptimizer) {
        for (auto&& i : GranulesStorage->GetTables()) {
            i.second->ResetOptimizer(indexInfoActual->GetCompactionPlannerConstructor(), StoragesManager, indexInfoActual->GetPrimaryKey());
        }
    }
}

void TColumnEngineForLogs::RegisterSchemaVersion(const TSnapshot& snapshot, const TSchemaInitializationData& schema) {
    std::optional<NOlap::TIndexInfo> indexInfoOptional;
    if (schema.GetDiff()) {
        AFL_VERIFY(!VersionedIndex.IsEmpty());
        indexInfoOptional = NOlap::TIndexInfo::BuildFromProto(
            *schema.GetDiff(), VersionedIndex.GetLastSchema()->GetIndexInfo(), StoragesManager, SchemaObjectsCache);
    } else {
        indexInfoOptional = NOlap::TIndexInfo::BuildFromProto(schema.GetSchemaVerified(), StoragesManager, SchemaObjectsCache);
    }
    AFL_VERIFY(indexInfoOptional);
    RegisterSchemaVersion(snapshot, std::move(*indexInfoOptional));
}

bool TColumnEngineForLogs::Load(IDbWrapper& db) {
    Y_ABORT_UNLESS(!Loaded);
    Loaded = true;
    THashMap<ui64, ui64> granuleToPathIdDecoder;
    {
        TMemoryProfileGuard g("TTxInit/LoadShardingInfo");
        if (!VersionedIndex.LoadShardingInfo(db)) {
            return false;
        }
    }

    {
        auto guard = GranulesStorage->GetStats()->StartPackModification();
        if (!LoadColumns(db)) {
            return false;
        }
        TMemoryProfileGuard g("TTxInit/LoadCounters");
        if (!LoadCounters(db)) {
            return false;
        }
    }

    for (const auto& [pathId, spg] : GranulesStorage->GetTables()) {
        for (const auto& [_, portionInfo] : spg->GetPortions()) {
            UpdatePortionStats(*portionInfo, EStatsUpdateType::ADD);
            if (portionInfo->CheckForCleanup()) {
                AddCleanupPortion(portionInfo);
            }
        }
    }

    Y_ABORT_UNLESS(!(LastPortion >> 63), "near to int overflow");
    Y_ABORT_UNLESS(!(LastGranule >> 63), "near to int overflow");
    return true;
}

bool TColumnEngineForLogs::LoadColumns(IDbWrapper& db) {
    TPortionConstructors constructors;
    {
        NColumnShard::TLoadTimeSignals::TLoadTimer timer = SignalCounters.PortionsLoadingTimeCounters.StartGuard();
        TMemoryProfileGuard g("TTxInit/LoadColumns/Portions");
        if (!db.LoadPortions([&](TPortionInfoConstructor&& portion, const NKikimrTxColumnShard::TIndexPortionMeta& metaProto) {
                const TIndexInfo& indexInfo = portion.GetSchema(VersionedIndex)->GetIndexInfo();
                AFL_VERIFY(portion.MutableMeta().LoadMetadata(metaProto, indexInfo, db.GetDsGroupSelectorVerified()));
                AFL_VERIFY(constructors.AddConstructorVerified(std::move(portion)));
            })) {
            timer.AddLoadingFail();
            return false;
        }
    }

    {
        NColumnShard::TLoadTimeSignals::TLoadTimer timer = SignalCounters.ColumnsLoadingTimeCounters.StartGuard();
        TMemoryProfileGuard g("TTxInit/LoadColumns/Records");
        TPortionInfo::TSchemaCursor schema(VersionedIndex);
        if (!db.LoadColumns([&](const TColumnChunkLoadContextV1& loadContext) {
                auto* constructor = constructors.GetConstructorVerified(loadContext.GetPathId(), loadContext.GetPortionId());
                constructor->LoadRecord(loadContext);
            })) {
            timer.AddLoadingFail();
            return false;
        }
    }

    {
        NColumnShard::TLoadTimeSignals::TLoadTimeSignals::TLoadTimer timer = SignalCounters.IndexesLoadingTimeCounters.StartGuard();
        TMemoryProfileGuard g("TTxInit/LoadIndexes/Indexes");
        if (!db.LoadIndexes([&](const ui64 pathId, const ui64 portionId, const TIndexChunkLoadContext& loadContext) {
                auto* constructor = constructors.GetConstructorVerified(pathId, portionId);
                constructor->LoadIndex(loadContext);
            })) {
            timer.AddLoadingFail();
            return false;
        };
    }

    {
        TMemoryProfileGuard g("TTxInit/LoadColumns/Constructors");
        for (auto&& [granuleId, pathConstructors] : constructors) {
            auto g = GetGranulePtrVerified(granuleId);
            for (auto&& [portionId, constructor] : pathConstructors) {
                g->UpsertPortionOnLoad(constructor.Build(false).MutablePortionInfoPtr());
            }
        }
    }
    {
        TMemoryProfileGuard g("TTxInit/LoadColumns/After");
        for (auto&& i : GranulesStorage->GetTables()) {
            i.second->OnAfterPortionsLoad();
        }
    }
    return true;
}

bool TColumnEngineForLogs::LoadCounters(IDbWrapper& db) {
    auto callback = [&](ui32 id, ui64 value) {
        switch (id) {
            case LAST_PORTION:
                LastPortion = value;
                break;
            case LAST_GRANULE:
                LastGranule = value;
                break;
            case LAST_PLAN_STEP:
                LastSnapshot = TSnapshot(value, LastSnapshot.GetTxId());
                break;
            case LAST_TX_ID:
                LastSnapshot = TSnapshot(LastSnapshot.GetPlanStep(), value);
                break;
        }
    };

    return db.LoadCounters(callback);
}

std::shared_ptr<TInsertColumnEngineChanges> TColumnEngineForLogs::StartInsert(std::vector<TCommittedData>&& dataToIndex) noexcept {
    Y_ABORT_UNLESS(dataToIndex.size());

    TSaverContext saverContext(StoragesManager);
    auto changes = std::make_shared<TInsertColumnEngineChanges>(std::move(dataToIndex), saverContext);
    auto pkSchema = VersionedIndex.GetLastSchema()->GetIndexInfo().GetReplaceKey();

    for (const auto& data : changes->GetDataToIndex()) {
        const ui64 pathId = data.GetPathId();

        if (changes->PathToGranule.contains(pathId)) {
            continue;
        }
        if (!data.GetRemove()) {
            AFL_VERIFY(changes->PathToGranule.emplace(pathId, GetGranulePtrVerified(pathId)->GetBucketPositions()).second);
        }
    }

    return changes;
}

ui64 TColumnEngineForLogs::GetCompactionPriority(const std::shared_ptr<NDataLocks::TManager>& dataLocksManager, const std::set<ui64>& pathIds,
    const std::optional<ui64> waitingPriority) noexcept {
    auto priority = GranulesStorage->GetCompactionPriority(dataLocksManager, pathIds, waitingPriority);
    if (!priority) {
        return 0;
    } else {
        return priority->GetGeneralPriority();
    }
}

std::shared_ptr<TColumnEngineChanges> TColumnEngineForLogs::StartCompaction(
    const std::shared_ptr<NDataLocks::TManager>& dataLocksManager) noexcept {
    AFL_VERIFY(dataLocksManager);
    auto granule = GranulesStorage->GetGranuleForCompaction(dataLocksManager);
    if (!granule) {
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "no granules for start compaction");
        return nullptr;
    }
    granule->OnStartCompaction();
    auto changes = granule->GetOptimizationTask(granule, dataLocksManager);
    if (!changes) {
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "cannot build optimization task for granule that need compaction")(
            "weight", granule->GetCompactionPriority().DebugString());
    }
    return changes;
}

std::shared_ptr<TCleanupTablesColumnEngineChanges> TColumnEngineForLogs::StartCleanupTables(const THashSet<ui64>& pathsToDrop) noexcept {
    if (pathsToDrop.empty()) {
        return nullptr;
    }
    auto changes = std::make_shared<TCleanupTablesColumnEngineChanges>(StoragesManager);

    ui64 txSize = 0;
    const ui64 txSizeLimit = TGlobalLimits::TxWriteLimitBytes / 4;
    for (ui64 pathId : pathsToDrop) {
        if (!HasDataInPathId(pathId)) {
            changes->TablesToDrop.emplace(pathId);
        }
        txSize += 256;
        if (txSize > txSizeLimit) {
            break;
        }
    }
    if (changes->TablesToDrop.empty()) {
        return nullptr;
    }
    return changes;
}

std::shared_ptr<TCleanupPortionsColumnEngineChanges> TColumnEngineForLogs::StartCleanupPortions(
    const TSnapshot& snapshot, const THashSet<ui64>& pathsToDrop, const std::shared_ptr<NDataLocks::TManager>& dataLocksManager) noexcept {
    AFL_VERIFY(dataLocksManager);
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "StartCleanup")("portions_count", CleanupPortions.size());
    auto changes = std::make_shared<TCleanupPortionsColumnEngineChanges>(StoragesManager);

    // Add all portions from dropped paths
    ui64 txSize = 0;
    const ui64 txSizeLimit = TGlobalLimits::TxWriteLimitBytes / 4;
    ui32 skipLocked = 0;
    ui32 portionsFromDrop = 0;
    bool limitExceeded = false;
    for (ui64 pathId : pathsToDrop) {
        auto g = GranulesStorage->GetGranuleOptional(pathId);
        if (!g) {
            continue;
        }

        for (auto& [portion, info] : g->GetPortions()) {
            if (info->CheckForCleanup()) {
                continue;
            }
            if (dataLocksManager->IsLocked(*info)) {
                ++skipLocked;
                continue;
            }
            if (txSize + info->GetTxVolume() < txSizeLimit || changes->PortionsToDrop.empty()) {
                txSize += info->GetTxVolume();
            } else {
                limitExceeded = true;
                break;
            }
            changes->PortionsToDrop.push_back(TPortionDataAccessor(info));
            ++portionsFromDrop;
        }
    }

    const TInstant snapshotInstant = snapshot.GetPlanInstant();
    for (auto it = CleanupPortions.begin(); !limitExceeded && it != CleanupPortions.end();) {
        if (it->first > snapshotInstant) {
            AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "StartCleanupStop")("snapshot", snapshot.DebugString())(
                "current_snapshot_ts", it->first.MilliSeconds());
            break;
        }
        for (ui32 i = 0; i < it->second.size();) {
            if (dataLocksManager->IsLocked(it->second[i])) {
                ++skipLocked;
                ++i;
                continue;
            }
            AFL_VERIFY(it->second[i]->CheckForCleanup(snapshot))("p_snapshot", it->second[i]->GetRemoveSnapshotOptional())("snapshot", snapshot);
            if (txSize + it->second[i]->GetTxVolume() < txSizeLimit || changes->PortionsToDrop.empty()) {
                txSize += it->second[i]->GetTxVolume();
            } else {
                limitExceeded = true;
                break;
            }
            changes->PortionsToDrop.push_back(TPortionDataAccessor(it->second[i]));
            if (i + 1 < it->second.size()) {
                it->second[i] = std::move(it->second.back());
            }
            it->second.pop_back();
        }
        if (limitExceeded) {
            break;
        }
        if (it->second.empty()) {
            it = CleanupPortions.erase(it);
        } else {
            ++it;
        }
    }
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "StartCleanup")("portions_count", CleanupPortions.size())(
        "portions_prepared", changes->PortionsToDrop.size())("drop", portionsFromDrop)("skip", skipLocked);

    if (changes->PortionsToDrop.empty()) {
        return nullptr;
    }

    return changes;
}

std::vector<std::shared_ptr<TTTLColumnEngineChanges>> TColumnEngineForLogs::StartTtl(const THashMap<ui64, TTiering>& pathEviction,
    const std::shared_ptr<NDataLocks::TManager>& dataLocksManager, const ui64 memoryUsageLimit) noexcept {
    AFL_VERIFY(dataLocksManager);
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "StartTtl")("external", pathEviction.size());

    TSaverContext saverContext(StoragesManager);
    NActualizer::TTieringProcessContext context(memoryUsageLimit, saverContext, dataLocksManager, SignalCounters, ActualizationController);
    const TDuration actualizationLag = NYDBTest::TControllers::GetColumnShardController()->GetActualizationTasksLag();
    for (auto&& i : pathEviction) {
        auto g = GetGranuleOptional(i.first);
        if (g) {
            if (!ActualizationStarted) {
                g->StartActualizationIndex();
            }
            g->RefreshTiering(i.second);
            context.ResetActualInstantForTest();
            g->BuildActualizationTasks(context, actualizationLag);
        }
    }

    if (ActualizationStarted) {
        TLogContextGuard lGuard(TLogContextBuilder::Build()("queue", "ttl")("external_count", pathEviction.size()));
        for (auto&& i : GranulesStorage->GetTables()) {
            if (pathEviction.contains(i.first)) {
                continue;
            }
            i.second->BuildActualizationTasks(context, actualizationLag);
        }
    } else {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "StartTtl")("skip", "not_ready_tiers");
    }
    std::vector<std::shared_ptr<TTTLColumnEngineChanges>> result;
    for (auto&& i : context.GetTasks()) {
        for (auto&& t : i.second) {
            SignalCounters.OnActualizationTask(t.GetTask()->GetPortionsToEvictCount(), t.GetTask()->GetPortionsToRemoveSize());
            result.emplace_back(t.GetTask());
        }
    }
    return result;
}

bool TColumnEngineForLogs::ApplyChangesOnTxCreate(std::shared_ptr<TColumnEngineChanges> indexChanges, const TSnapshot& snapshot) noexcept {
    TFinalizationContext context(LastGranule, LastPortion, snapshot);
    indexChanges->Compile(context);
    return true;
}

bool TColumnEngineForLogs::ApplyChangesOnExecute(
    IDbWrapper& db, std::shared_ptr<TColumnEngineChanges> /*indexChanges*/, const TSnapshot& snapshot) noexcept {
    db.WriteCounter(LAST_PORTION, LastPortion);
    db.WriteCounter(LAST_GRANULE, LastGranule);

    if (LastSnapshot < snapshot) {
        LastSnapshot = snapshot;
        db.WriteCounter(LAST_PLAN_STEP, LastSnapshot.GetPlanStep());
        db.WriteCounter(LAST_TX_ID, LastSnapshot.GetTxId());
    }
    return true;
}

void TColumnEngineForLogs::AppendPortion(const TPortionInfo::TPtr& portionInfo) {
    auto granule = GetGranulePtrVerified(portionInfo->GetPathId());
    AFL_VERIFY(!granule->GetPortionOptional(portionInfo->GetPortionId()));
    UpdatePortionStats(*portionInfo, EStatsUpdateType::ADD);
    granule->AppendPortion(portionInfo);
}

bool TColumnEngineForLogs::ErasePortion(const TPortionInfo& portionInfo, bool updateStats) {
    const ui64 portion = portionInfo.GetPortionId();
    auto& spg = MutableGranuleVerified(portionInfo.GetPathId());
    auto p = spg.GetPortionOptional(portion);

    if (!p) {
        LOG_S_WARN("Portion erased already " << portionInfo << " at tablet " << TabletId);
        return false;
    } else {
        if (updateStats) {
            UpdatePortionStats(*p, EStatsUpdateType::ERASE);
        }
        Y_ABORT_UNLESS(spg.ErasePortion(portion));
        return true;
    }
}

std::shared_ptr<TSelectInfo> TColumnEngineForLogs::Select(
    ui64 pathId, TSnapshot snapshot, const TPKRangesFilter& pkRangesFilter, const bool withUncommitted) const {
    auto out = std::make_shared<TSelectInfo>();
    auto spg = GranulesStorage->GetGranuleOptional(pathId);
    if (!spg) {
        return out;
    }

    if (withUncommitted) {
        for (const auto& [_, portionInfo] : spg->GetInsertedPortions()) {
            AFL_VERIFY(portionInfo->HasInsertWriteId());
            AFL_VERIFY(!portionInfo->HasCommitSnapshot());
            const bool skipPortion = !pkRangesFilter.IsPortionInUsage(*portionInfo);
            AFL_TRACE(NKikimrServices::TX_COLUMNSHARD_SCAN)("event", skipPortion ? "portion_skipped" : "portion_selected")("pathId", pathId)(
                "portion", portionInfo->DebugString());
            if (skipPortion) {
                continue;
            }
            out->PortionsOrderedPK.emplace_back(portionInfo);
        }
    }
    for (const auto& [_, portionInfo] : spg->GetPortions()) {
        if (!portionInfo->IsVisible(snapshot, !withUncommitted)) {
            continue;
        }
        const bool skipPortion = !pkRangesFilter.IsPortionInUsage(*portionInfo);
        AFL_TRACE(NKikimrServices::TX_COLUMNSHARD_SCAN)("event", skipPortion ? "portion_skipped" : "portion_selected")("pathId", pathId)(
            "portion", portionInfo->DebugString());
        if (skipPortion) {
            continue;
        }
        out->PortionsOrderedPK.emplace_back(portionInfo);
    }

    return out;
}

void TColumnEngineForLogs::OnTieringModified(
    const std::shared_ptr<NColumnShard::TTiersManager>& manager, const NColumnShard::TTtl& ttl, const std::optional<ui64> pathId) {
    if (!ActualizationStarted) {
        for (auto&& i : GranulesStorage->GetTables()) {
            i.second->StartActualizationIndex();
        }
    }

    ActualizationStarted = true;
    AFL_VERIFY(manager);
    THashMap<ui64, TTiering> tierings = manager->GetTiering();
    ttl.AddTtls(tierings);

    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "OnTieringModified")("new_count_tierings", tierings.size())(
        "new_count_ttls", ttl.PathsCount());
    // some string

    if (pathId) {
        auto g = GetGranulePtrVerified(*pathId);
        auto it = tierings.find(*pathId);
        if (it == tierings.end()) {
            g->RefreshTiering({});
        } else {
            g->RefreshTiering(it->second);
        }
    } else {
        for (auto&& [gPathId, g] : GranulesStorage->GetTables()) {
            auto it = tierings.find(gPathId);
            if (it == tierings.end()) {
                g->RefreshTiering({});
            } else {
                g->RefreshTiering(it->second);
            }
        }
    }
}

void TColumnEngineForLogs::DoRegisterTable(const ui64 pathId) {
    std::shared_ptr<TGranuleMeta> g = GranulesStorage->RegisterTable(pathId, SignalCounters.RegisterGranuleDataCounters(), VersionedIndex);
    if (ActualizationStarted) {
        g->StartActualizationIndex();
        g->RefreshScheme();
    }
}

}   // namespace NKikimr::NOlap
