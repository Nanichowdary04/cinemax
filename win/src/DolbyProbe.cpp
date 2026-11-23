// Which Dolby bitstreams an HDMI / optical output (TV, soundbar, AV receiver) can decode.
// Asks the driver with IAudioClient::IsFormatSupported in exclusive mode for IEC 61937
// formats. Nothing is opened or played; the answer comes from the TV's EDID via the driver.
#include "cinemax/win/Endpoints.h"

#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace cinemax::win
{
namespace
{
    bool supports (IAudioClient* client, const GUID& subtype, WORD channels, DWORD rate, DWORD mask)
    {
        WAVEFORMATEXTENSIBLE w {};
        w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        w.Format.nChannels = channels;
        w.Format.nSamplesPerSec = rate;
        w.Format.wBitsPerSample = 16;
        w.Format.nBlockAlign = (WORD) (channels * 2);
        w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
        w.Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
        w.Samples.wValidBitsPerSample = 16;
        w.dwChannelMask = mask;
        w.SubFormat = subtype;
        return client->IsFormatSupported (AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK;
    }
}

DolbySupport probeDolby (const std::wstring& endpointId)
{
    DolbySupport d;
    ComScope com;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&en))))
        return d;
    ComPtr<IMMDevice> dev;
    if (FAILED (en->GetDevice (endpointId.c_str(), &dev)))
        return d;
    ComPtr<IAudioClient> client;
    if (FAILED (dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) client.GetAddressOf())))
        return d;

    d.probed = true;
    // Dolby Digital (AC-3): IEC 61937 at 48 kHz, 2 "channels" carrying the bitstream.
    d.dolbyDigital = supports (client.Get(), KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL, 2, 48000, KSAUDIO_SPEAKER_STEREO);
    // Dolby Digital Plus (E-AC-3): IEC 61937 at 192 kHz.
    d.dolbyDigitalPlus = supports (client.Get(), KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS, 2, 192000, KSAUDIO_SPEAKER_STEREO);
    // Dolby Atmos: carried in DD+ (streaming services) or in TrueHD / MAT (high bit-rate, 8 lanes).
    d.atmos = supports (client.Get(), KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS_ATMOS, 2, 192000, KSAUDIO_SPEAKER_STEREO)
           || supports (client.Get(), KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP, 8, 192000, KSAUDIO_SPEAKER_7POINT1_SURROUND);
    return d;
}

} // namespace cinemax::win
