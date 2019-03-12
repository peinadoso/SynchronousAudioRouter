// SynchronousAudioRouter
// Copyright (C) 2015 Mackenzie Straight
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SynchronousAudioRouter.  If not, see <http://www.gnu.org/licenses/>.

#include "stdafx.h"
#include "mmwrapper.h"
#include "sarclient.h"
#include "utility.h"

namespace Sar {

SarClient::SarClient(
    const DriverConfig& driverConfig,
    const BufferConfig& bufferConfig)
    : _driverConfig(driverConfig), _bufferConfig(bufferConfig),
      _device(INVALID_HANDLE_VALUE), _completionPort(nullptr),
      _handleQueueStarted(false)
{
    ZeroMemory(&_handleQueueCompletion, sizeof(HandleQueueCompletion));
}

void SarClient::tick(long bufferIndex)
{
    ATLASSERT(bufferIndex == 0 || bufferIndex == 1);
    bool hasUpdatedNotificationHandles = false;

    // tick might be called from a different thread than the main thread.
    // guard against concurrent tick and close which cause the _sharedBuffer
    // to be invalidated. Accessing its stale value will cause a crash in that case.
    std::lock_guard<std::mutex> registersLockGuard(_registersLock);
    if (!_registers)
        return;

    if (_updateSampleRateOnTick.exchange(false)) {
        DWORD dummy;

        DeviceIoControl(_device, SAR_SEND_FORMAT_CHANGE_EVENT,
            nullptr, 0, nullptr, 0, &dummy, nullptr);
    }

    // for each endpoint
    // read isActive, generation and buffer offset/size/position
    //   if offset/size invalid, skip endpoint (fill asio buffers with 0)
    // if playback device:
    //   consume periodFrameSize * channelCount samples, demux to asio frames
    // if recording device:
    //   mux from asio frames
    // read isActive, generation
    //   if conflicted, skip endpoint and fill asio frames with 0
    // else increment position register
    for (size_t i = 0; i < _driverConfig.endpoints.size(); ++i) {
        auto& endpoint = _driverConfig.endpoints[i];
        auto& asioBuffers = _bufferConfig.asioBuffers[bufferIndex][i];
        auto asioBufferSize = (DWORD)(
            _bufferConfig.periodFrameSize * _bufferConfig.sampleSize);
        auto activeChannelCount = _registers[i].activeChannelCount;
        auto generation = _registers[i].generation;
        auto endpointBufferOffset = _registers[i].bufferOffset;
        auto endpointBufferSize = _registers[i].bufferSize;
        auto positionRegister = _registers[i].positionRegister;
        auto ntargets = (int)asioBuffers.size();
        auto frameChunkSize = asioBufferSize * activeChannelCount;
        auto notificationCount = _registers[i].notificationCount;

        if (!hasUpdatedNotificationHandles && notificationCount &&
            (GENERATION_NUMBER(generation) !=
             GENERATION_NUMBER(_notificationHandles[i].generation))) {

            updateNotificationHandles();
            hasUpdatedNotificationHandles = true;
        }

        // If endpoint is not active (no audio client), generate silence
        if (!GENERATION_IS_ACTIVE(generation) ||
            !endpointBufferSize ||
            positionRegister > endpointBufferSize ||
            endpointBufferOffset + endpointBufferSize > _sharedBufferSize) {
            for (int ti = 0; ti < ntargets; ++ti) {
                if (asioBuffers[ti]) {
                    ZeroMemory(asioBuffers[ti], asioBufferSize);
                }
            }

            continue;
        }

        auto nextPositionRegister =
            (positionRegister + frameChunkSize) % endpointBufferSize;
        auto position = positionRegister + endpointBufferOffset;
        void *endpointDataFirst = ((char *)_sharedBuffer) + position;
        void *endpointDataSecond =
            ((char *)_sharedBuffer) + endpointBufferOffset;
        auto firstSize = min(frameChunkSize, endpointBufferSize - positionRegister);
        auto secondSize = frameChunkSize - firstSize;

        if (endpoint.type == EndpointType::Playback) {
            demux(
                endpointDataFirst, firstSize,
                endpointDataSecond, secondSize,
                asioBuffers.data(), ntargets, activeChannelCount,
                asioBufferSize, _bufferConfig.sampleSize);
        } else {
            mux(
                endpointDataFirst, firstSize,
                endpointDataSecond, secondSize,
                asioBuffers.data(), ntargets, activeChannelCount,
                asioBufferSize, _bufferConfig.sampleSize);
        }

        auto lateGeneration = _registers[i].generation;

        if (!GENERATION_IS_ACTIVE(lateGeneration) ||
            (GENERATION_NUMBER(generation) !=
             GENERATION_NUMBER(lateGeneration))) {
            // The current generation changed, the client is not the same as before and
            // our data might be partially incomplete.
            // Discard everything and output silence on ASIO side

            for (int ti = 0; ti < ntargets; ++ti) {
                if (asioBuffers[ti]) {
                    ZeroMemory(asioBuffers[ti], asioBufferSize);
                }
            }
        } else {
            // Check if we need to notify client given NotificationCount from KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION
            // If NotificationCount == 1, notify only when crossing end of ring buffer
            // If NotificationCount == 2, notify at the mid-point and end of the ring buffer
            // Other values are not supported.
            // Crossing detection:
            //  - Detecting crossing end of buffer is done when:
            //    - The previous position was in the second part of the buffer
            //    - The next position is in the first part of the buffer
            //  - Detecting crossing mid-point of the buffer is done when:
            //    - The previous position was in the first part of the buffer
            //    - The next position is in the second part of the buffer
            if ((notificationCount >= 1 &&
                 positionRegister >= endpointBufferSize / 2 &&
                 nextPositionRegister < endpointBufferSize / 2) ||
                (notificationCount >= 2 &&
                 nextPositionRegister >= endpointBufferSize / 2 &&
                 positionRegister < endpointBufferSize / 2)) {

                auto evt = _notificationHandles[i].handle;
                auto targetGeneration = _notificationHandles[i].generation;

                if (evt &&
                    (GENERATION_NUMBER(targetGeneration) ==
                     GENERATION_NUMBER(generation))) {

                    _registers[i].positionRegister = nextPositionRegister;

                    if (!SetEvent(evt)) {
                        LOG(ERROR) << "SetEvent error " << GetLastError();
                    }
                } else {
                    // The handle generation is old, so it is not valid anymore => reset ASIO buffers to silence
                    for (int ti = 0; ti < ntargets; ++ti) {
                        if (asioBuffers[ti]) {
                            ZeroMemory(asioBuffers[ti], asioBufferSize);
                        }
                    }
                }
            } else {
                // No notification needed, just update the position register
                _registers[i].positionRegister = nextPositionRegister;
            }
        }
    }
}

bool SarClient::start()
{
    if (!openControlDevice()) {
        LOG(ERROR) << "Couldn't open control device";
        return false;
    }

    if (!openMmNotificationClient()) {
        LOG(ERROR) << "Couldn't open MMDevice notification client";
        stop();
        return false;
    }

    if (!setBufferLayout()) {
        LOG(ERROR) << "Couldn't set layout";
        stop();
        return false;
    }

    if (!createEndpoints()) {
        LOG(ERROR) << "Couldn't create endpoints";
        stop();
        return false;
    }

    if (_driverConfig.enableApplicationRouting && !enableRegistryFilter()) {
        LOG(ERROR) << "Couldn't enable registry filter";
    }

    return true;
}

void SarClient::stop()
{
    if (_mmNotificationClientRegistered) {
        _mmEnumerator->UnregisterEndpointNotificationCallback(
            _mmNotificationClient);
        _mmNotificationClientRegistered = false;
    }

    if (_mmNotificationClient) {
        _mmNotificationClient->Release();
        _mmNotificationClient = nullptr;
    }

    if (_mmEnumerator) {
        _mmEnumerator = nullptr;
    }

    if (_device != INVALID_HANDLE_VALUE) {
        _registersLock.lock();
        CancelIoEx(_device, nullptr);
        CloseHandle(_device);

        _device = INVALID_HANDLE_VALUE;
        _registers = nullptr;
        _sharedBuffer = nullptr;
        _sharedBufferSize = 0;
        _registersLock.unlock();
    }

    if (_completionPort) {
        CloseHandle(_completionPort);
        _completionPort = nullptr;
    }
}

bool SarClient::openControlDevice()
{
    HDEVINFO devinfo;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA interfaceDetail;
    DWORD requiredSize;

    interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    devinfo = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, nullptr, nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    if (devinfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    if (!SetupDiEnumDeviceInterfaces(devinfo, NULL,
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, 0, &interfaceData)) {

        SetupDiDestroyDeviceInfoList(devinfo);
        return false;
    }

    SetLastError(0);
    SetupDiGetDeviceInterfaceDetail(
        devinfo, &interfaceData, nullptr, 0, &requiredSize, nullptr);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        SetupDiDestroyDeviceInfoList(devinfo);
        return false;
    }

    interfaceDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(requiredSize);
    interfaceDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

    if (!SetupDiGetDeviceInterfaceDetail(
        devinfo, &interfaceData, interfaceDetail,
        requiredSize, nullptr, nullptr)) {

        free(interfaceDetail);
        SetupDiDestroyDeviceInfoList(devinfo);
        return false;
    }

    SetupDiDestroyDeviceInfoList(devinfo);

    _device = CreateFile(interfaceDetail->DevicePath,
        GENERIC_ALL, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED, nullptr);

    if (_device == INVALID_HANDLE_VALUE) {
        free(interfaceDetail);
        return false;
    }

    _completionPort = CreateIoCompletionPort(_device, nullptr, 0, 0);

    if (!_completionPort) {
        CloseHandle(_device);
        _device = nullptr;
        free(interfaceDetail);
        return false;
    }

    _notificationHandles.clear();
    _notificationHandles.resize(_driverConfig.endpoints.size());
    free(interfaceDetail);
    return true;
}

bool SarClient::openMmNotificationClient()
{
    if (FAILED(CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (LPVOID *)&_mmEnumerator))) {

        return false;
    }

