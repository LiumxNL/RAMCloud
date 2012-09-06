/* Copyright (c) 2009-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <cstring>

#include "TestUtil.h"
#include "BackupService.h"
#include "InMemoryStorage.h"
#include "LogDigest.h"
#include "MockCluster.h"
#include "SegmentIterator.h"
#include "Server.h"
#include "Key.h"
#include "SingleFileStorage.h"
#include "ShortMacros.h"
#include "StringUtil.h"
#include "TabletsBuilder.h"

namespace RAMCloud {

class BackupServiceTest : public ::testing::Test {
  public:
    Context context;
    ServerConfig config;
    Tub<MockCluster> cluster;
    Server* server;
    BackupService* backup;
    ServerList serverList;
    ServerId backupId;

    BackupServiceTest()
        : context()
        , config(ServerConfig::forTesting())
        , cluster()
        , server()
        , backup()
        , serverList(&context)
        , backupId(5, 0)
    {
        Logger::get().setLogLevels(RAMCloud::SILENT_LOG_LEVEL);

        cluster.construct(&context);
        config.services = {WireFormat::BACKUP_SERVICE};
        config.backup.numSegmentFrames = 5;
        server = cluster->addServer(config);
        backup = server->backup.get();

        serverList.add(backupId, server->config.localLocator,
                                {WireFormat::BACKUP_SERVICE}, 100);
    }

    ~BackupServiceTest()
    {
        cluster.destroy();
    }

    void
    closeSegment(ServerId masterId, uint64_t segmentId) {
        Segment segment;
        Segment::Certificate certificate;
        uint32_t length = segment.getAppendedLength(certificate);
        BackupClient::writeSegment(&context, backupId, masterId, segmentId,
                                   &segment, 0, length, &certificate,
                                   false, true, false);
    }

    vector<ServerId>
    openSegment(ServerId masterId, uint64_t segmentId, bool primary = true)
    {
        Segment segment;
        Segment::Certificate certificate;
        uint32_t length = segment.getAppendedLength(certificate);
        return BackupClient::writeSegment(&context, backupId, masterId,
                                          segmentId, &segment, 0, length,
                                          &certificate,
                                          true, false, primary);
    }

    /**
     * Write a raw string to the segment on backup (including the nul-
     * terminator). The segment will not be properly formatted and so
     * will not be recoverable.
     */
    void
    writeRawString(ServerId masterId, uint64_t segmentId,
                   uint32_t offset, const string& s, bool close = false)
    {
        Segment segment;
        segment.copyIn(offset, s.c_str(), downCast<uint32_t>(s.length()));
        BackupClient::writeSegment(&context, backupId, masterId, segmentId,
                                   &segment,
                                   offset,
                                   uint32_t(s.length() + 1), {},
                                   false, false, close);
    }

    const BackupReplicaMetadata*
    toMetadata(const void* metadata)
    {
        return static_cast<const BackupReplicaMetadata*>(metadata);
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(BackupServiceTest);
};


namespace {
bool constructFilter(string s) {
    return s == "BackupService" || s == "init";
}
};

TEST_F(BackupServiceTest, constructorNoReuseReplicas) {
    config.backup.inMemory = false;
    config.clusterName = "testing";
    config.backup.file = ""; // use auto-generated testing name.

    cluster->addServer(config);

    config.clusterName = "__unnamed__";
    TestLog::Enable _(constructFilter);
    BackupService* backup = cluster->addServer(config)->backup.get();
    EXPECT_EQ(ServerId(), backup->getFormerServerId());
    EXPECT_EQ(
        "BackupService: Cluster '__unnamed__'; ignoring existing backup "
            "storage. Any replicas stored will not be reusable by future "
            "backups. Specify clusterName for persistence across backup "
            "restarts. | "
        "init: My server ID is 3.0 | "
        "init: Backup 3.0 will store replicas under cluster name '__unnamed__'"
        , TestLog::get());
}

