/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <BnBinderRpcCallback.h>
#include <BnBinderRpcSession.h>
#include <BnBinderRpcTest.h>
#include <aidl/IBinderRpcTest.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android/binder_auto_utils.h>
#include <android/binder_libbinder.h>
#include <binder/Binder.h>
#include <binder/BpBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <binder/RpcServer.h>
#include <binder/RpcSession.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>

#include <sys/prctl.h>
#include <unistd.h>

#include "../RpcState.h"   // for debugging
#include "../vm_sockets.h" // for VMADDR_*

using namespace std::chrono_literals;

namespace android {

TEST(BinderRpcParcel, EntireParcelFormatted) {
    Parcel p;
    p.writeInt32(3);

    EXPECT_DEATH(p.markForBinder(sp<BBinder>::make()), "");
}

TEST(BinderRpc, SetExternalServer) {
    base::unique_fd sink(TEMP_FAILURE_RETRY(open("/dev/null", O_RDWR)));
    int sinkFd = sink.get();
    auto server = RpcServer::make();
    server->iUnderstandThisCodeIsExperimentalAndIWillNotUseItInProduction();
    ASSERT_FALSE(server->hasServer());
    ASSERT_TRUE(server->setupExternalServer(std::move(sink)));
    ASSERT_TRUE(server->hasServer());
    base::unique_fd retrieved = server->releaseServer();
    ASSERT_FALSE(server->hasServer());
    ASSERT_EQ(sinkFd, retrieved.get());
}

using android::binder::Status;

#define EXPECT_OK(status)                 \
    do {                                  \
        Status stat = (status);           \
        EXPECT_TRUE(stat.isOk()) << stat; \
    } while (false)

class MyBinderRpcSession : public BnBinderRpcSession {
public:
    static std::atomic<int32_t> gNum;

    MyBinderRpcSession(const std::string& name) : mName(name) { gNum++; }
    Status getName(std::string* name) override {
        *name = mName;
        return Status::ok();
    }
    ~MyBinderRpcSession() { gNum--; }

private:
    std::string mName;
};
std::atomic<int32_t> MyBinderRpcSession::gNum;

class MyBinderRpcCallback : public BnBinderRpcCallback {
    Status sendCallback(const std::string& value) {
        std::unique_lock _l(mMutex);
        mValues.push_back(value);
        _l.unlock();
        mCv.notify_one();
        return Status::ok();
    }
    Status sendOnewayCallback(const std::string& value) { return sendCallback(value); }

public:
    std::mutex mMutex;
    std::condition_variable mCv;
    std::vector<std::string> mValues;
};

class MyBinderRpcTest : public BnBinderRpcTest {
public:
    wp<RpcServer> server;

    Status sendString(const std::string& str) override {
        (void)str;
        return Status::ok();
    }
    Status doubleString(const std::string& str, std::string* strstr) override {
        *strstr = str + str;
        return Status::ok();
    }
    Status countBinders(std::vector<int32_t>* out) override {
        sp<RpcServer> spServer = server.promote();
        if (spServer == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }
        out->clear();
        for (auto session : spServer->listSessions()) {
            size_t count = session->state()->countBinders();
            if (count != 1) {
                // this is called when there is only one binder held remaining,
                // so to aid debugging
                session->state()->dump();
            }
            out->push_back(count);
        }
        return Status::ok();
    }
    Status pingMe(const sp<IBinder>& binder, int32_t* out) override {
        if (binder == nullptr) {
            std::cout << "Received null binder!" << std::endl;
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }
        *out = binder->pingBinder();
        return Status::ok();
    }
    Status repeatBinder(const sp<IBinder>& binder, sp<IBinder>* out) override {
        *out = binder;
        return Status::ok();
    }
    static sp<IBinder> mHeldBinder;
    Status holdBinder(const sp<IBinder>& binder) override {
        mHeldBinder = binder;
        return Status::ok();
    }
    Status getHeldBinder(sp<IBinder>* held) override {
        *held = mHeldBinder;
        return Status::ok();
    }
    Status nestMe(const sp<IBinderRpcTest>& binder, int count) override {
        if (count <= 0) return Status::ok();
        return binder->nestMe(this, count - 1);
    }
    Status alwaysGiveMeTheSameBinder(sp<IBinder>* out) override {
        static sp<IBinder> binder = new BBinder;
        *out = binder;
        return Status::ok();
    }
    Status openSession(const std::string& name, sp<IBinderRpcSession>* out) override {
        *out = new MyBinderRpcSession(name);
        return Status::ok();
    }
    Status getNumOpenSessions(int32_t* out) override {
        *out = MyBinderRpcSession::gNum;
        return Status::ok();
    }

    std::mutex blockMutex;
    Status lock() override {
        blockMutex.lock();
        return Status::ok();
    }
    Status unlockInMsAsync(int32_t ms) override {
        usleep(ms * 1000);
        blockMutex.unlock();
        return Status::ok();
    }
    Status lockUnlock() override {
        std::lock_guard<std::mutex> _l(blockMutex);
        return Status::ok();
    }

    Status sleepMs(int32_t ms) override {
        usleep(ms * 1000);
        return Status::ok();
    }

