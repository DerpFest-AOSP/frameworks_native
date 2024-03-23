/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include <sysexits.h>
#include <unistd.h>

#include <filesystem>
#include <iostream>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/debug/BnAdbCallback.h>
#include <android/debug/IAdbManager.h>
#include <android/os/BnServiceManager.h>
#include <android/os/IServiceManager.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <binder/RpcServer.h>

#include "file.h"

using android::BBinder;
using android::defaultServiceManager;
using android::OK;
using android::RpcServer;
using android::sp;
using android::status_t;
using android::statusToString;
using android::String16;
using android::base::GetBoolProperty;
using android::base::InitLogging;
using android::base::LogdLogger;
using android::base::LogId;
using android::base::LogSeverity;
using android::base::StdioLogger;
using std::string_view_literals::operator""sv;

namespace {

const char* kLocalInetAddress = "127.0.0.1";
using ServiceRetriever = decltype(&android::IServiceManager::checkService);
using android::debug::IAdbManager;

int Usage(std::filesystem::path program) {
    auto basename = program.filename();
    // clang-format off
    LOG(ERROR) << R"(dispatch calls to RPC service.
Usage:
  )" << basename << R"( [-g] [-i <ip_address>] <service_name>
    <service_name>: the service to connect to.
  )" << basename << R"( [-g] manager
    Runs an RPC-friendly service that redirects calls to servicemanager.

  -g: use getService() instead of checkService().
  -i: use ip_address when setting up the server instead of '127.0.0.1'

  If successful, writes port number and a new line character to stdout, and
  blocks until killed.
  Otherwise, writes error message to stderr and exits with non-zero code.
)";
    // clang-format on
    return EX_USAGE;
}

int Dispatch(const char* name, const ServiceRetriever& serviceRetriever,
             const char* ip_address = kLocalInetAddress) {
    auto sm = defaultServiceManager();
    if (nullptr == sm) {
        LOG(ERROR) << "No servicemanager";
        return EX_SOFTWARE;
    }
    auto binder = std::invoke(serviceRetriever, defaultServiceManager(), String16(name));
    if (nullptr == binder) {
        LOG(ERROR) << "No service \"" << name << "\"";
        return EX_SOFTWARE;
    }
    auto rpcServer = RpcServer::make();
    if (nullptr == rpcServer) {
        LOG(ERROR) << "Cannot create RpcServer";
        return EX_SOFTWARE;
    }
    unsigned int port;
    if (status_t status = rpcServer->setupInetServer(ip_address, 0, &port); status != OK) {
        LOG(ERROR) << "setupInetServer failed: " << statusToString(status);
        return EX_SOFTWARE;
    }
    auto socket = rpcServer->releaseServer();
    auto keepAliveBinder = sp<BBinder>::make();
    auto status = binder->setRpcClientDebug(std::move(socket), keepAliveBinder);
    if (status != OK) {
        LOG(ERROR) << "setRpcClientDebug failed with " << statusToString(status);
        return EX_SOFTWARE;
    }
    LOG(INFO) << "Finish setting up RPC on service " << name << " on port " << port;

    std::cout << port << std::endl;

    TEMP_FAILURE_RETRY(pause());

    PLOG(FATAL) << "TEMP_FAILURE_RETRY(pause()) exits; this should not happen!";
    __builtin_unreachable();
}

// Wrapper that wraps a BpServiceManager as a BnServiceManager.
class ServiceManagerProxyToNative : public android::os::BnServiceManager {
public:
    ServiceManagerProxyToNative(const sp<android::os::IServiceManager>& impl) : mImpl(impl) {}
    android::binder::Status getService(const std::string&,
                                       android::sp<android::IBinder>*) override {
        // We can't send BpBinder for regular binder over RPC.
        return android::binder::Status::fromStatusT(android::INVALID_OPERATION);
    }
    android::binder::Status checkService(const std::string&,
                                         android::sp<android::IBinder>*) override {
        // We can't send BpBinder for regular binder over RPC.
        return android::binder::Status::fromStatusT(android::INVALID_OPERATION);
    }
    android::binder::Status addService(const std::string&, const android::sp<android::IBinder>&,
                                       bool, int32_t) override {
        // We can't send BpBinder for RPC over regular binder.
        return android::binder::Status::fromStatusT(android::INVALID_OPERATION);
    }
    android::binder::Status listServices(int32_t dumpPriority,
                                         std::vector<std::string>* _aidl_return) override {
        return mImpl->listServices(dumpPriority, _aidl_return);
    }
    android::binder::Status registerForNotifications(
            const std::string&, const android::sp<android::os::IServiceCallback>&) override {
        // We can't send BpBinder for RPC over regular binder.
        return android::binder::Status::fromStatusT(android::INVALID_OPERATION);
    }
    android::binder::Status unregisterForNotifications(
            const std::string&, const android::sp<android::os::IServiceCallback>&) override {
        // We can't send BpBinder for RPC over regular binder.
        return android::binder::Status::fromStatusT(android::INVALID_OPERATION);
    }
    android::binder::Status isDeclared(const std::string& name, bool* _aidl_return) override {
        return mImpl->isDeclared(name, _aidl_return);
    }
    android::binder::Status getDeclaredInstances(const std::string& iface,
                                                 std::vector<std::string>* _aidl_return) override {
        return mImpl->getDeclaredInstances(iface, _aidl_return);
    }
    android::binder::Status updatableViaApex(const std::string& name,
                                             std::optional<std::string>* _aidl_return) override {
        return mImpl->updatableViaApex(name, _aidl_return);
    }
    android::binder::Status getUpdatableNames(const std::string& apexName,
                                              std::vector<std::string>* _aidl_return) override {
        return mImpl->getUpdatableNames(apexName, _aidl_return);
    }
    android::binder::Status getConnectionInfo(
            const std::string& name,
            std::optional<android::os::ConnectionInfo>* _aidl_return) override {
        return mImpl->getConnectionInfo(name, _aidl_return);
    }
    android::binder::Status registerClientCallback(
            const std::string&, const android::sp<android::IBinder>&,
            const android::sp<android::os::IClientCallback>&) override {
        // We can't send BpBinder for RPC over regular binder.
        return android::binder::Status::fromStatusT(android::INVALID_OPERATION);
    }
    android::binder::Status tryUnregisterService(const std::string&,
                                                 const android::sp<android::IBinder>&) override {
        // We can't send BpBinder for RPC over regular binder.
        return android::binder::Status::fromStatusT(android::INVALID_OPERATION);
    }
    android::binder::Status getServiceDebugInfo(
            std::vector<android::os::ServiceDebugInfo>* _aidl_return) override {
        return mImpl->getServiceDebugInfo(_aidl_return);
    }

private:
    sp<android::os::IServiceManager> mImpl;
};