TEST_F(BackupServiceTest, constructorDestroyConfusingReplicas) {
    config.backup.inMemory = false;
    config.clusterName = "__unnamed__";
    config.backup.file = ""; // use auto-generated testing name.

    cluster->addServer(config);

    config.clusterName = "testing";
    TestLog::Enable _(constructFilter);
    BackupService* backup = cluster->addServer(config)->backup.get();
    EXPECT_EQ(ServerId(), backup->getFormerServerId());
    EXPECT_EQ(
        "BackupService: Backup storing replicas with clusterName 'testing'. "
            "Future backups must be restarted with the same clusterName for "
            "replicas stored on this backup to be reused. | "
        "BackupService: Replicas stored on disk have a different clusterName "
            "('__unnamed__'). Scribbling storage to ensure any stale replicas "
            "left behind by old backups aren't used by future backups | "
        "init: My server ID is 3.0 | "
        "init: Backup 3.0 will store replicas under cluster name 'testing'"
        , TestLog::get());
}

TEST_F(BackupServiceTest, constructorReuseReplicas)
{
    config.backup.inMemory = false;
    config.clusterName = "testing";
    config.backup.file = ""; // use auto-generated testing name.

    Server* server = cluster->addServer(config);
    BackupService* backup = server->backup.get();

    SingleFileStorage* storage =
        static_cast<SingleFileStorage*>(backup->storage.get());
    // Use same auto-generated testing name as above.
    // Will cause double unlink from file system. Meh.
    config.backup.file = string(storage->tempFilePath);

    TestLog::Enable _(constructFilter);
    cluster->addServer(config);
    EXPECT_EQ(
        "BackupService: Backup storing replicas with clusterName 'testing'. "
            "Future backups must be restarted with the same clusterName for "
            "replicas stored on this backup to be reused. | "
        "BackupService: Replicas stored on disk have matching clusterName "
            "('testing'). Scanning storage to find all replicas and to make "
            "them available to recoveries. | "
        "BackupService: Will enlist as a replacement for formerly crashed "
            "server 2.0 which left replicas behind on disk | "
        "init: My server ID is 2.1 | "
        "init: Backup 2.1 will store replicas under cluster name "
            "'testing'"
        , TestLog::get());
}

TEST_F(BackupServiceTest, assignGroup) {
    uint64_t groupId = 100;
    const uint32_t numReplicas = 3;
    ServerId ids[numReplicas] = {ServerId(15), ServerId(16), ServerId(99)};
    BackupClient::assignGroup(&context, backupId, groupId, numReplicas, ids);
    EXPECT_EQ(groupId, backup->replicationId);
    EXPECT_EQ(15U, backup->replicationGroup.at(0).getId());
    EXPECT_EQ(16U, backup->replicationGroup.at(1).getId());
    EXPECT_EQ(99U, backup->replicationGroup.at(2).getId());
    ids[0] = ServerId(33);
    ids[1] = ServerId(22);
    ids[2] = ServerId(11);
    BackupClient::assignGroup(&context, backupId, groupId, numReplicas, ids);
    EXPECT_EQ(3U, backup->replicationGroup.size());
    EXPECT_EQ(33U, backup->replicationGroup.at(0).getId());
}

TEST_F(BackupServiceTest, freeSegment) {
    openSegment({99, 0}, 88);
    closeSegment({99, 0}, 88);
    EXPECT_NE(backup->frames.end(), backup->frames.find({{99, 0}, 88}));
    {
        TestLog::Enable _;
        BackupClient::freeSegment(&context, backupId, {99, 0}, 88);
        EXPECT_EQ("freeSegment: Freeing replica for master 99.0 segment 88",
                  TestLog::get());
    }
    BackupClient::freeSegment(&context, backupId, ServerId(99, 0), 88);
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{99, 0}, 88}));
}

TEST_F(BackupServiceTest, freeSegment_stillOpen) {
    openSegment({99, 0}, 88);
    BackupClient::freeSegment(&context, backupId, ServerId(99, 0), 88);
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{99, 0}, 88}));
}