    Status sleepMsAsync(int32_t ms) override {
        // In-process binder calls are asynchronous, but the call to this method
        // is synchronous wrt its client. This in/out-process threading model
        // diffentiation is a classic binder leaky abstraction (for better or
        // worse) and is preserved here the way binder sockets plugs itself
        // into BpBinder, as nothing is changed at the higher levels
        // (IInterface) which result in this behavior.
        return sleepMs(ms);
    }

    Status doCallback(const sp<IBinderRpcCallback>& callback, bool oneway, bool delayed,
                      const std::string& value) override {
        if (callback == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }

        if (delayed) {
            std::thread([=]() {
                ALOGE("Executing delayed callback: '%s'", value.c_str());
                Status status = doCallback(callback, oneway, false, value);
                ALOGE("Delayed callback status: '%s'", status.toString8().c_str());
            }).detach();
            return Status::ok();
        }

        if (oneway) {
            return callback->sendOnewayCallback(value);
        }

        return callback->sendCallback(value);
    }

    Status doCallbackAsync(const sp<IBinderRpcCallback>& callback, bool oneway, bool delayed,
                           const std::string& value) override {
        return doCallback(callback, oneway, delayed, value);
    }

    Status die(bool cleanup) override {
        if (cleanup) {
            exit(1);
        } else {
            _exit(1);
        }
    }

    Status scheduleShutdown() override {
        sp<RpcServer> strongServer = server.promote();
        if (strongServer == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }
        std::thread([=] {
            LOG_ALWAYS_FATAL_IF(!strongServer->shutdown(), "Could not shutdown");
        }).detach();
        return Status::ok();
    }

    Status useKernelBinderCallingId() override {
        // this is WRONG! It does not make sense when using RPC binder, and
        // because it is SO wrong, and so much code calls this, it should abort!

        (void)IPCThreadState::self()->getCallingPid();
        return Status::ok();
    }
};
sp<IBinder> MyBinderRpcTest::mHeldBinder;

class Process {
public:
    Process(Process&&) = default;
    Process(const std::function<void(android::base::borrowed_fd /* writeEnd */)>& f) {
        android::base::unique_fd writeEnd;
        CHECK(android::base::Pipe(&mReadEnd, &writeEnd)) << strerror(errno);
        if (0 == (mPid = fork())) {
            // racey: assume parent doesn't crash before this is set
            prctl(PR_SET_PDEATHSIG, SIGHUP);

            f(writeEnd);

            exit(0);
        }
    }
    ~Process() {
        if (mPid != 0) {
            waitpid(mPid, nullptr, 0);
        }
    }
    android::base::borrowed_fd readEnd() { return mReadEnd; }

private:
    pid_t mPid = 0;
    android::base::unique_fd mReadEnd;
};

static std::string allocateSocketAddress() {
    static size_t id = 0;
    std::string temp = getenv("TMPDIR") ?: "/tmp";
    return temp + "/binderRpcTest_" + std::to_string(id++);
};

static unsigned int allocateVsockPort() {
    static unsigned int vsockPort = 3456;
    return vsockPort++;
}

struct ProcessSession {
    // reference to process hosting a socket server
    Process host;

    struct SessionInfo {
        sp<RpcSession> session;
        sp<IBinder> root;
    };

    // client session objects associated with other process
    // each one represents a separate session
    std::vector<SessionInfo> sessions;

    ProcessSession(ProcessSession&&) = default;
    ~ProcessSession() {
        for (auto& session : sessions) {
            session.root = nullptr;
        }

        for (auto& info : sessions) {
            sp<RpcSession>& session = info.session;

            EXPECT_NE(nullptr, session);
            EXPECT_NE(nullptr, session->state());
            EXPECT_EQ(0, session->state()->countBinders()) << (session->state()->dump(), "dump:");

            wp<RpcSession> weakSession = session;
            session = nullptr;
            EXPECT_EQ(nullptr, weakSession.promote()) << "Leaked session";
        }
    }
};

// Process session where the process hosts IBinderRpcTest, the server used
// for most testing here
struct BinderRpcTestProcessSession {
    ProcessSession proc;

    // pre-fetched root object (for first session)
    sp<IBinder> rootBinder;

    // pre-casted root object (for first session)
    sp<IBinderRpcTest> rootIface;

    // whether session should be invalidated by end of run
    bool expectAlreadyShutdown = false;