// Workaround for b/191059588.
// TODO(b/191059588): Once we can run RpcServer on single-threaded services,
//   `servicedispatcher manager` should call Dispatch("manager") directly.
int wrapServiceManager(const ServiceRetriever& serviceRetriever,
                       const char* ip_address = kLocalInetAddress) {
    auto sm = defaultServiceManager();
    if (nullptr == sm) {
        LOG(ERROR) << "No servicemanager";
        return EX_SOFTWARE;
    }
    auto service = std::invoke(serviceRetriever, defaultServiceManager(), String16("manager"));
    if (nullptr == service) {
        LOG(ERROR) << "No service called `manager`";
        return EX_SOFTWARE;
    }
    auto interface = android::os::IServiceManager::asInterface(service);
    if (nullptr == interface) {
        LOG(ERROR) << "Cannot cast service called `manager` to IServiceManager";
        return EX_SOFTWARE;
    }

    // Work around restriction that doesn't allow us to send proxy over RPC.
    interface = sp<ServiceManagerProxyToNative>::make(interface);
    service = ServiceManagerProxyToNative::asBinder(interface);

    auto rpcServer = RpcServer::make();
    rpcServer->setRootObject(service);
    unsigned int port;
    if (status_t status = rpcServer->setupInetServer(ip_address, 0, &port); status != OK) {
        LOG(ERROR) << "Unable to set up inet server: " << statusToString(status);
        return EX_SOFTWARE;
    }
    LOG(INFO) << "Finish wrapping servicemanager with RPC on port " << port;
    std::cout << port << std::endl;
    rpcServer->join();

    LOG(FATAL) << "Wrapped servicemanager exits; this should not happen!";
    __builtin_unreachable();
}

class AdbCallback : public android::debug::BnAdbCallback {
public:
    android::binder::Status onDebuggingChanged(bool enabled,
                                               android::debug::AdbTransportType) override {
        if (!enabled) {
            LOG(ERROR) << "ADB debugging disabled, exiting.";
            exit(EX_SOFTWARE);
        }
        return android::binder::Status::ok();
    }
};

void exitOnAdbDebuggingDisabled() {
    auto adb = android::waitForService<IAdbManager>(String16("adb"));
    CHECK(adb != nullptr) << "Unable to retrieve service adb";
    auto status = adb->registerCallback(sp<AdbCallback>::make());
    CHECK(status.isOk()) << "Unable to call IAdbManager::registerCallback: " << status;
}

// Log to logd. For warning and more severe messages, also log to stderr.
class ServiceDispatcherLogger {
public:
    void operator()(LogId id, LogSeverity severity, const char* tag, const char* file,
                    unsigned int line, const char* message) {
        mLogdLogger(id, severity, tag, file, line, message);
        if (severity >= LogSeverity::WARNING) {
            std::cout << std::flush;
            auto progname = std::filesystem::path(getprogname()).filename();
            std::cerr << progname << ": " << message << std::endl;
        }
    }

private:
    LogdLogger mLogdLogger{};
};

} // namespace

int main(int argc, char* argv[]) {
    InitLogging(argv, ServiceDispatcherLogger());

    if (!GetBoolProperty("ro.debuggable", false)) {
        LOG(ERROR) << "servicedispatcher is only allowed on debuggable builds.";
        return EX_NOPERM;
    }
    LOG(WARNING) << "WARNING: servicedispatcher is debug only. Use with caution.";

    int opt;
    ServiceRetriever serviceRetriever = &android::IServiceManager::checkService;
    char* ip_address = nullptr;
    while (-1 != (opt = getopt(argc, argv, "gi:"))) {
        switch (opt) {
            case 'g': {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                serviceRetriever = &android::IServiceManager::getService;
#pragma clang diagnostic pop
            } break;
            case 'i': {
                ip_address = optarg;
            } break;
            default: {
                return Usage(argv[0]);
            }
        }
    }

    android::ProcessState::self()->setThreadPoolMaxThreadCount(1);
    android::ProcessState::self()->startThreadPool();
    exitOnAdbDebuggingDisabled();

    if (optind + 1 != argc) return Usage(argv[0]);
    auto name = argv[optind];

    if (name == "manager"sv) {
        if (ip_address) {
            return wrapServiceManager(serviceRetriever, ip_address);
        } else {
            return wrapServiceManager(serviceRetriever);
        }
    }
    if (ip_address) {
        return Dispatch(name, serviceRetriever, ip_address);
    } else {
        return Dispatch(name, serviceRetriever);
    }
}