    if (FAILED(CComObject<NotificationClient>::CreateInstance(
        &_mmNotificationClient))) {

        return false;
    }

    _mmNotificationClient->AddRef();
    _mmNotificationClient->setClient(shared_from_this());

    if (FAILED(_mmEnumerator->RegisterEndpointNotificationCallback(
        _mmNotificationClient))) {

        return false;
    }

    _mmNotificationClientRegistered = true;
    return true;
}

bool SarClient::setBufferLayout()
{
    SarSetBufferLayoutRequest request = {};
    SarSetBufferLayoutResponse response = {};
    DWORD dummy;

    request.bufferSize = 1024 * 1024 * 16; // TODO: size based on endpoint config
    request.periodSizeBytes =
        _bufferConfig.periodFrameSize * _bufferConfig.sampleSize;
    request.sampleRate = _bufferConfig.sampleRate;
    request.sampleSize = _bufferConfig.sampleSize;

    if (_driverConfig.waveRtMinimumFrames >= 2) {
        request.minimumFrameCount = _driverConfig.waveRtMinimumFrames;
    }

    if (!DeviceIoControl(_device, SAR_SET_BUFFER_LAYOUT,
        (LPVOID)&request, sizeof(request), (LPVOID)&response, sizeof(response),
        &dummy, nullptr)) {

        return false;
    }

    _sharedBuffer = response.virtualAddress;
    _sharedBufferSize = response.actualSize;
    _registers = (SarEndpointRegisters *)
        ((char *)response.virtualAddress + response.registerBase);
    return true;
}