    BinderRpcTestProcessSession(BinderRpcTestProcessSession&&) = default;
    ~BinderRpcTestProcessSession() {
        EXPECT_NE(nullptr, rootIface);
        if (rootIface == nullptr) return;

        if (!expectAlreadyShutdown) {
            std::vector<int32_t> remoteCounts;
            // calling over any sessions counts across all sessions
            EXPECT_OK(rootIface->countBinders(&remoteCounts));
            EXPECT_EQ(remoteCounts.size(), proc.sessions.size());
            for (auto remoteCount : remoteCounts) {
                EXPECT_EQ(remoteCount, 1);
            }

            EXPECT_OK(rootIface->scheduleShutdown());
        }

        rootIface = nullptr;
        rootBinder = nullptr;
    }
};

enum class SocketType {
    UNIX,
    VSOCK,
    INET,
};
static inline std::string PrintSocketType(const testing::TestParamInfo<SocketType>& info) {
    switch (info.param) {
        case SocketType::UNIX:
            return "unix_domain_socket";
        case SocketType::VSOCK:
            return "vm_socket";
        case SocketType::INET:
            return "inet_socket";
        default:
            LOG_ALWAYS_FATAL("Unknown socket type");
            return "";
    }
}

class BinderRpc : public ::testing::TestWithParam<SocketType> {
public:
    // This creates a new process serving an interface on a certain number of
    // threads.
    ProcessSession createRpcTestSocketServerProcess(
            size_t numThreads, size_t numSessions, size_t numReverseConnections,
            const std::function<void(const sp<RpcServer>&)>& configure) {
        CHECK_GE(numSessions, 1) << "Must have at least one session to a server";

        SocketType socketType = GetParam();

        unsigned int vsockPort = allocateVsockPort();
        std::string addr = allocateSocketAddress();
        unlink(addr.c_str());

        auto ret = ProcessSession{
                .host = Process([&](android::base::borrowed_fd writeEnd) {
                    sp<RpcServer> server = RpcServer::make();

                    server->iUnderstandThisCodeIsExperimentalAndIWillNotUseItInProduction();
                    server->setMaxThreads(numThreads);

                    unsigned int outPort = 0;

                    switch (socketType) {
                        case SocketType::UNIX:
                            CHECK(server->setupUnixDomainServer(addr.c_str())) << addr;
                            break;
                        case SocketType::VSOCK:
                            CHECK(server->setupVsockServer(vsockPort));
                            break;
                        case SocketType::INET: {
                            CHECK(server->setupInetServer(0, &outPort));
                            CHECK_NE(0, outPort);
                            break;
                        }
                        default:
                            LOG_ALWAYS_FATAL("Unknown socket type");
                    }

                    CHECK(android::base::WriteFully(writeEnd, &outPort, sizeof(outPort)));

                    configure(server);

                    server->join();

                    // Another thread calls shutdown. Wait for it to complete.
                    (void)server->shutdown();
                }),
        };

        // always read socket, so that we have waited for the server to start
        unsigned int outPort = 0;
        CHECK(android::base::ReadFully(ret.host.readEnd(), &outPort, sizeof(outPort)));
        if (socketType == SocketType::INET) {
            CHECK_NE(0, outPort);
        }

        for (size_t i = 0; i < numSessions; i++) {
            sp<RpcSession> session = RpcSession::make();
            session->setMaxThreads(numReverseConnections);

            switch (socketType) {
                case SocketType::UNIX:
                    if (session->setupUnixDomainClient(addr.c_str())) goto success;
                    break;
                case SocketType::VSOCK:
                    if (session->setupVsockClient(VMADDR_CID_LOCAL, vsockPort)) goto success;
                    break;
                case SocketType::INET:
                    if (session->setupInetClient("127.0.0.1", outPort)) goto success;
                    break;
                default:
                    LOG_ALWAYS_FATAL("Unknown socket type");
            }
            LOG_ALWAYS_FATAL("Could not connect");
        success:
            ret.sessions.push_back({session, session->getRootObject()});
        }
        return ret;
    }

    BinderRpcTestProcessSession createRpcTestSocketServerProcess(size_t numThreads,
                                                                 size_t numSessions = 1,
                                                                 size_t numReverseConnections = 0) {
        BinderRpcTestProcessSession ret{
                .proc = createRpcTestSocketServerProcess(numThreads, numSessions,
                                                         numReverseConnections,
                                                         [&](const sp<RpcServer>& server) {
                                                             sp<MyBinderRpcTest> service =
                                                                     new MyBinderRpcTest;
                                                             server->setRootObject(service);
                                                             service->server = server;
                                                         }),
        };

        ret.rootBinder = ret.proc.sessions.at(0).root;
        ret.rootIface = interface_cast<IBinderRpcTest>(ret.rootBinder);

        return ret;
    }
};

TEST_P(BinderRpc, Ping) {
    auto proc = createRpcTestSocketServerProcess(1);
    ASSERT_NE(proc.rootBinder, nullptr);
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());
}

TEST_P(BinderRpc, GetInterfaceDescriptor) {
    auto proc = createRpcTestSocketServerProcess(1);
    ASSERT_NE(proc.rootBinder, nullptr);
    EXPECT_EQ(IBinderRpcTest::descriptor, proc.rootBinder->getInterfaceDescriptor());
}

TEST_P(BinderRpc, MultipleSessions) {
    auto proc = createRpcTestSocketServerProcess(1 /*threads*/, 5 /*sessions*/);
    for (auto session : proc.proc.sessions) {
        ASSERT_NE(nullptr, session.root);
        EXPECT_EQ(OK, session.root->pingBinder());
    }
}

TEST_P(BinderRpc, TransactionsMustBeMarkedRpc) {
    auto proc = createRpcTestSocketServerProcess(1);
    Parcel data;
    Parcel reply;
    EXPECT_EQ(BAD_TYPE, proc.rootBinder->transact(IBinder::PING_TRANSACTION, data, &reply, 0));
}

TEST_P(BinderRpc, AppendSeparateFormats) {
    auto proc = createRpcTestSocketServerProcess(1);

    Parcel p1;
    p1.markForBinder(proc.rootBinder);
    p1.writeInt32(3);

    Parcel p2;

    EXPECT_EQ(BAD_TYPE, p1.appendFrom(&p2, 0, p2.dataSize()));
    EXPECT_EQ(BAD_TYPE, p2.appendFrom(&p1, 0, p1.dataSize()));
}

