/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <sensor/ISensorServer.h>

#include <stdint.h>
#include <sys/types.h>

#include <cutils/native_handle.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/Timers.h>
#include <utils/Vector.h>

#include <binder/IInterface.h>
#include <binder/IResultReceiver.h>
#include <binder/Parcel.h>

#include <sensor/Sensor.h>
#include <sensor/ISensorEventConnection.h>

namespace android {
// ----------------------------------------------------------------------------

enum {
    GET_SENSOR_LIST = IBinder::FIRST_CALL_TRANSACTION,
    CREATE_SENSOR_EVENT_CONNECTION,
    ENABLE_DATA_INJECTION,
    GET_DYNAMIC_SENSOR_LIST,
    CREATE_SENSOR_DIRECT_CONNECTION,
    SET_OPERATION_PARAMETER,
    GET_RUNTIME_SENSOR_LIST,
    ENABLE_REPLAY_DATA_INJECTION,
    ENABLE_HAL_BYPASS_REPLAY_DATA_INJECTION,
};

class BpSensorServer : public BpInterface<ISensorServer>
{
public:
    explicit BpSensorServer(const sp<IBinder>& impl)
        : BpInterface<ISensorServer>(impl)
    {
    }

    virtual ~BpSensorServer();

    virtual Vector<Sensor> getSensorList(const String16& opPackageName)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ISensorServer::getInterfaceDescriptor());
        data.writeString16(opPackageName);
        remote()->transact(GET_SENSOR_LIST, data, &reply);
        Sensor s;
        Vector<Sensor> v;
        uint32_t n = reply.readUint32();
        // The size of the n Sensor elements on the wire is what we really want, but
        // this is better than nothing.
        if (n > reply.dataAvail()) {
            ALOGE("Failed to get a reasonable size of the sensor list. This is likely a "
                  "malformed reply parcel. Number of elements: %d, data available in reply: %zu",
                  n, reply.dataAvail());
            return v;
        }
        v.setCapacity(n);
        while (n) {
            n--;
            if(reply.read(s) != OK) {
                ALOGE("Failed to read reply from getSensorList");
                v.clear();
                break;
            }
            v.add(s);
        }
        return v;
    }

    virtual Vector<Sensor> getDynamicSensorList(const String16& opPackageName)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ISensorServer::getInterfaceDescriptor());
        data.writeString16(opPackageName);
        remote()->transact(GET_DYNAMIC_SENSOR_LIST, data, &reply);
        Sensor s;
        Vector<Sensor> v;
        uint32_t n = reply.readUint32();
        // The size of the n Sensor elements on the wire is what we really want, but
        // this is better than nothing.
        if (n > reply.dataAvail()) {
            ALOGE("Failed to get a reasonable size of the sensor list. This is likely a "
                  "malformed reply parcel. Number of elements: %d, data available in reply: %zu",
                  n, reply.dataAvail());
            return v;
        }
        v.setCapacity(n);
        while (n) {
            n--;
            if(reply.read(s) != OK) {
                ALOGE("Failed to read reply from getDynamicSensorList");
                v.clear();
                break;
            }
            v.add(s);
        }
        return v;
    }

    virtual Vector<Sensor> getRuntimeSensorList(const String16& opPackageName, int deviceId)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ISensorServer::getInterfaceDescriptor());
        data.writeString16(opPackageName);
        data.writeInt32(deviceId);
        remote()->transact(GET_RUNTIME_SENSOR_LIST, data, &reply);
        Sensor s;
        Vector<Sensor> v;
        uint32_t n = reply.readUint32();
        // The size of the n Sensor elements on the wire is what we really want, but
        // this is better than nothing.
        if (n > reply.dataAvail()) {
            ALOGE("Failed to get a reasonable size of the sensor list. This is likely a "
                  "malformed reply parcel. Number of elements: %d, data available in reply: %zu",
                  n, reply.dataAvail());
            return v;
        }
        v.setCapacity(n);
        while (n) {
            n--;
            reply.read(s);
            v.add(s);
        }
        return v;
    }

    virtual sp<ISensorEventConnection> createSensorEventConnection(const String8& packageName,
             int mode, const String16& opPackageName, const String16& attributionTag)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ISensorServer::getInterfaceDescriptor());
        data.writeString8(packageName);
        data.writeInt32(mode);
        data.writeString16(opPackageName);
        data.writeString16(attributionTag);
        remote()->transact(CREATE_SENSOR_EVENT_CONNECTION, data, &reply);
        return interface_cast<ISensorEventConnection>(reply.readStrongBinder());
    }

    virtual int isDataInjectionEnabled() {
        Parcel data, reply;
        data.writeInterfaceToken(ISensorServer::getInterfaceDescriptor());
        remote()->transact(ENABLE_DATA_INJECTION, data, &reply);
        return reply.readInt32();
    }

    virtual int isReplayDataInjectionEnabled() {
        Parcel data, reply;
        data.writeInterfaceToken(ISensorServer::getInterfaceDescriptor());
        remote()->transact(ENABLE_REPLAY_DATA_INJECTION, data, &reply);
        return reply.readInt32();
    }

    virtual int isHalBypassReplayDataInjectionEnabled() {
        Parcel data, reply;
        data.writeInterfaceToken(ISensorServer::getInterfaceDescriptor());
        remote()->transact(ENABLE_HAL_BYPASS_REPLAY_DATA_INJECTION, data, &reply);
        return reply.readInt32();
    }

    virtual sp<ISensorEventConnection> createSensorDirectConnection(const String16& opPackageName,
            int deviceId, uint32_t size, int32_t type, int32_t format,
            const native_handle_t *resource) {
        Parcel data, reply;
        data.writeInterfaceToken(ISensorServer::getInterfaceDescriptor());
        data.writeString16(opPackageName);
        data.writeInt32(deviceId);
        data.writeUint32(size);
        data.writeInt32(type);
        data.writeInt32(format);
        data.writeNativeHandle(resource);
        remote()->transact(CREATE_SENSOR_DIRECT_CONNECTION, data, &reply);
        return interface_cast<ISensorEventConnection>(reply.readStrongBinder());
    }

    virtual int setOperationParameter(int32_t handle, int32_t type,
                                      const Vector<float> &floats,
                                      const Vector<int32_t> &ints) {
        Parcel data, reply;
        data.writeInterfaceToken(ISensorServer::getInterfaceDescriptor());
        data.writeInt32(handle);
        data.writeInt32(type);
        data.writeUint32(static_cast<uint32_t>(floats.size()));
        for (auto i : floats) {
            data.writeFloat(i);
        }
        data.writeUint32(static_cast<uint32_t>(ints.size()));
        for (auto i : ints) {
            data.writeInt32(i);
        }
        remote()->transact(SET_OPERATION_PARAMETER, data, &reply);
        return reply.readInt32();
    }
};

