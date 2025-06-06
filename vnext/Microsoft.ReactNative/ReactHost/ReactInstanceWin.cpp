// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "ReactInstanceWin.h"

#include <AppModelHelpers.h>
#include <CallInvoker.h>
#include <CppRuntimeOptions.h>
#include <CreateInstance.h>
#include <CreateModules.h>
#include <JSCallInvokerScheduler.h>
#include <OInstance.h>
#include <PackagerConnection.h>
#include <QuirkSettings.h>
#include <Shared/DevServerHelper.h>
#include <Threading/MessageDispatchQueue.h>
#include <Threading/MessageQueueThreadFactory.h>
#include <TurboModuleManager.h>
#include <Utils/Helpers.h>
#include <Views/ViewManager.h>
#include <appModel.h>
#include <comUtil/qiCast.h>
#include <dispatchQueue/dispatchQueue.h>
#include <react/renderer/runtimescheduler/RuntimeScheduler.h>
#include <react/renderer/runtimescheduler/RuntimeSchedulerCallInvoker.h>
#include <winrt/Windows.Storage.h>
#include <tuple>
#include "BaseScriptStoreImpl.h"
#include "ChakraRuntimeHolder.h"
#include "CrashManager.h"
#include "DevMenu.h"
#include "DynamicWriter.h"
#include "HermesRuntimeHolder.h"
#include "IReactContext.h"
#include "IReactDispatcher.h"
#include "IReactNotificationService.h"
#include "JSI/JSExecutorFactorySettings.h"
#include "JsiApi.h"
#include "Modules/DevSettingsModule.h"
#include "Modules/ExceptionsManager.h"
#include "Modules/PlatformConstantsWinModule.h"
#include "Modules/ReactRootViewTagGenerator.h"
#include "Modules/SampleTurboModule.h"
#include "Modules/SourceCode.h"
#include "Modules/StatusBarManager.h"
#include "Modules/Timing.h"
#include "MoveOnCopy.h"
#include "MsoUtils.h"
#include "NativeModules.h"
#include "NativeModulesProvider.h"
#include "ReactCoreInjection.h"
#include "ReactErrorProvider.h"
#include "RedBox.h"
#include "Unicode.h"

#ifdef USE_FABRIC
#include <Fabric/Composition/UriImageManager.h>
#include <Fabric/FabricUIManagerModule.h>
#include <Fabric/WindowsComponentDescriptorRegistry.h>
#include <SchedulerSettings.h>
#include <jserrorhandler/JsErrorHandler.h>
#include <jsitooling/react/runtime/JSRuntimeFactory.h>
#include <react/nativemodule/core/ReactCommon/TurboModuleBinding.h>
#include <react/renderer/componentregistry/componentNameByReactViewName.h>
#include <react/renderer/componentregistry/native/NativeComponentRegistryBinding.h>
#include <react/runtime/PlatformTimerRegistry.h>
#include <react/runtime/TimerManager.h>
#include <react/threading/MessageQueueThreadImpl.h>
#endif

#if !defined(CORE_ABI) && !defined(USE_FABRIC)
#include <LayoutService.h>
#include <XamlUIService.h>
#include "Modules/NativeUIManager.h"
#include "Modules/PaperUIManagerModule.h"
#endif

#ifndef CORE_ABI
#include <Utils/UwpPreparedScriptStore.h>
#include <Utils/UwpScriptStore.h>
#include "ConfigureBundlerDlg.h"
#include "Modules/AccessibilityInfoModule.h"
#include "Modules/AlertModule.h"
#include "Modules/AppStateModule.h"
#include "Modules/AppThemeModuleUwp.h"
#include "Modules/ClipboardModule.h"
#include "Modules/DeviceInfoModule.h"
#include "Modules/I18nManagerModule.h"
#include "Modules/LinkingManagerModule.h"
#include "Modules/LogBoxModule.h"
#else
#include "Modules/DesktopTimingModule.h"
#endif
#include "Modules/ExceptionsManager.h"
#include "Modules/PlatformConstantsWinModule.h"
#include "Modules/ReactRootViewTagGenerator.h"
#include "Modules/SourceCode.h"
#include "Modules/StatusBarManager.h"

#if !defined(CORE_ABI) || defined(USE_FABRIC)
#include <Modules/ImageViewManagerModule.h>
#include "Modules/Animated/NativeAnimatedModule.h"
#endif

#if defined(USE_V8)
#include "JSI/V8RuntimeHolder.h"
#include "V8JSIRuntimeHolder.h"
#endif // USE_V8

namespace Microsoft::ReactNative {

void AddStandardViewManagers(
    std::vector<std::unique_ptr<Microsoft::ReactNative::IViewManager>> &viewManagers,
    const Mso::React::IReactContext &context) noexcept;

std::shared_ptr<facebook::react::IUIManager> CreateUIManager2(
    Mso::React::IReactContext *context,
    std::vector<Microsoft::ReactNative::IViewManager> &&viewManagers) noexcept;

} // namespace Microsoft::ReactNative

using namespace winrt::Microsoft::ReactNative;

namespace Mso::React {

//=============================================================================================
// LoadedCallbackGuard ensures that the OnReactInstanceLoaded is always called.
// It calls OnReactInstanceLoaded in destructor with a cancellation error.
// If loading was previously succeeded this call with an error code is ignored.
//=============================================================================================

struct LoadedCallbackGuard {
  LoadedCallbackGuard(ReactInstanceWin &reactInstance) noexcept : m_reactInstance{&reactInstance} {}

  LoadedCallbackGuard(const LoadedCallbackGuard &other) = delete;
  LoadedCallbackGuard &operator=(const LoadedCallbackGuard &other) = delete;

  LoadedCallbackGuard(LoadedCallbackGuard &&other) = default;
  LoadedCallbackGuard &operator=(LoadedCallbackGuard &&other) = default;

  ~LoadedCallbackGuard() noexcept {
    if (m_reactInstance) {
      m_reactInstance->OnReactInstanceLoaded(Mso::CancellationErrorProvider().MakeErrorCode(true));
    }
  }

 private:
  Mso::CntPtr<ReactInstanceWin> m_reactInstance;
};

struct BridgeUIBatchInstanceCallback final : public facebook::react::InstanceCallback {
  BridgeUIBatchInstanceCallback(Mso::WeakPtr<ReactInstanceWin> wkInstance) : m_wkInstance(wkInstance) {}
  virtual ~BridgeUIBatchInstanceCallback() = default;
  void onBatchComplete() override {
    if (auto instance = m_wkInstance.GetStrongPtr()) {
      auto state = instance->State();
      if (state != ReactInstanceState::HasError && state != ReactInstanceState::Unloaded) {
        if (instance->UseWebDebugger()) {
          // While using a CxxModule for UIManager (which we do when running under webdebugger)
          // We need to post the batch complete to the NativeQueue to ensure that the UIManager
          // has posted everything from this batch into its queue before we complete the batch.
          instance->m_jsDispatchQueue.Load().Post([wkInstance = m_wkInstance]() {
            if (auto instance = wkInstance.GetStrongPtr()) {
              instance->m_batchingUIThread->runOnQueue([wkInstance]() {
                if (auto instance = wkInstance.GetStrongPtr()) {
                  auto propBag = ReactPropertyBag(instance->m_reactContext->Properties());
                  if (auto callback = propBag.Get(winrt::Microsoft::ReactNative::implementation::ReactCoreInjection::
                                                      UIBatchCompleteCallbackProperty())) {
                    (*callback)(instance->m_reactContext->Properties());
                  }
#if !defined(CORE_ABI) && !defined(USE_FABRIC)
                  if (auto uiManager = Microsoft::ReactNative::GetNativeUIManager(*instance->m_reactContext).lock()) {
                    uiManager->onBatchComplete();
                  }
#endif
                }
              });

              // For UWP we use a batching message queue to optimize the usage
              // of the CoreDispatcher.  Win32 already has an optimized queue.
              facebook::react::BatchingMessageQueueThread *batchingUIThread =
                  static_cast<facebook::react::BatchingMessageQueueThread *>(instance->m_batchingUIThread.get());
              if (batchingUIThread != nullptr) {
                batchingUIThread->onBatchComplete();
              }
            }
          });
        } else {
          instance->m_batchingUIThread->runOnQueue([wkInstance = m_wkInstance]() {
            if (auto instance = wkInstance.GetStrongPtr()) {
              auto propBag = ReactPropertyBag(instance->m_reactContext->Properties());
              if (auto callback = propBag.Get(winrt::Microsoft::ReactNative::implementation::ReactCoreInjection::
                                                  UIBatchCompleteCallbackProperty())) {
                (*callback)(instance->m_reactContext->Properties());
              }
#if !defined(CORE_ABI) && !defined(USE_FABRIC)
              if (auto uiManager = Microsoft::ReactNative::GetNativeUIManager(*instance->m_reactContext).lock()) {
                uiManager->onBatchComplete();
              }
#endif
            }
          });
          // For UWP we use a batching message queue to optimize the usage
          // of the CoreDispatcher.  Win32 already has an optimized queue.
          facebook::react::BatchingMessageQueueThread *batchingUIThread =
              static_cast<facebook::react::BatchingMessageQueueThread *>(instance->m_batchingUIThread.get());
          if (batchingUIThread != nullptr) {
            batchingUIThread->onBatchComplete();
          }
        }
      }
    }
  }
  void incrementPendingJSCalls() override {}
  void decrementPendingJSCalls() override {}