TEST_P(BinderRpc, UnknownTransaction) {
    auto proc = createRpcTestSocketServerProcess(1);
    Parcel data;
    data.markForBinder(proc.rootBinder);
    Parcel reply;
    EXPECT_EQ(UNKNOWN_TRANSACTION, proc.rootBinder->transact(1337, data, &reply, 0));
}

TEST_P(BinderRpc, SendSomethingOneway) {
    auto proc = createRpcTestSocketServerProcess(1);
    EXPECT_OK(proc.rootIface->sendString("asdf"));
}

TEST_P(BinderRpc, SendAndGetResultBack) {
    auto proc = createRpcTestSocketServerProcess(1);
    std::string doubled;
    EXPECT_OK(proc.rootIface->doubleString("cool ", &doubled));
    EXPECT_EQ("cool cool ", doubled);
}

TEST_P(BinderRpc, SendAndGetResultBackBig) {
    auto proc = createRpcTestSocketServerProcess(1);
    std::string single = std::string(1024, 'a');
    std::string doubled;
    EXPECT_OK(proc.rootIface->doubleString(single, &doubled));
    EXPECT_EQ(single + single, doubled);
}

TEST_P(BinderRpc, CallMeBack) {
    auto proc = createRpcTestSocketServerProcess(1);

    int32_t pingResult;
    EXPECT_OK(proc.rootIface->pingMe(new MyBinderRpcSession("foo"), &pingResult));
    EXPECT_EQ(OK, pingResult);

    EXPECT_EQ(0, MyBinderRpcSession::gNum);
}

TEST_P(BinderRpc, RepeatBinder) {
    auto proc = createRpcTestSocketServerProcess(1);

    sp<IBinder> inBinder = new MyBinderRpcSession("foo");
    sp<IBinder> outBinder;
    EXPECT_OK(proc.rootIface->repeatBinder(inBinder, &outBinder));
    EXPECT_EQ(inBinder, outBinder);

    wp<IBinder> weak = inBinder;
    inBinder = nullptr;
    outBinder = nullptr;

    // Force reading a reply, to process any pending dec refs from the other
    // process (the other process will process dec refs there before processing
    // the ping here).
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());

    EXPECT_EQ(nullptr, weak.promote());

    EXPECT_EQ(0, MyBinderRpcSession::gNum);
}

TEST_P(BinderRpc, RepeatTheirBinder) {
    auto proc = createRpcTestSocketServerProcess(1);

    sp<IBinderRpcSession> session;
    EXPECT_OK(proc.rootIface->openSession("aoeu", &session));

    sp<IBinder> inBinder = IInterface::asBinder(session);
    sp<IBinder> outBinder;
    EXPECT_OK(proc.rootIface->repeatBinder(inBinder, &outBinder));
    EXPECT_EQ(inBinder, outBinder);

    wp<IBinder> weak = inBinder;
    session = nullptr;
    inBinder = nullptr;
    outBinder = nullptr;

    // Force reading a reply, to process any pending dec refs from the other
    // process (the other process will process dec refs there before processing
    // the ping here).
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());

    EXPECT_EQ(nullptr, weak.promote());
}

TEST_P(BinderRpc, RepeatBinderNull) {
    auto proc = createRpcTestSocketServerProcess(1);

    sp<IBinder> outBinder;
    EXPECT_OK(proc.rootIface->repeatBinder(nullptr, &outBinder));
    EXPECT_EQ(nullptr, outBinder);
}

TEST_P(BinderRpc, HoldBinder) {
    auto proc = createRpcTestSocketServerProcess(1);

    IBinder* ptr = nullptr;
    {
        sp<IBinder> binder = new BBinder();
        ptr = binder.get();
        EXPECT_OK(proc.rootIface->holdBinder(binder));
    }

    sp<IBinder> held;
    EXPECT_OK(proc.rootIface->getHeldBinder(&held));

    EXPECT_EQ(held.get(), ptr);

    // stop holding binder, because we test to make sure references are cleaned
    // up
    EXPECT_OK(proc.rootIface->holdBinder(nullptr));
    // and flush ref counts
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());
}

// START TESTS FOR LIMITATIONS OF SOCKET BINDER
// These are behavioral differences form regular binder, where certain usecases
// aren't supported.

TEST_P(BinderRpc, CannotMixBindersBetweenUnrelatedSocketSessions) {
    auto proc1 = createRpcTestSocketServerProcess(1);
    auto proc2 = createRpcTestSocketServerProcess(1);

    sp<IBinder> outBinder;
    EXPECT_EQ(INVALID_OPERATION,
              proc1.rootIface->repeatBinder(proc2.rootBinder, &outBinder).transactionError());
}

TEST_P(BinderRpc, CannotMixBindersBetweenTwoSessionsToTheSameServer) {
    auto proc = createRpcTestSocketServerProcess(1 /*threads*/, 2 /*sessions*/);

    sp<IBinder> outBinder;
    EXPECT_EQ(INVALID_OPERATION,
              proc.rootIface->repeatBinder(proc.proc.sessions.at(1).root, &outBinder)
                      .transactionError());
}

