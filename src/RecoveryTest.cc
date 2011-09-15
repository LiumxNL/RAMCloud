/* Copyright (c) 2010-2011 Stanford University
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
#include "BindTransport.h"
#include "BackupManager.h"
#include "BackupService.h"
#include "BackupStorage.h"
#include "CoordinatorClient.h"
#include "CoordinatorService.h"
#include "ShortMacros.h"
#include "MasterService.h"
#include "Tablets.pb.h"
#include "TransportManager.h"
#include "Recovery.h"

namespace RAMCloud {

/**
 * Unit tests for Recovery.
 */
class RecoveryTest : public ::testing::Test {
  public:

    /**
     * Used to control precise timing of destruction of the Segment object
     * which implicitly calls freeSegment.
     */
    struct WriteValidSegment {
        ProtoBuf::ServerList backupList;
        Tub<uint64_t> masterIdTub;
        BackupManager* mgr;
        void *segMem;
        Segment* seg;

        WriteValidSegment(uint64_t masterId,
                          uint64_t segmentId,
                          vector<uint64_t> digestIds,
                          const uint32_t segmentSize,
                          const vector<const char*> locators,
                          bool close)
            : backupList()
            , masterIdTub(masterId)
            , mgr()
            , segMem()
            , seg()
        {
            mgr = new BackupManager(NULL, masterIdTub,
                                    downCast<uint32_t>(locators.size()));
            foreach (const auto& locator, locators) {
                ProtoBuf::ServerList::Entry& e(*backupList.add_server());
                e.set_service_locator(locator);
                e.set_server_type(ProtoBuf::BACKUP);
            }
            mgr->hosts = backupList;

            segMem = xmemalign(segmentSize, segmentSize);
            seg = new Segment(masterId, segmentId, segMem, segmentSize, mgr);

            char temp[LogDigest::getBytesFromCount(
                                        downCast<uint32_t>(digestIds.size()))];
            LogDigest ld(downCast<uint32_t>(digestIds.size()),
                         temp,
                         downCast<uint32_t>(sizeof(temp)));
            for (unsigned int i = 0; i < digestIds.size(); i++)
                ld.addSegment(digestIds[i]);
            seg->append(LOG_ENTRY_TYPE_LOGDIGEST, temp,
                        downCast<uint32_t>(sizeof(temp)));

            if (close)
                seg->close();
        }

        ~WriteValidSegment()
        {
            delete seg;
            free(segMem);
            delete mgr;
        }

        DISALLOW_COPY_AND_ASSIGN(WriteValidSegment);
    };

    BackupClient* backup1;
    BackupClient* backup2;
    BackupClient* backup3;
    BackupService* backupService1;
    BackupService* backupService2;
    BackupService* backupService3;
    CoordinatorClient* coordinator;
    CoordinatorService* coordinatorService;
    BackupService::Config* config1;
    BackupService::Config* config2;
    BackupService::Config* config3;
    ProtoBuf::ServerList* masterHosts;
    ProtoBuf::ServerList* backupHosts;
    const uint32_t segmentFrames;
    const uint32_t segmentSize;
    vector<WriteValidSegment*> segmentsToFree;
    BackupStorage* storage1;
    BackupStorage* storage2;
    BackupStorage* storage3;
    BindTransport* transport;

