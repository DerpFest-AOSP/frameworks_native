/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "binderRpcTestCommon.h"

using namespace android;
using android::binder::ReadFdToString;
using android::binder::unique_fd;

class MyBinderRpcTestAndroid : public MyBinderRpcTestBase {
public:
    wp<RpcServer> server;

    Status countBinders(std::vector<int32_t>* out) override {
        return countBindersImpl(server, out);
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
        RpcMaybeThread([=] {
            LOG_ALWAYS_FATAL_IF(!strongServer->shutdown(), "Could not shutdown");
        }).detach();
        return Status::ok();
    }

    Status useKernelBinderCallingId() override {
        // this is WRONG! It does not make sense when using RPC binder, and
        // because it is SO wrong, and so much code calls this, it should abort!

        if constexpr (kEnableKernelIpc) {
            (void)IPCThreadState::self()->getCallingPid();
        }
        return Status::ok();
    }

    Status echoAsFile(const std::string& content, android::os::ParcelFileDescriptor* out) override {
        out->reset(mockFileDescriptor(content));
        return Status::ok();
    }

    Status concatFiles(const std::vector<android::os::ParcelFileDescriptor>& files,
                       android::os::ParcelFileDescriptor* out) override {
        std::string acc;
        for (const auto& file : files) {
            std::string result;
            LOG_ALWAYS_FATAL_IF(!ReadFdToString(file.get(), &result));
            acc.append(result);
        }
        out->reset(mockFileDescriptor(acc));
        return Status::ok();
    }

    HandoffChannel<unique_fd> mFdChannel;

    Status blockingSendFdOneway(const android::os::ParcelFileDescriptor& fd) override {
        mFdChannel.write(unique_fd(fcntl(fd.get(), F_DUPFD_CLOEXEC, 0)));
        return Status::ok();
    }

    Status blockingRecvFd(android::os::ParcelFileDescriptor* fd) override {
        fd->reset(mFdChannel.read());
        return Status::ok();
    }

    HandoffChannel<int> mIntChannel;

    Status blockingSendIntOneway(int n) override {
        mIntChannel.write(n);
        return Status::ok();
    }

    Status blockingRecvInt(int* n) override {
        *n = mIntChannel.read();
        return Status::ok();
    }
};

int main(int argc, char* argv[]) {
    __android_log_set_logger(__android_log_stderr_logger);

    LOG_ALWAYS_FATAL_IF(argc != 3, "Invalid number of arguments: %d", argc);
    unique_fd writeEnd(atoi(argv[1]));
    unique_fd readEnd(atoi(argv[2]));

    auto serverConfig = readFromFd<BinderRpcTestServerConfig>(readEnd);
    auto socketType = static_cast<SocketType>(serverConfig.socketType);
    auto rpcSecurity = static_cast<RpcSecurity>(serverConfig.rpcSecurity);

    std::vector<RpcSession::FileDescriptorTransportMode>
            serverSupportedFileDescriptorTransportModes;
    for (auto mode : serverConfig.serverSupportedFileDescriptorTransportModes) {
        serverSupportedFileDescriptorTransportModes.push_back(
                static_cast<RpcSession::FileDescriptorTransportMode>(mode));
    }

    auto certVerifier = std::make_shared<RpcCertificateVerifierSimple>();
    sp<RpcServer> server = RpcServer::make(newTlsFactory(rpcSecurity, certVerifier));

    LOG_ALWAYS_FATAL_IF(!server->setProtocolVersion(serverConfig.serverVersion));
    server->setMaxThreads(serverConfig.numThreads);
    server->setSupportedFileDescriptorTransportModes(serverSupportedFileDescriptorTransportModes);

    unsigned int outPort = 0;
    unique_fd socketFd(serverConfig.socketFd);

    switch (socketType) {
        case SocketType::PRECONNECTED:
            [[fallthrough]];
        case SocketType::UNIX:
            LOG_ALWAYS_FATAL_IF(OK != server->setupUnixDomainServer(serverConfig.addr.c_str()),
                                "%s", serverConfig.addr.c_str());
            break;
        case SocketType::UNIX_BOOTSTRAP:
            LOG_ALWAYS_FATAL_IF(OK !=
                                server->setupUnixDomainSocketBootstrapServer(std::move(socketFd)));
            break;
        case SocketType::UNIX_RAW:
            LOG_ALWAYS_FATAL_IF(OK != server->setupRawSocketServer(std::move(socketFd)));
            break;
        case SocketType::VSOCK:
            LOG_ALWAYS_FATAL_IF(OK !=
                                        server->setupVsockServer(VMADDR_CID_LOCAL,
                                                                 serverConfig.vsockPort),
                                "Need `sudo modprobe vsock_loopback`?");
            break;
        case SocketType::INET: {
            LOG_ALWAYS_FATAL_IF(OK != server->setupInetServer(kLocalInetAddress, 0, &outPort));
            LOG_ALWAYS_FATAL_IF(0 == outPort);
            break;
        }
        default:
            LOG_ALWAYS_FATAL("Unknown socket type");
    }

    BinderRpcTestServerInfo serverInfo;
    serverInfo.port = static_cast<int64_t>(outPort);
    serverInfo.cert.data = server->getCertificate(RpcCertificateFormat::PEM);
    writeToFd(writeEnd, serverInfo);
    auto clientInfo = readFromFd<BinderRpcTestClientInfo>(readEnd);

    if (rpcSecurity == RpcSecurity::TLS) {
        for (const auto& clientCert : clientInfo.certs) {
            LOG_ALWAYS_FATAL_IF(OK !=
                                certVerifier->addTrustedPeerCertificate(RpcCertificateFormat::PEM,
                                                                        clientCert.data));
        }
    }

    server->setPerSessionRootObject([&](wp<RpcSession> session, const void* addrPtr, size_t len) {
        {
            sp<RpcSession> spSession = session.promote();
            LOG_ALWAYS_FATAL_IF(nullptr == spSession.get());
        }

        // UNIX sockets with abstract addresses return
        // sizeof(sa_family_t)==2 in addrlen
        LOG_ALWAYS_FATAL_IF(len < sizeof(sa_family_t));
        const sockaddr* addr = reinterpret_cast<const sockaddr*>(addrPtr);
        sp<MyBinderRpcTestAndroid> service = sp<MyBinderRpcTestAndroid>::make();
        switch (addr->sa_family) {
            case AF_UNIX:
                // nothing to save
                break;
            case AF_VSOCK:
                LOG_ALWAYS_FATAL_IF(len != sizeof(sockaddr_vm));
                service->port = reinterpret_cast<const sockaddr_vm*>(addr)->svm_port;
                break;
            case AF_INET:
                LOG_ALWAYS_FATAL_IF(len != sizeof(sockaddr_in));
                service->port = ntohs(reinterpret_cast<const sockaddr_in*>(addr)->sin_port);
                break;
            case AF_INET6:
                LOG_ALWAYS_FATAL_IF(len != sizeof(sockaddr_in));
                service->port = ntohs(reinterpret_cast<const sockaddr_in6*>(addr)->sin6_port);
                break;
            default:
                LOG_ALWAYS_FATAL("Unrecognized address family %d", addr->sa_family);
        }
        service->server = server;
        return service;
    });

    server->join();

    // Another thread calls shutdown. Wait for it to complete.
    (void)server->shutdown();

    return 0;
}