TEST_P(BinderRpc, CannotSendRegularBinderOverSocketBinder) {
    auto proc = createRpcTestSocketServerProcess(1);

    sp<IBinder> someRealBinder = IInterface::asBinder(defaultServiceManager());
    sp<IBinder> outBinder;
    EXPECT_EQ(INVALID_OPERATION,
              proc.rootIface->repeatBinder(someRealBinder, &outBinder).transactionError());
}

TEST_P(BinderRpc, CannotSendSocketBinderOverRegularBinder) {
    auto proc = createRpcTestSocketServerProcess(1);

    // for historical reasons, IServiceManager interface only returns the
    // exception code
    EXPECT_EQ(binder::Status::EX_TRANSACTION_FAILED,
              defaultServiceManager()->addService(String16("not_suspicious"), proc.rootBinder));
}

// END TESTS FOR LIMITATIONS OF SOCKET BINDER

TEST_P(BinderRpc, RepeatRootObject) {
    auto proc = createRpcTestSocketServerProcess(1);

    sp<IBinder> outBinder;
    EXPECT_OK(proc.rootIface->repeatBinder(proc.rootBinder, &outBinder));
    EXPECT_EQ(proc.rootBinder, outBinder);
}

TEST_P(BinderRpc, NestedTransactions) {
    auto proc = createRpcTestSocketServerProcess(1);

    auto nastyNester = sp<MyBinderRpcTest>::make();
    EXPECT_OK(proc.rootIface->nestMe(nastyNester, 10));

    wp<IBinder> weak = nastyNester;
    nastyNester = nullptr;
    EXPECT_EQ(nullptr, weak.promote());
}

TEST_P(BinderRpc, SameBinderEquality) {
    auto proc = createRpcTestSocketServerProcess(1);

    sp<IBinder> a;
    EXPECT_OK(proc.rootIface->alwaysGiveMeTheSameBinder(&a));

    sp<IBinder> b;
    EXPECT_OK(proc.rootIface->alwaysGiveMeTheSameBinder(&b));

    EXPECT_EQ(a, b);
}

TEST_P(BinderRpc, SameBinderEqualityWeak) {
    auto proc = createRpcTestSocketServerProcess(1);

    sp<IBinder> a;
    EXPECT_OK(proc.rootIface->alwaysGiveMeTheSameBinder(&a));
    wp<IBinder> weak = a;
    a = nullptr;

    sp<IBinder> b;
    EXPECT_OK(proc.rootIface->alwaysGiveMeTheSameBinder(&b));

    // this is the wrong behavior, since BpBinder
    // doesn't implement onIncStrongAttempted
    // but make sure there is no crash
    EXPECT_EQ(nullptr, weak.promote());

    GTEST_SKIP() << "Weak binders aren't currently re-promotable for RPC binder.";

    // In order to fix this:
    // - need to have incStrongAttempted reflected across IPC boundary (wait for
    //   response to promote - round trip...)
    // - sendOnLastWeakRef, to delete entries out of RpcState table
    EXPECT_EQ(b, weak.promote());
}

#define expectSessions(expected, iface)                   \
    do {                                                  \
        int session;                                      \
        EXPECT_OK((iface)->getNumOpenSessions(&session)); \
        EXPECT_EQ(expected, session);                     \
    } while (false)

TEST_P(BinderRpc, SingleSession) {
    auto proc = createRpcTestSocketServerProcess(1);

    sp<IBinderRpcSession> session;
    EXPECT_OK(proc.rootIface->openSession("aoeu", &session));
    std::string out;
    EXPECT_OK(session->getName(&out));
    EXPECT_EQ("aoeu", out);

    expectSessions(1, proc.rootIface);
    session = nullptr;
    expectSessions(0, proc.rootIface);
}

TEST_P(BinderRpc, ManySessions) {
    auto proc = createRpcTestSocketServerProcess(1);

    std::vector<sp<IBinderRpcSession>> sessions;

    for (size_t i = 0; i < 15; i++) {
        expectSessions(i, proc.rootIface);
        sp<IBinderRpcSession> session;
        EXPECT_OK(proc.rootIface->openSession(std::to_string(i), &session));
        sessions.push_back(session);
    }
    expectSessions(sessions.size(), proc.rootIface);
    for (size_t i = 0; i < sessions.size(); i++) {
        std::string out;
        EXPECT_OK(sessions.at(i)->getName(&out));
        EXPECT_EQ(std::to_string(i), out);
    }
    expectSessions(sessions.size(), proc.rootIface);

    while (!sessions.empty()) {
        sessions.pop_back();
        expectSessions(sessions.size(), proc.rootIface);
    }
    expectSessions(0, proc.rootIface);
}