TEST_F(BackupServiceTest, freeSegment_underRecovery) {
    auto storage = static_cast<InMemoryStorage*>(backup->storage.get());
    size_t totalFrames = storage->freeMap.count();
    openSegment({99, 0}, 88);

    ProtoBuf::Tablets tablets;
    TabletsBuilder{tablets}
        // partition 0
        (123, Key::getHash(123, "9", 1), Key::getHash(123, "9", 1),
            TabletsBuilder::RECOVERING, 0)
        (123, Key::getHash(123, "10", 2), Key::getHash(123, "10", 2),
            TabletsBuilder::RECOVERING, 0)
        (123, Key::getHash(123, "29", 2), Key::getHash(123, "29", 2),
            TabletsBuilder::RECOVERING, 0)
        (123, Key::getHash(123, "20", 2), Key::getHash(123, "20", 2),
            TabletsBuilder::RECOVERING, 0)
        // partition 1
        (123, Key::getHash(123, "30", 2), Key::getHash(123, "30", 2),
            TabletsBuilder::RECOVERING, 1)
        (125, 0, ~0lu, TabletsBuilder::RECOVERING, 1);

    backup->taskQueue.halt();
    BackupClient::startReadingData(&context, backupId,
                                   456lu, {99, 0}, &tablets);
    BackupClient::freeSegment(&context, backupId, {99, 0}, 88);
    EXPECT_EQ(totalFrames - 1, storage->freeMap.count());
}

TEST_F(BackupServiceTest, getRecoveryData) {
    openSegment({99, 0}, 88);
    closeSegment({99, 0}, 88);

    ProtoBuf::Tablets tablets;
    TabletsBuilder{tablets}
        (1, 0, ~0lu, TabletsBuilder::RECOVERING, 0);
    auto results = BackupClient::startReadingData(&context, backupId,
                                                  456lu, {99, 0}, &tablets);
    EXPECT_EQ(1lu, results.segmentIdAndLength.size());
    EXPECT_EQ(1lu, backup->recoveries.size());

    Buffer recoverySegment;
    auto certificate = BackupClient::getRecoveryData(&context, backupId,
                                                     456lu, {99, 0}, 88, 0,
                                                     &recoverySegment);
    EXPECT_THROW(BackupClient::getRecoveryData(&context, backupId,
                                               457lu, {99, 0}, 88, 0,
                                               &recoverySegment),
                BackupBadSegmentIdException);
}

