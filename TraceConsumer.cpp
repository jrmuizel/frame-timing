/*
Copyright 2017-2020 Intel Corporation

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
#include "TraceConsumer.hpp"
#include "EventMetadataEventStructs.hpp"

namespace {

uint32_t GetPropertyDataOffset(TRACE_EVENT_INFO const& tei, EVENT_RECORD const& eventRecord, uint32_t index);

// If ((epi.Flags & PropertyParamLength) != 0), the epi.lengthPropertyIndex
// field contains the index of the property that contains the number of
// CHAR/WCHARs in the string.
//
// Else if ((epi.Flags & PropertyLength) != 0 || epi.length != 0), the
// epi.length field contains number of CHAR/WCHARs in the string.
//
// Else the string is terminated by (CHAR/WCHAR)0.
//
// Note that some event providers do not correctly null-terminate the last
// string field in the event. While this is technically invalid, event decoders
// may silently tolerate such behavior instead of rejecting the event as
// invalid.

template<typename T>
uint32_t GetStringPropertySize(TRACE_EVENT_INFO const& tei, EVENT_RECORD const& eventRecord, uint32_t index, uint32_t offset,
                               uint32_t* propStatus)
{
    auto const& epi = tei.EventPropertyInfoArray[index];

    if ((epi.Flags & PropertyParamLength) != 0) {
        assert(false); // TODO: just not implemented yet
        return 0;
    }

    if (epi.length != 0) {
        return epi.length * sizeof(T);
    }

    if (offset == UINT32_MAX) {
        offset = GetPropertyDataOffset(tei, eventRecord, index);
        assert(offset <= eventRecord.UserDataLength);
    }

    for (uint32_t size = 0;; size += sizeof(T)) {
        if (offset + size > eventRecord.UserDataLength) {
            // string ends at end of block, possibly ok (see note above)
            return size - sizeof(T);
        }
        if (*(T const*) ((uintptr_t) eventRecord.UserData + offset + size) == (T) 0) {
            *propStatus |= PROP_STATUS_NULL_TERMINATED;
            return size + sizeof(T);
        }
    }
}

void GetPropertySize(TRACE_EVENT_INFO const& tei, EVENT_RECORD const& eventRecord, uint32_t index, uint32_t offset,
                     uint32_t* outSize, uint32_t* outCount, uint32_t* propStatus)
{
    // We don't handle all flags yet, these are the ones we do:
    auto const& epi = tei.EventPropertyInfoArray[index];
    assert((epi.Flags & ~(PropertyStruct | PropertyParamCount | PropertyParamFixedCount)) == 0);

    // Use the epi length and count by default.  There are cases where the count
    // is valid but (epi.Flags & PropertyParamFixedCount) == 0.
    uint32_t size = epi.length;
    uint32_t count = epi.count;

    if (epi.Flags & PropertyStruct) {
        size = 0;
        for (USHORT i = 0; i < epi.structType.NumOfStructMembers; ++i) {
            uint32_t memberSize = 0;
            uint32_t memberCount = 0;
            uint32_t memberStatus = 0;
            GetPropertySize(tei, eventRecord, epi.structType.StructStartIndex + i, UINT32_MAX, &memberSize, &memberCount, &memberStatus);
            size += memberSize * memberCount;
        }
    } else {
        switch (epi.nonStructType.InType) {
        case TDH_INTYPE_UNICODESTRING:
            *propStatus |= PROP_STATUS_WCHAR_STRING;
            size = GetStringPropertySize<wchar_t>(tei, eventRecord, index, offset, propStatus);
            break;
        case TDH_INTYPE_ANSISTRING:
            *propStatus |= PROP_STATUS_CHAR_STRING;
            size = GetStringPropertySize<char>(tei, eventRecord, index, offset, propStatus);
            break;

        case TDH_INTYPE_POINTER:    // TODO: Not sure this is needed, epi.length seems to be correct?
        case TDH_INTYPE_SIZET:
            size = (eventRecord.EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) ? 8 : 4;
            break;

        case TDH_INTYPE_WBEMSID:
            // TODO: can't figure out how to decode this... so reverting to TDH for now
            {
                PROPERTY_DATA_DESCRIPTOR descriptor;
                descriptor.PropertyName = (ULONGLONG) &tei + epi.NameOffset;
                descriptor.ArrayIndex = UINT32_MAX;
                auto status = TdhGetPropertySize((EVENT_RECORD*) &eventRecord, 0, nullptr, 1, &descriptor, (ULONG*) &size);
                (void) status;
            }
            break;
        }
    }

    if (epi.Flags & PropertyParamCount) {
        auto countIdx = epi.countPropertyIndex;
        auto addr = (uintptr_t) eventRecord.UserData + GetPropertyDataOffset(tei, eventRecord, countIdx);

        assert(tei.EventPropertyInfoArray[countIdx].Flags == 0);
        switch (tei.EventPropertyInfoArray[countIdx].nonStructType.InType) {
        case TDH_INTYPE_INT8:   count = *(int8_t const*) addr; break;
        case TDH_INTYPE_UINT8:  count = *(uint8_t const*) addr; break;
        case TDH_INTYPE_INT16:  count = *(int16_t const*) addr; break;
        case TDH_INTYPE_UINT16: count = *(uint16_t const*) addr; break;
        case TDH_INTYPE_INT32:  count = *(int32_t const*) addr; break;
        case TDH_INTYPE_UINT32: count = *(uint32_t const*) addr; break;
        default: assert(!"INTYPE not yet implemented for count.");
        }
    }

    assert(size > 0);
    assert(count > 0);

    *outSize = size;
    *outCount = count;
}

uint32_t GetPropertyDataOffset(TRACE_EVENT_INFO const& tei, EVENT_RECORD const& eventRecord, uint32_t index)
{
    assert(index < tei.TopLevelPropertyCount);
    uint32_t size = 0;
    uint32_t count = 0;
    uint32_t offset = 0;
    uint32_t status = 0;
    for (uint32_t i = 0; i < index; ++i) {
        GetPropertySize(tei, eventRecord, i, offset, &size, &count, &status);
        offset += size * count;
    }
    return offset;
}

TRACE_EVENT_INFO* GetTraceEventInfo(EventMetadata* metadata, EVENT_RECORD* eventRecord)
{
    // Look up stored metadata
    EventMetadataKey key;
    key.guid_ = eventRecord->EventHeader.ProviderId;
    key.desc_ = eventRecord->EventHeader.EventDescriptor;
    auto ii = metadata->metadata_.find(key);

    // If not found, look up metadata using TDH
    if (ii == metadata->metadata_.end()) {
        ULONG bufferSize = 0;
        auto status = TdhGetEventInformation(eventRecord, 0, nullptr, nullptr, &bufferSize);
        assert(status == ERROR_INSUFFICIENT_BUFFER);

        ii = metadata->metadata_.emplace(key, std::vector<uint8_t>(bufferSize, 0)).first;

        status = TdhGetEventInformation(eventRecord, 0, nullptr, (TRACE_EVENT_INFO*) ii->second.data(), &bufferSize);
        assert(status == ERROR_SUCCESS);
    }

    return (TRACE_EVENT_INFO*) ii->second.data();
}

}

size_t EventMetadataKeyHash::operator()(EventMetadataKey const& key) const
{
    static_assert((sizeof(key) % sizeof(size_t)) == 0, "sizeof(EventMetadataKey) must be multiple of sizeof(size_t)");
    auto p = (size_t const*) &key;
    auto h = (size_t) 0;
    for (size_t i = 0; i < sizeof(key) / sizeof(size_t); ++i) {
        h ^= p[i];
    }
    return h;
}

bool EventMetadataKeyEqual::operator()(EventMetadataKey const& lhs, EventMetadataKey const& rhs) const
{
    return memcmp(&lhs, &rhs, sizeof(EventMetadataKey)) == 0;
}

void EventMetadata::AddMetadata(EVENT_RECORD* eventRecord)
{
    if (eventRecord->EventHeader.EventDescriptor.Opcode == Microsoft_Windows_EventMetadata::EventInfo::Opcode) {
        auto userData = (uint8_t const*) eventRecord->UserData;
        auto tei = (TRACE_EVENT_INFO const*) userData;

        if (tei->DecodingSource == DecodingSourceTlg || tei->EventDescriptor.Channel == 0xB) {
            return; // Don't store tracelogging metadata
        }

        // Store metadata (overwriting any previous)
        EventMetadataKey key;
        key.guid_ = tei->ProviderGuid;
        key.desc_ = tei->EventDescriptor;
        metadata_[key].assign(userData, userData + eventRecord->UserDataLength);
    }
}

// Look up metadata for this provider/event and use it to look up the property.
// If the metadata isn't found look it up using TDH.  Then, look up each
// property in the metadata to obtain it's data pointer and size.
void EventMetadata::GetEventData(EVENT_RECORD* eventRecord, EventDataDesc* desc, uint32_t descCount, uint32_t optionalCount /*=0*/)
{
    // Look up metadata
    auto tei = GetTraceEventInfo(this, eventRecord);

    // Lookup properties in metadata
    uint32_t foundCount = 0;
    for (uint32_t i = 0, offset = 0; i < tei->TopLevelPropertyCount; ++i) {
        uint32_t size   = 0;
        uint32_t count  = 0;
        uint32_t status = PROP_STATUS_FOUND;
        GetPropertySize(*tei, *eventRecord, i, offset, &size, &count, &status);

        auto propName = TEI_PROPERTY_NAME(tei, &tei->EventPropertyInfoArray[i]);
        for (uint32_t j = 0; j < descCount; ++j) {
            if (desc[j].status_ == PROP_STATUS_NOT_FOUND && wcscmp(propName, desc[j].name_) == 0) {
                assert(desc[j].arrayIndex_ < count);

                desc[j].data_   = (void*) ((uintptr_t) eventRecord->UserData + offset + desc[j].arrayIndex_ * size);
                desc[j].size_   = size;
                desc[j].status_ = status;

                foundCount += 1;
                if (foundCount == descCount) {
                    return;
                }
            }
        }

        offset += size * count;
    }

    assert(foundCount >= descCount - optionalCount);
    (void) optionalCount;
}

