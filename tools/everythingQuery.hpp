#pragma once
#include <QStringList>
#include <windows.h>
#include "externalLibs/Everything/src/include/Everything.h"
#define EVERYTHING3_USERAPI
#include "externalLibs/Everything/sdk3/include/Everything3.h"
#undef EVERYTHING3_USERAPI

// One helper owns one connection. Killing the helper closes its pipe as well,
// so an obsolete generation can never publish into a newer search.
class EverythingQuery final {
public:
    ~EverythingQuery()
    {
        if (list_) Everything3_DestroyResultList(list_);
        if (state_) Everything3_DestroySearchState(state_);
        if (client_) { Everything3_ShutdownClient(client_); Everything3_DestroyClient(client_); }
    }
    bool Start(const QString& Pattern)
    {
        const auto Text = Pattern.toStdWString();
        client_ = Everything3_ConnectW(nullptr);
        if (!client_) client_ = Everything3_ConnectW(L"1.5a");
        if (client_) {
            state_ = Everything3_CreateSearchState();
            if (!state_) return false;
            if (!Everything3_SetSearchTextW(state_, Text.c_str()) || !Everything3_SetSearchRegex(state_, TRUE)
                || !Everything3_SetSearchMatchPath(state_, FALSE) || !Everything3_SetSearchMatchCase(state_, FALSE)
                || !Everything3_SetSearchSort(state_, EVERYTHING3_PROPERTY_ID_NAME, TRUE)
                || !Everything3_AddSearchPropertyRequest(state_, EVERYTHING3_PROPERTY_ID_FULL_PATH)) return false;
            // SDK 3.0.1.12 setters assign successfully but return FALSE.
            Everything3_SetSearchViewportCount(state_, 200);
            if (Everything3_GetSearchViewportCount(state_) != 200) return false;
            list_ = Everything3_Search(client_, state_);
            if (!list_) return false;
            total_ = Everything3_GetResultListCount(list_);
        } else {
            if (!Everything_IsDBLoaded()) return false;
            Everything_Reset();
            Everything_SetRegex(TRUE); Everything_SetMatchPath(FALSE); Everything_SetMatchCase(FALSE);
            Everything_SetSearchW(Text.c_str()); Everything_SetMax(MAXDWORD);
            Everything_SetSort(EVERYTHING_SORT_NAME_ASCENDING);
            if (!Everything_QueryW(TRUE)) return false;
            total_ = Everything_GetNumResults();
        }
        return true;
    }
    quint64 Total() const { return total_; }
    bool UsesSdk3() const { return client_ != nullptr; }
    bool Stable() const { return !client_ || !Everything3_IsResultListChange(client_); }
    bool Next(QStringList& Paths)
    {
        Paths.clear();
        if (offset_ >= total_) return true;
        quint64 Count = qMin<quint64>(200, total_ - offset_);
        if (client_) {
            if (Everything3_IsResultListChange(client_)) return false;
            if (offset_) {
                Everything3_DestroyResultList(list_); list_ = nullptr;
                Everything3_SetSearchViewportOffset(state_, size_t(offset_));
                if (Everything3_GetSearchViewportOffset(state_) != offset_) return false;
                list_ = Everything3_GetResults(client_, state_);
                if (!list_ || Everything3_GetResultListCount(list_) != total_ || Everything3_IsResultListChange(client_)) return false;
            }
            if (Everything3_GetResultListViewportCount(list_) != Count) return false;
        }
        for (quint64 Row = 0; Row < Count; ++Row) {
            wchar_t Buffer[32768];
            const auto Size = client_ ? Everything3_GetResultFullPathNameW(list_, size_t(Row), Buffer, 32768)
                : Everything_GetResultFullPathNameW(DWORD(offset_ + Row), Buffer, 32768);
            if (!Size || Size >= 32767) return false;
            Paths.append(QString::fromWCharArray(Buffer, int(Size)));
        }
        offset_ += Count;
        return true;
    }
private:
    EVERYTHING3_CLIENT* client_ = nullptr;
    EVERYTHING3_SEARCH_STATE* state_ = nullptr;
    EVERYTHING3_RESULT_LIST* list_ = nullptr;
    quint64 total_ = 0, offset_ = 0;
};