TEST_F(BackupServiceTest, restartFromStorage)
{
    ServerConfig config = ServerConfig::forTesting();
    config.backup.inMemory = false;
    config.segmentSize = 4096;
    config.backup.numSegmentFrames = 6;
    config.backup.file = ""; // use auto-generated testing name.
    config.services = {WireFormat::BACKUP_SERVICE};
    config.clusterName = "testing";

    server = cluster->addServer(config);
    backup = server->backup.get();
    SingleFileStorage* storage =
        static_cast<SingleFileStorage*>(backup->storage.get());

    Buffer empty;
    Segment::Certificate certificate;
    Tub<BackupReplicaMetadata> metadata;
    std::vector<BackupStorage::FrameRef> frames;
    { // closed
        metadata.construct(certificate,
                           70, 88, config.segmentSize, 0,
                           true, false);
        BackupStorage::FrameRef frame = storage->open(true);
        frames.push_back(frame);
        frame->append(empty, 0, 0, 0, &metadata, sizeof(metadata));
    }
    { // open
        metadata.construct(certificate,
                           70, 89, config.segmentSize, 0,
                           false, false);
        BackupStorage::FrameRef frame = storage->open(true);
        frames.push_back(frame);
        frame->append(empty, 0, 0, 0, &metadata, sizeof(metadata));
    }
    { // bad checksum
        metadata.construct(certificate,
                           70, 90, config.segmentSize, 0,
                           true, false);
        metadata->checksum = 0;
        BackupStorage::FrameRef frame = storage->open(true);
        frames.push_back(frame);
        frame->append(empty, 0, 0, 0, &metadata, sizeof(metadata));
    }
    { // bad segment capacity
        metadata.construct(certificate,
                           70, 91, config.segmentSize, 0,
                           true, false);
        metadata->checksum = 0;
        BackupStorage::FrameRef frame = storage->open(true);
        frames.push_back(frame);
        frame->append(empty, 0, 0, 0, &metadata, sizeof(metadata));
    }
    { // closed, different master
        metadata.construct(certificate,
                           71, 89, config.segmentSize, 0,
                           false, false);
        BackupStorage::FrameRef frame = storage->open(true);
        frames.push_back(frame);
        frame->append(empty, 0, 0, 0, &metadata, sizeof(metadata));
    }
    frames.clear();

    TestLog::Enable _;
    backup->restartFromStorage();

    EXPECT_NE(backup->frames.end(), backup->frames.find({{70, 0}, 88}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{70, 0}, 89}));
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{70, 0}, 90}));
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{70, 0}, 91}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{71, 0}, 89}));

    EXPECT_FALSE(storage->freeMap.test(0));
    EXPECT_FALSE(storage->freeMap.test(1));
    EXPECT_TRUE(storage->freeMap.test(2));
    EXPECT_TRUE(storage->freeMap.test(3));
    EXPECT_FALSE(storage->freeMap.test(4));

    EXPECT_TRUE(StringUtil::contains(TestLog::get(),
        "restartFromStorage: Found stored replica <70.0,88> "
        "on backup storage in frame which was closed"));
    EXPECT_TRUE(StringUtil::contains(TestLog::get(),
        "restartFromStorage: Found stored replica <70.0,89> "
        "on backup storage in frame which was open"));
    EXPECT_TRUE(StringUtil::contains(TestLog::get(),
        "restartFromStorage: Found stored replica <71.0,89> "
        "on backup storage in frame which was open"));

    EXPECT_EQ(2lu, backup->taskQueue.outstandingTasks());
    // Because config.backup.gc is false these tasks delete themselves
    // immediately when performed.
    backup->taskQueue.performTask();
    backup->taskQueue.performTask();
    EXPECT_EQ(0lu, backup->taskQueue.outstandingTasks());
}

TEST_F(BackupServiceTest, startReadingData) {
    openSegment({99, 0}, 88);
    closeSegment({99, 0}, 88);
    openSegment({99, 0}, 89);
    closeSegment({99, 0}, 89);

    ProtoBuf::Tablets tablets;
    auto results = BackupClient::startReadingData(&context, backupId,
                                                  456lu, {99, 0}, &tablets);
    EXPECT_EQ(2lu, results.segmentIdAndLength.size());
    EXPECT_EQ(1lu, backup->recoveries.size());

    results = BackupClient::startReadingData(&context, backupId,
                                             456lu, {99, 0}, &tablets);
    EXPECT_EQ(2lu, results.segmentIdAndLength.size());
    EXPECT_EQ(1lu, backup->recoveries.size());

    TestLog::Enable _;
    results = BackupClient::startReadingData(&context, backupId,
                                             457lu, {99, 0}, &tablets);
    EXPECT_EQ(2lu, results.segmentIdAndLength.size());
    EXPECT_EQ(1lu, backup->recoveries.size());
    EXPECT_EQ(
        "startReadingData: Got startReadingData for recovery 457 for crashed "
            "master 99.0; abandoning existing recovery 456 for that master and "
            "starting anew. | "
        "free: Recovery 456 for crashed master 99.0 is no longer needed; "
            "will clean up as next possible chance. | "
        "BackupMasterRecovery: Recovery 457 building 0 recovery segments for "
            "each replica for crashed master 99.0 | "
        "start: Backup preparing for recovery of crashed server 99.0; "
            "loading replicas and filtering them according to the following "
            "partitions:\n | "
        "schedule: scheduled | "
        "start: Kicked off building recovery segments | "
        "populateStartResponse: Crashed master 99.0 had segment 88 "
            "(secondary) with len 0 | "
        "populateStartResponse: Crashed master 99.0 had segment 89 "
            "(secondary) with len 0 | "
        "populateStartResponse: Sending 2 segment ids for this master "
            "(0 primary)", TestLog::get());
}