// Out-of-line virtual method definition to trigger vtable emission in this
// translation unit (see clang warning -Wweak-vtables)
BpSensorServer::~BpSensorServer() {}

IMPLEMENT_META_INTERFACE(SensorServer, "android.gui.SensorServer");

// ----------------------------------------------------------------------

status_t BnSensorServer::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch(code) {
        case GET_SENSOR_LIST: {
            CHECK_INTERFACE(ISensorServer, data, reply);
            const String16& opPackageName = data.readString16();
            Vector<Sensor> v(getSensorList(opPackageName));
            size_t n = v.size();
            reply->writeUint32(static_cast<uint32_t>(n));
            for (size_t i = 0; i < n; i++) {
                reply->write(v[i]);
            }
            return NO_ERROR;
        }
        case CREATE_SENSOR_EVENT_CONNECTION: {
            CHECK_INTERFACE(ISensorServer, data, reply);
            String8 packageName = data.readString8();
            int32_t mode = data.readInt32();
            const String16& opPackageName = data.readString16();
            const String16& attributionTag = data.readString16();
            sp<ISensorEventConnection> connection(createSensorEventConnection(packageName, mode,
                    opPackageName, attributionTag));
            reply->writeStrongBinder(IInterface::asBinder(connection));
            return NO_ERROR;
        }
        case ENABLE_DATA_INJECTION: {
            CHECK_INTERFACE(ISensorServer, data, reply);
            int32_t ret = isDataInjectionEnabled();
            reply->writeInt32(static_cast<int32_t>(ret));
            return NO_ERROR;
        }
        case ENABLE_REPLAY_DATA_INJECTION: {
            CHECK_INTERFACE(ISensorServer, data, reply);
            int32_t ret = isReplayDataInjectionEnabled();
            reply->writeInt32(static_cast<int32_t>(ret));
            return NO_ERROR;
        }
        case ENABLE_HAL_BYPASS_REPLAY_DATA_INJECTION: {
            CHECK_INTERFACE(ISensorServer, data, reply);
            int32_t ret = isHalBypassReplayDataInjectionEnabled();
            reply->writeInt32(static_cast<int32_t>(ret));
            return NO_ERROR;
        }
        case GET_DYNAMIC_SENSOR_LIST: {
            CHECK_INTERFACE(ISensorServer, data, reply);
            const String16& opPackageName = data.readString16();
            Vector<Sensor> v(getDynamicSensorList(opPackageName));
            size_t n = v.size();
            reply->writeUint32(static_cast<uint32_t>(n));
            for (size_t i = 0; i < n; i++) {
                reply->write(v[i]);
            }
            return NO_ERROR;
        }
        case GET_RUNTIME_SENSOR_LIST: {
            CHECK_INTERFACE(ISensorServer, data, reply);
            const String16& opPackageName = data.readString16();
            const int deviceId = data.readInt32();
            Vector<Sensor> v(getRuntimeSensorList(opPackageName, deviceId));
            size_t n = v.size();
            reply->writeUint32(static_cast<uint32_t>(n));
            for (size_t i = 0; i < n; i++) {
                reply->write(v[i]);
            }
            return NO_ERROR;
        }
        case CREATE_SENSOR_DIRECT_CONNECTION: {
            CHECK_INTERFACE(ISensorServer, data, reply);
            const String16& opPackageName = data.readString16();
            const int deviceId = data.readInt32();
            uint32_t size = data.readUint32();
            int32_t type = data.readInt32();
            int32_t format = data.readInt32();
            native_handle_t *resource = data.readNativeHandle();
            // Avoid a crash in native_handle_close if resource is nullptr
            if (resource == nullptr) {
                return BAD_VALUE;
            }
            native_handle_set_fdsan_tag(resource);
            sp<ISensorEventConnection> ch = createSensorDirectConnection(
                    opPackageName, deviceId, size, type, format, resource);
            native_handle_close_with_tag(resource);
            native_handle_delete(resource);
            reply->writeStrongBinder(IInterface::asBinder(ch));
            return NO_ERROR;
        }
        case SET_OPERATION_PARAMETER: {
            CHECK_INTERFACE(ISensorServer, data, reply);
            int32_t handle;
            int32_t type;
            Vector<float> floats;
            Vector<int32_t> ints;
            uint32_t count;

            handle = data.readInt32();
            type = data.readInt32();

            count = data.readUint32();
            if (count > (data.dataAvail() / sizeof(float))) {
              return BAD_VALUE;
            }
            floats.resize(count);
            for (auto &i : floats) {
                i = data.readFloat();
            }

            count = data.readUint32();
            if (count > (data.dataAvail() / sizeof(int32_t))) {
              return BAD_VALUE;
            }
            ints.resize(count);
            for (auto &i : ints) {
                i = data.readInt32();
            }

            int32_t ret = setOperationParameter(handle, type, floats, ints);
            reply->writeInt32(ret);
            return NO_ERROR;
        }
        case SHELL_COMMAND_TRANSACTION: {
            int in = data.readFileDescriptor();
            int out = data.readFileDescriptor();
            int err = data.readFileDescriptor();
            int argc = data.readInt32();
            Vector<String16> args;
            for (int i = 0; i < argc && data.dataAvail() > 0; i++) {
               args.add(data.readString16());
            }
            sp<IBinder> unusedCallback;
            sp<IResultReceiver> resultReceiver;
            status_t status;
            if ((status = data.readNullableStrongBinder(&unusedCallback)) != NO_ERROR) {
                return status;
            }
            if ((status = data.readNullableStrongBinder(&resultReceiver)) != NO_ERROR) {
                return status;
            }
            status = shellCommand(in, out, err, args);
            if (resultReceiver != nullptr) {
                resultReceiver->send(status);
            }
            return NO_ERROR;
        }
    }
    return BBinder::onTransact(code, data, reply, flags);
}

// ----------------------------------------------------------------------------
}; // namespace android
