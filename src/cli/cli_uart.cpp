/*
 *  Copyright (c) 2016, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements the CLI server on the UART service.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cli/cli.hpp>
#include <cli/cli-uart.h>
#include <cli/cli_uart.hpp>
#include <common/code_utils.hpp>
#include <common/encoding.hpp>
#include <common/new.hpp>
#include <common/tasklet.hpp>
#include <platform/logging.h>
#include <platform/uart.h>

namespace Thread {
namespace Cli {

static const char sCommandPrompt[] = {'>', ' '};
static const char sEraseString[] = {'\b', ' ', '\b'};
static const char CRNL[] = {'\r', '\n'};
Uart *Uart::sUartServer;

static otDEFINE_ALIGNED_VAR(sCliUartRaw, sizeof(Uart), uint64_t);

extern "C" void otCliUartInit(otInstance *aInstance)
{
    Uart::sUartServer = new(&sCliUartRaw) Uart(aInstance);
}

Uart::Uart(otInstance *aInstance):
    mInterpreter(aInstance)
{
    mRxLength = 0;
    mTxHead = 0;
    mTxLength = 0;
    mSendLength = 0;
}

extern "C" void otPlatUartReceived(const uint8_t *aBuf, uint16_t aBufLength)
{
    Uart::sUartServer->ReceiveTask(aBuf, aBufLength);
}

void Uart::ReceiveTask(const uint8_t *aBuf, uint16_t aBufLength)
{
    const uint8_t *end;

    end = aBuf + aBufLength;

    for (; aBuf < end; aBuf++)
    {
        switch (*aBuf)
        {
        case '\r':
        case '\n':
            Output(CRNL, sizeof(CRNL));

            if (mRxLength > 0)
            {
                mRxBuffer[mRxLength] = '\0';
                ProcessCommand();
            }

            Output(sCommandPrompt, sizeof(sCommandPrompt));

            break;

#ifdef OPENTHREAD_EXAMPLES_POSIX

        case 0x03: // ASCII for Ctrl-C
            exit(EXIT_SUCCESS);
            break;
#endif

        case '\b':
        case 127:
            if (mRxLength > 0)
            {
                Output(sEraseString, sizeof(sEraseString));
                mRxBuffer[--mRxLength] = '\0';
            }

            break;

        default:
            Output(reinterpret_cast<const char *>(aBuf), 1);
            mRxBuffer[mRxLength++] = static_cast<char>(*aBuf);
            break;
        }
    }
}

ThreadError Uart::ProcessCommand(void)
{
    ThreadError error = kThreadError_None;

    if (mRxBuffer[mRxLength - 1] == '\n')
    {
        mRxBuffer[--mRxLength] = '\0';
    }

    if (mRxBuffer[mRxLength - 1] == '\r')
    {
        mRxBuffer[--mRxLength] = '\0';
    }

    mInterpreter.ProcessLine(mRxBuffer, mRxLength, *this);

    mRxLength = 0;

    return error;
}

int Uart::Output(const char *aBuf, uint16_t aBufLength)
{
    uint16_t remaining = kTxBufferSize - mTxLength;
    uint16_t tail;

    if (aBufLength > remaining)
    {
        aBufLength = remaining;
    }

    for (int i = 0; i < aBufLength; i++)
    {
        tail = (mTxHead + mTxLength) % kTxBufferSize;
        mTxBuffer[tail] = *aBuf++;
        mTxLength++;
    }

    Send();

    return aBufLength;
}

int Uart::OutputFormat(const char *fmt, ...)
{
    char buf[kMaxLineLength];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    return Output(buf, static_cast<uint16_t>(strlen(buf)));
}

void Uart::Send(void)
{
    VerifyOrExit(mSendLength == 0, ;);

    if (mTxLength > kTxBufferSize - mTxHead)
    {
        mSendLength = kTxBufferSize - mTxHead;
    }
    else
    {
        mSendLength = mTxLength;
    }

    if (mSendLength > 0)
    {
        otPlatUartSend(reinterpret_cast<uint8_t *>(mTxBuffer + mTxHead), mSendLength);
    }

exit:
    return;
}

extern "C" void otPlatUartSendDone(void)
{
    Uart::sUartServer->SendDoneTask();
}

void Uart::SendDoneTask(void)
{
    mTxHead = (mTxHead + mSendLength) % kTxBufferSize;
    mTxLength -= mSendLength;
    mSendLength = 0;

    Send();
}

#if OPENTHREAD_ENABLE_CLI_LOGGING
#ifdef __cplusplus
extern "C" {
#endif
void otCliLog(otLogLevel aLogLevel, otLogRegion aLogRegion, const char *aFormat, ...)
{
    if (NULL == Uart::sUartServer)
    {
        return;
    }

    switch (aLogLevel)
    {
    case kLogLevelNone:
        Uart::sUartServer->OutputFormat("NONE ");
        break;

    case kLogLevelCrit:
        Uart::sUartServer->OutputFormat("CRIT ");
        break;

    case kLogLevelWarn:
        Uart::sUartServer->OutputFormat("WARN ");
        break;

    case kLogLevelInfo:
        Uart::sUartServer->OutputFormat("INFO ");
        break;

    case kLogLevelDebg:
        Uart::sUartServer->OutputFormat("DEBG ");
        break;

    default:
        return;
    }

    switch (aLogRegion)
    {
    case kLogRegionApi:
        Uart::sUartServer->OutputFormat("API  ");
        break;

    case kLogRegionMle:
        Uart::sUartServer->OutputFormat("MLE  ");
        break;

    case kLogRegionArp:
        Uart::sUartServer->OutputFormat("ARP  ");
        break;

    case kLogRegionNetData:
        Uart::sUartServer->OutputFormat("NETD ");
        break;

    case kLogRegionIp6:
        Uart::sUartServer->OutputFormat("IPV6 ");
        break;

    case kLogRegionIcmp:
        Uart::sUartServer->OutputFormat("ICMP ");
        break;

    case kLogRegionMac:
        Uart::sUartServer->OutputFormat("MAC  ");
        break;

    case kLogRegionMem:
        Uart::sUartServer->OutputFormat("MEM  ");
        break;

    case kLogRegionNcp:
        Uart::sUartServer->OutputFormat("NCP  ");
        break;

    case kLogRegionMeshCoP:
        Uart::sUartServer->OutputFormat("MCOP ");
        break;

    default:
        return;
    }

    va_list args;
    va_start(args, aFormat);
    Uart::sUartServer->OutputFormat(aFormat, args);
    va_end(args);
}
#ifdef __cplusplus
}  // extern "C"
#endif
#endif // OPENTHREAD_ENABLE_CLI_LOGGING

}  // namespace Cli
}  // namespace Thread
