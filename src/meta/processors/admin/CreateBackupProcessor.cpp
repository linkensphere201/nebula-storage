/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "meta/processors/admin/CreateBackupProcessor.h"
#include "meta/ActiveHostsMan.h"
#include "meta/processors/admin/SnapShot.h"

namespace nebula {
namespace meta {

ErrorOr<cpp2::ErrorCode, std::unordered_set<GraphSpaceID>>
CreateBackupProcessor::spaceNameToId(
    const std::vector<std::string>* backupSpaces) {
    folly::SharedMutex::ReadHolder rHolder(LockUtils::spaceLock());
    std::unordered_set<GraphSpaceID> spaces;

    if (backupSpaces != nullptr) {
        DCHECK(!backupSpaces->empty());
        std::vector<std::string> keys;
        keys.reserve(backupSpaces->size());

        std::transform(
            backupSpaces->begin(), backupSpaces->end(), std::back_inserter(keys), [](auto& name) {
                return MetaServiceUtils::indexSpaceKey(name);
            });

        auto result = doMultiGet(std::move(keys));
        if (!nebula::ok(result)) {
            auto retCode = nebula::error(result);
            LOG(ERROR) << "MultiGet space failed, error: "
                       << apache::thrift::util::enumNameSafe(retCode);
            return retCode;
        }

        auto values = std::move(nebula::value(result));
        std::transform(std::make_move_iterator(values.begin()),
                       std::make_move_iterator(values.end()),
                       std::inserter(spaces, spaces.end()),
                       [](const std::string& rawID) {
                           return *reinterpret_cast<const GraphSpaceID*>(rawID.c_str());
                       });

    } else {
        const auto& prefix = MetaServiceUtils::spacePrefix();
        auto iterRet = doPrefix(prefix);
        if (!nebula::ok(iterRet)) {
            auto retCode = nebula::error(iterRet);
            LOG(ERROR) << "Space prefix failed, error: "
                       << apache::thrift::util::enumNameSafe(retCode);
            return retCode;
        }

        auto iter = nebula::value(iterRet).get();
        while (iter->valid()) {
            auto spaceId = MetaServiceUtils::spaceId(iter->key());
            auto spaceName = MetaServiceUtils::spaceName(iter->val());
            VLOG(3) << "List spaces " << spaceId << ", name " << spaceName;
            spaces.emplace(spaceId);
            iter->next();
        }
    }

    if (spaces.empty()) {
        LOG(ERROR) << "Failed to create a full backup because there is currently no space.";
        return cpp2::ErrorCode::E_BACKUP_SPACE_NOT_FOUND;
    }

    return spaces;
}

ErrorOr<cpp2::ErrorCode, bool> CreateBackupProcessor::isIndexRebuilding() {
    folly::SharedMutex::ReadHolder rHolder(LockUtils::spaceLock());
    const auto& prefix = MetaServiceUtils::rebuildIndexStatusPrefix();
    auto ret = doPrefix(prefix);
    if (!nebula::ok(ret)) {
        auto retCode = nebula::error(ret);
        LOG(ERROR) << "Prefix index rebuilding state failed, result code: "
                   << static_cast<int32_t>(retCode);;
        return retCode;
    }

    auto iter = nebula::value(ret).get();
    while (iter->valid()) {
        if (iter->val() == "RUNNING") {
            return true;
        }
        iter->next();
    }

    return false;
}

void CreateBackupProcessor::process(const cpp2::CreateBackupReq& req) {
    auto* backupSpaces = req.get_spaces();
    auto* store = static_cast<kvstore::NebulaStore*>(kvstore_);
    if (!store->isLeader(kDefaultSpaceId, kDefaultPartId)) {
        handleErrorCode(cpp2::ErrorCode::E_LEADER_CHANGED);
        onFinished();
        return;
    }

    auto result = isIndexRebuilding();
    if (!nebula::ok(result)) {
        handleErrorCode(nebula::error(result));
        onFinished();
        return;
    }

    if (nebula::value(result)) {
        LOG(ERROR) << "Index is rebuilding, not allowed to create backup.";
        handleErrorCode(cpp2::ErrorCode::E_BACKUP_BUILDING_INDEX);
        onFinished();
        return;
    }

    folly::SharedMutex::WriteHolder wHolder(LockUtils::snapshotLock());

    auto activeHostsRet = ActiveHostsMan::getActiveHosts(kvstore_);
    if (!nebula::ok(activeHostsRet)) {
        handleErrorCode(nebula::error(activeHostsRet));
        onFinished();
        return;
    }
    auto hosts = std::move(nebula::value(activeHostsRet));

    if (hosts.empty()) {
        LOG(ERROR) << "There has some offline hosts";
        handleErrorCode(cpp2::ErrorCode::E_NO_HOSTS);
        onFinished();
        return;
    }

    auto spaceIdRet = spaceNameToId(backupSpaces);
    if (!nebula::ok(spaceIdRet)) {
        handleErrorCode(nebula::error(spaceIdRet));
        onFinished();
        return;
    }

    auto spaces = nebula::value(spaceIdRet);

    // The entire process follows mostly snapshot logic.
    std::vector<kvstore::KV> data;
    auto backupName = folly::format("BACKUP_{}", MetaServiceUtils::genTimestampStr()).str();

    data.emplace_back(MetaServiceUtils::snapshotKey(backupName),
                      MetaServiceUtils::snapshotVal(cpp2::SnapshotStatus::INVALID,
                                                    NetworkUtils::toHostsStr(hosts)));
    Snapshot::instance(kvstore_, client_)->setSpaces(spaces);

    // step 1 : Blocking all writes action for storage engines.
    auto ret = Snapshot::instance(kvstore_, client_)->blockingWrites(SignType::BLOCK_ON);
    if (ret != cpp2::ErrorCode::SUCCEEDED) {
        LOG(ERROR) << "Send blocking sign to storage engine error";
        handleErrorCode(ret);
        ret = Snapshot::instance(kvstore_, client_)->blockingWrites(SignType::BLOCK_OFF);
        if (ret != cpp2::ErrorCode::SUCCEEDED) {
            LOG(ERROR) << "Cancel write blocking error";
        }
        onFinished();
        return;
    }

    // step 3 : Create checkpoint for all storage engines.
    auto sret = Snapshot::instance(kvstore_, client_)->createSnapshot(backupName);
    if (!nebula::ok(sret)) {
        LOG(ERROR) << "Checkpoint create error on storage engine";
        handleErrorCode(nebula::error(sret));
        ret = Snapshot::instance(kvstore_, client_)->blockingWrites(SignType::BLOCK_OFF);
        if (ret != cpp2::ErrorCode::SUCCEEDED) {
            LOG(ERROR) << "Cancel write blocking error";
        }
        onFinished();
        return;
    }

    // step 4 created backup for meta(export sst).
    auto backupFiles = MetaServiceUtils::backup(kvstore_, spaces, backupName, backupSpaces);
    if (!backupFiles.hasValue()) {
        LOG(ERROR) << "Failed backup meta";
        handleErrorCode(cpp2::ErrorCode::E_BACKUP_FAILURE);
        onFinished();
        return;
    }

    // step 5 : checkpoint created done, so release the write blocking.
    ret = Snapshot::instance(kvstore_, client_)->blockingWrites(SignType::BLOCK_OFF);
    if (ret != cpp2::ErrorCode::SUCCEEDED) {
        LOG(ERROR) << "Cancel write blocking error";
        handleErrorCode(ret);
        onFinished();
        return;
    }

    // step 6 : update snapshot status from INVALID to VALID.
    data.emplace_back(MetaServiceUtils::snapshotKey(backupName),
                      MetaServiceUtils::snapshotVal(cpp2::SnapshotStatus::VALID,
                                                    NetworkUtils::toHostsStr(hosts)));

    auto putRet = doSyncPut(std::move(data));
    if (putRet != cpp2::ErrorCode::SUCCEEDED) {
        LOG(ERROR) << "All checkpoint creations are done, "
                      "but update checkpoint status error. "
                      "backup : "
                   << backupName;
        handleErrorCode(putRet);
        onFinished();
        return;
    }

    std::unordered_map<GraphSpaceID, cpp2::SpaceBackupInfo> backupInfo;

    // set backup info
    auto snapshotInfo = std::move(nebula::value(sret));
    for (auto id : spaces) {
        LOG(INFO) << "backup space " << id;
        cpp2::SpaceBackupInfo spaceInfo;
        auto spaceKey = MetaServiceUtils::spaceKey(id);
        auto spaceRet = doGet(spaceKey);
        if (!nebula::ok(spaceRet)) {
            handleErrorCode(nebula::error(spaceRet));
            onFinished();
            return;
        }
        auto properties = MetaServiceUtils::parseSpace(nebula::value(spaceRet));
        // todo we should save partition info.
        auto it = snapshotInfo.find(id);
        DCHECK(it != snapshotInfo.end());
        spaceInfo.set_cp_dirs(it->second);
        spaceInfo.set_space(std::move(properties));
        backupInfo.emplace(id, std::move(spaceInfo));
    }
    cpp2::BackupMeta backup;
    LOG(INFO) << "sst files count was:" << backupFiles.value().size();
    backup.set_meta_files(std::move(backupFiles.value()));
    backup.set_backup_info(std::move(backupInfo));
    backup.set_backup_name(std::move(backupName));

    handleErrorCode(cpp2::ErrorCode::SUCCEEDED);
    resp_.set_meta(std::move(backup));
    LOG(INFO) << "backup done";

    onFinished();
}

}   // namespace meta
}   // namespace nebula