  public:
    RecoveryTest()
        : backup1()
        , backup2()
        , backup3()
        , backupService1()
        , backupService2()
        , backupService3()
        , coordinator()
        , coordinatorService()
        , config1()
        , config2()
        , config3()
        , masterHosts()
        , backupHosts()
        , segmentFrames(3)
        , segmentSize(1 << 16)
        , segmentsToFree()
        , storage1()
        , storage2()
        , storage3()
        , transport()
    {
        TransportManager& transportManager = *Context::get().transportManager;

        transport = new BindTransport;
        transportManager.registerMock(transport);

        config1 = new BackupService::Config;
        config1->coordinatorLocator = "mock:host=coordinator";
        config1->localLocator = "mock:host=backup1";

        config2 = new BackupService::Config;
        config2->coordinatorLocator = "mock:host=coordinator";
        config2->localLocator = "mock:host=backup2";

        config3 = new BackupService::Config;
        config3->coordinatorLocator = "mock:host=coordinator";
        config3->localLocator = "mock:host=backup3";

        coordinatorService = new CoordinatorService;
        transport->addService(*coordinatorService,
                config1->coordinatorLocator, COORDINATOR_SERVICE);

        coordinator =
            new CoordinatorClient(config1->coordinatorLocator.c_str());

        storage1 = new InMemoryStorage(segmentSize, segmentFrames);
        storage2 = new InMemoryStorage(segmentSize, segmentFrames);
        storage3 = new InMemoryStorage(segmentSize, segmentFrames);

        backupService1 = new BackupService(*config1, *storage1);
        backupService2 = new BackupService(*config2, *storage2);
        backupService3 = new BackupService(*config3, *storage3);

        transport->addService(*backupService1, "mock:host=backup1",
                BACKUP_SERVICE);
        transport->addService(*backupService2, "mock:host=backup2",
                BACKUP_SERVICE);
        transport->addService(*backupService3, "mock:host=backup3",
                BACKUP_SERVICE);

        backupService1->init();
        backupService2->init();
        backupService3->init();

        backup1 =
            new BackupClient(transportManager.getSession("mock:host=backup1"));
        backup2 =
            new BackupClient(transportManager.getSession("mock:host=backup2"));
        backup3 =
            new BackupClient(transportManager.getSession("mock:host=backup3"));

        masterHosts = new ProtoBuf::ServerList();
        {
            ProtoBuf::ServerList::Entry& host(*masterHosts->add_server());
            host.set_server_type(ProtoBuf::MASTER);
            host.set_server_id(9999998);
            host.set_service_locator("mock:host=master1");
        }{
            ProtoBuf::ServerList::Entry& host(*masterHosts->add_server());
            host.set_server_type(ProtoBuf::MASTER);
            host.set_server_id(9999999);
            host.set_service_locator("mock:host=master2");
        }

        backupHosts = new ProtoBuf::ServerList();
        {
            ProtoBuf::ServerList::Entry& host(*backupHosts->add_server());
            host.set_server_type(ProtoBuf::BACKUP);
            host.set_server_id(backupService1->getServerId());
            host.set_service_locator("mock:host=backup1");
        }{
            ProtoBuf::ServerList::Entry& host(*backupHosts->add_server());
            host.set_server_type(ProtoBuf::BACKUP);
            host.set_server_id(backupService2->getServerId());
            host.set_service_locator("mock:host=backup2");
        }{
            ProtoBuf::ServerList::Entry& host(*backupHosts->add_server());
            host.set_server_type(ProtoBuf::BACKUP);
            host.set_server_id(backupService3->getServerId());
            host.set_service_locator("mock:host=backup3");
        }
    }