  Mso::WeakPtr<ReactInstanceWin> m_wkInstance;
  Mso::CntPtr<Mso::React::ReactContext> m_context;
  std::weak_ptr<facebook::react::MessageQueueThread> m_uiThread;
};

//=============================================================================================
// ReactInstanceWin implementation
//=============================================================================================

/*static*/ std::mutex ReactInstanceWin::s_registryMutex;
/*static*/ std::vector<ReactInstanceWin *> ReactInstanceWin::s_instanceRegistry;

ReactInstanceWin::ReactInstanceWin(
    IReactHost &reactHost,
    ReactOptions const &options,
    Mso::Promise<void> &&whenCreated,
    Mso::Promise<void> &&whenLoaded,
    Mso::VoidFunctor &&updateUI) noexcept
    : Super{reactHost.NativeQueue()},
      m_weakReactHost{&reactHost},
      m_options{options},
      m_whenCreated{std::move(whenCreated)},
      m_isFastReloadEnabled(options.UseFastRefresh()),
      m_isLiveReloadEnabled(options.UseLiveReload()),
      m_updateUI{std::move(updateUI)},
      m_useWebDebugger(options.UseWebDebugger()),
      m_useDirectDebugger(options.UseDirectDebugger()),
      m_debuggerBreakOnNextLine(options.DebuggerBreakOnNextLine()),
      m_reactContext{Mso::Make<ReactContext>(
          this,
          options.Properties,
          winrt::make<implementation::ReactNotificationService>(options.Notifications))} {
  // As soon as the bundle is loaded or failed to load, we set the m_whenLoaded promise value in JS queue.
  // It then synchronously raises the OnInstanceLoaded event in the JS queue.
  // Then, we notify the ReactHost about the load event in the internal queue.
  m_whenLoaded.AsFuture()
      .Then<Mso::Executors::Inline>(
          [onLoaded = m_options.OnInstanceLoaded, reactContext = m_reactContext](Mso::Maybe<void> &&value) noexcept {
            auto errCode = value.IsError() ? value.TakeError() : Mso::ErrorCode();
            if (onLoaded) {
              onLoaded.Get()->Invoke(reactContext, errCode);
            }
            return Mso::Maybe<void>(errCode);
          })
      .Then(Queue(), [whenLoaded = std::move(whenLoaded)](Mso::Maybe<void> &&value) noexcept {
        whenLoaded.SetValue(std::move(value));
      });

  // When the JS queue is shutdown, we set the m_whenDestroyed promise value as the last work item in the JS queue.
  // No JS queue work can be done after that for the instance.
  // The promise continuation synchronously calls the OnInstanceDestroyed event.
  // Then, the Destroy() method returns the m_whenDestroyedResult future to ReactHost to handle instance destruction.
  m_whenDestroyedResult =
      m_whenDestroyed.AsFuture().Then<Mso::Executors::Inline>([whenLoaded = m_whenLoaded,
                                                               onDestroyed = m_options.OnInstanceDestroyed,
                                                               // If the ReactHost has been released, this
                                                               // instance might be the only thing keeping
                                                               // the propertyBag alive.
                                                               // We want it to remain alive for the
                                                               // InstanceDestroyed callbacks
                                                               propBag = m_options.Properties,
                                                               reactContext = m_reactContext]() noexcept {
        whenLoaded.TryCancel(); // It only has an effect if whenLoaded was not set before
        Microsoft::ReactNative::HermesRuntimeHolder::storeTo(ReactPropertyBag(reactContext->Properties()), nullptr);
        if (onDestroyed) {
          onDestroyed.Get()->Invoke(reactContext);
        }
      });

  // We notify the ReactHost immediately that the instance is created, but the
  // OnInstanceCreated event is raised only after the internal react-native instance is ready and
  // it starts handling JS queue work items.
  m_whenCreated.SetValue();

  if (m_options.EnableDefaultCrashHandler()) {
    CrashManager::RegisterCustomHandler();
  }

  {
    std::scoped_lock lock{s_registryMutex};
    s_instanceRegistry.push_back(this);
  }
}

ReactInstanceWin::~ReactInstanceWin() noexcept {
  std::scoped_lock lock{s_registryMutex};
  auto it = std::find(s_instanceRegistry.begin(), s_instanceRegistry.end(), this);
  if (it != s_instanceRegistry.end()) {
    s_instanceRegistry.erase(it);
  }

  if (m_options.EnableDefaultCrashHandler()) {
    CrashManager::UnregisterCustomHandler();
  }
}

void ReactInstanceWin::InstanceCrashHandler(int fileDescriptor) noexcept {
  if (!m_options.EnableDefaultCrashHandler()) {
    return;
  }

  if (m_jsiRuntimeHolder) {
    m_jsiRuntimeHolder->crashHandler(fileDescriptor);
  }

  // record additional information that could be useful for debugging crash dumps here
  // (perhaps properties and settings or clues about the react tree)
}

/*static*/ void ReactInstanceWin::CrashHandler(int fileDescriptor) noexcept {
  std::scoped_lock lock{s_registryMutex};
  for (auto &entry : s_instanceRegistry) {
    entry->InstanceCrashHandler(fileDescriptor);
  }
}

void ReactInstanceWin::LoadModules(
    const std::shared_ptr<facebook::react::DevSettings> &devSettings,
    const std::shared_ptr<winrt::Microsoft::ReactNative::NativeModulesProvider> &nativeModulesProvider,
    const std::shared_ptr<winrt::Microsoft::ReactNative::TurboModulesProvider> &turboModulesProvider) noexcept {
  auto registerTurboModule = [this, &nativeModulesProvider, &turboModulesProvider](
                                 const wchar_t *name, const ReactModuleProvider &provider) noexcept {
    if (m_options.UseWebDebugger()) {
      nativeModulesProvider->AddModuleProvider(name, provider);
    } else {
      turboModulesProvider->AddModuleProvider(name, provider, false);
    }
  };

#ifdef USE_FABRIC
  if (Microsoft::ReactNative::IsFabricEnabled(m_reactContext->Properties())) {
    registerTurboModule(
        L"FabricUIManagerBinding",
        winrt::Microsoft::ReactNative::MakeModuleProvider<::Microsoft::ReactNative::FabricUIManager>());
  }
#endif

#if !defined(CORE_ABI) && !defined(USE_FABRIC)
  registerTurboModule(
      L"UIManager",
      // TODO: Use MakeTurboModuleProvider after it satisfies ReactNativeSpecs::UIManagerSpec
      winrt::Microsoft::ReactNative::MakeModuleProvider<::Microsoft::ReactNative::UIManager>());
#endif

#ifndef CORE_ABI
  registerTurboModule(
      L"AccessibilityInfo",
      winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::AccessibilityInfo>());

  registerTurboModule(
      L"Alert", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::Alert>());

  registerTurboModule(
      L"Appearance", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::Appearance>());

  registerTurboModule(
      L"AppState", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::AppState>());

  registerTurboModule(
      L"AppTheme", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::AppTheme>());

  registerTurboModule(
      L"LogBox", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::LogBox>());

  registerTurboModule(
      L"Clipboard", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::Clipboard>());

  registerTurboModule(
      L"DeviceInfo", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::DeviceInfo>());

  registerTurboModule(
      L"ImageLoader", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::ImageLoader>());

  registerTurboModule(
      L"NativeAnimatedModule",
      winrt::Microsoft::ReactNative::MakeModuleProvider<::Microsoft::ReactNative::NativeAnimatedModule>());

#elif defined(CORE_ABI) && defined(USE_FABRIC)
  if (Microsoft::ReactNative::IsFabricEnabled(m_reactContext->Properties())) {
    registerTurboModule(
        L"ImageLoader",
        winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::ImageLoader>());

    registerTurboModule(
        L"NativeAnimatedModule",
        winrt::Microsoft::ReactNative::MakeModuleProvider<::Microsoft::ReactNative::NativeAnimatedModule>());
  }
#endif

  if (!m_options.UseWebDebugger()) {
    turboModulesProvider->AddModuleProvider(
        L"SampleTurboModule",
        winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::SampleTurboModule>(),
        false);
  }

  if (devSettings->useTurboModulesOnly) {
    ::Microsoft::ReactNative::ExceptionsManager::SetRedBoxHander(
        winrt::Microsoft::ReactNative::ReactPropertyBag(m_reactContext->Properties()), m_redboxHandler);
    registerTurboModule(
        L"ExceptionsManager",
        winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::ExceptionsManager>());

    registerTurboModule(
        L"StatusBarManager",
        winrt::Microsoft::ReactNative::MakeModuleProvider<::Microsoft::ReactNative::StatusBarManager>());

    registerTurboModule(
        L"PlatformConstants",
        winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::PlatformConstants>());
    uint32_t hermesBytecodeVersion = 0;
#if defined(USE_HERMES) && defined(ENABLE_DEVSERVER_HBCBUNDLES)
    hermesBytecodeVersion = ::hermes::hbc::BYTECODE_VERSION;
#endif

    std::string bundleUrl = (devSettings->useWebDebugger || devSettings->liveReloadCallback)
        ? facebook::react::DevServerHelper::get_BundleUrl(
              devSettings->sourceBundleHost,
              devSettings->sourceBundlePort,
              devSettings->debugBundlePath,
              devSettings->platformName,
              devSettings->bundleAppId,
              devSettings->devBundle,
              devSettings->useFastRefresh,
              devSettings->inlineSourceMap,
              hermesBytecodeVersion)
        : devSettings->bundleRootPath;
    ::Microsoft::ReactNative::SourceCode::SetScriptUrl(
        winrt::Microsoft::ReactNative::ReactPropertyBag(m_reactContext->Properties()), bundleUrl);

    registerTurboModule(
        L"SourceCode", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::SourceCode>());
  }

  registerTurboModule(
      L"DevSettings", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::DevSettings>());

#ifndef CORE_ABI
  registerTurboModule(
      L"I18nManager", winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::I18nManager>());

  registerTurboModule(
      L"LinkingManager",
      winrt::Microsoft::ReactNative::MakeTurboModuleProvider<::Microsoft::ReactNative::LinkingManager>());

  registerTurboModule(L"Timing", winrt::Microsoft::ReactNative::MakeModuleProvider<::Microsoft::ReactNative::Timing>());
#else

#if defined(USE_FABRIC)
  if (Microsoft::ReactNative::IsFabricEnabled(m_reactContext->Properties())) {
    registerTurboModule(
        L"Timing", winrt::Microsoft::ReactNative::MakeModuleProvider<::Microsoft::ReactNative::Timing>());
  } else
#endif
  {
    registerTurboModule(L"Timing", winrt::Microsoft::ReactNative::MakeModuleProvider<::facebook::react::Timing>());
  }
#endif

  registerTurboModule(
      ::Microsoft::React::GetWebSocketTurboModuleName(), ::Microsoft::React::GetWebSocketModuleProvider());

  if (!Microsoft::React::GetRuntimeOptionBool("Blob.DisableModule")) {
    registerTurboModule(::Microsoft::React::GetHttpTurboModuleName(), ::Microsoft::React::GetHttpModuleProvider());

    registerTurboModule(::Microsoft::React::GetBlobTurboModuleName(), ::Microsoft::React::GetBlobModuleProvider());

    registerTurboModule(
        ::Microsoft::React::GetFileReaderTurboModuleName(), ::Microsoft::React::GetFileReaderModuleProvider());
  }
}

//! Initialize() is called from the native queue.
void ReactInstanceWin::Initialize() noexcept {
#ifdef USE_FABRIC
  if (Microsoft::ReactNative::IsFabricEnabled(m_reactContext->Properties())) {
    InitializeBridgeless();
  } else
#endif
  {
    InitializeWithBridge();
  }
}

void ReactInstanceWin::InitDevMenu() noexcept {
  Microsoft::ReactNative::DevMenuManager::InitDevMenu(m_reactContext, [weakReactHost = m_weakReactHost]() noexcept {
#if !defined(CORE_ABI) && !defined(USE_FABRIC)
    Microsoft::ReactNative::ShowConfigureBundlerDialog(weakReactHost);
#endif // CORE_ABI
  });
}

void ReactInstanceWin::InitUIDependentCalls() noexcept {
#ifndef CORE_ABI
  Microsoft::ReactNative::AppThemeHolder::InitAppThemeHolder(GetReactContext());
  Microsoft::ReactNative::I18nManager::InitI18nInfo(
      winrt::Microsoft::ReactNative::ReactPropertyBag(Options().Properties));
  Microsoft::ReactNative::Appearance::InitOnUIThread(GetReactContext());
  Microsoft::ReactNative::DeviceInfoHolder::InitDeviceInfoHolder(GetReactContext());
#endif // CORE_ABI
}

std::shared_ptr<facebook::react::DevSettings> ReactInstanceWin::CreateDevSettings() noexcept {
  auto devSettings = std::make_shared<facebook::react::DevSettings>();
  devSettings->useJITCompilation = m_options.EnableJITCompilation;
  devSettings->sourceBundleHost = SourceBundleHost();
  devSettings->sourceBundlePort = SourceBundlePort();
  devSettings->inlineSourceMap = RequestInlineSourceMap();
  devSettings->debugBundlePath = DebugBundlePath();
  devSettings->liveReloadCallback = GetLiveReloadCallback();
  devSettings->errorCallback = GetErrorCallback();
  devSettings->loggingCallback = GetLoggingCallback();
  m_redboxHandler = devSettings->redboxHandler = std::move(GetRedBoxHandler());
  devSettings->useDirectDebugger = m_useDirectDebugger;
  devSettings->debuggerBreakOnNextLine = m_debuggerBreakOnNextLine;
  devSettings->debuggerPort = m_options.DeveloperSettings.DebuggerPort;
  devSettings->debuggerRuntimeName = m_options.DeveloperSettings.DebuggerRuntimeName;
  devSettings->useWebDebugger = m_useWebDebugger;
  devSettings->useFastRefresh = m_isFastReloadEnabled;
  devSettings->bundleRootPath = BundleRootPath();
  devSettings->platformName =
      winrt::Microsoft::ReactNative::implementation::ReactCoreInjection::GetPlatformName(m_reactContext->Properties());
  devSettings->waitingForDebuggerCallback = GetWaitingForDebuggerCallback();
  devSettings->debuggerAttachCallback = GetDebuggerAttachCallback();
  devSettings->enableDefaultCrashHandler = m_options.EnableDefaultCrashHandler();
  devSettings->bundleAppId = BundleAppId();
  devSettings->devBundle = RequestDevBundle();
  devSettings->showDevMenuCallback = [weakThis = Mso::WeakPtr{this}]() noexcept {
    if (auto strongThis = weakThis.GetStrongPtr()) {
      strongThis->m_uiQueue->Post(
          [context = strongThis->m_reactContext]() { Microsoft::ReactNative::DevMenuManager::Show(context); });
    }
  };

  bool useRuntimeScheduler = winrt::Microsoft::ReactNative::implementation::QuirkSettings::GetUseRuntimeScheduler(
      winrt::Microsoft::ReactNative::ReactPropertyBag(m_reactContext->Properties()));

  devSettings->useRuntimeScheduler = useRuntimeScheduler;

  return devSettings;
}

Mso::DispatchQueueSettings CreateDispatchQueueSettings(
    const winrt::Microsoft::ReactNative::IReactNotificationService &service) {
  Mso::DispatchQueueSettings queueSettings{};
  queueSettings.TaskStarting = [service](Mso::DispatchQueue const &) noexcept {
    service.SendNotification(
        winrt::Microsoft::ReactNative::ReactDispatcherHelper::JSDispatcherTaskStartingEventName(), nullptr, nullptr);
  };
  queueSettings.IdleWaitStarting = [service](Mso::DispatchQueue const &) noexcept {
    service.SendNotification(
        winrt::Microsoft::ReactNative::ReactDispatcherHelper::JSDispatcherIdleWaitStartingEventName(),
        nullptr,
        nullptr);
  };
  queueSettings.IdleWaitCompleted = [service](Mso::DispatchQueue const &) noexcept {
    service.SendNotification(
        winrt::Microsoft::ReactNative::ReactDispatcherHelper::JSDispatcherIdleWaitCompletedEventName(),
        nullptr,
        nullptr);
  };
  return queueSettings;
}

std::unique_ptr<facebook::jsi::PreparedScriptStore> CreatePreparedScriptStore() noexcept {
  std::unique_ptr<facebook::jsi::PreparedScriptStore> preparedScriptStore = nullptr;
  wchar_t tempPath[MAX_PATH];
  if (GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath)) {
    preparedScriptStore = std::make_unique<facebook::react::BasePreparedScriptStoreImpl>(winrt::to_string(tempPath));
  }
  return preparedScriptStore;
}

