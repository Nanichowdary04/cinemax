#include "VolumeMirror.h"

#include <juce_events/juce_events.h>

#include "cinemax/win/Win32Common.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace cinemax::audio
{
namespace
{
    // Event context used for our own volume changes so their notifications are ignored.
    constexpr GUID kMirrorContext = { 0x3c1e9a52, 0x6b0d, 0x4f6e, { 0x8a, 0x77, 0x2d, 0x51, 0xc4, 0x90, 0xee, 0x31 } };

    class VolumeCallback final : public IAudioEndpointVolumeCallback
    {
    public:
        explicit VolumeCallback (std::function<void()> fn) : onChange (std::move (fn)) {}

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
            if (riid == __uuidof (IUnknown) || riid == __uuidof (IAudioEndpointVolumeCallback))
            {
                *ppv = static_cast<IAudioEndpointVolumeCallback*> (this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE OnNotify (PAUDIO_VOLUME_NOTIFICATION_DATA data) override
        {
            // Runs on a system thread: never block here, just post to the message thread.
            if (data != nullptr && IsEqualGUID (data->guidEventContext, kMirrorContext))
                return S_OK;
            if (onChange)
                onChange();
            return S_OK;
        }

    private:
        LONG refs = 1;
        std::function<void()> onChange;
    };
}

//==============================================================================
struct VolumeMirror::Impl
{
    Impl (std::atomic<float>& g, std::atomic<float>* d) : inputGain (g), deviceVolumeDb (d) {}

    std::atomic<float>& inputGain;
    std::atomic<float>* deviceVolumeDb = nullptr;
    ComPtr<IAudioEndpointVolume> source, output;
    VolumeCallback* sourceCb = nullptr;
    VolumeCallback* outputCb = nullptr;
    bool compensate = true;
    bool softwareVolume = true;
    std::shared_ptr<int> alive = std::make_shared<int> (0);

    // "Never louder on a switch": the level the user was listening at, and on which device.
    // Kept across pipeline restarts (the mirror lives as long as the engine).
    float listenLevel = -1.0f;
    std::wstring lastOutputId;
    // Just after a switch, a device may push its own (often high) volume - Bluetooth earbuds
    // send their absolute volume a moment after connecting. Until 'guardUntil', such rises
    // are undone; any volume change the user makes ends the guard.
    float guardLevel = -1.0f;
    double guardUntil = 0.0;

    static double now()
    {
        using namespace std::chrono;
        return duration<double> (steady_clock::now().time_since_epoch()).count();
    }

    static float levelOf (IAudioEndpointVolume* v)
    {
        float level = -1.0f;
        if (v == nullptr || FAILED (v->GetMasterVolumeLevelScalar (&level)))
            return -1.0f;
        return level;
    }

    void rememberLevel()
    {
        if (const float l = levelOf (source.Get()); l >= 0.0f)
            listenLevel = l;
    }

    static ComPtr<IAudioEndpointVolume> open (const std::wstring& id)
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioEndpointVolume> vol;
        if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&en))))
            return nullptr;
        if (FAILED (en->GetDevice (id.c_str(), &dev)))
            return nullptr;
        if (FAILED (dev->Activate (__uuidof (IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (vol.GetAddressOf()))))
            return nullptr;
        return vol;
    }

    void updateCompensation()
    {
        float gain = 1.0f;

        if (compensate && softwareVolume && source != nullptr)
        {
            float db = 0.0f;
            BOOL muted = FALSE;
            source->GetMute (&muted);
            if (SUCCEEDED (source->GetMasterVolumeLevel (&db)) && ! muted)
            {
                // Undo the software attenuation (cap at +60 dB; below that it's silence anyway).
                gain = (float) std::pow (10.0, std::min (60.0f, -db) / 20.0);
            }
        }

        inputGain.store (gain);

        if (deviceVolumeDb != nullptr)
        {
            float db = 0.0f;
            BOOL muted = FALSE;
            if (output != nullptr && SUCCEEDED (output->GetMasterVolumeLevel (&db)))
            {
                output->GetMute (&muted);
                deviceVolumeDb->store (muted ? -96.0f : std::clamp (db, -96.0f, 0.0f));
            }
            else
            {
                deviceVolumeDb->store (0.0f);
            }
        }
    }

    void copyVolume (IAudioEndpointVolume* from, IAudioEndpointVolume* to)
    {
        float level = 0.0f;
        BOOL muted = FALSE;
        if (from == nullptr || to == nullptr)
            return;
        if (SUCCEEDED (from->GetMasterVolumeLevelScalar (&level)))
            to->SetMasterVolumeLevelScalar (level, &kMirrorContext);
        if (SUCCEEDED (from->GetMute (&muted)))
            to->SetMute (muted, &kMirrorContext);
    }

    void sourceChanged()
    {
        guardUntil = 0.0;   // the user changed the volume: theirs from now on
        copyVolume (source.Get(), output.Get());
        updateCompensation();
        rememberLevel();
    }

    void outputChanged()
    {
        if (guardUntil > 0.0 && now() < guardUntil && guardLevel >= 0.0f && output != nullptr)
        {
            const float level = levelOf (output.Get());
            if (level > guardLevel + 0.005f)
            {
                output->SetMasterVolumeLevelScalar (guardLevel, &kMirrorContext);
                win::log::info ("Volume: the new device raised itself to %.0f%% - kept at %.0f%%",
                                level * 100.0f, guardLevel * 100.0f);
                return;
            }
        }
        copyVolume (output.Get(), source.Get());
        updateCompensation();
        rememberLevel();
    }

    std::function<void()> poster (void (Impl::*fn)())
    {
        std::weak_ptr<int> token = alive;
        return [this, token, fn]
        {
            juce::MessageManager::callAsync ([this, token, fn]
            {
                if (token.lock())
                    (this->*fn)();
            });
        };
    }
};

