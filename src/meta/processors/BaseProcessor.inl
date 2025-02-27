/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "meta/processors/BaseProcessor.h"

namespace nebula {
namespace meta {

template<typename RESP>
void BaseProcessor<RESP>::doPut(std::vector<kvstore::KV> data) {
    folly::Baton<true, std::atomic> baton;
    kvstore_->asyncMultiPut(kDefaultSpaceId,
                            kDefaultPartId,
                            std::move(data),
                            [this, &baton] (kvstore::ResultCode code) {
        this->handleErrorCode(MetaCommon::to(code));
        baton.post();
    });
    baton.wait();
    this->onFinished();
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, std::unique_ptr<kvstore::KVIterator>>
BaseProcessor<RESP>::doPrefix(const std::string& key) {
    std::unique_ptr<kvstore::KVIterator> iter;
    auto code = kvstore_->prefix(kDefaultSpaceId, kDefaultPartId, key, &iter);
    if (code != kvstore::ResultCode::SUCCEEDED) {
        VLOG(2) << "Prefix Failed";
        return MetaCommon::to(code);
    }
    return iter;
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, std::string>
BaseProcessor<RESP>::doGet(const std::string& key) {
    std::string value;
    auto code = kvstore_->get(kDefaultSpaceId, kDefaultPartId, key, &value);
    if (code != kvstore::ResultCode::SUCCEEDED) {
        VLOG(2) << "Get Failed";
        return MetaCommon::to(code);
    }
    return value;
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, std::vector<std::string>>
BaseProcessor<RESP>::doMultiGet(const std::vector<std::string>& keys) {
    std::vector<std::string> values;
    auto ret = kvstore_->multiGet(kDefaultSpaceId, kDefaultPartId, keys, &values);
    auto code = ret.first;
    if (code != kvstore::ResultCode::SUCCEEDED) {
        VLOG(2) << "MultiGet Failed";
        return MetaCommon::to(code);
    }
    return values;
}


template<typename RESP>
void BaseProcessor<RESP>::doRemove(const std::string& key) {
    folly::Baton<true, std::atomic> baton;
    kvstore_->asyncRemove(kDefaultSpaceId,
                          kDefaultPartId,
                          key,
                          [this, &baton] (kvstore::ResultCode code) {
        this->handleErrorCode(MetaCommon::to(code));
        baton.post();
    });
    baton.wait();
    this->onFinished();
}


template<typename RESP>
void BaseProcessor<RESP>::doMultiRemove(std::vector<std::string> keys) {
    folly::Baton<true, std::atomic> baton;
    kvstore_->asyncMultiRemove(kDefaultSpaceId,
                               kDefaultPartId,
                               std::move(keys),
                               [this, &baton] (kvstore::ResultCode code) {
        this->handleErrorCode(MetaCommon::to(code));
        baton.post();
    });
    baton.wait();
    this->onFinished();
}


template<typename RESP>
void BaseProcessor<RESP>::doRemoveRange(const std::string& start,
                                        const std::string& end) {
    folly::Baton<true, std::atomic> baton;
    kvstore_->asyncRemoveRange(kDefaultSpaceId,
                               kDefaultPartId,
                               start,
                               end,
                               [this, &baton] (kvstore::ResultCode code) {
        this->handleErrorCode(MetaCommon::to(code));
        baton.post();
    });
    baton.wait();
    this->onFinished();
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, std::vector<std::string>>
BaseProcessor<RESP>::doScan(const std::string& start, const std::string& end) {
    std::unique_ptr<kvstore::KVIterator> iter;
    auto code = kvstore_->range(kDefaultSpaceId, kDefaultPartId, start, end, &iter);
    if (code != kvstore::ResultCode::SUCCEEDED) {
        VLOG(2) << "Scan Failed";
        return MetaCommon::to(code);
    }

    std::vector<std::string> values;
    while (iter->valid()) {
        values.emplace_back(iter->val());
        iter->next();
    }
    return values;
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, std::vector<HostAddr>> BaseProcessor<RESP>::allHosts() {
    std::vector<HostAddr> hosts;
    const auto& prefix = MetaServiceUtils::hostPrefix();
    std::unique_ptr<kvstore::KVIterator> iter;
    auto code = kvstore_->prefix(kDefaultSpaceId, kDefaultPartId, prefix, &iter);
    if (code != kvstore::ResultCode::SUCCEEDED) {
        VLOG(2) << "Can't find any hosts";
        return MetaCommon::to(code);
    }

    while (iter->valid()) {
        HostAddr h;
        auto hostAddrPiece = iter->key().subpiece(prefix.size());
        memcpy(&h, hostAddrPiece.data(), hostAddrPiece.size());
        hosts.emplace_back(std::move(h));
        iter->next();
    }
    return hosts;
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, int32_t> BaseProcessor<RESP>::autoIncrementId() {
    folly::SharedMutex::WriteHolder holder(LockUtils::idLock());
    static const std::string kIdKey = "__id__";
    int32_t id;
    std::string val;
    auto ret = kvstore_->get(kDefaultSpaceId, kDefaultPartId, kIdKey, &val);
    if (ret != kvstore::ResultCode::SUCCEEDED) {
        if (ret != kvstore::ResultCode::ERR_KEY_NOT_FOUND) {
            return MetaCommon::to(ret);
        }
        id = 1;
    } else {
        id = *reinterpret_cast<const int32_t*>(val.c_str()) + 1;
    }

    std::vector<kvstore::KV> data;
    data.emplace_back(kIdKey,
                      std::string(reinterpret_cast<const char*>(&id), sizeof(id)));
    folly::Baton<true, std::atomic> baton;
    kvstore_->asyncMultiPut(kDefaultSpaceId,
                            kDefaultPartId,
                            std::move(data),
                            [&] (kvstore::ResultCode code) {
        ret = code;
        baton.post();
    });
    baton.wait();
    if (ret != kvstore::ResultCode::SUCCEEDED) {
        return MetaCommon::to(ret);
    } else {
        return id;
    }
}


template<typename RESP>
cpp2::ErrorCode BaseProcessor<RESP>::spaceExist(GraphSpaceID spaceId) {
    folly::SharedMutex::ReadHolder rHolder(LockUtils::spaceLock());
    auto spaceKey = MetaServiceUtils::spaceKey(spaceId);
    auto ret = doGet(std::move(spaceKey));
    if (nebula::ok(ret)) {
        return cpp2::ErrorCode::SUCCEEDED;
    }
    return nebula::error(ret);
}


template<typename RESP>
cpp2::ErrorCode BaseProcessor<RESP>::userExist(const std::string& account) {
    auto userKey = MetaServiceUtils::userKey(account);
    auto ret = doGet(std::move(userKey));
    if (nebula::ok(ret)) {
        return cpp2::ErrorCode::SUCCEEDED;
    }
    return nebula::error(ret);
}


template<typename RESP>
cpp2::ErrorCode BaseProcessor<RESP>::hostExist(const std::string& hostKey) {
    auto ret = doGet(hostKey);
    if (nebula::ok(ret)) {
        return cpp2::ErrorCode::SUCCEEDED;
    }
    return nebula::error(ret);
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, GraphSpaceID> BaseProcessor<RESP>::getSpaceId(const std::string& name) {
    auto indexKey = MetaServiceUtils::indexSpaceKey(name);
    auto ret = doGet(indexKey);
    if (nebula::ok(ret)) {
        return *reinterpret_cast<const GraphSpaceID*>(nebula::value(ret).c_str());
    }
    return nebula::error(ret);
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, TagID>
BaseProcessor<RESP>::getTagId(GraphSpaceID spaceId, const std::string& name) {
    auto indexKey = MetaServiceUtils::indexTagKey(spaceId, name);
    std::string val;
    auto ret = doGet(std::move(indexKey));
    if (nebula::ok(ret)) {
        return *reinterpret_cast<const TagID*>(nebula::value(ret).c_str());
    }
    return nebula::error(ret);
}

template<typename RESP>
ErrorOr<cpp2::ErrorCode, EdgeType>
BaseProcessor<RESP>::getEdgeType(GraphSpaceID spaceId, const std::string& name) {
    auto indexKey = MetaServiceUtils::indexEdgeKey(spaceId, name);
    auto ret = doGet(std::move(indexKey));
    if (nebula::ok(ret)) {
        return *reinterpret_cast<const EdgeType*>(nebula::value(ret).c_str());
    }
    return nebula::error(ret);
}


template <typename RESP>
ErrorOr<cpp2::ErrorCode, cpp2::Schema>
BaseProcessor<RESP>::getLatestTagSchema(GraphSpaceID spaceId, const TagID tagId) {
    const auto& key = MetaServiceUtils::schemaTagPrefix(spaceId, tagId);
    auto ret = doPrefix(key);
    if (!nebula::ok(ret)) {
        LOG(ERROR) << "Tag Prefix " << key << " failed";
        return nebula::error(ret);
    }

    auto iter = nebula::value(ret).get();
    if (iter->valid()) {
        return MetaServiceUtils::parseSchema(iter->val());
    } else {
        LOG(ERROR) << "Tag Prefix " << key << " not found";
        return cpp2::ErrorCode::E_NOT_FOUND;
    }
}


template <typename RESP>
ErrorOr<cpp2::ErrorCode, cpp2::Schema>
BaseProcessor<RESP>::getLatestEdgeSchema(GraphSpaceID spaceId, const EdgeType edgeType) {
    const auto& key = MetaServiceUtils::schemaEdgePrefix(spaceId, edgeType);
    auto ret = doPrefix(key);
    if (!nebula::ok(ret)) {
        LOG(ERROR) << "Edge Prefix " << key << " failed";
        return nebula::error(ret);
    }

    auto iter = nebula::value(ret).get();
    if (iter->valid()) {
        return MetaServiceUtils::parseSchema(iter->val());
    } else {
        LOG(ERROR) << "Edge Prefix " << key << " not found";
        return cpp2::ErrorCode::E_NOT_FOUND;
    }
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, IndexID>
BaseProcessor<RESP>::getIndexID(GraphSpaceID spaceId, const std::string& indexName) {
    auto indexKey = MetaServiceUtils::indexIndexKey(spaceId, indexName);
    auto ret = doGet(indexKey);
    if (nebula::ok(ret)) {
        return *reinterpret_cast<const IndexID*>(nebula::value(ret).c_str());
    }
    return nebula::error(ret);
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, bool>
BaseProcessor<RESP>::checkPassword(const std::string& account, const std::string& password) {
    auto userKey = MetaServiceUtils::userKey(account);
    auto ret = doGet(userKey);
    if (nebula::ok(ret)) {
        return MetaServiceUtils::parseUserPwd(nebula::value(ret)) == password;
    }
    return nebula::error(ret);
}


template<typename RESP>
cpp2::ErrorCode BaseProcessor<RESP>::doSyncPut(std::vector<kvstore::KV> data) {
    folly::Baton<true, std::atomic> baton;
    auto ret = kvstore::ResultCode::SUCCEEDED;
    kvstore_->asyncMultiPut(kDefaultSpaceId,
                            kDefaultPartId,
                            std::move(data),
                            [&ret, &baton] (kvstore::ResultCode code) {
                                if (kvstore::ResultCode::SUCCEEDED != code) {
                                    ret = code;
                                    LOG(INFO) << "Put data error on meta server";
                                }
                                baton.post();
                            });
    baton.wait();
    return MetaCommon::to(ret);
}


template<typename RESP>
void BaseProcessor<RESP>::doSyncPutAndUpdate(std::vector<kvstore::KV> data) {
    folly::Baton<true, std::atomic> baton;
    auto ret = kvstore::ResultCode::SUCCEEDED;
    kvstore_->asyncMultiPut(kDefaultSpaceId,
                            kDefaultPartId,
                            std::move(data),
                            [&ret, &baton] (kvstore::ResultCode code) {
        if (kvstore::ResultCode::SUCCEEDED != code) {
            ret = code;
            LOG(INFO) << "Put data error on meta server";
        }
        baton.post();
    });
    baton.wait();
    if (ret != kvstore::ResultCode::SUCCEEDED) {
        this->handleErrorCode(MetaCommon::to(ret));
        this->onFinished();
        return;
    }
    auto retCode = LastUpdateTimeMan::update(kvstore_, time::WallClock::fastNowInMilliSec());
    this->handleErrorCode(retCode);
    this->onFinished();
}


template<typename RESP>
void BaseProcessor<RESP>::doSyncMultiRemoveAndUpdate(std::vector<std::string> keys) {
    folly::Baton<true, std::atomic> baton;
    auto ret = kvstore::ResultCode::SUCCEEDED;
    kvstore_->asyncMultiRemove(kDefaultSpaceId,
                               kDefaultPartId,
                               std::move(keys),
                               [&ret, &baton] (kvstore::ResultCode code) {
        if (kvstore::ResultCode::SUCCEEDED != code) {
            ret = code;
            LOG(INFO) << "Remove data error on meta server";
        }
        baton.post();
    });
    baton.wait();
    if (ret != kvstore::ResultCode::SUCCEEDED) {
        this->handleErrorCode(MetaCommon::to(ret));
        this->onFinished();
        return;
    }
    auto retCode = LastUpdateTimeMan::update(kvstore_, time::WallClock::fastNowInMilliSec());
    this->handleErrorCode(retCode);
    this->onFinished();
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, std::vector<cpp2::IndexItem>>
BaseProcessor<RESP>::getIndexes(GraphSpaceID spaceId, int32_t tagOrEdge) {
    std::vector<cpp2::IndexItem> items;
    const auto& indexPrefix = MetaServiceUtils::indexPrefix(spaceId);
    auto iterRet = doPrefix(indexPrefix);
    if (!nebula::ok(iterRet)) {
        auto retCode = nebula::error(iterRet);
        LOG(ERROR) << "Tag or edge index prefix failed, error :"
                   << apache::thrift::util::enumNameSafe(retCode);
        return retCode;
    }
    auto indexIter = nebula::value(iterRet).get();

    while (indexIter->valid()) {
        auto item = MetaServiceUtils::parseIndex(indexIter->val());
        if (item.get_schema_id().getType() == cpp2::SchemaID::Type::tag_id &&
            item.get_schema_id().get_tag_id() == tagOrEdge) {
            items.emplace_back(std::move(item));
        } else if (item.get_schema_id().getType() == cpp2::SchemaID::Type::edge_type &&
                   item.get_schema_id().get_edge_type() == tagOrEdge) {
            items.emplace_back(std::move(item));
        }
        indexIter->next();
    }
    return items;
}


template<typename RESP>
cpp2::ErrorCode
BaseProcessor<RESP>::indexCheck(const std::vector<cpp2::IndexItem>& items,
                                const std::vector<cpp2::AlterSchemaItem>& alterItems) {
    for (const auto& index : items) {
        for (const auto& tagItem : alterItems) {
            if (*tagItem.op_ref() == nebula::meta::cpp2::AlterSchemaOp::CHANGE ||
                *tagItem.op_ref() == nebula::meta::cpp2::AlterSchemaOp::DROP) {
                const auto& tagCols = tagItem.get_schema().get_columns();
                const auto& indexCols = index.get_fields();
                for (const auto& tCol : tagCols) {
                    auto it = std::find_if(indexCols.begin(), indexCols.end(),
                                           [&] (const auto& iCol) {
                                               return tCol.name == iCol.name;
                                           });
                    if (it != indexCols.end()) {
                        LOG(ERROR) << "Index conflict, index :" << index.get_index_name()
                                   << ", column : " << tCol.name;
                        return cpp2::ErrorCode::E_CONFLICT;
                    }
                }
            }
        }
    }
    return cpp2::ErrorCode::SUCCEEDED;
}


template<typename RESP>
bool BaseProcessor<RESP>::checkIndexExist(const std::vector<cpp2::IndexFieldDef>& fields,
                                          const cpp2::IndexItem& item) {
    if (fields.size() == 0) {
        LOG(ERROR) << "Index " << item.get_index_name() << " has existed";
        return true;
    }

    for (size_t i = 0; i < fields.size(); i++) {
        if (fields[i].get_name() != item.get_fields()[i].get_name()) {
            break;
        }

        if (i == fields.size() - 1) {
            LOG(ERROR) << "Index " << item.get_index_name() << " has existed";
            return true;
        }
    }
    return false;
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, GroupID> BaseProcessor<RESP>::getGroupId(const std::string& groupName) {
    auto indexKey = MetaServiceUtils::indexGroupKey(groupName);
    auto ret = doGet(std::move(indexKey));
    if (nebula::ok(ret)) {
        return *reinterpret_cast<const GroupID*>(nebula::value(ret).c_str());
    }
    return nebula::error(ret);
}


template<typename RESP>
ErrorOr<cpp2::ErrorCode, ZoneID> BaseProcessor<RESP>::getZoneId(const std::string& zoneName) {
    auto indexKey = MetaServiceUtils::indexZoneKey(zoneName);
    auto ret = doGet(std::move(indexKey));
    if (nebula::ok(ret)) {
        return *reinterpret_cast<const ZoneID*>(nebula::value(ret).c_str());
    }
    return nebula::error(ret);
}


template<typename RESP>
cpp2::ErrorCode BaseProcessor<RESP>::listenerExist(GraphSpaceID space, cpp2::ListenerType type) {
    folly::SharedMutex::ReadHolder rHolder(LockUtils::listenerLock());
    const auto& prefix = MetaServiceUtils::listenerPrefix(space, type);
    auto ret = doPrefix(prefix);
    if (!nebula::ok(ret)) {
        return nebula::error(ret);
    }

    auto iterRet = nebula::value(ret).get();
    if (!iterRet->valid()) {
        return cpp2::ErrorCode::E_NOT_FOUND;
    }
    return cpp2::ErrorCode::SUCCEEDED;
}

}  // namespace meta
}  // namespace nebula