#ifdef USE_FABRIC

typedef HRESULT(__stdcall *SetThreadDescriptionFn)(HANDLE, PCWSTR);
void SetJSThreadDescription() noexcept {
  // Office still supports Server 2016 so we need to use Run Time Dynamic Linking and cannot just use:
  // ::SetThreadDescription(GetCurrentThread(), L"React-Native JavaScript Thread");

  auto moduleHandle = GetModuleHandleW(L"kernelbase.dll");
  // The description is just for developer experience, so we can skip it if kernelbase isn't already loaded
  if (!moduleHandle)
    return;

  auto proc = GetProcAddress(moduleHandle, "SetThreadDescription");
  if (!proc)
    return;

  reinterpret_cast<SetThreadDescriptionFn>(proc)(GetCurrentThread(), L"React-Native JavaScript Thread");
}

void ReactInstanceWin::InitializeBridgeless() noexcept {
  InitUIQueue();

  m_uiMessageThread.Exchange(std::make_shared<MessageDispatchQueue2>(
      *m_uiQueue, Mso::MakeWeakMemberFunctor(this, &ReactInstanceWin::OnError)));

  ReactPropertyBag(m_reactContext->Properties())
      .Set(
          winrt::Microsoft::ReactNative::implementation::ReactCoreInjection::PostToUIBatchingQueueProperty(),
          [wkBatchingUIThread = std::weak_ptr<facebook::react::MessageQueueThread>(m_uiMessageThread.Load())](
              winrt::Microsoft::ReactNative::ReactDispatcherCallback const &callback) {
            if (auto batchingUIThread = wkBatchingUIThread.lock()) {
              batchingUIThread->runOnQueue(callback);
            }
          });

  InitDevMenu();
  winrt::Microsoft::ReactNative::Composition::implementation::UriImageManager::Install(
      ReactPropertyBag(m_reactContext->Properties()), m_options.UriImageManager);

  m_uiQueue->Post([this, weakThis = Mso::WeakPtr{this}]() noexcept {
    // Objects that must be created on the UI thread
    if (auto strongThis = weakThis.GetStrongPtr()) {
      InitUIDependentCalls();

      strongThis->Queue().Post([this, weakThis]() noexcept {
        if (auto strongThis = weakThis.GetStrongPtr()) {
          auto devSettings = strongThis->CreateDevSettings();
          devSettings->useTurboModulesOnly = true;

          try {
            if (devSettings->useFastRefresh || devSettings->liveReloadCallback) {
              Microsoft::ReactNative::PackagerConnection::CreateOrReusePackagerConnection(*devSettings);
            }
            // null moduleProvider since native modules are not supported in bridgeless
            LoadModules(devSettings, nullptr, m_options.TurboModuleProvider);

            auto jsMessageThread = std::make_shared<facebook::react::MessageQueueThreadImpl>();
            m_jsMessageThread.Exchange(jsMessageThread);

            std::shared_ptr<facebook::react::CallInvoker> callInvoker;

            m_jsMessageThread.Load()->runOnQueueSync([&]() {
              SetJSThreadDescription();
              auto timerRegistry =
                  ::Microsoft::ReactNative::TimerRegistry::CreateTimerRegistry(m_reactContext->Properties());
              auto timerRegistryRaw = timerRegistry.get();

              auto timerManager = std::make_shared<facebook::react::TimerManager>(std::move(timerRegistry));
              timerRegistryRaw->setTimerManager(timerManager);

              auto jsErrorHandlingFunc = [this](
                                             facebook::jsi::Runtime &runtime,
                                             const facebook::react::JsErrorHandler::ProcessedError &error) noexcept {
                OnJSError(runtime, std::move(error));
              };

              if (devSettings->useDirectDebugger) {
                ::Microsoft::ReactNative::GetSharedDevManager()->EnsureHermesInspector(
                    devSettings->sourceBundleHost, devSettings->sourceBundlePort);
              }

              m_jsiRuntimeHolder = std::make_shared<Microsoft::ReactNative::HermesRuntimeHolder>(
                  devSettings, jsMessageThread, CreatePreparedScriptStore());
              auto jsRuntime = std::make_unique<Microsoft::ReactNative::HermesJSRuntime>(m_jsiRuntimeHolder);
              jsRuntime->getRuntime();
              m_bridgelessReactInstance = std::make_unique<facebook::react::ReactInstance>(
                  std::move(jsRuntime), jsMessageThread, timerManager, jsErrorHandlingFunc);

              auto bufferedRuntimeExecutor = m_bridgelessReactInstance->getBufferedRuntimeExecutor();
              timerManager->setRuntimeExecutor(bufferedRuntimeExecutor);

              Microsoft::ReactNative::SchedulerSettings::SetRuntimeScheduler(
                  winrt::Microsoft::ReactNative::ReactPropertyBag(m_options.Properties),
                  m_bridgelessReactInstance->getRuntimeScheduler());

              callInvoker = std::make_shared<facebook::react::RuntimeSchedulerCallInvoker>(
                  m_bridgelessReactInstance->getRuntimeScheduler());

              winrt::Microsoft::ReactNative::implementation::CallInvoker::SetProperties(
                  ReactPropertyBag(m_options.Properties),
                  winrt::make<winrt::Microsoft::ReactNative::implementation::CallInvoker>(
                      *m_reactContext, std::shared_ptr<facebook::react::CallInvoker>(callInvoker)));
            });

            m_options.TurboModuleProvider->SetReactContext(
                winrt::make<implementation::ReactContext>(Mso::Copy(m_reactContext)));

            facebook::react::ReactInstance::JSRuntimeFlags options;
            m_bridgelessReactInstance->initializeRuntime(
                options,
                [=, onCreated = m_options.OnInstanceCreated, reactContext = m_reactContext](
                    facebook::jsi::Runtime &runtime) {
                  auto logger = [loggingHook = GetLoggingCallback()](
                                    const std::string &message, unsigned int logLevel) {
                    if (loggingHook)
                      loggingHook(static_cast<facebook::react::RCTLogLevel>(logLevel), message.c_str());
                  };
                  facebook::react::bindNativeLogger(runtime, logger);

                  auto turboModuleManager =
                      std::make_shared<facebook::react::TurboModuleManager>(m_options.TurboModuleProvider, callInvoker);

                  auto binding =
                      [turboModuleManager](const std::string &name) -> std::shared_ptr<facebook::react::TurboModule> {
                    return turboModuleManager->getModule(name);
                  };

                  // Use a legacy native module binding that always returns null
                  // This means that calls to NativeModules.XXX will always return null, rather than crashing on access
                  auto legacyNativeModuleBinding =
                      [](const std::string & /*name*/) -> std::shared_ptr<facebook::react::TurboModule> {
                    return nullptr;
                  };

                  facebook::react::TurboModuleBinding::install(
                      runtime,
                      std::function(binding),
                      std::function(legacyNativeModuleBinding),
                      m_options.TurboModuleProvider->LongLivedObjectCollection());

                  auto componentDescriptorRegistry =
                      Microsoft::ReactNative::WindowsComponentDescriptorRegistry::FromProperties(
                          winrt::Microsoft::ReactNative::ReactPropertyBag(m_options.Properties));
                  auto hasComponentProvider = [componentDescriptorRegistry](const std::string &name) -> bool {
                    return componentDescriptorRegistry->hasComponentProvider(
                        facebook::react::componentNameByReactViewName(name));
                  };
                  facebook::react::bindHasComponentProvider(runtime, std::move(hasComponentProvider));

                  // init TurboModule
                  for (const auto &moduleName : turboModuleManager->getEagerInitModuleNames()) {
                    turboModuleManager->getModule(moduleName);
                  }

                  if (onCreated) {
                    onCreated.Get()->Invoke(reactContext);
                  }
                });

            LoadJSBundlesBridgeless(devSettings);
            SetupHMRClient();

          } catch (std::exception &e) {
            OnErrorWithMessage(e.what());
            OnErrorWithMessage("ReactInstanceWin: Failed to create React Instance.");
          } catch (winrt::hresult_error const &e) {
            OnErrorWithMessage(Microsoft::Common::Unicode::Utf16ToUtf8(e.message().c_str(), e.message().size()));
            OnErrorWithMessage("ReactInstanceWin: Failed to create React Instance.");
          } catch (...) {
            OnErrorWithMessage("ReactInstanceWin: Failed to create React Instance.");
          }
        }
      });
    }
  });
}
#endif