//==============================================================================
VolumeMirror::VolumeMirror (std::atomic<float>& inputGain, std::atomic<float>* deviceVolumeDb)
    : impl (std::make_unique<Impl> (inputGain, deviceVolumeDb)) {}

VolumeMirror::~VolumeMirror()
{
    stop();
}

bool VolumeMirror::start (const std::wstring& sourceId, const std::wstring& outputId, bool compensate)
{
    stop();

    impl->compensate = compensate;
    impl->source = Impl::open (sourceId);
    impl->output = Impl::open (outputId);

    if (impl->source == nullptr)
    {
        impl->inputGain.store (1.0f);
        return false;
    }

    DWORD support = 0;
    impl->softwareVolume = ! (SUCCEEDED (impl->source->QueryHardwareSupport (&support))
                              && (support & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0);

    // Start from the volume the user was listening at. Same device as before (or the first
    // start): that device's own level, as Windows had it. Another device (earbuds connected,
    // speakers picked...): the level just listened at, or the device's own if that is lower -
    // a switch never makes it louder.
    impl->guardUntil = 0.0;
    if (impl->output != nullptr)
    {
        const bool switched = impl->listenLevel >= 0.0f && ! impl->lastOutputId.empty()
                              && outputId != impl->lastOutputId;
        const float own = Impl::levelOf (impl->output.Get());
        if (switched && own >= 0.0f)
        {
            const float level = std::min (impl->listenLevel, own);
            impl->output->SetMasterVolumeLevelScalar (level, &kMirrorContext);
            impl->source->SetMasterVolumeLevelScalar (level, &kMirrorContext);
            BOOL muted = FALSE;
            if (SUCCEEDED (impl->output->GetMute (&muted)))
                impl->source->SetMute (muted, &kMirrorContext);
            impl->guardLevel = level;
            impl->guardUntil = Impl::now() + 5.0;
            win::log::info ("Volume: %.0f%% on the new device (its own setting was %.0f%%, you were listening at %.0f%%)",
                            level * 100.0f, own * 100.0f, impl->listenLevel * 100.0f);
        }
        else
        {
            impl->copyVolume (impl->output.Get(), impl->source.Get());
        }
    }
    impl->lastOutputId = outputId;
    impl->rememberLevel();

    impl->updateCompensation();

    impl->sourceCb = new VolumeCallback (impl->poster (&Impl::sourceChanged));
    impl->source->RegisterControlChangeNotify (impl->sourceCb);

    if (impl->output != nullptr)
    {
        impl->outputCb = new VolumeCallback (impl->poster (&Impl::outputChanged));
        impl->output->RegisterControlChangeNotify (impl->outputCb);
    }

    win::log::info ("Volume mirror started (source software volume: %s, compensate: %s)",
                    impl->softwareVolume ? "yes" : "no", compensate ? "yes" : "no");
    return true;
}

void VolumeMirror::stop()
{
    if (impl->source != nullptr && impl->sourceCb != nullptr)
        impl->source->UnregisterControlChangeNotify (impl->sourceCb);
    if (impl->output != nullptr && impl->outputCb != nullptr)
        impl->output->UnregisterControlChangeNotify (impl->outputCb);

    if (impl->sourceCb != nullptr) { impl->sourceCb->Release(); impl->sourceCb = nullptr; }
    if (impl->outputCb != nullptr) { impl->outputCb->Release(); impl->outputCb = nullptr; }

    impl->source.Reset();
    impl->output.Reset();
    impl->alive = std::make_shared<int> (0);   // invalidate pending posts
    impl->inputGain.store (1.0f);
}

void VolumeMirror::setCompensate (bool shouldCompensate)
{
    impl->compensate = shouldCompensate;
    impl->updateCompensation();
}

bool VolumeMirror::sourceUsesSoftwareVolume() const noexcept
{
    return impl->softwareVolume;
}

} // namespace cinemax::audio