size_t epochMillis() {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    using std::chrono::system_clock;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

TEST_P(BinderRpc, ThreadPoolGreaterThanEqualRequested) {
    constexpr size_t kNumThreads = 10;

    auto proc = createRpcTestSocketServerProcess(kNumThreads);

    EXPECT_OK(proc.rootIface->lock());

    // block all but one thread taking locks
    std::vector<std::thread> ts;
    for (size_t i = 0; i < kNumThreads - 1; i++) {
        ts.push_back(std::thread([&] { proc.rootIface->lockUnlock(); }));
    }

    usleep(100000); // give chance for calls on other threads

    // other calls still work
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());

    constexpr size_t blockTimeMs = 500;
    size_t epochMsBefore = epochMillis();
    // after this, we should never see a response within this time
    EXPECT_OK(proc.rootIface->unlockInMsAsync(blockTimeMs));

    // this call should be blocked for blockTimeMs
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());

    size_t epochMsAfter = epochMillis();
    EXPECT_GE(epochMsAfter, epochMsBefore + blockTimeMs) << epochMsBefore;

    for (auto& t : ts) t.join();
}

TEST_P(BinderRpc, ThreadPoolOverSaturated) {
    constexpr size_t kNumThreads = 10;
    constexpr size_t kNumCalls = kNumThreads + 3;
    constexpr size_t kSleepMs = 500;

    auto proc = createRpcTestSocketServerProcess(kNumThreads);

    size_t epochMsBefore = epochMillis();

    std::vector<std::thread> ts;
    for (size_t i = 0; i < kNumCalls; i++) {
        ts.push_back(std::thread([&] { proc.rootIface->sleepMs(kSleepMs); }));
    }

    for (auto& t : ts) t.join();

    size_t epochMsAfter = epochMillis();

    EXPECT_GE(epochMsAfter, epochMsBefore + 2 * kSleepMs);

    // Potential flake, but make sure calls are handled in parallel.
    EXPECT_LE(epochMsAfter, epochMsBefore + 3 * kSleepMs);
}

TEST_P(BinderRpc, ThreadingStressTest) {
    constexpr size_t kNumClientThreads = 10;
    constexpr size_t kNumServerThreads = 10;
    constexpr size_t kNumCalls = 100;

    auto proc = createRpcTestSocketServerProcess(kNumServerThreads);

    std::vector<std::thread> threads;
    for (size_t i = 0; i < kNumClientThreads; i++) {
        threads.push_back(std::thread([&] {
            for (size_t j = 0; j < kNumCalls; j++) {
                sp<IBinder> out;
                EXPECT_OK(proc.rootIface->repeatBinder(proc.rootBinder, &out));
                EXPECT_EQ(proc.rootBinder, out);
            }
        }));
    }

    for (auto& t : threads) t.join();
}

TEST_P(BinderRpc, OnewayStressTest) {
    constexpr size_t kNumClientThreads = 10;
    constexpr size_t kNumServerThreads = 10;
    constexpr size_t kNumCalls = 500;

    auto proc = createRpcTestSocketServerProcess(kNumServerThreads);

    std::vector<std::thread> threads;
    for (size_t i = 0; i < kNumClientThreads; i++) {
        threads.push_back(std::thread([&] {
            for (size_t j = 0; j < kNumCalls; j++) {
                EXPECT_OK(proc.rootIface->sendString("a"));
            }

            // check threads are not stuck
            EXPECT_OK(proc.rootIface->sleepMs(250));
        }));
    }

    for (auto& t : threads) t.join();
}

TEST_P(BinderRpc, OnewayCallDoesNotWait) {
    constexpr size_t kReallyLongTimeMs = 100;
    constexpr size_t kSleepMs = kReallyLongTimeMs * 5;

    auto proc = createRpcTestSocketServerProcess(1);

    size_t epochMsBefore = epochMillis();

    EXPECT_OK(proc.rootIface->sleepMsAsync(kSleepMs));

    size_t epochMsAfter = epochMillis();
    EXPECT_LT(epochMsAfter, epochMsBefore + kReallyLongTimeMs);
}

TEST_P(BinderRpc, OnewayCallQueueing) {
    constexpr size_t kNumSleeps = 10;
    constexpr size_t kNumExtraServerThreads = 4;
    constexpr size_t kSleepMs = 50;

    // make sure calls to the same object happen on the same thread
    auto proc = createRpcTestSocketServerProcess(1 + kNumExtraServerThreads);

    EXPECT_OK(proc.rootIface->lock());

    for (size_t i = 0; i < kNumSleeps; i++) {
        // these should be processed serially
        proc.rootIface->sleepMsAsync(kSleepMs);
    }
    // should also be processesed serially
    EXPECT_OK(proc.rootIface->unlockInMsAsync(kSleepMs));

    size_t epochMsBefore = epochMillis();
    EXPECT_OK(proc.rootIface->lockUnlock());
    size_t epochMsAfter = epochMillis();

    EXPECT_GT(epochMsAfter, epochMsBefore + kSleepMs * kNumSleeps);

    // pending oneway transactions hold ref, make sure we read data on all
    // sockets
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 1 + kNumExtraServerThreads; i++) {
        threads.push_back(std::thread([&] { EXPECT_OK(proc.rootIface->sleepMs(250)); }));
    }
    for (auto& t : threads) t.join();
}