bool SarClient::createEndpoints()
{
    int i = 0;
    DWORD dummy;

    for (auto& endpoint : _driverConfig.endpoints) {
        SarCreateEndpointRequest request = {};

        request.type = endpoint.type == EndpointType::Playback ?
            SAR_ENDPOINT_TYPE_PLAYBACK : SAR_ENDPOINT_TYPE_RECORDING;
        request.channelCount = endpoint.channelCount;
        request.index = i++;
        wcscpy_s(request.name, endpoint.description.c_str());
        wcscpy_s(request.id, UTF8ToWide(endpoint.id).c_str());

        if (!DeviceIoControl(_device, SAR_CREATE_ENDPOINT,
            (LPVOID)&request, sizeof(request), nullptr, 0, &dummy, nullptr)) {

            LOG(ERROR) << "Endpoint creation for " << TCHARToUTF8(endpoint.description.c_str())
               << " failed.";
            return false;
        }
    }

    return true;
}

bool SarClient::enableRegistryFilter()
{
    DWORD dummy;

    return DeviceIoControl(_device, SAR_START_REGISTRY_FILTER,
        nullptr, 0, nullptr, 0, &dummy, nullptr) == TRUE;
}

void SarClient::updateNotificationHandles()
{
    bool startNewOperation = false;
    BOOL status;
    DWORD error;

    if (_handleQueueStarted) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;

        if (GetQueuedCompletionStatus(
            _completionPort, &bytes, &key, &overlapped, 0)) {

            processNotificationHandleUpdates(
                bytes / sizeof(SarHandleQueueResponse));
            startNewOperation = true;
        } else if (overlapped) {
            // Ignore failed operations. Can happen due to tick/stop races.
            startNewOperation = true;
        }
    } else {
        _handleQueueStarted = true;
        startNewOperation = true;
    }

    if (!startNewOperation) {
        return;
    }

    ZeroMemory((LPOVERLAPPED)&_handleQueueCompletion, sizeof(OVERLAPPED));
    status = DeviceIoControl(
        _device, SAR_WAIT_HANDLE_QUEUE, nullptr, 0,
        _handleQueueCompletion.responses,
        sizeof(_handleQueueCompletion.responses),
        nullptr, &_handleQueueCompletion);
    error = GetLastError();

    // DeviceIoControl completed synchronously, so process the result.
    if (status) {
        processNotificationHandleUpdates(
            (int)(_handleQueueCompletion.InternalHigh /
                  sizeof(SarHandleQueueResponse)));
        _handleQueueStarted = false;
    }

    if (error != ERROR_IO_PENDING) {
        // Ignore failed operations. Can happen due to tick/stop races.
        _handleQueueStarted = false;
    }
}

