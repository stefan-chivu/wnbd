/*
 * Copyright (C) 2022 Cloudbase Solutions
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include "pch.h"

#include "mock_wnbd_daemon.h"
#include "utils.h"

MockWnbdDaemon::~MockWnbdDaemon()
{
    if (Started && WnbdDisk) {
        Shutdown();

        WnbdClose(WnbdDisk);

        Started = false;
    }
}

void MockWnbdDaemon::Start()
{
    DWORD err = WnbdCreate(
        WnbdProps, (const PWNBD_INTERFACE) &MockWnbdInterface,
        this, &WnbdDisk);
    ASSERT_FALSE(err) << "WnbdCreate failed";

    Started = true;

    err = WnbdStartDispatcher(WnbdDisk, IO_REQ_WORKERS);
    ASSERT_FALSE(err) << "WnbdStartDispatcher failed";

    if (!WnbdProps->Flags.ReadOnly) {
        std::string InstanceName = WnbdProps->InstanceName;
        SetDiskWritable(InstanceName);
    }
}

void MockWnbdDaemon::Shutdown()
{
    std::unique_lock<std::mutex> Lock(ShutdownLock);
    if (!Terminated && WnbdDisk) {
        TerminateInProgress = true;
        // We're requesting the disk to be removed but continue serving IO
        // requests until the driver sends us the "Disconnect" event.
        DWORD Ret = WnbdRemove(WnbdDisk, NULL);
        if (Ret && Ret != ERROR_FILE_NOT_FOUND)
            ASSERT_FALSE(Ret) << "couldn't stop the wnbd dispatcher, err: " << Ret;
        Wait();
        Terminated = true;
    }
}

void MockWnbdDaemon::Wait()
{
    if (Started && WnbdDisk) {
        DWORD err = WnbdWaitDispatcher(WnbdDisk);
        ASSERT_FALSE(err) << "failed waiting for the dispatcher to stop";
    }
}

void MockWnbdDaemon::Read(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    PVOID Buffer,
    UINT64 BlockAddress,
    UINT32 BlockCount,
    BOOLEAN ForceUnitAccess)
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    ASSERT_TRUE(handler->WnbdProps->BlockSize);
    ASSERT_TRUE(handler->WnbdProps->BlockSize * BlockCount
        <= WNBD_DEFAULT_MAX_TRANSFER_LENGTH);

    WnbdRequestType RequestType = WnbdReqTypeRead;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.Read.BlockAddress = BlockAddress;
    WnbdReq.Cmd.Read.BlockCount = BlockCount;
    WnbdReq.Cmd.Read.ForceUnitAccess = ForceUnitAccess;

    handler->ReqLog.AddEntry(WnbdReq);

    if (Disk->Properties.BlockCount < BlockAddress + BlockCount) {
        // Overflow
        // TODO: consider moving this check to the driver.
        WnbdSetSense(
            &handler->MockStatus,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_VOLUME_OVERFLOW);
    } else {
        memset(Buffer, READ_BYTE_CONTENT, BlockCount * handler->WnbdProps->BlockSize);
    }

    handler->SendIoResponse(
        RequestHandle, RequestType,
        handler->MockStatus,
        Buffer, handler->WnbdProps->BlockSize * BlockCount);
}

void MockWnbdDaemon::Write(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    PVOID Buffer,
    UINT64 BlockAddress,
    UINT32 BlockCount,
    BOOLEAN ForceUnitAccess)
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    ASSERT_TRUE(handler->WnbdProps->BlockSize);
    ASSERT_TRUE(handler->WnbdProps->BlockSize * BlockCount
        <= WNBD_DEFAULT_MAX_TRANSFER_LENGTH);

    WnbdRequestType RequestType = WnbdReqTypeWrite;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.Write.BlockAddress = BlockAddress;
    WnbdReq.Cmd.Write.BlockCount = BlockCount;
    WnbdReq.Cmd.Write.ForceUnitAccess = ForceUnitAccess;

    handler->ReqLog.AddEntry(WnbdReq, Buffer, BlockCount * handler->WnbdProps->BlockSize);

    WNBD_STATUS Status = handler->MockStatus;

    if (Disk->Properties.BlockCount < BlockAddress + BlockCount) {
        // Overflow
        // TODO: consider moving this check to the driver.
        WnbdSetSense(
            &Status,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_VOLUME_OVERFLOW);
    }

    handler->SendIoResponse(
        RequestHandle, RequestType,
        Status,
        Buffer, handler->WnbdProps->BlockSize * BlockCount);
}

void MockWnbdDaemon::Flush(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    UINT64 BlockAddress,
    UINT32 BlockCount)
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    WnbdRequestType RequestType = WnbdReqTypeFlush;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.Flush.BlockAddress = BlockAddress;
    WnbdReq.Cmd.Flush.BlockCount = BlockCount;

    handler->ReqLog.AddEntry(WnbdReq);

    if (Disk->Properties.BlockCount < BlockAddress + BlockCount) {
        // Overflow
        // TODO: consider moving this check to the driver.
        WnbdSetSense(
            &handler->MockStatus,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_VOLUME_OVERFLOW);
    }

    handler->SendIoResponse(
        RequestHandle, RequestType,
        handler->MockStatus, NULL, 0);
}

void MockWnbdDaemon::Unmap(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    PWNBD_UNMAP_DESCRIPTOR Descriptors,
    UINT32 Count)
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    WnbdRequestType RequestType = WnbdReqTypeUnmap;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.Unmap.Count = Count;

    handler->ReqLog.AddEntry(
        WnbdReq,
        (void*)Descriptors,
        sizeof(WNBD_UNMAP_DESCRIPTOR) * Count);

    // TODO: validate unmap descriptors

    handler->SendIoResponse(
        RequestHandle, RequestType,
        handler->MockStatus, NULL, 0);
}

void MockWnbdDaemon::PersistentReserveIn(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    UINT8 ServiceAction)
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    WnbdRequestType RequestType = WnbdReqTypePersistResIn;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.PersistResIn.ServiceAction = ServiceAction;

    handler->ReqLog.AddEntry(WnbdReq);

    switch (ServiceAction)
    {
    case RESERVATION_ACTION_READ_KEYS:
    case RESERVATION_ACTION_READ_RESERVATIONS:
        break;
    default:
        WnbdSetSense(
            &handler->MockStatus,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_ILLEGAL_COMMAND);
        break;
    }

    MOCK_PRI_LIST MockPrList = { MOCK_PR_GENERATION, 0 };
    handler->SendIoResponse(
        RequestHandle, RequestType,
        handler->MockStatus, &MockPrList, sizeof(MockPrList));
}

void MockWnbdDaemon::PersistentReserveOut(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    UINT8 ServiceAction,
    UINT8 Scope,
    UINT8 Type,
    PVOID Buffer,
    UINT32 ParameterListLength) 
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    WnbdRequestType RequestType = WnbdReqTypePersistResOut;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.PersistResOut.ServiceAction = ServiceAction;
    WnbdReq.Cmd.PersistResOut.ParameterListLength = ParameterListLength;
    WnbdReq.Cmd.PersistResOut.Scope = Scope;
    WnbdReq.Cmd.PersistResOut.Type = Type;

    handler->ReqLog.AddEntry(WnbdReq, Buffer, ParameterListLength);

    switch (ServiceAction)
    {
    case RESERVATION_ACTION_REGISTER:
    case RESERVATION_ACTION_REGISTER_IGNORE_EXISTING:
    case RESERVATION_ACTION_RESERVE:
    case RESERVATION_ACTION_RELEASE:
    case RESERVATION_ACTION_CLEAR:
    case RESERVATION_ACTION_PREEMPT:
        break;
    default:
        WnbdSetSense(
            &handler->MockStatus,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_ILLEGAL_COMMAND);
        break;
    }

    handler->SendIoResponse(
        RequestHandle, RequestType,
        handler->MockStatus, NULL, 0);
}

void MockWnbdDaemon::SendIoResponse(
    UINT64 RequestHandle,
    WnbdRequestType RequestType,
    WNBD_STATUS& Status,
    PVOID DataBuffer,
    UINT32 DataBufferSize
) {
    ASSERT_TRUE(WNBD_DEFAULT_MAX_TRANSFER_LENGTH >= DataBufferSize)
        << "wnbd response too large";

    WNBD_IO_RESPONSE Resp = { 0 };
    Resp.RequestHandle = RequestHandle;
    Resp.RequestType = RequestType;
    Resp.Status = Status;

    int err = WnbdSendResponse(
        WnbdDisk,
        &Resp,
        DataBuffer,
        DataBufferSize);

    if (err && TerminateInProgress) {
        // Suppress errors that might occur because of pending disk removals.
        err = 0;
    }

    ASSERT_FALSE(err) << "unable to send wnbd response, error: "
                      << GetLastError();
}

PWNBD_DISK MockWnbdDaemon::GetDisk() {
    return WnbdDisk;
}

void MockWnbdDaemon::TerminatingInProgress() {
    TerminateInProgress = true;
}
