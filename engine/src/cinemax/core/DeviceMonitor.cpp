#include "DeviceMonitor.h"

#include <juce_events/juce_events.h>

#include "cinemax/win/Win32Common.h"

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace cinemax
{
namespace
{
    class NotificationClient final : public IMMNotificationClient
    {
    public:
        explicit NotificationClient (std::function<void (DeviceMonitor::Event)> f) : post (std::move (f)) {}

        ULONG STDMETHODCALLTYPE AddRef() override  { return (ULONG) InterlockedIncrement (&refs); }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG r = (ULONG) InterlockedDecrement (&refs);
            if (r == 0)
                delete this;
            return r;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** ppv) override
        {
            if (riid == __uuidof (IUnknown) || riid == __uuidof (IMMNotificationClient))
            {
                *ppv = static_cast<IMMNotificationClient*> (this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged (LPCWSTR id, DWORD state) override
        {
            post ({ DeviceMonitor::Kind::StateChanged, id != nullptr ? id : L"", state, -1 });
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceAdded (LPCWSTR id) override
        {
            post ({ DeviceMonitor::Kind::Added, id != nullptr ? id : L"", 0, -1 });
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceRemoved (LPCWSTR id) override
        {
            post ({ DeviceMonitor::Kind::Removed, id != nullptr ? id : L"", 0, -1 });
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged (EDataFlow flow, ERole role, LPCWSTR id) override
        {
            if (flow == eRender)
                post ({ DeviceMonitor::Kind::DefaultChanged, id != nullptr ? id : L"", 0, (int) role });
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged (LPCWSTR id, const PROPERTYKEY key) override
        {
            if (key.fmtid == PKEY_AudioEngine_DeviceFormat.fmtid && key.pid == PKEY_AudioEngine_DeviceFormat.pid)
                post ({ DeviceMonitor::Kind::FormatChanged, id != nullptr ? id : L"", 0, -1 });
            return S_OK;
        }

    private:
        LONG refs = 1;
        std::function<void (DeviceMonitor::Event)> post;
    };
}

struct DeviceMonitor::Impl
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    NotificationClient* client = nullptr;
    std::shared_ptr<std::function<void (const Event&)>> handler;
};

DeviceMonitor::DeviceMonitor() : impl (std::make_unique<Impl>()) {}

DeviceMonitor::~DeviceMonitor()
{
    stop();
}

bool DeviceMonitor::start (std::function<void (const Event&)> h)
{
    stop();

    impl->handler = std::make_shared<std::function<void (const Event&)>> (std::move (h));
    std::weak_ptr<std::function<void (const Event&)>> weak = impl->handler;

    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&impl->enumerator))))
        return false;

    impl->client = new NotificationClient ([weak] (Event e)
    {
        juce::MessageManager::callAsync ([weak, e]
        {
            if (auto h = weak.lock())
                if (*h)
                    (*h) (e);
        });
    });

    if (FAILED (impl->enumerator->RegisterEndpointNotificationCallback (impl->client)))
    {
        impl->client->Release();
        impl->client = nullptr;
        return false;
    }

    return true;
}

void DeviceMonitor::stop()
{
    if (impl->enumerator != nullptr && impl->client != nullptr)
        impl->enumerator->UnregisterEndpointNotificationCallback (impl->client);

    if (impl->client != nullptr)
    {
        impl->client->Release();
        impl->client = nullptr;
    }

    impl->enumerator.Reset();
    impl->handler.reset();
}

} // namespace cinemax
