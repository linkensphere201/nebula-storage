/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#ifndef KVSTORE_KVSTORE_H_
#define KVSTORE_KVSTORE_H_

#include <rocksdb/compaction_filter.h>
#include <rocksdb/merge_operator.h>
#include "common/base/Base.h"
#include "common/base/ErrorOr.h"
#include "common/base/Status.h"
#include "common/meta/SchemaManager.h"
#include "kvstore/Common.h"
#include "kvstore/CompactionFilter.h"
#include "kvstore/KVIterator.h"
#include "kvstore/PartManager.h"
#include "kvstore/raftex/RaftPart.h"

namespace nebula {
namespace kvstore {

struct KVOptions {
    // HBase thrift server address.
    HostAddr hbaseServer_;

    // SchemaManager instance, help the hbasestore to encode/decode data.
    meta::SchemaManager* schemaMan_{nullptr};

    // Paths for data. It would be used by rocksdb engine.
    // Be careful! We should ensure each "paths" has only one instance,
    // otherwise it would mix up the data on disk.
    std::vector<std::string> dataPaths_;

    // Path for listener, only wal is stored, the structure would be spaceId/partId/wal
    std::string listenerPath_;

    // PartManager instance for kvstore.
    std::unique_ptr<PartManager> partMan_{nullptr};

    // Custom MergeOperator used in rocksdb.merge method.
    std::shared_ptr<rocksdb::MergeOperator> mergeOp_{nullptr};
    /**
     * Custom CompactionFilter used in compaction.
     * */
    std::unique_ptr<CompactionFilterFactoryBuilder> cffBuilder_{nullptr};
};

struct StoreCapability {
    static const uint32_t SC_FILTERING = 1;
    static const uint32_t SC_ASYNC = 2;
};
#define SUPPORT_FILTERING(store) (store.capability() & StoreCapability::SC_FILTERING)

class Part;
/**
 * Interface for all kv-stores
 **/
class KVStore {
public:
    virtual ~KVStore() = default;

    // Return bit-OR of StoreCapability values;
    virtual uint32_t capability() const = 0;

    virtual void stop() = 0;

    // Retrieve the current leader for the given partition. This
    // is usually called when ERR_LEADER_CHANGED result code is
    // returned
    virtual ErrorOr<ResultCode, HostAddr> partLeader(GraphSpaceID spaceId, PartitionID partID) = 0;

    virtual PartManager* partManager() const {
        return nullptr;
    }

    // Read a single key
    virtual ResultCode get(GraphSpaceID spaceId,
                           PartitionID partId,
                           const std::string& key,
                           std::string* value,
                           bool canReadFromFollower = false) = 0;

    // Read multiple keys, if error occurs a ResultCode is returned,
    // If key[i] does not exist, the i-th value in return value would be Status::KeyNotFound
    virtual std::pair<ResultCode, std::vector<Status>>
    multiGet(GraphSpaceID spaceId,
             PartitionID partId,
             const std::vector<std::string>& keys,
             std::vector<std::string>* values,
             bool canReadFromFollower = false) = 0;

    // Get all results in range [start, end)
    virtual ResultCode range(GraphSpaceID spaceId,
                             PartitionID partId,
                             const std::string& start,
                             const std::string& end,
                             std::unique_ptr<KVIterator>* iter,
                             bool canReadFromFollower = false) = 0;

    // Since the `range' interface will hold references to its 3rd & 4th parameter, in `iter',
    // thus the arguments must outlive `iter'.
    // Here we forbid one to invoke `range' with rvalues, which is the common mistake.
    virtual ResultCode range(GraphSpaceID spaceId,
                             PartitionID partId,
                             std::string&& start,
                             std::string&& end,
                             std::unique_ptr<KVIterator>* iter,
                             bool canReadFromFollower = false) = delete;

    // Get all results with prefix.
    virtual ResultCode prefix(GraphSpaceID spaceId,
                              PartitionID partId,
                              const std::string& prefix,
                              std::unique_ptr<KVIterator>* iter,
                              bool canReadFromFollower = false) = 0;