    ~RecoveryTest()
    {
        delete backupHosts;
        delete masterHosts;
        foreach (WriteValidSegment* s, segmentsToFree)
            delete s;
        delete backup3;
        delete backup2;
        delete backup1;
        delete backupService3;
        delete backupService2;
        delete backupService1;
        delete storage3;
        delete storage2;
        delete storage1;
        delete coordinator;
        delete coordinatorService;
        delete config1;
        delete config2;
        delete config3;
        Context::get().transportManager->unregisterMock();
        delete transport;
        EXPECT_EQ(0,
            BackupStorage::Handle::resetAllocatedHandlesCount());
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(RecoveryTest);
};

TEST_F(RecoveryTest, buildSegmentIdToBackups) {
    MockRandom _(1);
    // Two segs on backup1, one that overlaps with backup2
    segmentsToFree.push_back(
        new WriteValidSegment(99, 88, { 88 }, segmentSize,
            {"mock:host=backup1"}, true));
    segmentsToFree.push_back(
        new WriteValidSegment(99, 89, { 88, 89 }, segmentSize,
            {"mock:host=backup1"}, false));
    // One seg on backup2
    segmentsToFree.push_back(
        new WriteValidSegment(99, 88, { 88 }, segmentSize,
            {"mock:host=backup2"}, true));
    // Zero segs on backup3

    ProtoBuf::Tablets tablets;
    Recovery recovery(99, tablets, *masterHosts, *backupHosts);

    EXPECT_EQ(3, recovery.backups.server_size());
    {
        const ProtoBuf::ServerList::Entry&
            backup(recovery.backups.server(0));
        EXPECT_EQ(89U, backup.segment_id());
        EXPECT_EQ("mock:host=backup1", backup.service_locator());
        EXPECT_EQ(ProtoBuf::BACKUP, backup.server_type());
    }{
        const ProtoBuf::ServerList::Entry&
            backup(recovery.backups.server(1));
        EXPECT_EQ(88U, backup.segment_id());
        EXPECT_EQ("mock:host=backup2", backup.service_locator());
        EXPECT_EQ(ProtoBuf::BACKUP, backup.server_type());
    }{
        const ProtoBuf::ServerList::Entry&
            backup(recovery.backups.server(2));
        EXPECT_EQ(88U, backup.segment_id());
        EXPECT_EQ("mock:host=backup1", backup.service_locator());
        EXPECT_EQ(ProtoBuf::BACKUP, backup.server_type());
    }
}

TEST_F(RecoveryTest, buildSegmentIdToBackups_secondariesEarlyInSomeList) {
    // Two segs on backup1, one that overlaps with backup2
    segmentsToFree.push_back(
        new WriteValidSegment(99, 88, { 88 }, segmentSize,
            {"mock:host=backup1"}, true));
    segmentsToFree.push_back(
        new WriteValidSegment(99, 89, { 88, 89 }, segmentSize,
            {"mock:host=backup1"}, true));
    // One seg on backup2
    segmentsToFree.push_back(
        new WriteValidSegment(99, 88, { 88 }, segmentSize,
            {"mock:host=backup2"}, true));
    // Zero segs on backup3
    // Add one more primary to backup1
    // Add a primary/secondary segment pair to backup2 and backup3
    // No matter which host its placed on it appears earlier in the
    // segment list of 2 or 3 than the latest primary on 1 (which is
    // in slot 3).  Check to make sure the code prevents this secondary
    // from showing up before any primary in the list.
    segmentsToFree.push_back(
        new WriteValidSegment(99, 90, { 88, 89, 90 }, segmentSize,
            {"mock:host=backup1"}, false));
    segmentsToFree.push_back(
        new WriteValidSegment(99, 91, { 88, 89, 90, 91 }, segmentSize,
            {"mock:host=backup2", "mock:host=backup3"}, true));

    ProtoBuf::Tablets tablets;
    Recovery recovery(99, tablets, *masterHosts, *backupHosts);

    EXPECT_EQ(4, recovery.backups.server_size());
    bool sawSecondary = false;
    foreach (const auto& backup, recovery.backups.server()) {
        if (!backup.user_data())
            sawSecondary = true;
        else
            EXPECT_FALSE(sawSecondary);
    }
}

static bool
verifyCompleteLogFilter(string s)
{
    return s == "verifyCompleteLog";
}

TEST_F(RecoveryTest, verifyCompleteLog) {
    // TODO(ongaro): The buildSegmentIdToBackups method needs to be
    // refactored before it can be reasonably tested (see RAM-243).
    // Sorry. Kick me off the project.
    TestLog::Enable _(&verifyCompleteLogFilter);
#if 0
    ProtoBuf::Tablets tablets;
    Recovery recovery(99, tablets, *masterHosts, *backupHosts);

    vector<Recovery::SegmentAndDigestTuple> oldDigestList =
        recovery.digestList;
    EXPECT_EQ(1, oldDigestList.size());

    // no head is very bad news.
    recovery.digestList.clear();
    EXPECT_THROW(recovery.verifyCompleteLog(), Exception);

    // ensure the newest head is chosen
    recovery.digestList = oldDigestList;
    recovery.digestList.push_back({ oldDigestList[0].segmentId + 1,
        oldDigestList[0].segmentLength,
        oldDigestList[0].logDigest.getRawPointer(),
        oldDigestList[0].logDigest.getBytes() });
    recovery.verifyCompleteLog();
    EXPECT_EQ("verifyCompleteLog: Segment 90 of length "
        "64 bytes is the head of the log", TestLog::get());

    // ensure the longest newest head is chosen
    TestLog::reset();
    recovery.digestList.push_back({ oldDigestList[0].segmentId + 1,
        oldDigestList[0].segmentLength + 1,
        oldDigestList[0].logDigest.getRawPointer(),
        oldDigestList[0].logDigest.getBytes() });
    recovery.verifyCompleteLog();
    EXPECT_EQ("verifyCompleteLog: Segment 90 of length "
        "65 bytes is the head of the log", TestLog::get());

    // ensure we log missing segments
    TestLog::reset();
    recovery.segmentMap.erase(88);
    recovery.verifyCompleteLog();
    EXPECT_EQ("verifyCompleteLog: Segment 90 of length 65 bytes "
        "is the head of the log | verifyCompleteLog: Segment 88 is missing!"
        " | verifyCompleteLog: 1 segments in the digest, but not obtained "
        "from backups!", TestLog::get());
#endif
}

/// Create a master along with its config and clean them up on destruction.
struct AutoMaster {
    AutoMaster(BindTransport& transport,
                CoordinatorClient &coordinator,
                const string& locator)
        : config()
        , master()
    {
        config.coordinatorLocator = "mock:host=coordinator";
        config.localLocator = locator;
        MasterService::sizeLogAndHashTable("16", "1", &config);
        master = new MasterService(config, &coordinator, 0);
        transport.addService(*master, locator, MASTER_SERVICE);
        master->init();
    }

