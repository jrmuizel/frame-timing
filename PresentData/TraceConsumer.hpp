/*
Copyright 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

#include <windows.h>
#include <stdio.h>
#include <string>
#include <map>
#include <memory>
#include <tdh.h>
#include <vector>

inline bool operator<(GUID const& lhs, GUID const& rhs)
{
    return memcmp(&lhs, &rhs, sizeof(lhs)) < 0;
}
inline bool operator<(EVENT_DESCRIPTOR const& lhs, EVENT_DESCRIPTOR const& rhs)
{
    return memcmp(&lhs, &rhs, sizeof(lhs)) < 0;
}
class EventMetadataContainer
{
public:
    void InsertMetadata(GUID const& Provider, EVENT_DESCRIPTOR const& EventDescriptor, TRACE_EVENT_INFO const* pInfo, SIZE_T TeiSize);

    template <typename T> bool GetEventData(EVENT_RECORD* pEventRecord, wchar_t const* name, T* out);
    template <typename T> T GetEventData(EVENT_RECORD* pEventRecord, wchar_t const* name);
    template <typename T> T GetEventDataFromArray(EVENT_RECORD* pEventRecord, wchar_t const* name, uint32_t index);

private:
    const void* GetEventDataImpl(EVENT_RECORD* pEventRecord, wchar_t const* name, SIZE_T* pSize);
    const void* GetEventDataFromArrayImpl(EVENT_RECORD* pEventRecord, wchar_t const* name, uint32_t index, SIZE_T* pSize);
    std::map<GUID, std::map<EVENT_DESCRIPTOR, std::unique_ptr<byte[]>>> mMetadata;
};

void PrintEventInformationFromTdh(FILE* fp, EVENT_RECORD* pEventRecord);
std::wstring GetEventTaskNameFromTdh(EVENT_RECORD* pEventRecord);

template <typename T>
bool GetEventDataFromTdh(EVENT_RECORD* pEventRecord, wchar_t const* name, T* out, uint32_t arrayIndex, bool bPrintOnError = true)
{
    PROPERTY_DATA_DESCRIPTOR descriptor;
    descriptor.PropertyName = (ULONGLONG) name;
    descriptor.ArrayIndex = arrayIndex;

    auto status = TdhGetProperty(pEventRecord, 0, nullptr, 1, &descriptor, sizeof(T), (BYTE*) out);
    if (status != ERROR_SUCCESS) {
        if (bPrintOnError) {
            fprintf(stderr, "error: could not get event %ls property (error=%lu).\n", name, status);
            PrintEventInformationFromTdh(stderr, pEventRecord);
        }
        return false;
    }

    return true;
}

template <typename T>
T GetEventDataFromArrayFromTdh(EVENT_RECORD* pEventRecord, wchar_t const* name, uint32_t index)
{
    T value = {};
    auto ok = GetEventDataFromTdh(pEventRecord, name, &value, index);
    (void)ok;
    return value;
}

template <typename T>
bool GetEventDataFromTdh(EVENT_RECORD* pEventRecord, wchar_t const* name, T* out, bool bPrintOnError = true)
{
    return GetEventDataFromTdh<T>(pEventRecord, name, out, ULONG_MAX, bPrintOnError);
}

template <typename T>
bool EventMetadataContainer::GetEventData(EVENT_RECORD* pEventRecord, wchar_t const* name, T* out)
{
    SIZE_T Size = 0;
    const void* pData = GetEventDataImpl(pEventRecord, name, &Size);
    if (pData != nullptr && Size <= sizeof(*out))
    {
        memcpy(out, pData, Size);
        return true;
    }
    return GetEventDataFromTdh(pEventRecord, name, out);
}

template <typename T>
T GetEventDataFromTdh(EVENT_RECORD* pEventRecord, wchar_t const* name)
{
    T value = {};
    auto ok = GetEventDataFromTdh(pEventRecord, name, &value);
    (void) ok;
    return value;
}

template <typename T>
T EventMetadataContainer::GetEventData(EVENT_RECORD* pEventRecord, wchar_t const* name)
{
    T value = {};
    auto ok = GetEventData(pEventRecord, name, &value);
    (void)ok;
    return value;
}

template <typename T>
T EventMetadataContainer::GetEventDataFromArray(EVENT_RECORD* pEventRecord, wchar_t const* name, uint32_t index)
{
    T value = {};
    SIZE_T Size = 0;
    const void* pData = GetEventDataFromArrayImpl(pEventRecord, name, index, &Size);
    if (pData != nullptr && Size <= sizeof(T))
    {
        memcpy(&value, pData, Size);
        return true;
    }
    return GetEventDataFromArrayFromTdh<T>(pEventRecord, name, index);
}

template <> bool GetEventDataFromTdh<std::string>(EVENT_RECORD* pEventRecord, wchar_t const* name, std::string* out, bool bPrintOnError);
template <> bool EventMetadataContainer::GetEventData<std::string>(EVENT_RECORD* pEventRecord, wchar_t const* name, std::string* out);