TEST_F(BackupServiceTest, writeSegment) {
    openSegment({99, 0}, 88);
    // test for idempotence
    for (int i = 0; i < 2; ++i)
        writeRawString({99, 0}, 88, 10, "test");
    auto frameIt = backup->frames.find({{99, 0}, 88});
    EXPECT_STREQ("test",
                 static_cast<char*>(frameIt->second->load()) + 10);
}

TEST_F(BackupServiceTest, writeSegment_response) {
    uint64_t groupId = 100;
    const uint32_t numReplicas = 3;
    ServerId ids[numReplicas] = {ServerId(15), ServerId(16), ServerId(33)};
    BackupClient::assignGroup(&context, backupId, groupId, numReplicas, ids);
    const vector<ServerId> group =
        openSegment(ServerId(99, 0), 88);
    EXPECT_EQ(3U, group.size());
    EXPECT_EQ(15U, group.at(0).getId());
    EXPECT_EQ(16U, group.at(1).getId());
    EXPECT_EQ(33U, group.at(2).getId());
    ServerId newIds[1] = {ServerId(99)};
    BackupClient::assignGroup(&context, backupId, 0, 1, newIds);
    const vector<ServerId> newGroup =
        openSegment(ServerId(99, 0), 88);
    EXPECT_EQ(1U, newGroup.size());
    EXPECT_EQ(99U, newGroup.at(0).getId());
}

TEST_F(BackupServiceTest, writeSegment_segmentNotOpen) {
    EXPECT_THROW(
        writeRawString({99, 0}, 88, 10, "test"),
        BackupBadSegmentIdException);
}

TEST_F(BackupServiceTest, writeSegment_segmentClosed) {
    openSegment(ServerId(99, 0), 88);
    closeSegment(ServerId(99, 0), 88);
    EXPECT_THROW(
        writeRawString({99, 0}, 88, 10, "test"),
        BackupBadSegmentIdException);
}

TEST_F(BackupServiceTest, writeSegment_segmentClosedRedundantClosingWrite) {
    // This may seem counterintuitive, but throwing an exception on a write
    // after close is actually better than idempotent behavior. The backup
    // throws a client exception on subsequent writes. If the master retried
    // the write rpc and the backup had already received the request then the
    // master should never receive the response with the client exception
    // (the request will have gotten the response from the first request).
    // If the backup never received the first request from the master then
    // it won't generate a client exception on the retry.
    openSegment(ServerId(99, 0), 88);
    closeSegment(ServerId(99, 0), 88);
    EXPECT_THROW(writeRawString({99, 0}, 88, 10, "test", true),
                 BackupBadSegmentIdException);
}

TEST_F(BackupServiceTest, writeSegment_badOffset) {
    openSegment(ServerId(99, 0), 88);
    EXPECT_THROW(
        writeRawString({99, 0}, 88, 500000, "test"),
        BackupSegmentOverflowException);
}

TEST_F(BackupServiceTest, writeSegment_badLength) {
    openSegment(ServerId(99, 0), 88);
    uint32_t length = config.segmentSize + 1;
    ASSERT_TRUE(Segment::DEFAULT_SEGMENT_SIZE >= length);
    Segment segment;
    EXPECT_THROW(
        BackupClient::writeSegment(&context, backupId, ServerId(99, 0),
                                   88, &segment, 0, length, {},
                                   false, false, false),
        BackupSegmentOverflowException);
}