TEST_P(BinderRpc, OnewayCallExhaustion) {
    constexpr size_t kNumClients = 2;
    constexpr size_t kTooLongMs = 1000;

    auto proc = createRpcTestSocketServerProcess(kNumClients /*threads*/, 2 /*sessions*/);

    // Build up oneway calls on the second session to make sure it terminates
    // and shuts down. The first session should be unaffected (proc destructor
    // checks the first session).
    auto iface = interface_cast<IBinderRpcTest>(proc.proc.sessions.at(1).root);

    std::vector<std::thread> threads;
    for (size_t i = 0; i < kNumClients; i++) {
        // one of these threads will get stuck queueing a transaction once the
        // socket fills up, the other will be able to fill up transactions on
        // this object
        threads.push_back(std::thread([&] {
            while (iface->sleepMsAsync(kTooLongMs).isOk()) {
            }
        }));
    }
    for (auto& t : threads) t.join();

    Status status = iface->sleepMsAsync(kTooLongMs);
    EXPECT_EQ(DEAD_OBJECT, status.transactionError()) << status;

    // the second session should be shutdown in the other process by the time we
    // are able to join above (it'll only be hung up once it finishes processing
    // any pending commands). We need to erase this session from the record
    // here, so that the destructor for our session won't check that this
    // session is valid, but we still want it to test the other session.
    proc.proc.sessions.erase(proc.proc.sessions.begin() + 1);
}

TEST_P(BinderRpc, Callbacks) {
    const static std::string kTestString = "good afternoon!";

    for (bool callIsOneway : {true, false}) {
        for (bool callbackIsOneway : {true, false}) {
            for (bool delayed : {true, false}) {
                auto proc = createRpcTestSocketServerProcess(1, 1, 1);
                auto cb = sp<MyBinderRpcCallback>::make();

                if (callIsOneway) {
                    EXPECT_OK(proc.rootIface->doCallbackAsync(cb, callbackIsOneway, delayed,
                                                              kTestString));
                } else {
                    EXPECT_OK(
                            proc.rootIface->doCallback(cb, callbackIsOneway, delayed, kTestString));
                }

                using std::literals::chrono_literals::operator""s;
                std::unique_lock<std::mutex> _l(cb->mMutex);
                cb->mCv.wait_for(_l, 1s, [&] { return !cb->mValues.empty(); });

                EXPECT_EQ(cb->mValues.size(), 1)
                        << "callIsOneway: " << callIsOneway
                        << " callbackIsOneway: " << callbackIsOneway << " delayed: " << delayed;
                if (cb->mValues.empty()) continue;
                EXPECT_EQ(cb->mValues.at(0), kTestString)
                        << "callIsOneway: " << callIsOneway
                        << " callbackIsOneway: " << callbackIsOneway << " delayed: " << delayed;

                // since we are severing the connection, we need to go ahead and
                // tell the server to shutdown and exit so that waitpid won't hang
                EXPECT_OK(proc.rootIface->scheduleShutdown());

                // since this session has a reverse connection w/ a threadpool, we
                // need to manually shut it down
                EXPECT_TRUE(proc.proc.sessions.at(0).session->shutdownAndWait(true));

                proc.expectAlreadyShutdown = true;
            }
        }
    }
}

TEST_P(BinderRpc, OnewayCallbackWithNoThread) {
    auto proc = createRpcTestSocketServerProcess(1);
    auto cb = sp<MyBinderRpcCallback>::make();

    Status status = proc.rootIface->doCallback(cb, true /*oneway*/, false /*delayed*/, "anything");
    EXPECT_EQ(WOULD_BLOCK, status.transactionError());
}

TEST_P(BinderRpc, Die) {
    for (bool doDeathCleanup : {true, false}) {
        auto proc = createRpcTestSocketServerProcess(1);

        // make sure there is some state during crash
        // 1. we hold their binder
        sp<IBinderRpcSession> session;
        EXPECT_OK(proc.rootIface->openSession("happy", &session));
        // 2. they hold our binder
        sp<IBinder> binder = new BBinder();
        EXPECT_OK(proc.rootIface->holdBinder(binder));

        EXPECT_EQ(DEAD_OBJECT, proc.rootIface->die(doDeathCleanup).transactionError())
                << "Do death cleanup: " << doDeathCleanup;

        proc.expectAlreadyShutdown = true;
    }
}

TEST_P(BinderRpc, UseKernelBinderCallingId) {
    auto proc = createRpcTestSocketServerProcess(1);

    // we can't allocate IPCThreadState so actually the first time should
    // succeed :(
    EXPECT_OK(proc.rootIface->useKernelBinderCallingId());

    // second time! we catch the error :)
    EXPECT_EQ(DEAD_OBJECT, proc.rootIface->useKernelBinderCallingId().transactionError());

    proc.expectAlreadyShutdown = true;
}

TEST_P(BinderRpc, WorksWithLibbinderNdkPing) {
    auto proc = createRpcTestSocketServerProcess(1);

    ndk::SpAIBinder binder = ndk::SpAIBinder(AIBinder_fromPlatformBinder(proc.rootBinder));
    ASSERT_NE(binder, nullptr);

    ASSERT_EQ(STATUS_OK, AIBinder_ping(binder.get()));
}

TEST_P(BinderRpc, WorksWithLibbinderNdkUserTransaction) {
    auto proc = createRpcTestSocketServerProcess(1);

    ndk::SpAIBinder binder = ndk::SpAIBinder(AIBinder_fromPlatformBinder(proc.rootBinder));
    ASSERT_NE(binder, nullptr);

    auto ndkBinder = aidl::IBinderRpcTest::fromBinder(binder);
    ASSERT_NE(ndkBinder, nullptr);

    std::string out;
    ndk::ScopedAStatus status = ndkBinder->doubleString("aoeu", &out);
    ASSERT_TRUE(status.isOk()) << status.getDescription();
    ASSERT_EQ("aoeuaoeu", out);
}