    ~AutoMaster()
    {
        delete master;
    }

    ServerConfig config;
    MasterService* master;

    DISALLOW_COPY_AND_ASSIGN(AutoMaster);
};

static bool
getRecoveryDataFilter(string s)
{
    return s == "getRecoveryData" ||
            s == "start";
}

TEST_F(RecoveryTest, start) {
    MockRandom __(1);

    // Two segs on backup1, one that overlaps with backup2
    segmentsToFree.push_back(
        new WriteValidSegment(99, 88, { 88 }, segmentSize,
            {"mock:host=backup1"}, true));
    segmentsToFree.push_back(
        new WriteValidSegment(99, 89, { 88, 89 }, segmentSize,
            {"mock:host=backup1"}, false));
    // One seg on backup2
    segmentsToFree.push_back(
        new WriteValidSegment(99, 88, { 88 }, segmentSize,
            {"mock:host=backup2"}, true));
    // Zero segs on backup3


    AutoMaster am1(*transport, *coordinator, "mock:host=master1");
    AutoMaster am2(*transport, *coordinator, "mock:host=master2");

    ProtoBuf::Tablets tablets; {
        ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
        tablet.set_table_id(123);
        tablet.set_start_object_id(0);
        tablet.set_end_object_id(9);
        tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
        tablet.set_user_data(0); // partition 0
    }{
        ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
        tablet.set_table_id(123);
        tablet.set_start_object_id(20);
        tablet.set_end_object_id(29);
        tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
        tablet.set_user_data(0); // partition 0
    }{
        ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
        tablet.set_table_id(123);
        tablet.set_start_object_id(10);
        tablet.set_end_object_id(19);
        tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
        tablet.set_user_data(1); // partition 1
    }

    Recovery recovery(99, tablets, *masterHosts, *backupHosts);
    TestLog::Enable _(&getRecoveryDataFilter);
    recovery.start();
    EXPECT_EQ(3U, recovery.tabletsUnderRecovery);
    EXPECT_EQ(
        "start: Starting recovery for 2 partitions | "
        "getRecoveryData: getRecoveryData masterId 99, segmentId 89, "
        "partitionId 0 | "
        "getRecoveryData: getRecoveryData complete | "
        "getRecoveryData: getRecoveryData masterId 99, segmentId 88, "
        "partitionId 0 | "
        "getRecoveryData: getRecoveryData complete | "
        "getRecoveryData: getRecoveryData masterId 99, segmentId 89, "
        "partitionId 1 | "
        "getRecoveryData: getRecoveryData complete | "
        "getRecoveryData: getRecoveryData masterId 99, segmentId 88, "
        "partitionId 1 | "
        "getRecoveryData: getRecoveryData complete",
        TestLog::get());
}

TEST_F(RecoveryTest, start_notEnoughMasters) {
    // Two segs on backup1, one that overlaps with backup2
    segmentsToFree.push_back(
        new WriteValidSegment(99, 88, { 88 }, segmentSize,
            {"mock:host=backup1"}, true));
    segmentsToFree.push_back(
        new WriteValidSegment(99, 89, { 88, 89 }, segmentSize,
            {"mock:host=backup1"}, false));
    // One seg on backup2
    segmentsToFree.push_back(
        new WriteValidSegment(99, 88, { 88 }, segmentSize,
            {"mock:host=backup2"}, true));
    // Zero segs on backup3

    AutoMaster am1(*transport, *coordinator, "mock:host=master1");
    AutoMaster am2(*transport, *coordinator, "mock:host=master2");

    ProtoBuf::Tablets tablets; {
        ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
        tablet.set_table_id(123);
        tablet.set_start_object_id(0);
        tablet.set_end_object_id(9);
        tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
        tablet.set_user_data(0); // partition 0
    }{
        ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
        tablet.set_table_id(123);
        tablet.set_start_object_id(10);
        tablet.set_end_object_id(19);
        tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
        tablet.set_user_data(1); // partition 1
    }{
        ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
        tablet.set_table_id(123);
        tablet.set_start_object_id(20);
        tablet.set_end_object_id(29);
        tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
        tablet.set_user_data(2); // partition 2
    }

    Recovery recovery(99, tablets, *masterHosts, *backupHosts);
    MockRandom __(1); // triggers deterministic rand().
    TestLog::Enable _(&getRecoveryDataFilter);
    EXPECT_THROW(recovery.start(), FatalError);
}

} // namespace RAMCloud