TEST_F(BackupServiceTest, writeSegment_badOffsetPlusLength) {
    openSegment(ServerId(99, 0), 88);
    uint32_t length = config.segmentSize;
    ASSERT_TRUE(Segment::DEFAULT_SEGMENT_SIZE >= length);
    Segment segment;
    EXPECT_THROW(
        BackupClient::writeSegment(&context, backupId, ServerId(99, 0),
                                   88, &segment, 1, length, {},
                                   false, false, false),
        BackupSegmentOverflowException);
}

TEST_F(BackupServiceTest, writeSegment_closeSegment) {
    openSegment(ServerId(99, 0), 88);
    writeRawString({99, 0}, 88, 10, "test");
    // loop to test for idempotence
    for (int i = 0; i > 2; ++i) {
        closeSegment(ServerId(99, 0), 88);
        auto frameIt = backup->frames.find({{99, 0}, 88});
        const char* replicaData =
            static_cast<const char*>(frameIt->second->load());
        EXPECT_STREQ("test", &replicaData[10]);
    }
}

TEST_F(BackupServiceTest, writeSegment_closeSegmentSegmentNotOpen) {
    EXPECT_THROW(closeSegment(ServerId(99, 0), 88),
                            BackupBadSegmentIdException);
}

TEST_F(BackupServiceTest, writeSegment_openSegment) {
    // loop to test for idempotence
    BackupService::FrameMap::iterator frameIt;
    for (int i = 0; i < 2; ++i) {
        openSegment(ServerId(99, 0), 88);
        frameIt = backup->frames.find({{99, 0}, 88});
        auto metadata = toMetadata(frameIt->second->getMetadata());
        EXPECT_TRUE(metadata->primary);
    }
    const char* replicaData = static_cast<const char*>(frameIt->second->load());
    EXPECT_EQ(0, *replicaData);
}

TEST_F(BackupServiceTest, writeSegment_openSegmentSecondary) {
    openSegment(ServerId(99, 0), 88, false);
    auto frameIt = backup->frames.find({{99, 0}, 88});
    auto metadata = toMetadata(frameIt->second->getMetadata());
    EXPECT_TRUE(!metadata->primary);
}

TEST_F(BackupServiceTest, writeSegment_openSegmentOutOfStorage) {
    openSegment(ServerId(99, 0), 85);
    openSegment(ServerId(99, 0), 86);
    openSegment(ServerId(99, 0), 87);
    openSegment(ServerId(99, 0), 88);
    openSegment(ServerId(99, 0), 89);
    EXPECT_THROW(
        openSegment(ServerId(99, 0), 90),
        BackupOpenRejectedException);
}

TEST_F(BackupServiceTest, GarbageCollectDownServerTask) {
    openSegment({99, 0}, 88);
    openSegment({99, 0}, 89);
    openSegment({99, 1}, 88);

    EXPECT_NE(backup->frames.end(), backup->frames.find({{99, 0}, 88}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{99, 0}, 89}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{99, 1}, 88}));

    ProtoBuf::Tablets tablets;
    backup->recoveries[ServerId{99, 0}] =
        new BackupMasterRecovery(backup->taskQueue, 456, {99, 0}, tablets, 0);
    EXPECT_NE(backup->recoveries.end(), backup->recoveries.find({99, 0}));

    typedef BackupService::GarbageCollectDownServerTask Task;
    std::unique_ptr<Task> task(new Task(*backup, {99, 0}));
    task->schedule();
    const_cast<ServerConfig&>(backup->config).backup.gc = true;

    backup->taskQueue.performTask();
    EXPECT_EQ(backup->recoveries.end(), backup->recoveries.find({99, 0}));
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{99, 0}, 88}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{99, 0}, 89}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{99, 1}, 88}));

    TestLog::Enable _;
    // Runs the now scheduled BackupMasterRecovery to free it up.
    backup->taskQueue.performTask();
    EXPECT_EQ("performTask: State for recovery 456 for crashed master 99.0 "
              "freed on backup", TestLog::get());

    backup->taskQueue.performTask();
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{99, 0}, 88}));
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{99, 0}, 89}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{99, 1}, 88}));

    backup->taskQueue.performTask();
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{99, 0}, 88}));
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{99, 0}, 89}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{99, 1}, 88}));

    task.release();
}

