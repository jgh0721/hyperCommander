#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

int wmain(int Argc, wchar_t** Argv)
{
    if (Argc != 3) return 2;
    const auto Name = L"Local\\hc-wcx-helper-" + std::wstring(Argv[1]);
    const auto Event = OpenEventW(EVENT_MODIFY_STATE, FALSE, Name.c_str());
    if (Event) { SetEvent(Event); CloseHandle(Event); }
    Sleep(Argv[2][0] == L'F' ? 700 : INFINITE);
    return 0;
}