void ReactInstanceWin::FireInstanceCreatedCallback() noexcept {
  // The InstanceCreated event can be used to augment the JS environment for all JS code.  So it needs to be
  // triggered before any platform JS code is run. Using m_jsMessageThread instead of jsDispatchQueue avoids
  // waiting for the JSCaller which can delay the event until after certain JS code has already run
  m_jsMessageThread.Load()->runOnQueue(
      [onCreated = m_options.OnInstanceCreated, reactContext = m_reactContext]() noexcept {
        if (onCreated) {
          onCreated.Get()->Invoke(reactContext);
        }
      });
}

void ReactInstanceWin::InitializeWithBridge() noexcept {
  InitJSMessageThread();
  InitNativeMessageThread();

  InitUIQueue();
  InitUIMessageThread();

#if !defined(CORE_ABI) && !defined(USE_FABRIC)
  // InitUIManager uses m_legacyReactInstance
  InitUIManager();
#endif

  InitDevMenu();
#ifdef USE_FABRIC
  winrt::Microsoft::ReactNative::Composition::implementation::UriImageManager::Install(
      ReactPropertyBag(m_reactContext->Properties()), m_options.UriImageManager);
#endif

  m_uiQueue->Post([this, weakThis = Mso::WeakPtr{this}]() noexcept {
    // Objects that must be created on the UI thread
    if (auto strongThis = weakThis.GetStrongPtr()) {
      InitUIDependentCalls();
      strongThis->Queue().Post([this, weakThis]() noexcept {
        if (auto strongThis = weakThis.GetStrongPtr()) {
          auto devSettings = strongThis->CreateDevSettings();

          auto getBoolProperty = [properties = ReactPropertyBag{m_options.Properties}](
                                     const wchar_t *ns, const wchar_t *name, bool defaultValue) noexcept -> bool {
            ReactPropertyId<bool> propId{ns == nullptr ? ReactPropertyNamespace() : ReactPropertyNamespace(ns), name};
            std::optional<bool> propValue = properties.Get(propId);
            return propValue.value_or(defaultValue);
          };

          devSettings->omitNetworkingCxxModules = getBoolProperty(nullptr, L"OmitNetworkingCxxModules", false);
          devSettings->useWebSocketTurboModule = getBoolProperty(nullptr, L"UseWebSocketTurboModule", false);
          devSettings->useTurboModulesOnly = getBoolProperty(L"DevSettings", L"UseTurboModulesOnly", false);

          std::vector<facebook::react::NativeModuleDescription> cxxModules;
          auto nmp = std::make_shared<winrt::Microsoft::ReactNative::NativeModulesProvider>();

          LoadModules(devSettings, nmp, m_options.TurboModuleProvider);

          auto modules = nmp->GetModules(m_reactContext, m_jsMessageThread.Load());
          cxxModules.insert(
              cxxModules.end(), std::make_move_iterator(modules.begin()), std::make_move_iterator(modules.end()));

          if (m_options.ModuleProvider != nullptr) {
            std::vector<facebook::react::NativeModuleDescription> customCxxModules =
                m_options.ModuleProvider->GetModules(m_reactContext, m_jsMessageThread.Load());
            cxxModules.insert(std::end(cxxModules), std::begin(customCxxModules), std::end(customCxxModules));
          }

          std::unique_ptr<facebook::jsi::ScriptStore> scriptStore = nullptr;
          std::unique_ptr<facebook::jsi::PreparedScriptStore> preparedScriptStore = nullptr;

          if (const auto jsExecutorFactoryDelegate =
                  Microsoft::JSI::JSExecutorFactorySettings::GetJSExecutorFactoryDelegate(
                      winrt::Microsoft::ReactNative::ReactPropertyBag(strongThis->Options().Properties))) {
            devSettings->jsExecutorFactoryDelegate = jsExecutorFactoryDelegate;
            if (m_options.JsiEngine() == JSIEngine::Hermes) {
              devSettings->jsiEngineOverride = facebook::react::JSIEngineOverride::Hermes;
            }
          } else {
            switch (m_options.JsiEngine()) {
              case JSIEngine::Hermes: {
                preparedScriptStore = CreatePreparedScriptStore();

                auto hermesRuntimeHolder = std::make_shared<Microsoft::ReactNative::HermesRuntimeHolder>(
                    devSettings, m_jsMessageThread.Load(), std::move(preparedScriptStore));
                Microsoft::ReactNative::HermesRuntimeHolder::storeTo(
                    ReactPropertyBag(m_reactContext->Properties()), hermesRuntimeHolder);
                devSettings->jsiRuntimeHolder = hermesRuntimeHolder;
                break;
              }
              case JSIEngine::V8:
#if defined(USE_V8)
              {
                preparedScriptStore = CreatePreparedScriptStore();
                bool enableMultiThreadSupport{false};
#ifdef USE_FABRIC
                enableMultiThreadSupport = Microsoft::ReactNative::IsFabricEnabled(m_reactContext->Properties());
#endif // USE_FABRIC

                if (m_options.JsiEngineV8NodeApi()) {
                  devSettings->jsiRuntimeHolder = std::make_shared<Microsoft::ReactNative::V8RuntimeHolder>(
                      devSettings, m_jsMessageThread.Load(), std::move(preparedScriptStore), enableMultiThreadSupport);
                } else {
                  devSettings->jsiRuntimeHolder = std::make_shared<facebook::react::V8JSIRuntimeHolder>(
                      devSettings,
                      m_jsMessageThread.Load(),
                      std::move(scriptStore),
                      std::move(preparedScriptStore),
                      enableMultiThreadSupport);
                }

                break;
              }
#endif // USE_V8
              case JSIEngine::Chakra:
#ifndef CORE_ABI
                if (m_options.EnableByteCodeCaching || !m_options.ByteCodeFileUri.empty()) {
                  scriptStore = std::make_unique<Microsoft::ReactNative::UwpScriptStore>();
                  preparedScriptStore = std::make_unique<Microsoft::ReactNative::UwpPreparedScriptStore>(
                      winrt::to_hstring(m_options.ByteCodeFileUri));
                }
#endif
                devSettings->jsiRuntimeHolder = std::make_shared<Microsoft::JSI::ChakraRuntimeHolder>(
                    devSettings, m_jsMessageThread.Load(), std::move(scriptStore), std::move(preparedScriptStore));
                break;
            }
          }

          m_jsiRuntimeHolder = devSettings->jsiRuntimeHolder;

          try {
            // We need to keep the instance wrapper alive as its destruction shuts down the native queue.
            m_options.TurboModuleProvider->SetReactContext(
                winrt::make<implementation::ReactContext>(Mso::Copy(m_reactContext)));

            auto bundleRootPath = devSettings->bundleRootPath;
            auto jsiRuntimeHolder = devSettings->jsiRuntimeHolder;
            auto instanceWrapper = facebook::react::CreateReactInstance(
                std::shared_ptr<facebook::react::Instance>(strongThis->m_instance.Load()),
                std::move(bundleRootPath), // bundleRootPath
                std::move(cxxModules),
                m_options.TurboModuleProvider,
                m_options.TurboModuleProvider->LongLivedObjectCollection(),
                m_reactContext->Properties(),
                std::make_unique<BridgeUIBatchInstanceCallback>(weakThis),
                m_jsMessageThread.Load(),
                m_nativeMessageThread.Load(),
                std::move(devSettings));

            m_instanceWrapper.Exchange(std::move(instanceWrapper));

            FireInstanceCreatedCallback();
            LoadJSBundles();
            SetupHMRClient();

          } catch (std::exception &e) {
            OnErrorWithMessage(e.what());
            OnErrorWithMessage("UwpReactInstance: Failed to create React Instance.");
          } catch (winrt::hresult_error const &e) {
            OnErrorWithMessage(Microsoft::Common::Unicode::Utf16ToUtf8(e.message().c_str(), e.message().size()));
            OnErrorWithMessage("UwpReactInstance: Failed to create React Instance.");
          } catch (...) {
            OnErrorWithMessage("UwpReactInstance: Failed to create React Instance.");
          }
        }
      });
    };
  });
}