namespace {

template <typename T>
T GetEventString(EventMetadata* metadata, EVENT_RECORD* eventRecord, wchar_t const* name, uint32_t arrayIndex, uint32_t statusCheck)
{
    EventDataDesc desc = { name, arrayIndex, };
    metadata->GetEventData(eventRecord, &desc, 1);

    assert(desc.status_ & statusCheck);
    (void) statusCheck;

    // Don't include null termination character
    if (desc.status_ & PROP_STATUS_NULL_TERMINATED) {
        assert(desc.size_ >= sizeof(T::value_type));
        desc.size_ -= sizeof(T::value_type);
    }

    auto start = (typename T::value_type*) desc.data_;
    auto end   = (typename T::value_type*) ((uintptr_t) desc.data_ + desc.size_);
    return T(start, end);
}

}

template <>
std::string EventMetadata::GetEventData<std::string>(EVENT_RECORD* eventRecord, wchar_t const* name, uint32_t arrayIndex)
{
    return GetEventString<std::string>(this, eventRecord, name, arrayIndex, PROP_STATUS_CHAR_STRING);
}

template <>
std::wstring EventMetadata::GetEventData<std::wstring>(EVENT_RECORD* eventRecord, wchar_t const* name, uint32_t arrayIndex)
{
    return GetEventString<std::wstring>(this, eventRecord, name, arrayIndex, PROP_STATUS_WCHAR_STRING);
}

