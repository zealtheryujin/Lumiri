#include "common.h"
#include "../common/stream_utils.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <vector>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
static const CLSID CLSID_PolicyConfigClient =
    { 0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9} };
static const IID IID_IPolicyConfig =
    { 0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8} };

static WCHAR g_prevDefaultId[512] = L"";

static bool setDefaultEndpoint(PCWSTR id) {
    IPolicyConfig* pc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
        IID_IPolicyConfig, (void**)&pc))) return false;
    bool ok = SUCCEEDED(pc->SetDefaultEndpoint(id, eConsole));
    pc->SetDefaultEndpoint(id, eMultimedia);
    pc->Release();
    return ok;
}

bool audioRouteToCable() {
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&en))) return false;

    WCHAR curId[512] = L"";
    IMMDevice* cur = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &cur))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(cur->GetId(&id))) { wcscpy_s(curId, id); CoTaskMemFree(id); }
        cur->Release();
    }

    bool routed = false;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n && !routed; i++) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal &&
                    (wcsstr(pv.pwszVal, L"CABLE Input") || wcsstr(pv.pwszVal, L"VB-Audio"))) {
                    LPWSTR id = nullptr;
                    if (SUCCEEDED(dev->GetId(&id))) {
                        if (wcscmp(id, curId) == 0) {
                            routed = true;
                        } else if (setDefaultEndpoint(id)) {
                            wcscpy_s(g_prevDefaultId, curId);
                            routed = true;
                            logmsg("Ses VB-Cable'a yönlendirildi (PC hoparlörü sessiz)");
                        }
                        CoTaskMemFree(id);
                    }
                }
                PropVariantClear(&pv);
                ps->Release();
            }
            dev->Release();
        }
        col->Release();
    }
    en->Release();
    if (!routed) logmsg("VB-Cable çıkışı bulunamadı, ses PC'den de çalmaya devam edecek");
    return routed;
}

bool hasCableDevice() {
    bool found = false;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&en))) return false;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n && !found; i++) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal &&
                    (wcsstr(pv.pwszVal, L"CABLE Input") || wcsstr(pv.pwszVal, L"VB-Audio")))
                    found = true;
                PropVariantClear(&pv);
                ps->Release();
            }
            dev->Release();
        }
        col->Release();
    }
    en->Release();
    return found;
}

void audioRestoreRoute() {
    if (g_prevDefaultId[0]) {
        setDefaultEndpoint(g_prevDefaultId);
        g_prevDefaultId[0] = 0;
        logmsg("Ses aygıtı eski haline döndürüldü");
    }
}

uint32_t g_audioRate = 48000;
uint16_t g_audioChannels = 2;

bool queryAudioFormat() {

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&en))) return false;
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) { en->Release(); return false; }
    IAudioClient* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) {
        dev->Release(); en->Release(); return false;
    }
    WAVEFORMATEX* wf = nullptr;
    if (SUCCEEDED(ac->GetMixFormat(&wf))) {
        g_audioRate = wf->nSamplesPerSec;
        CoTaskMemFree(wf);
    }
    ac->Release(); dev->Release(); en->Release();
    return true;
}

void audioThread() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    DWORD mmIdx = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIdx);
    if (mmTask) AvSetMmThreadPriority(mmTask, AVRT_PRIORITY_HIGH);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* wf = nullptr;
    HANDLE captureEvent = nullptr;
    bool eventMode = false;

    const char* fail = nullptr;
    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&en))) { fail = "enumerator"; break; }
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) { fail = "varsayılan aygıt"; break; }
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) { fail = "activate"; break; }
        if (FAILED(ac->GetMixFormat(&wf))) { fail = "mix format"; break; }
        captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (captureEvent) {
            HRESULT initHr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                20 * 10000  , 0, wf, nullptr);
            if (SUCCEEDED(initHr) && SUCCEEDED(ac->SetEventHandle(captureEvent)))
                eventMode = true;
        }
        if (!eventMode) {
            if (captureEvent) { CloseHandle(captureEvent); captureEvent = nullptr; }

            ac->Release(); ac = nullptr;
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) {
                fail = "polling activate"; break;
            }
            if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                10 * 10000   * 4, 0, wf, nullptr))) {
                fail = "loopback init";
                break;
            }
        }
        if (FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) { fail = "capture client"; break; }
        if (FAILED(ac->Start())) { fail = "start"; break; }

        logmsg("Ses: %u Hz %u kanal (loopback)", wf->nSamplesPerSec, wf->nChannels);
        const UINT srcCh = wf->nChannels;
        const bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             ((WAVEFORMATEXTENSIBLE*)wf)->SubFormat.Data1 == 3  );

        const UINT chunkFrames = rp::audioChunkFrames(wf->nSamplesPerSec);
        std::vector<int16_t> acc;
        acc.reserve(chunkFrames * 4);
        uint32_t chunkId = 0;
        bool logged = false;

        while (!g_session.stop) {
            if (eventMode) WaitForSingleObject(captureEvent, 20);
            else Sleep(2);
            for (;;) {
                UINT32 pktLen = 0;
                if (FAILED(cap->GetNextPacketSize(&pktLen)) || !pktLen) break;
                BYTE* data; UINT32 frames; DWORD flags;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                for (UINT32 i = 0; i < frames; i++) {
                    float l = 0, r = 0;
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {   }
                    else if (isFloat) {
                        const float* s = (const float*)data + (size_t)i * srcCh;
                        l = s[0]; r = srcCh > 1 ? s[1] : s[0];
                    } else {
                        const BYTE* s = data + (size_t)i * wf->nBlockAlign;
                        auto sample = [&](UINT ch) {
                            const BYTE* p = s + ch * (wf->wBitsPerSample / 8);
                            return rp::pcmSample(p, wf->wBitsPerSample);
                        };
                        l = sample(0); r = sample(srcCh > 1 ? 1 : 0);
                    }
                    auto clamp16 = [](float v) {
                        if (!std::isfinite(v)) return (int16_t)0;
                        return (int16_t)(v <= -1.f ? -32768 : v >= 1.f ? 32767 : v * 32767.f); };
                    acc.push_back(clamp16(l));
                    acc.push_back(clamp16(r));
                    if (acc.size() >= chunkFrames * 2) {
                        if (g_session.active && g_session.audioEnabled) {
                            sendMedia(RP_CH_AUDIO, 0, ++chunkId,
                                      (const uint8_t*)acc.data(), acc.size() * 2);
                            if (!logged) {
                                logged = true;
                                logmsg("Ses akışı gönderiliyor (%zu B / 5ms)", acc.size() * 2);
                            }
                        }
                        acc.clear();
                    }
                }
                cap->ReleaseBuffer(frames);
            }
        }
        ac->Stop();
    } while (0);
    if (fail) logmsg("Ses yakalama başarısız: %s adımı", fail);

    if (cap) cap->Release();
    if (wf) CoTaskMemFree(wf);
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (captureEvent) CloseHandle(captureEvent);
    if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
    CoUninitialize();
}