void ReactInstanceWin::SetupHMRClient() noexcept {
  if (UseDeveloperSupport() && State() != ReactInstanceState::HasError) {
    folly::dynamic params = folly::dynamic::array(
        winrt::Microsoft::ReactNative::implementation::ReactCoreInjection::GetPlatformName(
            m_reactContext->Properties()),
        DebugBundlePath(),
        SourceBundleHost(),
        SourceBundlePort(),
        m_isFastReloadEnabled,
        "ws");
    CallJsFunction("HMRClient", "setup", std::move(params));
  }
}

void ReactInstanceWin::LoadJSBundles() noexcept {
  //
  // We use m_jsMessageThread to load JS bundles synchronously. In that case we only load
  // them if the m_jsMessageThread is not shut down (quitSynchronous() is not called).
  // After the load we call OnReactInstanceLoaded callback on native queue.
  //
  // Note that the instance could be destroyed while we are loading JS Bundles.
  // Though, the JS engine is not destroyed until this work item is not finished.
  // Thus, we check the m_isDestroyed flag to see if we should do an early exit.
  // Also, since we have to guarantee that the OnReactInstanceLoaded callback is called before
  // the OnReactInstanceDestroyed callback, the OnReactInstanceLoaded is called right before the
  // OnReactInstanceDestroyed callback in the Destroy() method. In that case any OnReactInstanceLoaded
  // calls after we finish this JS message queue work item is ignored.
  //
  // The LoadedCallbackGuard is used for the case when runOnQueue does not execute the lambda
  // before destroying it. It may happen if the m_jsMessageThread is already shutdown.
  // In that case, the LoadedCallbackGuard notifies about cancellation by calling OnReactInstanceLoaded.
  // The OnReactInstanceLoaded internally only accepts the first call and ignores others.
  //

  if (m_useWebDebugger || m_isFastReloadEnabled) {
    // Getting bundle from the packager, so do everything async.
    auto instanceWrapper = m_instanceWrapper.LoadWithLock();
    instanceWrapper->loadBundle(Mso::Copy(JavaScriptBundleFile()));

    m_jsMessageThread.Load()->runOnQueue(
        [weakThis = Mso::WeakPtr{this},
         loadCallbackGuard = Mso::MakeMoveOnCopyWrapper(LoadedCallbackGuard{*this})]() noexcept {
          if (auto strongThis = weakThis.GetStrongPtr()) {
            if (strongThis->State() != ReactInstanceState::HasError) {
              strongThis->OnReactInstanceLoaded(Mso::ErrorCode{});
            }
          }
        });
  } else {
    m_jsMessageThread.Load()->runOnQueue(
        [weakThis = Mso::WeakPtr{this},
         loadCallbackGuard = Mso::MakeMoveOnCopyWrapper(LoadedCallbackGuard{*this})]() noexcept {
          if (auto strongThis = weakThis.GetStrongPtr()) {
            auto instance = strongThis->m_instance.LoadWithLock();
            auto instanceWrapper = strongThis->m_instanceWrapper.LoadWithLock();
            if (!instance || !instanceWrapper) {
              return;
            }

            try {
              instanceWrapper->loadBundleSync(Mso::Copy(strongThis->JavaScriptBundleFile()));
              if (strongThis->State() != ReactInstanceState::HasError) {
                strongThis->OnReactInstanceLoaded(Mso::ErrorCode{});
              }
            } catch (...) {
              strongThis->OnReactInstanceLoaded(Mso::ExceptionErrorProvider().MakeErrorCode(std::current_exception()));
            }
          }
        });
  }
}

