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

#include "sar.h"

NTSTATUS SarKsPinRtGetBufferCore(
    PIRP irp, PVOID baseAddress, ULONG requestedBufferSize,
    ULONG notificationCount, PKSRTAUDIO_BUFFER buffer)
{
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    SarControlContext *controlContext = endpoint->owner;
    SarEndpointProcessContext *processContext;
    NTSTATUS status;

    if (!endpoint) {
        SAR_ERROR("No valid endpoint");
        return STATUS_NOT_FOUND;
    }

    if (baseAddress != nullptr) {
        SAR_ERROR("It wants a specific address");
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (endpoint->activeChannelCount == 0) {
        SAR_ERROR("activeChannelCount not set, assuming channelCount");
        KdBreakPoint();
        endpoint->activeChannelCount = endpoint->channelCount;
    }

    status = SarGetOrCreateEndpointProcessContext(
        endpoint, PsGetCurrentProcess(), &processContext);

    if (!NT_SUCCESS(status)) {
        SAR_ERROR("Get process context failed: %08X", status);
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    ULONG actualSize = ROUND_UP(
        max(requestedBufferSize,
            controlContext->minimumFrameCount *
            controlContext->periodSizeBytes *
            endpoint->activeChannelCount),
        controlContext->sampleSize * endpoint->activeChannelCount);
    SIZE_T viewSize = ROUND_UP(actualSize, SAR_BUFFER_CELL_SIZE);

    ExAcquireFastMutex(&controlContext->mutex);

    if (!controlContext->bufferSection) {
        SAR_ERROR("Buffer isn't allocated");
        ExReleaseFastMutex(&controlContext->mutex);
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ULONG cellIndex = RtlFindClearBitsAndSet(
        &controlContext->bufferMap,
        (ULONG)(viewSize / SAR_BUFFER_CELL_SIZE), 0);

    if (cellIndex == 0xFFFFFFFF) {
        SAR_ERROR("Cell index full 0xFFFFFFFF");
        ExReleaseFastMutex(&controlContext->mutex);
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    endpoint->activeCellIndex = cellIndex;
    endpoint->activeViewSize = viewSize;
    endpoint->activeBufferSize = actualSize;
    ExReleaseFastMutex(&controlContext->mutex);

    PVOID mappedAddress = nullptr;
    LARGE_INTEGER sectionOffset;

    sectionOffset.QuadPart = cellIndex * SAR_BUFFER_CELL_SIZE;
    SAR_DEBUG("Mapping %08lX %016llX %lu %lu", (ULONG)viewSize, sectionOffset.QuadPart,
        actualSize, requestedBufferSize);
    status = ZwMapViewOfSection(
        endpoint->owner->bufferSection, ZwCurrentProcess(),
        &mappedAddress, 0, 0, &sectionOffset, &viewSize, ViewUnmap,
        0, PAGE_READWRITE);

    if (!NT_SUCCESS(status)) {
        SAR_ERROR("Section mapping failed %08X", status);
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    SarEndpointRegisters regs = {};

    processContext->bufferUVA = mappedAddress;
    status = SarReadEndpointRegisters(&regs, endpoint);

    if (!NT_SUCCESS(status)) {
        SAR_ERROR("Read endpoint registers failed %08X", status);
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    regs.bufferOffset = cellIndex * SAR_BUFFER_CELL_SIZE;
    regs.bufferSize = actualSize;
    regs.notificationCount = notificationCount;
    status = SarWriteEndpointRegisters(&regs, endpoint);

    if (!NT_SUCCESS(status)) {
        SAR_ERROR("Couldn't write endpoint registers: %08X %p %p", status,
            processContext->process, PsGetCurrentProcess());
        SarReleaseEndpointAndContext(endpoint);
        return status; // TODO: goto err_out
    }

    buffer->ActualBufferSize = actualSize;
    buffer->BufferAddress = mappedAddress;
    buffer->CallMemoryBarrier = FALSE;
    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinRtGetBuffer(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSRTAUDIO_BUFFER_PROPERTY prop = (PKSRTAUDIO_BUFFER_PROPERTY)request;
    PKSRTAUDIO_BUFFER buffer = (PKSRTAUDIO_BUFFER)data;

    return SarKsPinRtGetBufferCore(
        irp, prop->BaseAddress, prop->RequestedBufferSize, 0, buffer);
}

NTSTATUS SarKsPinRtGetBufferWithNotification(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION prop =
        (PKSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION)request;
    PKSRTAUDIO_BUFFER buffer = (PKSRTAUDIO_BUFFER)data;

    return SarKsPinRtGetBufferCore(
        irp, prop->BaseAddress, prop->RequestedBufferSize,
        prop->NotificationCount, buffer);
}

NTSTATUS SarKsPinRtGetClockRegister(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);

    // The clock register should be a continuously updated register at a fixed frequency.
    // We don't have anything like this to map as a register, report it as not implemented.
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinRtGetHwLatency(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    PKSRTAUDIO_HWLATENCY latency = (PKSRTAUDIO_HWLATENCY)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        SAR_ERROR("Get endpoint failed");
        return STATUS_UNSUCCESSFUL;
    }

    latency->CodecDelay = latency->ChipsetDelay = latency->FifoSize = 0;
    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinRtGetPacketCount(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinRtGetPositionRegister(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(request);

    NTSTATUS status;
    PKSRTAUDIO_HWREGISTER reg = (PKSRTAUDIO_HWREGISTER)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    SarEndpointProcessContext *context;

    if (!endpoint) {
        SAR_ERROR("Get endpoint failed");
        return STATUS_UNSUCCESSFUL;
    }

    status = SarGetOrCreateEndpointProcessContext(
        endpoint, PsGetCurrentProcess(), &context);

    if (!NT_SUCCESS(status)) {
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    reg->Register =
        &context->registerFileUVA[endpoint->index].positionRegister;
    reg->Width = 32;
    reg->Accuracy = endpoint->owner->periodSizeBytes * endpoint->activeChannelCount;
    reg->Numerator = 0;
    reg->Denominator = 0;
    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinRtGetPresentationPosition(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinRtQueryNotificationSupport(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);

    *((BOOL *)data) = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinRtRegisterNotificationEvent(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(data);
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    SarEndpointRegisters regs;
    NTSTATUS status;
    PKSRTAUDIO_NOTIFICATION_EVENT_PROPERTY prop =
        (PKSRTAUDIO_NOTIFICATION_EVENT_PROPERTY)request;

    if (!endpoint) {
        SAR_ERROR("Get endpoint failed");
        return STATUS_UNSUCCESSFUL;
    }

    status = SarReadEndpointRegisters(&regs, endpoint);

    if (!NT_SUCCESS(status)) {
        SAR_ERROR("Read endpoint registers failed %08X", status);
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    ULONG64 associatedData = regs.generation | ((ULONG64)endpoint->index << 32);

    status = SarPostHandleQueue(&endpoint->owner->handleQueue,
        prop->NotificationEvent, associatedData);
    SarReleaseEndpointAndContext(endpoint);
    return status;
}

NTSTATUS SarKsPinRtUnregisterNotificationEvent(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    return STATUS_NOT_IMPLEMENTED;
}
