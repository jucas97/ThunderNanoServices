
#include "Module.h"
#include "SubsystemControl.h"

/*
    // Copy the code below to Butler class definition
    // Note: The Butler class must inherit from PluginHost::JSONRPC


*/

namespace WPEFramework {

namespace Plugin {

    // Registration
    //
    void SubsystemControl::RegisterAll()
    {

        PluginHost::JSONRPC::Register<JsonData::SubsystemControl::ActivateParamsData, Core::JSON::DecUInt32 >(_T("activate"), &SubsystemControl::activate, this);
        PluginHost::JSONRPC::Register<Core::JSON::EnumType<JsonData::SubsystemControl::SubsystemType>, void >(_T("deactivate"), &SubsystemControl::deactivate, this);
    }

    void SubsystemControl::UnregisterAll()
    {
        PluginHost::JSONRPC::Unregister(_T("deactivate"));
        PluginHost::JSONRPC::Unregister(_T("activate"));
    }

    // API implementation
    //

    uint32_t SubsystemControl::activate(const JsonData::SubsystemControl::ActivateParamsData& parameter, Core::JSON::DecUInt32& response) {

        return (Core::ERROR_NONE);
    }

    uint32_t SubsystemControl::deactivate(const Core::JSON::EnumType<JsonData::SubsystemControl::SubsystemType>& parameter) {

        return (Core::ERROR_NONE);        
    }

    // Event: activity - Notifies about device activity
    void SubsystemControl::event_activity()
    {
        PluginHost::JSONRPC::Notify(_T("activity"));
    }

} // namespace Plugin

}