#ifdef USE_FABRIC
void ReactInstanceWin::LoadJSBundlesBridgeless(std::shared_ptr<facebook::react::DevSettings> devSettings) noexcept {
  if (m_isFastReloadEnabled) {
    // Getting bundle from the packager, so do everything async.

    ::Microsoft::ReactNative::LoadRemoteUrlScript(
        devSettings,
        ::Microsoft::ReactNative::GetSharedDevManager(),
        Mso::Copy(JavaScriptBundleFile()),
        [=](std::unique_ptr<const facebook::react::JSBigStdString> script, const std::string &sourceURL) {
          m_bridgelessReactInstance->loadScript(std::move(script), sourceURL);
        });

    m_jsMessageThread.Load()->runOnQueue(
        [weakThis = Mso::WeakPtr{this},
         loadCallbackGuard = Mso::MakeMoveOnCopyWrapper(LoadedCallbackGuard{*this})]() noexcept {
          if (auto strongThis = weakThis.GetStrongPtr()) {
            if (strongThis->State() != ReactInstanceState::HasError) {
              strongThis->OnReactInstanceLoaded(Mso::ErrorCode{});
            }
          }
        });
  } else {
    auto bundleString = ::Microsoft::ReactNative::JsBigStringFromPath(devSettings, Mso::Copy(JavaScriptBundleFile()));
    m_bridgelessReactInstance->loadScript(std::move(bundleString), Mso::Copy(JavaScriptBundleFile()));

    m_jsMessageThread.Load()->runOnQueue(
        [weakThis = Mso::WeakPtr{this},
         loadCallbackGuard = Mso::MakeMoveOnCopyWrapper(LoadedCallbackGuard{*this})]() noexcept {
          if (auto strongThis = weakThis.GetStrongPtr()) {
            try {
              if (strongThis->State() != ReactInstanceState::HasError) {
                strongThis->OnReactInstanceLoaded(Mso::ErrorCode{});
              }
            } catch (...) {
              strongThis->OnReactInstanceLoaded(Mso::ExceptionErrorProvider().MakeErrorCode(std::current_exception()));
            }
          }
        });
  }
}
#endif

void ReactInstanceWin::OnReactInstanceLoaded(const Mso::ErrorCode &errorCode) noexcept {
  bool isLoadedExpected = false;
  if (m_isLoaded.compare_exchange_strong(isLoadedExpected, true)) {
    if (!errorCode) {
      m_state = ReactInstanceState::Loaded;
      m_whenLoaded.SetValue();
      DrainJSCallQueue();
    } else {
      m_state = ReactInstanceState::HasError;
      m_whenLoaded.SetError(errorCode);
      OnError(errorCode);
    }
  }
}

Mso::Future<void> ReactInstanceWin::Destroy() noexcept {
  // This method must be called from the native queue.
  VerifyIsInQueueElseCrash();

  if (m_isDestroyed) {
    return m_whenDestroyedResult;
  }

  m_isDestroyed = true;
  m_state = ReactInstanceState::Unloaded;
  AbandonJSCallQueue();

  // Make sure that the instance is not destroyed yet
  if (auto instance = m_instance.Exchange(nullptr)) {
    {
      // Release the JSI runtime
      std::scoped_lock lock{m_mutex};
      m_jsiRuntimeHolder = nullptr;
      m_jsiRuntime = nullptr;
    }
    // Release the message queues before the ui manager and instance.
    m_nativeMessageThread.Exchange(nullptr);
    m_jsMessageThread.Exchange(nullptr);
    m_instanceWrapper.Exchange(nullptr);
    m_jsDispatchQueue.Exchange(nullptr);
  }

#ifdef USE_FABRIC
  if (m_bridgelessReactInstance) {
    if (auto jsMessageThread = m_jsMessageThread.Exchange(nullptr)) {
      jsMessageThread->runOnQueueSync([&]() noexcept {
        {
          // Release the JSI runtime
          std::scoped_lock lock{m_mutex};

          this->m_jsiRuntimeHolder = nullptr;
          this->m_jsiRuntime = nullptr;
        }
        this->m_bridgelessReactInstance = nullptr;
        jsMessageThread->quitSynchronous();
        m_whenDestroyed.SetValue();
      });
    }
  }
#endif

  return m_whenDestroyedResult;
}

const ReactOptions &ReactInstanceWin::Options() const noexcept {
  return m_options;
}

ReactInstanceState ReactInstanceWin::State() const noexcept {
  return m_state;
}

void ReactInstanceWin::InitJSMessageThread() noexcept {
  m_instance.Exchange(std::make_shared<facebook::react::Instance>());

  auto callInvoker = m_instance.Load()->getJSCallInvoker();
  auto scheduler = Mso::MakeJSCallInvokerScheduler(
      CreateDispatchQueueSettings(m_reactContext->Notifications()),
      std::shared_ptr<facebook::react::CallInvoker>(callInvoker),
      Mso::MakeWeakMemberFunctor(this, &ReactInstanceWin::OnError),
      Mso::Copy(m_whenDestroyed));
  auto jsDispatchQueue = Mso::DispatchQueue::MakeCustomQueue(Mso::CntPtr(scheduler));

  winrt::Microsoft::ReactNative::implementation::CallInvoker::SetProperties(
      ReactPropertyBag(m_options.Properties),
      winrt::make<winrt::Microsoft::ReactNative::implementation::CallInvoker>(
          *m_reactContext, std::shared_ptr<facebook::react::CallInvoker>(callInvoker)));

  auto jsDispatcher =
      winrt::make<winrt::Microsoft::ReactNative::implementation::ReactDispatcher>(Mso::Copy(jsDispatchQueue));
  m_options.Properties.Set(ReactDispatcherHelper::JSDispatcherProperty(), jsDispatcher);

  m_jsMessageThread.Exchange(qi_cast<Mso::IJSCallInvokerQueueScheduler>(scheduler.Get())->GetMessageQueue());
  m_jsDispatchQueue.Exchange(std::move(jsDispatchQueue));
}

void ReactInstanceWin::InitNativeMessageThread() noexcept {
  // Native queue was already given us in constructor.
  m_nativeMessageThread.Exchange(
      std::make_shared<MessageDispatchQueue>(Queue(), Mso::MakeWeakMemberFunctor(this, &ReactInstanceWin::OnError)));
}

void ReactInstanceWin::InitUIQueue() noexcept {
  m_uiQueue = winrt::Microsoft::ReactNative::implementation::ReactDispatcher::GetUIDispatchQueue2(m_options.Properties);
  VerifyElseCrashSz(m_uiQueue, "No UI Dispatcher provided");
}