ssize_t countFds() {
    DIR* dir = opendir("/proc/self/fd/");
    if (dir == nullptr) return -1;
    ssize_t ret = 0;
    dirent* ent;
    while ((ent = readdir(dir)) != nullptr) ret++;
    closedir(dir);
    return ret;
}

TEST_P(BinderRpc, Fds) {
    ssize_t beforeFds = countFds();
    ASSERT_GE(beforeFds, 0);
    {
        auto proc = createRpcTestSocketServerProcess(10);
        ASSERT_EQ(OK, proc.rootBinder->pingBinder());
    }
    ASSERT_EQ(beforeFds, countFds()) << (system("ls -l /proc/self/fd/"), "fd leak?");
}

static bool testSupportVsockLoopback() {
    unsigned int vsockPort = allocateVsockPort();
    sp<RpcServer> server = RpcServer::make();
    server->iUnderstandThisCodeIsExperimentalAndIWillNotUseItInProduction();
    CHECK(server->setupVsockServer(vsockPort));
    server->start();

    sp<RpcSession> session = RpcSession::make();
    bool okay = session->setupVsockClient(VMADDR_CID_LOCAL, vsockPort);
    CHECK(server->shutdown());
    ALOGE("Detected vsock loopback supported: %d", okay);
    return okay;
}

static std::vector<SocketType> testSocketTypes() {
    std::vector<SocketType> ret = {SocketType::UNIX, SocketType::INET};

    static bool hasVsockLoopback = testSupportVsockLoopback();

    if (hasVsockLoopback) {
        ret.push_back(SocketType::VSOCK);
    }

    return ret;
}

INSTANTIATE_TEST_CASE_P(PerSocket, BinderRpc, ::testing::ValuesIn(testSocketTypes()),
                        PrintSocketType);

class BinderRpcServerRootObject : public ::testing::TestWithParam<std::tuple<bool, bool>> {};

TEST_P(BinderRpcServerRootObject, WeakRootObject) {
    using SetFn = std::function<void(RpcServer*, sp<IBinder>)>;
    auto setRootObject = [](bool isStrong) -> SetFn {
        return isStrong ? SetFn(&RpcServer::setRootObject) : SetFn(&RpcServer::setRootObjectWeak);
    };

    auto server = RpcServer::make();
    auto [isStrong1, isStrong2] = GetParam();
    auto binder1 = sp<BBinder>::make();
    IBinder* binderRaw1 = binder1.get();
    setRootObject(isStrong1)(server.get(), binder1);
    EXPECT_EQ(binderRaw1, server->getRootObject());
    binder1.clear();
    EXPECT_EQ((isStrong1 ? binderRaw1 : nullptr), server->getRootObject());

    auto binder2 = sp<BBinder>::make();
    IBinder* binderRaw2 = binder2.get();
    setRootObject(isStrong2)(server.get(), binder2);
    EXPECT_EQ(binderRaw2, server->getRootObject());
    binder2.clear();
    EXPECT_EQ((isStrong2 ? binderRaw2 : nullptr), server->getRootObject());
}

INSTANTIATE_TEST_CASE_P(BinderRpc, BinderRpcServerRootObject,
                        ::testing::Combine(::testing::Bool(), ::testing::Bool()));

class OneOffSignal {
public:
    // If notify() was previously called, or is called within |duration|, return true; else false.
    template <typename R, typename P>
    bool wait(std::chrono::duration<R, P> duration) {
        std::unique_lock<std::mutex> lock(mMutex);
        return mCv.wait_for(lock, duration, [this] { return mValue; });
    }
    void notify() {
        std::unique_lock<std::mutex> lock(mMutex);
        mValue = true;
        lock.unlock();
        mCv.notify_all();
    }

private:
    std::mutex mMutex;
    std::condition_variable mCv;
    bool mValue = false;
};

TEST(BinderRpc, Shutdown) {
    auto addr = allocateSocketAddress();
    unlink(addr.c_str());
    auto server = RpcServer::make();
    server->iUnderstandThisCodeIsExperimentalAndIWillNotUseItInProduction();
    ASSERT_TRUE(server->setupUnixDomainServer(addr.c_str()));
    auto joinEnds = std::make_shared<OneOffSignal>();

    // If things are broken and the thread never stops, don't block other tests. Because the thread
    // may run after the test finishes, it must not access the stack memory of the test. Hence,
    // shared pointers are passed.
    std::thread([server, joinEnds] {
        server->join();
        joinEnds->notify();
    }).detach();

    bool shutdown = false;
    for (int i = 0; i < 10 && !shutdown; i++) {
        usleep(300 * 1000); // 300ms; total 3s
        if (server->shutdown()) shutdown = true;
    }
    ASSERT_TRUE(shutdown) << "server->shutdown() never returns true";

    ASSERT_TRUE(joinEnds->wait(2s))
            << "After server->shutdown() returns true, join() did not stop after 2s";
}

} // namespace android

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    android::base::InitLogging(argv, android::base::StderrLogger, android::base::DefaultAborter);
    return RUN_ALL_TESTS();
}