    // To forbid to pass rvalue via the `prefix' parameter.
    virtual ResultCode prefix(GraphSpaceID spaceId,
                              PartitionID partId,
                              std::string&& prefix,
                              std::unique_ptr<KVIterator>* iter,
                              bool canReadFromFollower = false) = delete;

    // Get all results with prefix starting from start
    virtual ResultCode rangeWithPrefix(GraphSpaceID spaceId,
                                       PartitionID partId,
                                       const std::string& start,
                                       const std::string& prefix,
                                       std::unique_ptr<KVIterator>* iter,
                                       bool canReadFromFollower = false) = 0;

    // To forbid to pass rvalue via the `rangeWithPrefix' parameter.
    virtual ResultCode rangeWithPrefix(GraphSpaceID spaceId,
                                       PartitionID partId,
                                       std::string&& start,
                                       std::string&& prefix,
                                       std::unique_ptr<KVIterator>* iter,
                                       bool canReadFromFollower = false) = delete;

    virtual ResultCode sync(GraphSpaceID spaceId, PartitionID partId) = 0;

    virtual void asyncMultiPut(GraphSpaceID spaceId,
                               PartitionID partId,
                               std::vector<KV>&& keyValues,
                               KVCallback cb) = 0;

    // Asynchronous version of remove methods
    virtual void asyncRemove(GraphSpaceID spaceId,
                             PartitionID partId,
                             const std::string& key,
                             KVCallback cb) = 0;

    virtual void asyncMultiRemove(GraphSpaceID spaceId,
                                  PartitionID partId,
                                  std::vector<std::string>&& keys,
                                  KVCallback cb) = 0;

    virtual void asyncRemoveRange(GraphSpaceID spaceId,
                                  PartitionID partId,
                                  const std::string& start,
                                  const std::string& end,
                                  KVCallback cb) = 0;

    virtual void asyncAtomicOp(GraphSpaceID spaceId,
                               PartitionID partId,
                               raftex::AtomicOp op,
                               KVCallback cb) = 0;

    /**
     * @brief async commit multi operation.
     *        difference between asyncMultiPut or asyncMultiRemove is
     *        this func allow contains both put and remove together
     *        difference between asyncAtomicOp is asyncAtomicOp may have CAS
     */
    virtual void asyncAppendBatch(GraphSpaceID spaceId,
                                  PartitionID partId,
                                  std::string&& batch,
                                  KVCallback cb) = 0;

    virtual ResultCode ingest(GraphSpaceID spaceId) = 0;

    virtual int32_t allLeader(std::unordered_map<GraphSpaceID,
                              std::vector<meta::cpp2::LeaderInfo>>& leaderIds) = 0;

    virtual ErrorOr<ResultCode, std::shared_ptr<Part>> part(GraphSpaceID spaceId,
                                                            PartitionID partId) = 0;

    virtual ResultCode compact(GraphSpaceID spaceId) = 0;

    virtual ResultCode flush(GraphSpaceID spaceId) = 0;

    virtual ErrorOr<ResultCode, std::string> createCheckpoint(GraphSpaceID spaceId,
                                                              const std::string& name) = 0;

    virtual ResultCode dropCheckpoint(GraphSpaceID spaceId, const std::string& name) = 0;

    virtual ResultCode setWriteBlocking(GraphSpaceID spaceId, bool sign) = 0;

    virtual ErrorOr<ResultCode, std::vector<std::string>> backupTable(
        GraphSpaceID spaceId,
        const std::string& name,
        const std::string& tablePrefix,
        std::function<bool(const folly::StringPiece& key)> filter) = 0;

    virtual ResultCode restoreFromFiles(GraphSpaceID spaceId,
                                        const std::vector<std::string>& files) = 0;

    virtual ResultCode multiPutWithoutReplicator(GraphSpaceID spaceId,
                                                std::vector<KV> keyValues) = 0;

protected:
    KVStore() = default;
};

}   // namespace kvstore
}   // namespace nebula
#endif   // KVSTORE_KVSTORE_H_