namespace {
class GcMockMasterService : public Service {
    void dispatch(WireFormat::Opcode opcode, Rpc& rpc) {
        const WireFormat::RequestCommon* hdr =
            rpc.requestPayload.getStart<WireFormat::RequestCommon>();
        switch (hdr->service) {
        case WireFormat::MEMBERSHIP_SERVICE:
            switch (opcode) {
            case WireFormat::Opcode::GET_SERVER_ID:
            {
                auto* resp = new(&rpc.replyPayload, APPEND)
                    WireFormat::GetServerId::Response();
                resp->serverId = ServerId(13, 0).getId();
                resp->common.status = STATUS_OK;
                break;
            }
            default:
                FAIL();
                break;
            }
            break;
        case WireFormat::MASTER_SERVICE:
            switch (hdr->opcode) {
            case WireFormat::Opcode::IS_REPLICA_NEEDED:
            {
                const WireFormat::IsReplicaNeeded::Request* req =
                    rpc.requestPayload.getStart<
                    WireFormat::IsReplicaNeeded::Request>();
                auto* resp =
                    new(&rpc.replyPayload, APPEND)
                        WireFormat::IsReplicaNeeded::Response();
                resp->needed = req->segmentId % 2;
                resp->common.status = STATUS_OK;
                break;
            }
            default:
                FAIL();
                break;
            }
            break;
        default:
            FAIL();
            break;
        }
    }
};
};

TEST_F(BackupServiceTest, GarbageCollectReplicaFoundOnStorageTask) {
    GcMockMasterService master;
    cluster->transport.addService(master, "mock:host=m",
                                  WireFormat::MEMBERSHIP_SERVICE);
    cluster->transport.addService(master, "mock:host=m",
                                  WireFormat::MASTER_SERVICE);
    ServerList* backupServerList = static_cast<ServerList*>(
        backup->context->serverList);
    backupServerList->add({13, 0}, "mock:host=m", {}, 100);
    serverList.add({13, 0}, "mock:host=m", {}, 100);

    openSegment({13, 0}, 10);
    closeSegment({13, 0}, 10);
    openSegment({13, 0}, 11);
    closeSegment({13, 0}, 11);
    openSegment({13, 0}, 12);
    closeSegment({13, 0}, 12);

    typedef BackupService::GarbageCollectReplicasFoundOnStorageTask Task;
    std::unique_ptr<Task> task(new Task(*backup, {13, 0}));
    task->addSegmentId(10);
    task->addSegmentId(11);
    task->addSegmentId(12);
    task->schedule();
    const_cast<ServerConfig&>(backup->config).backup.gc = true;

    EXPECT_FALSE(task->rpc);
    backup->taskQueue.performTask(); // send rpc to probe 10
    ASSERT_TRUE(task->rpc);

    TestLog::Enable _;
    backup->taskQueue.performTask(); // get response - false for 10
    EXPECT_FALSE(task->rpc);
    EXPECT_TRUE(StringUtil::contains(TestLog::get(),
        "tryToFreeReplica: Server has recovered from lost replica; "
        "freeing replica for <13.0,10>"));
    EXPECT_EQ(1lu, backup->taskQueue.outstandingTasks());
    EXPECT_EQ(backup->frames.end(), backup->frames.find({{13, 0}, 10}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{13, 0}, 11}));
    EXPECT_NE(backup->frames.end(), backup->frames.find({{13, 0}, 12}));

    EXPECT_FALSE(task->rpc);
    backup->taskQueue.performTask(); // send rpc to probe 11
    ASSERT_TRUE(task->rpc);

    TestLog::reset();
    backup->taskQueue.performTask(); // get response - true for 11
    EXPECT_TRUE(StringUtil::contains(TestLog::get(),
        "tryToFreeReplica: Server has not recovered from lost replica; "
        "retaining replica for <13.0,11>; "
        "will probe replica status again later"));
    EXPECT_EQ(1lu, backup->taskQueue.outstandingTasks());

    backupServerList->crashed({13, 0}, "mock:host=m", {}, 100);

    TestLog::reset();
    EXPECT_FALSE(task->rpc);
    backup->taskQueue.performTask(); // find out server crashed
    EXPECT_TRUE(StringUtil::contains(TestLog::get(),
        "tryToFreeReplica: Server 13.0 marked crashed; "
        "waiting for cluster to recover from its failure "
        "before freeing <13.0,11>"));
    EXPECT_EQ(1lu, backup->taskQueue.outstandingTasks());

    backupServerList->remove({13, 0});

    TestLog::reset();
    EXPECT_FALSE(task->rpc);
    backup->taskQueue.performTask(); // send rpc
    EXPECT_TRUE(task->rpc);
    backup->taskQueue.performTask(); // get response - server doesn't exist
    EXPECT_TRUE(StringUtil::contains(TestLog::get(),
        "tryToFreeReplica: Server 13.0 marked down; cluster has recovered from "
            "its failure | "
        "tryToFreeReplica: Server has recovered from lost replica; "
            "freeing replica for <13.0,12>"));
    EXPECT_EQ(1lu, backup->taskQueue.outstandingTasks());

    // Final perform finds no segments to free and just cleans up
    backup->taskQueue.performTask();
    EXPECT_EQ(0lu, backup->taskQueue.outstandingTasks());
    task.release();
}