void ReactInstanceWin::InitUIMessageThread() noexcept {
  m_uiMessageThread.Exchange(std::make_shared<MessageDispatchQueue2>(
      *m_uiQueue, Mso::MakeWeakMemberFunctor(this, &ReactInstanceWin::OnError)));

  auto batchingUIThread = Microsoft::ReactNative::MakeBatchingQueueThread(m_uiMessageThread.Load());
  m_batchingUIThread = batchingUIThread;

  ReactPropertyBag(m_reactContext->Properties())
      .Set(
          winrt::Microsoft::ReactNative::implementation::ReactCoreInjection::PostToUIBatchingQueueProperty(),
          [wkBatchingUIThread = std::weak_ptr<facebook::react::BatchingMessageQueueThread>(batchingUIThread)](
              winrt::Microsoft::ReactNative::ReactDispatcherCallback const &callback) {
            if (auto batchingUIThread = wkBatchingUIThread.lock()) {
              batchingUIThread->runOnQueue(callback);
            }
          });

  m_jsDispatchQueue.Load().Post(
      [batchingUIThread, instance = std::weak_ptr<facebook::react::Instance>(m_instance.Load())]() noexcept {
        batchingUIThread->decoratedNativeCallInvokerReady(instance);
      });
}

#if !defined(CORE_ABI) && !defined(USE_FABRIC)
void ReactInstanceWin::InitUIManager() noexcept {
  std::vector<std::unique_ptr<Microsoft::ReactNative::IViewManager>> viewManagers;

  // Custom view managers
  if (m_options.ViewManagerProvider) {
    viewManagers = m_options.ViewManagerProvider->GetViewManagers(m_reactContext);
  }

  Microsoft::ReactNative::AddStandardViewManagers(viewManagers, *m_reactContext);

  auto uiManagerSettings = std::make_unique<Microsoft::ReactNative::UIManagerSettings>(
      m_batchingUIThread, m_uiMessageThread.Load(), std::move(viewManagers));
  Microsoft::ReactNative::UIManager::SetSettings(m_reactContext->Properties(), std::move(uiManagerSettings));

  m_reactContext->Properties().Set(
      implementation::XamlUIService::XamlUIServiceProperty().Handle(),
      winrt::make<implementation::XamlUIService>(m_reactContext));

  m_reactContext->Properties().Set(
      implementation::LayoutService::LayoutServiceProperty().Handle(),
      winrt::make<implementation::LayoutService>(m_reactContext));
}
#endif

facebook::react::NativeLoggingHook ReactInstanceWin::GetLoggingCallback() noexcept {
  if (m_options.OnLogging) {
    return [logCallback = m_options.OnLogging](facebook::react::RCTLogLevel logLevel, const char *message) {
      logCallback(static_cast<LogLevel>(logLevel), message);
    };
  } else {
    // When no logging callback was specified, use a default one in DEBUG builds
#if DEBUG
    return [telemetryTag{JavaScriptBundleFile()}](facebook::react::RCTLogLevel logLevel, const char *message) {
      std::ostringstream ss;
      ss << "ReactNative ['" << telemetryTag << "'] (";
      switch (logLevel) {
        case facebook::react::RCTLogLevel::Trace:
          ss << "trace";
          break;
        case facebook::react::RCTLogLevel::Info:
          ss << "info";
          break;
        case facebook::react::RCTLogLevel::Warning:
          ss << "warning";
          break;
        case facebook::react::RCTLogLevel::Error:
          ss << "error";
          break;
        case facebook::react::RCTLogLevel::Fatal:
          ss << "fatal";
          break;
      }
      ss << "): '" << message << "'\n";
      OutputDebugStringA(ss.str().c_str());
    };
#else
    return facebook::react::NativeLoggingHook{};
#endif
  }
}

std::shared_ptr<IRedBoxHandler> ReactInstanceWin::GetRedBoxHandler() noexcept {
  if (m_options.RedBoxHandler) {
    return m_options.RedBoxHandler;
#ifndef CORE_ABI
  } else if (UseDeveloperSupport()) {
    auto localWkReactHost = m_weakReactHost;
    return CreateDefaultRedBoxHandler(
        winrt::Microsoft::ReactNative::ReactPropertyBag(m_reactContext->Properties()),
        std::move(localWkReactHost),
        *m_uiQueue);
#endif
  } else {
    return {};
  }
}

std::function<void()> ReactInstanceWin::GetLiveReloadCallback() noexcept {
  // Live reload is enabled if we provide a callback function.
  if (m_isLiveReloadEnabled || m_isFastReloadEnabled) {
    return Mso::MakeWeakMemberStdFunction(this, &ReactInstanceWin::OnLiveReload);
  }
  return std::function<void()>{};
}

std::string ReactInstanceWin::GetBytecodeFileName() noexcept {
  // use bytecode caching if enabled and not debugging
  // (ChakraCore debugging does not work when bytecode caching is enabled)
  // TODO: implement
  // bool useByteCode = Mso::React::BytecodeOptimizationEnabled() && !m_options.DeveloperSettings.UseDirectDebugger;
  // return useByteCode ? Mso::React::GetBytecodeFilePath(m_options.Identity) : "";
  return "";
}

std::function<void(std::string)> ReactInstanceWin::GetErrorCallback() noexcept {
  return Mso::MakeWeakMemberStdFunction(this, &ReactInstanceWin::OnErrorWithMessage);
}

void ReactInstanceWin::OnErrorWithMessage(const std::string &errorMessage) noexcept {
  OnError(Mso::React::ReactErrorProvider().MakeErrorCode(Mso::React::ReactError{errorMessage.c_str()}));
}

void ReactInstanceWin::OnError(const Mso::ErrorCode &errorCode) noexcept {
  m_state = ReactInstanceState::HasError;
  AbandonJSCallQueue();

  if (m_redboxHandler && m_redboxHandler->isDevSupportEnabled()) {
    ErrorInfo errorInfo;
    errorInfo.Message = errorCode.ToString();
    errorInfo.Id = 0;
    m_redboxHandler->showNewError(std::move(errorInfo), ErrorType::Native);
  }

  InvokeInQueue([this, errorCode]() noexcept { m_options.OnError(errorCode); });

  m_updateUI();
}

#ifdef USE_FABRIC
void ReactInstanceWin::OnJSError(
    facebook::jsi::Runtime &runtime,
    const facebook::react::JsErrorHandler::ProcessedError &error) noexcept {
  ErrorInfo errorInfo;
  errorInfo.Message = error.message;
  auto errorCode = Mso::React::ReactErrorProvider().MakeErrorCode(Mso::React::ReactError{errorInfo.Message.c_str()});

  for (const facebook::react::JsErrorHandler::ProcessedError::StackFrame &frame : error.stack) {
    errorInfo.Callstack.push_back(
        {frame.file.value(), frame.methodName, frame.lineNumber.value(), frame.column.value()});
  }

  errorInfo.Id = error.id;

  bool isFatal = error.isFatal;

  m_state = ReactInstanceState::HasError;
  AbandonJSCallQueue();

  OnReactInstanceLoaded(errorCode);
  if (m_redboxHandler && m_redboxHandler->isDevSupportEnabled()) {
    m_redboxHandler->showNewError(std::move(errorInfo), isFatal ? ErrorType::JSFatal : ErrorType::JSSoft);
  }

  InvokeInQueue([this, errorCode]() noexcept { m_options.OnError(errorCode); });

  m_updateUI();
}
#endif

void ReactInstanceWin::OnLiveReload() noexcept {
  if (auto reactHost = m_weakReactHost.GetStrongPtr()) {
    reactHost->ReloadInstance();
  }
}

std::function<void()> ReactInstanceWin::GetWaitingForDebuggerCallback() noexcept {
  if (m_useWebDebugger) {
    return Mso::MakeWeakMemberStdFunction(this, &ReactInstanceWin::OnWaitingForDebugger);
  }

  return {};
}

void ReactInstanceWin::OnWaitingForDebugger() noexcept {
  auto state = m_state.load();
  while (state == ReactInstanceState::Loading) {
    if (m_state.compare_exchange_weak(state, ReactInstanceState::WaitingForDebugger)) {
      break;
    }
  }

  m_updateUI();
}

std::function<void()> ReactInstanceWin::GetDebuggerAttachCallback() noexcept {
  if (m_useWebDebugger) {
    return Mso::MakeWeakMemberStdFunction(this, &ReactInstanceWin::OnDebuggerAttach);
  }

  return {};
}

void ReactInstanceWin::OnDebuggerAttach() noexcept {
  m_updateUI();
}