void SarClient::processNotificationHandleUpdates(int updateCount)
{
    for (int i = 0; i < updateCount; ++i) {
        SarHandleQueueResponse *response = &_handleQueueCompletion.responses[i];
        DWORD endpointIndex = (DWORD)(response->associatedData >> 32);
        DWORD generation = (DWORD)(response->associatedData & 0xFFFFFFFF);

        if (_notificationHandles[endpointIndex].handle) {
            CloseHandle(_notificationHandles[endpointIndex].handle);
        }

        _notificationHandles[endpointIndex].generation = generation;
        _notificationHandles[endpointIndex].handle = response->handle;
    }
}

void SarClient::demux(
    void *muxBufferFirst, size_t firstSize,
    void *muxBufferSecond, size_t secondSize,
    void **targetBuffers, int ntargets, int nsources,
    size_t targetSize, int sampleSize)
{
    int i;
    size_t sourceStride = (size_t)(sampleSize * nsources);
    if (nsources > ntargets)
        nsources = ntargets;

    // TODO: gotta go fast
    for (i = 0; i < nsources; ++i) {
        auto buf = ((char *)muxBufferFirst) + sampleSize * i;
        auto remaining = firstSize;

        if (!targetBuffers[i]) {
            continue;
        }

        for (size_t j = 0;
             j < targetSize && remaining >= sourceStride;
             j += sampleSize) {

            memcpy((char *)(targetBuffers[i]) + j, buf, sampleSize);
            buf += sourceStride;
            remaining -= sourceStride;

            if (!remaining) {
                buf = ((char *)muxBufferSecond) + sampleSize * i;
                remaining = secondSize;
            }
        }
    }

    // Silence target channels not present in source
    for (; i < ntargets; i++) {
        if (targetBuffers[i]) {
            memset(targetBuffers[i], 0, targetSize);
        }
    }
}