static bool
taskScheduleFilter(string s)
{
    return s != "schedule";
}

TEST_F(BackupServiceTest, GarbageCollectReplicaFoundOnStorageTask_freedFirst) {
    typedef BackupService::GarbageCollectReplicasFoundOnStorageTask Task;
    std::unique_ptr<Task> task(new Task(*backup, {99, 0}));
    task->addSegmentId(88);
    task->schedule();
    const_cast<ServerConfig&>(backup->config).backup.gc = true;

    TestLog::Enable _(taskScheduleFilter);
    backup->taskQueue.performTask();
    EXPECT_EQ("", TestLog::get());

    // Final perform finds no segments to free and just cleans up
    backup->taskQueue.performTask();
    EXPECT_EQ(0lu, backup->taskQueue.outstandingTasks());
    task.release();
}

TEST_F(BackupServiceTest, trackerChangesEnqueued) {
    backup->testingDoNotStartGcThread = true;
    backup->gcTracker.enqueueChange({{99, 0}, "", {}, 0, ServerStatus::UP},
                                    SERVER_ADDED);
    backup->trackerChangesEnqueued();
    EXPECT_EQ(0lu, backup->taskQueue.outstandingTasks());

    backup->gcTracker.enqueueChange({{99, 0}, "", {}, 0, ServerStatus::CRASHED},
                                    SERVER_CRASHED);
    backup->trackerChangesEnqueued();
    EXPECT_EQ(0lu, backup->taskQueue.outstandingTasks());

    backup->gcTracker.enqueueChange({{99, 0}, "", {}, 0, ServerStatus::DOWN},
                                    SERVER_REMOVED);
    backup->gcTracker.enqueueChange({{98, 0}, "", {}, 0, ServerStatus::UP},
                                    SERVER_ADDED);
    backup->gcTracker.enqueueChange({{98, 0}, "", {}, 0, ServerStatus::DOWN},
                                    SERVER_REMOVED);
    backup->trackerChangesEnqueued();
    EXPECT_EQ(2lu, backup->taskQueue.outstandingTasks());
    backup->taskQueue.performTask();
    backup->taskQueue.performTask();
    EXPECT_EQ(0lu, backup->taskQueue.outstandingTasks());
}

} // namespace RAMCloud
