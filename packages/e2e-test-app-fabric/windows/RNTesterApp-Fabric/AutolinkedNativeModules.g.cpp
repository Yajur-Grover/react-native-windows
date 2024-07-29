// AutolinkedNativeModules.g.cpp contents generated by "@react-native-community/cli autolink-windows"
// clang-format off
#include "pch.h"
#include "AutolinkedNativeModules.g.h"

// Includes from @react-native-windows/automation-channel
#include <winrt/AutomationChannel.h>

namespace winrt::Microsoft::ReactNative
{

void RegisterAutolinkedNativeModulePackages(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::ReactNative::IReactPackageProvider> const& packageProviders)
{ 
    // IReactPackageProviders from @react-native-windows/automation-channel
    packageProviders.Append(winrt::AutomationChannel::ReactPackageProvider());
}

}