// TODO: copypasta
void SarClient::mux(
    void *muxBufferFirst, size_t firstSize,
    void *muxBufferSecond, size_t secondSize,
    void **targetBuffers, int ntargets, int nsources,
    size_t targetSize, int sampleSize)
{
    size_t sourceStride = (size_t)(sampleSize * nsources);
    if (nsources > ntargets)
        nsources = ntargets;

    // TODO: gotta go fast
    // Channels in target not present in source are not used
    for (int i = 0; i < nsources; ++i) {
        auto buf = ((char *)muxBufferFirst) + sampleSize * i;
        auto remaining = firstSize;

        if (!targetBuffers[i]) {
            continue;
        }

        for (size_t j = 0;
             j < targetSize && remaining >= sourceStride;
             j += sampleSize) {

            memcpy(buf, (char *)(targetBuffers[i]) + j, sampleSize);
            buf += sourceStride;
            remaining -= sourceStride;

            if (!remaining) {
                buf = ((char *)muxBufferSecond) + sampleSize * i;
                remaining = secondSize;
            }
        }
    }
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnDeviceStateChanged(
    _In_  LPCWSTR pwstrDeviceId,
    _In_  DWORD dwNewState)
{
    // When a SAR endpoint is re-activated after its initial creation, its
    // supported sample rate may be different. To force the audio engine to
    // notice the possible format change, we listen for device state change
    // events and tell the kernel mode driver to broadcast a
    // KSEVENT_PINCAPS_FORMATCHANGE event, which causes the audio engine to
    // re-query the pin capabilities. This isn't needed for newly added
    // endpoints or non-SAR endpoints, so we filter out those events.
    if (dwNewState != DEVICE_STATE_ACTIVE) {
        return S_OK;
    }

    if (auto client = _client.lock()) {
        do {
            CComPtr<IMMDeviceEnumerator> mmEnumerator;
            CComPtr<IMMDevice> device;
            CComPtr<IPropertyStore> ps;
            PROPVARIANT pvalue = {};

            // This seems a bit shady, but MSDN's device events example
            // initializes COM the same way. It's not clear what the ownership
            // of the thread that delivers the IMMNotificationClient events is.
            CoInitialize(NULL);

            if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), (LPVOID *)&mmEnumerator))) {

                break;
            }

            if (FAILED(mmEnumerator->GetDevice(pwstrDeviceId, &device))) {
                break;
            }

            if (FAILED(device->OpenPropertyStore(STGM_READ, &ps))) {
                break;
            }

            if (FAILED(ps->GetValue(
                PKEY_SynchronousAudioRouter_EndpointId, &pvalue))) {

                break;
            }

            client->updateSampleRateOnTick();
            PropVariantClear(&pvalue);
        } while(false);

        CoUninitialize();
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnDeviceAdded(
    _In_  LPCWSTR pwstrDeviceId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnDeviceRemoved(
    _In_  LPCWSTR pwstrDeviceId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnDefaultDeviceChanged(
    _In_  EDataFlow flow,
    _In_  ERole role,
    _In_  LPCWSTR pwstrDefaultDeviceId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnPropertyValueChanged(
    _In_  LPCWSTR pwstrDeviceId,
    _In_  const PROPERTYKEY key)
{
    return S_OK;
}

} // namespace Sar