void ReactInstanceWin::DrainJSCallQueue() noexcept {
  // Handle all items in the queue one by one.
  for (;;) {
    JSCallEntry entry; // To avoid callJSFunction under the lock
    {
      std::scoped_lock lock{m_mutex};
      if (m_state == ReactInstanceState::Loaded && !m_jsCallQueue.empty()) {
        entry = std::move(m_jsCallQueue.front());
        m_jsCallQueue.pop_front();
      } else {
        break;
      }
    }

#ifdef USE_FABRIC
    if (m_bridgelessReactInstance) {
      m_bridgelessReactInstance->callFunctionOnModule(entry.ModuleName, entry.MethodName, std::move(entry.Args));
    } else
#endif
        if (auto instance = m_instance.LoadWithLock()) {
      instance->callJSFunction(std::move(entry.ModuleName), std::move(entry.MethodName), std::move(entry.Args));
    }
  }
}

void ReactInstanceWin::AbandonJSCallQueue() noexcept {
  std::deque<JSCallEntry> jsCallQueue; // To avoid destruction under the lock
  {
    std::scoped_lock lock{m_mutex};
    if (m_state == ReactInstanceState::HasError || m_state == ReactInstanceState::Unloaded) {
      jsCallQueue = std::move(m_jsCallQueue);
    }
  }
}

void ReactInstanceWin::CallJsFunction(
    std::string &&moduleName,
    std::string &&method,
    folly::dynamic &&params) noexcept {
  bool shouldCall{false}; // To call callJSFunction outside of lock
  {
    std::scoped_lock lock{m_mutex};
    if (m_state == ReactInstanceState::Loaded && m_jsCallQueue.empty()) {
      shouldCall = true;
    } else if (
        m_state == ReactInstanceState::Loading || m_state == ReactInstanceState::WaitingForDebugger ||
        (m_state == ReactInstanceState::Loaded && !m_jsCallQueue.empty())) {
      m_jsCallQueue.push_back(JSCallEntry{std::move(moduleName), std::move(method), std::move(params)});
    }
    // otherwise ignore the call
  }

  if (shouldCall) {
#ifdef USE_FABRIC
    if (m_bridgelessReactInstance) {
      m_bridgelessReactInstance->callFunctionOnModule(moduleName, method, std::move(params));
    } else
#endif
        if (auto instance = m_instance.LoadWithLock()) {
      instance->callJSFunction(std::move(moduleName), std::move(method), std::move(params));
    }
  }
}

void ReactInstanceWin::DispatchEvent(int64_t viewTag, std::string &&eventName, folly::dynamic &&eventData) noexcept {
  folly::dynamic params = folly::dynamic::array(viewTag, std::move(eventName), std::move(eventData));
  CallJsFunction("RCTEventEmitter", "receiveEvent", std::move(params));
}

winrt::Microsoft::ReactNative::JsiRuntime ReactInstanceWin::JsiRuntime() noexcept {
  std::shared_ptr<Microsoft::JSI::RuntimeHolderLazyInit> jsiRuntimeHolder;
  {
    std::scoped_lock lock{m_mutex};
    if (m_jsiRuntime) {
      return m_jsiRuntime;
    } else {
      jsiRuntimeHolder = m_jsiRuntimeHolder;
    }
  }

  auto jsiRuntime = jsiRuntimeHolder ? jsiRuntimeHolder->getRuntime() : nullptr;

  {
    std::scoped_lock lock{m_mutex};
    if (!m_jsiRuntime && jsiRuntime) {
      // Set only if other thread did not do it yet.
      m_jsiRuntime =
          winrt::Microsoft::ReactNative::implementation::JsiRuntime::GetOrCreate(jsiRuntimeHolder, jsiRuntime);
    }

    return m_jsiRuntime;
  }
}

std::shared_ptr<facebook::react::Instance> ReactInstanceWin::GetInnerInstance() noexcept {
  return m_instance.LoadWithLock();
}

bool ReactInstanceWin::IsLoaded() const noexcept {
  return m_state == ReactInstanceState::Loaded;
}

void ReactInstanceWin::AttachMeasuredRootView(
    facebook::react::IReactRootView *rootView,
    const winrt::Microsoft::ReactNative::JSValueArgWriter &initialProps,
    bool useFabric) noexcept {
  if (State() == ReactInstanceState::HasError)
    return;

  assert(!useFabric);
#ifndef CORE_ABI
  if (!useFabric || m_useWebDebugger) {
    int64_t rootTag = -1;

#if !defined(CORE_ABI) && !defined(USE_FABRIC)
    if (auto uiManager = Microsoft::ReactNative::GetNativeUIManager(*m_reactContext).lock()) {
      rootTag = uiManager->AddMeasuredRootView(rootView);
      rootView->SetTag(rootTag);
    } else {
      assert(false);
    }
#endif

    std::string jsMainModuleName = rootView->JSComponentName();
    folly::dynamic params = folly::dynamic::array(
        std::move(jsMainModuleName),
        folly::dynamic::object("initialProps", DynamicWriter::ToDynamic(initialProps))("rootTag", rootTag)(
            "fabric", false));
    CallJsFunction("AppRegistry", "runApplication", std::move(params));
  }
#endif
}

void ReactInstanceWin::DetachRootView(facebook::react::IReactRootView *rootView, bool useFabric) noexcept {
  if (State() == ReactInstanceState::HasError)
    return;

  auto rootTag = rootView->GetTag();
  folly::dynamic params = folly::dynamic::array(rootTag);

#ifdef USE_FABRIC
  if (useFabric && !m_useWebDebugger) {
    auto uiManager = ::Microsoft::ReactNative::FabricUIManager::FromProperties(
        winrt::Microsoft::ReactNative::ReactPropertyBag(m_reactContext->Properties()));
    uiManager->stopSurface(static_cast<facebook::react::SurfaceId>(rootTag));
  } else
#endif
  {
    CallJsFunction("AppRegistry", "unmountApplicationComponentAtRootTag", std::move(params));
  }

  // Give the JS thread time to finish executing
  m_jsMessageThread.Load()->runOnQueueSync([]() {});
}

Mso::CntPtr<IReactInstanceInternal> MakeReactInstance(
    IReactHost &reactHost,
    ReactOptions &&options,
    Mso::Promise<void> &&whenCreated,
    Mso::Promise<void> &&whenLoaded,
    Mso::VoidFunctor &&updateUI) noexcept {
  return Mso::Make<ReactInstanceWin, IReactInstanceInternal>(
      reactHost, std::move(options), std::move(whenCreated), std::move(whenLoaded), std::move(updateUI));
}

bool ReactInstanceWin::UseWebDebugger() const noexcept {
  return m_useWebDebugger;
}

bool ReactInstanceWin::UseFastRefresh() const noexcept {
  return m_isFastReloadEnabled;
}

bool ReactInstanceWin::UseDirectDebugger() const noexcept {
  return m_useDirectDebugger;
}

bool ReactInstanceWin::DebuggerBreakOnNextLine() const noexcept {
  return m_debuggerBreakOnNextLine;
}

uint16_t ReactInstanceWin::DebuggerPort() const noexcept {
  return m_options.DeveloperSettings.DebuggerPort;
}

std::string ReactInstanceWin::DebugBundlePath() const noexcept {
  return m_options.DeveloperSettings.SourceBundleName.empty() ? m_options.Identity
                                                              : m_options.DeveloperSettings.SourceBundleName;
}

std::string ReactInstanceWin::BundleRootPath() const noexcept {
  return m_options.BundleRootPath.empty() ? "ms-appx:///Bundle/" : m_options.BundleRootPath;
}

std::string ReactInstanceWin::SourceBundleHost() const noexcept {
  return m_options.DeveloperSettings.SourceBundleHost.empty() ? facebook::react::DevServerHelper::DefaultPackagerHost
                                                              : m_options.DeveloperSettings.SourceBundleHost;
}

uint16_t ReactInstanceWin::SourceBundlePort() const noexcept {
  return m_options.DeveloperSettings.SourceBundlePort ? m_options.DeveloperSettings.SourceBundlePort
                                                      : facebook::react::DevServerHelper::DefaultPackagerPort;
}

bool ReactInstanceWin::RequestInlineSourceMap() const noexcept {
  return m_options.DeveloperSettings.RequestInlineSourceMap;
}

JSIEngine ReactInstanceWin::JsiEngine() const noexcept {
  return m_options.JsiEngine();
}

std::string ReactInstanceWin::JavaScriptBundleFile() const noexcept {
  return m_options.Identity;
}

std::string ReactInstanceWin::BundleAppId() const noexcept {
  return m_options.DeveloperSettings.BundleAppId;
}

bool ReactInstanceWin::RequestDevBundle() const noexcept {
  return m_options.DeveloperSettings.DevBundle;
}

bool ReactInstanceWin::UseDeveloperSupport() const noexcept {
  return m_options.UseDeveloperSupport();
}

Mso::React::IReactContext &ReactInstanceWin::GetReactContext() const noexcept {
  return *m_reactContext;
}

} // namespace Mso::React
