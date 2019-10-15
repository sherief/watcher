//Watcher - maintained by Sherief Farouk - github.com/sherief/watcher
//Code is GPL v3 licensed, see LICENSE file.

#include <iostream>
#include <cassert>

#include <Windows.h>
#include <process.h>
#include <PathCch.h>
#include <Shlwapi.h>

#define EXIT_ARG_ERROR 1
#define EXIT_INVALID_PATH 2
#define EXIT_INIT_FAIL 3
#define EXIT_WATCH_FAILURE 4
#define EXIT_WAIT_FAILURE 5
#define EXIT_RECORD_FAILURE 6

static constexpr int BufferSizeInDWords = 4096;
//Use an array of DWORDS to guarantee DWORD alignment (a ReadDirectoryChangesW requirement).
DWORD Buffer[BufferSizeInDWords];
#define MAX_LONG_PATH 32768
wchar_t TargetPath[MAX_LONG_PATH] = {};
wchar_t ParentPath[MAX_LONG_PATH] = {};

int wmain(int argc, wchar_t** argv)
{
    if(argc != 3)
    {
        std::cerr << "Usage: watch <path> <command>\n";
        return EXIT_ARG_ERROR;
    }
    wchar_t* InputPath = argv[1];
    if(PathIsRelative(InputPath))
    {
        if(GetCurrentDirectory(MAX_LONG_PATH, TargetPath) == 0)
        {
            const auto Error = GetLastError();
            constexpr int BufferSize = 256;
            wchar_t Buffer[BufferSize] = {};
            DWORD CharCount = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), Buffer, BufferSize, nullptr);
            std::wcerr << "'GetCurrentDirectory()' error: (" << Error << ") - " << Buffer;
            return EXIT_INIT_FAIL;
        }
        if(PathCchAppend(TargetPath, MAX_LONG_PATH, InputPath) != S_OK)
        {
            std::wcerr << "Path append error.\n";
            return EXIT_INIT_FAIL;
        }
    }
    else
    {
        if(wcscpy_s(TargetPath, InputPath) != 0)
        {
            std::cerr << "Path copy error (path name too long?).\n";
            return EXIT_INIT_FAIL;
        }
    }
    wchar_t* Command = argv[2];
    if(wcscpy_s(ParentPath, TargetPath) != 0)
    {
        std::cerr << "Path error 1 (path name too long?).\n";
        return EXIT_INIT_FAIL;
    }
    if(PathCchRemoveFileSpec(ParentPath, MAX_LONG_PATH) != S_OK)
    {
        std::cerr << "Path error 2 (path name too long?).\n";
        return EXIT_INIT_FAIL;
    }
    const bool IsDirectory = PathIsDirectory(TargetPath);
    const wchar_t* FileName = PathFindFileName(TargetPath);
    const auto FileNameByteLength = wcslen(FileName) * sizeof(wchar_t);
    //Open directory handle
    const DWORD ShareMode = FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE;
    HANDLE DirectoryHandle = CreateFile(IsDirectory ? TargetPath : ParentPath, FILE_LIST_DIRECTORY, ShareMode, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if(DirectoryHandle == INVALID_HANDLE_VALUE)
    {
        const auto Error = GetLastError();
        constexpr int BufferSize = 256;
        wchar_t Buffer[BufferSize] = {};
        DWORD CharCount = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), Buffer, BufferSize, nullptr);
        std::wcerr << "Error creating handle to path [" << TargetPath << "]: (" << Error << ") - " << Buffer;
        return EXIT_INVALID_PATH;
    }
    //Initiate directory watching
    HANDLE CleanupCompletedEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if(CleanupCompletedEvent == nullptr)
    {
        std::cerr << "Error allocating resources.\n";
        return EXIT_INIT_FAIL;
    }
    //Completion event must be manual reset (for use with GetOverlappedResult()).
    HANDLE CompletionEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if(CompletionEvent == nullptr)
    {
        std::cerr << "Error allocating resources.\n";
        return EXIT_INIT_FAIL;
    }
    OVERLAPPED Overlapped;
    Overlapped.hEvent = CompletionEvent;
    BOOL WatchSubtree = IsDirectory;
    auto fire = [Command]()
    {
        _wsystem(Command);
    };
    while(1)
    {
        BOOL ReadDirectoryChangesResult =
            ReadDirectoryChangesW(DirectoryHandle, Buffer, BufferSizeInDWords * sizeof(DWORD), WatchSubtree,
                FILE_NOTIFY_CHANGE_LAST_WRITE
                , nullptr, &Overlapped, nullptr);
        if(ReadDirectoryChangesResult == 0)
        {
            const auto Error = GetLastError();
            if(Error == ERROR_NOTIFY_ENUM_DIR)
            {
                fire();
                continue;
            }
            else
            {
                constexpr int BufferSize = 256;
                wchar_t Buffer[BufferSize] = {};
                DWORD CharCount = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), Buffer, BufferSize, nullptr);
                std::wcerr << "Error watching changes: (" << Error << ") - " << Buffer;
                return EXIT_INVALID_PATH;
            }
        }
        DWORD WaitResult = WaitForSingleObject(CompletionEvent, INFINITE);
        if(WaitResult != WAIT_OBJECT_0)
        {
            if(WaitResult == WAIT_FAILED)
            {
                DWORD Error = GetLastError();
                constexpr int BufferSize = 256;
                wchar_t Buffer[BufferSize] = {};
                DWORD CharCount = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), Buffer, BufferSize, nullptr);
                std::wcerr << "Error waiting for object: (" << Error << ") - " << Buffer;
            }
            else
            {
                std::cerr << "Wait failed with result: " << WaitResult << "\n";
            }
            return EXIT_WAIT_FAILURE;
        }
        DWORD BytesTransferred = 0xFFFFFFFF;
        const auto OverlappedResultRetrieval = GetOverlappedResult(DirectoryHandle, &Overlapped, &BytesTransferred, FALSE);
        if(OverlappedResultRetrieval == 0)
        {
            const auto Error = GetLastError();
            constexpr int BufferSize = 256;
            wchar_t Buffer[BufferSize] = {};
            DWORD CharCount = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), Buffer, BufferSize, nullptr);
            std::wcerr << "Error retrieving change records: (" << Error << ") - " << Buffer;
            return EXIT_RECORD_FAILURE;
        }
        const DWORD ErrorCode = Overlapped.Internal;
        assert(BytesTransferred == Overlapped.InternalHigh);
        auto N = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(Buffer);
        do
        {
            if(IsDirectory)
            {
                fire();
            }
            else
            {
                if(FileNameByteLength == N->FileNameLength)
                {
                    if(memcmp(FileName, N->FileName, FileNameByteLength) == 0)
                    {
                        fire();
                    }
                }
            }
            const auto NextNotificationPointer = reinterpret_cast<const BYTE*>(N) + N->NextEntryOffset;
            N = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(NextNotificationPointer);
        } while(N->NextEntryOffset != 0);
    }
}
