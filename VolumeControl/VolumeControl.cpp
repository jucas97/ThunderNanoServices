/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 Metrological
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include "VolumeControl.h"

namespace WPEFramework {
namespace Plugin {

    SERVICE_REGISTRATION(VolumeControl, 1, 0);

    const string VolumeControl::Initialize(PluginHost::IShell* service)
    {
        ASSERT (_service == nullptr);
        ASSERT (service != nullptr);

        _service = service;
        _service->Register(&_connectionNotification);

        string result;
        _implementation = _service->Root<Exchange::IVolumeControl>(_connectionId, 2000, _T("VolumeControlImplementation"));
        if (_implementation == nullptr) {
            result = _T("Couldn't create volume control instance");
        } else {
          _implementation->Register(&_volumeNotification);
          Exchange::JVolumeControl::Register(*this, _implementation);
        }

        return (result);
    }

    void VolumeControl::Deinitialize(PluginHost::IShell* service)
    {
        ASSERT(_service == service);

        Exchange::JVolumeControl::Unregister(*this);

        _service->Unregister(&_connectionNotification);
        _implementation->Unregister(&_volumeNotification);
            
        RPC::IRemoteConnection* connection(_service->RemoteConnection(_connectionId));

        VARIABLE_IS_NOT_USED uint32_t result = _implementation->Release();
        
        // It should have been the last reference we are releasing,
        // so it should end up in a DESCRUCTION_SUCCEEDED, if not we
        // are leaking...
        ASSERT(result == Core::ERROR_DESCRUCTION_SUCCEEDED);

        // If this was running in a (container) process...
        if (connection != nullptr) {
            // Lets trigger the cleanup sequence for
            // out-of-process code. Which will guard
            // that unwilling processes, get shot if
            // not stopped friendly :~)
            connection->Terminate();
            connection->Release();
        }

        _service = nullptr;
        _implementation = nullptr;
    }

    string VolumeControl::Information() const
    {
        return string();
    }

    void VolumeControl::Deactivated(RPC::IRemoteConnection* connection)
    {
        if (connection->Id() == _connectionId) {
            ASSERT(_service != nullptr);
            Core::IWorkerPool::Instance().Submit(PluginHost::IShell::Job::Create(_service,
                PluginHost::IShell::DEACTIVATED,
                PluginHost::IShell::FAILURE));
        }
    }

} // namespace Plugin
} // namespace WPEFramework
