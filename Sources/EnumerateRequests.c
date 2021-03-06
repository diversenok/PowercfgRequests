#include <phnt_windows.h>
#define PHNT_VERSION PHNT_20H1

#include <phnt.h>
#include <subprocesstag.h>
#include <stdio.h>
#include "helper.h"

ULONG DisplayRequest(
    _In_ PPOWER_REQUEST Request,
    _In_ POWER_REQUEST_TYPE RequestType
)
{
    PPOWER_REQUEST_BODY requestBody;

    // Determine if the request matches the type
    if ((ULONG)RequestType >= SupportedModeCount || !Request->V4.ActiveCount[RequestType])
        return FALSE;

    // The location of the request's body depends on the supported modes
    switch (SupportedModeCount)
    {
        case POWER_REQUEST_SUPPORTED_TYPES_V1:
            requestBody = &Request->V1.Body;
            break;

        case POWER_REQUEST_SUPPORTED_TYPES_V2:
            requestBody = &Request->V2.Body;
            break;

        case POWER_REQUEST_SUPPORTED_TYPES_V3:
            requestBody = &Request->V3.Body;
            break;

        case POWER_REQUEST_SUPPORTED_TYPES_V4:
            requestBody = &Request->V4.Body;
            break;

        default:
            return FALSE;
    }

    // Print the requester kind
    switch (requestBody->Origin)
    {
        case POWER_REQUEST_ORIGIN_DRIVER:
            wprintf_s(L"[DRIVER] ");
            break;

        case POWER_REQUEST_ORIGIN_PROCESS:
            wprintf_s(L"[PROCESS (PID %d)] ", requestBody->ProcessId);
            break;

        case POWER_REQUEST_ORIGIN_SERVICE:
            wprintf_s(L"[SERVICE (PID %d)] ", requestBody->ProcessId);
            break;

        default:
            wprintf_s(L"[UNKNOWN] ");
            break;
    }

    PCWSTR requesterName = L"Legacy Kernel Caller";
    PCWSTR requesterDetails = NULL;
    TAG_INFO_NAME_FROM_TAG serviceInfo = { 0 };

    // Power requests are reentrant and maintain a counter
    if (Request->V4.ActiveCount[RequestType] > 1)
        wprintf_s(L"[%d times] ", Request->V4.ActiveCount[RequestType]);

    // Retrieve general requester information
    if (requestBody->OffsetToRequester)
        requesterName = (PCWSTR)RtlOffsetToPointer(requestBody, requestBody->OffsetToRequester);

    // For drivers, locate their full names
    if (requestBody->Origin == POWER_REQUEST_ORIGIN_DRIVER && requestBody->OffsetToDriverName)
        requesterDetails = (PCWSTR)RtlOffsetToPointer(requestBody, requestBody->OffsetToDriverName);

    // For services, convert their tags to names
    if (requestBody->Origin == POWER_REQUEST_ORIGIN_SERVICE)
    {
        PQUERY_TAG_INFORMATION I_QueryTagInformation = I_QueryTagInformationLoader();

        serviceInfo.InParams.dwPid = requestBody->ProcessId;
        serviceInfo.InParams.dwTag = requestBody->ServiceTag;

        if (I_QueryTagInformation &&
            I_QueryTagInformation(NULL, eTagInfoLevelNameFromTag, &serviceInfo) == ERROR_SUCCESS)
            requesterDetails = serviceInfo.OutParams.pszName;
    }

    if (requesterDetails)
        wprintf_s(L"%s (%s)\r\n", requesterName, requesterDetails);
    else
        wprintf_s(L"%s\r\n", requesterName);

    // The context section stores the reason of the request
    if (requestBody->OffsetToContext)
    {
        PCOUNTED_REASON_CONTEXT_RELATIVE context =
            (PCOUNTED_REASON_CONTEXT_RELATIVE)RtlOffsetToPointer(requestBody, requestBody->OffsetToContext);
        
        if (context->Flags & POWER_REQUEST_CONTEXT_SIMPLE_STRING)
        {
            // Simple strings are packed into the buffer

            wprintf_s(L"%s\r\n", (PCWCHAR)RtlOffsetToPointer(context, context->OffsetToSimpleString));
        }
        else if (context->Flags & POWER_REQUEST_CONTEXT_DETAILED_STRING)
        {
            // Detailed strings are located in an external module

            HMODULE hModule = LoadLibraryExW(
                (PCWSTR)RtlOffsetToPointer(context, context->OffsetToResourceFileName),
                NULL,
                LOAD_LIBRARY_AS_DATAFILE
            );

            if (hModule)
            {
                PCWSTR reasonString;
                int reasonLength = LoadStringW(
                    hModule,
                    context->ResourceReasonId,
                    (LPWSTR)&reasonString,
                    0
                );

                // TODO: substitute caller-supplied parameters

                if (reasonLength)
                    wprintf_s(L"%s\r\n", reasonString);

                // Clean-up
                FreeLibrary(hModule);
            }
        }
    }

    // Clean-up
    if (serviceInfo.OutParams.pszName)
        LocalFree(serviceInfo.OutParams.pszName);

    return TRUE;
}

void DisplayRequests(
    _In_ PPOWER_REQUEST_LIST RequestList,
    _In_ POWER_REQUEST_TYPE Condition,
    _In_ LPCWSTR Caption
)
{
    wprintf_s(L"%s:\r\n", Caption);

    ULONG found = FALSE;

    for (ULONG i = 0; i < RequestList->cElements; i++)
    {
        found = DisplayRequest(
            (PPOWER_REQUEST)RtlOffsetToPointer(RequestList, RequestList->OffsetsToRequests[i]),
            Condition
        ) || found;
    }

    if (!found)
        wprintf_s(L"None.\r\n");

    wprintf_s(L"\r\n");
}

int main()
{
    // Do not allow running under WoW64
    if (IsWoW64())
        return 1;

    NTSTATUS status;
    PPOWER_REQUEST_LIST buffer;
    ULONG bufferLength = 4096;

    do
    {
        buffer = RtlAllocateHeap(
            RtlGetCurrentPeb()->ProcessHeap,
            0,
            bufferLength
        );

        if (!buffer)
        {
            wprintf_s(L"Not enough memory");
            return 1;
        }

        // Query the power request list
        status = NtPowerInformation(
            GetPowerRequestList,
            NULL,
            0,
            buffer,
            bufferLength
        );

        if (!NT_SUCCESS(status))
        {
            RtlFreeHeap(RtlGetCurrentPeb()->ProcessHeap, 0, buffer);
            buffer = NULL;

            // Prepare for expansion
            bufferLength += 4096;
        }

    } while (status == STATUS_BUFFER_TOO_SMALL);

    if (!IsSuccess(status, L"Querying power request list") || !buffer)
        return 1;

    InitializeSupportedModeCount();

    DisplayRequests(buffer, PowerRequestDisplayRequired, L"DISPLAY");
    DisplayRequests(buffer, PowerRequestSystemRequired, L"SYSTEM");
    DisplayRequests(buffer, PowerRequestAwayModeRequired, L"AWAYMODE");
    DisplayRequests(buffer, PowerRequestExecutionRequired, L"EXECUTION");
    DisplayRequests(buffer, PowerRequestPerfBoostRequired, L"PERFBOOST");
    DisplayRequests(buffer, PowerRequestActiveLockScreenRequired, L"ACTIVELOCKSCREEN");

    RtlFreeHeap(RtlGetCurrentPeb()->ProcessHeap, 0, buffer);

    return 0;
}
