#ifndef STANDARD_MODERN_C_PLATFORM_WINDOWS
#define STANDARD_MODERN_C_PLATFORM_WINDOWS

/*
        Reference only -- deliberately not included by library.c.

        This is the former C platform layer. It is kept as behavioral input
        for a future Windows port, not as a buildable fallback: the shared x64
        assembly uses the SysV register ABI and ELF symbol directives, while
        Win64 uses a different calling convention and COFF. library.c therefore
        stops a Windows build until these routines and the shared primitives
        have hardware-floor Windows-ABI assembly implementations.

        This fragment was already incomplete. In particular it supplies no
        Win32 declarations, tests undefined O_RDONLY/O_WRONLY/O_RDWR flags,
        and file_get_status writes an undeclared `result` instead of
        source->status. Do not re-add it to the include graph.
*/

#include "../library.c"

bool raw_windows_paths = false;

fn file_new_lazy(file_address result, string_address path, positive flags)
{
        result->path = path;
        result->flags = flags;

        HANDLE h = CreateFileA(path, ((flags & O_RDONLY) ? GENERIC_READ : 0) | ((flags & O_WRONLY) ? GENERIC_WRITE : 0) | ((flags & O_RDWR) ? (GENERIC_READ | GENERIC_WRITE) : 0),
                               FILE_SHARE_READ, NULL,
                               ((flags & O_CREAT) ? ((flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS) : OPEN_EXISTING),
                               FILE_ATTRIBUTE_NORMAL, NULL);

        result->handle = (h == INVALID_HANDLE_VALUE) ? -1 : (positive)h;
}

fn file_get_status(file_address source)
{
        BY_HANDLE_FILE_INFORMATION info = {0};
        if (GetFileInformationByHandle((HANDLE)source->handle, address_of info))
        {
                result.size = info.nFileSizeLow;
                result.last_access = info.ftLastAccessTime.dwLowDateTime;
                result.last_edit = info.ftLastWriteTime.dwLowDateTime;
                result.last_update = info.ftCreationTime.dwLowDateTime;
        }
}

fn file_new(file_address result, string_address path, positive flags)
{
        file_new_lazy(result, path, flags);

        file_get_status(result);
}

address_any file_load(file_address source)
{
        if (!file_valid(source))
                return null;

        if (source->loaded && source->data)
                return source->data;

        positive size = source->status.size;

        if (size == 0)
                return null;

        positive page_size = 4096;
        positive pages = (size + page_size - 1) / page_size;

        source->data = VirtualAlloc(NULL, pages * page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!source->data)
                return null;

        DWORD bytes_read;
        SetFilePointer((HANDLE)source->handle, 0, NULL, FILE_BEGIN);
        if (!ReadFile((HANDLE)source->handle, source->data, (DWORD)size, address_of bytes_read, NULL) ||
            bytes_read != size)
        {
                VirtualFree(source->data, 0, MEM_RELEASE);
                source->data = null;
                return null;
        }

        source->loaded = true;
        return source->data;
}

positive file_read(file_address source, address_any buffer, positive size, positive offset)
{
        if (!file_valid(source))
                return -1;

        if (source->loaded && source->data)
        {

                if (offset >= source->status.size)
                        return 0;

                positive available = source->status.size - offset;
                positive to_read = size < available ? size : available;
                memory_copy(buffer, (p8 address_to)source->data + offset, to_read);
                return to_read;
        }

        LARGE_INTEGER li_offset;
        li_offset.QuadPart = offset;
        SetFilePointerEx((HANDLE)source->handle, li_offset, NULL, FILE_BEGIN);

        DWORD bytes_read;
        if (!ReadFile((HANDLE)source->handle, buffer, (DWORD)size, address_of bytes_read, NULL))
                return -1;
        return bytes_read;
}

fn file_unload(file_address source)
{
        if (!source->loaded && !source->data)
                return;

        positive page_size = 4096;
        positive pages = (source->status.size + page_size - 1) / page_size;

        VirtualFree(source->data, 0, MEM_RELEASE);

        source->data = null;
        source->loaded = false;
}

positive file_write(file_address source, address_any buffer, positive size, positive offset)
{
        if (!file_valid(source))
                return -1;

        bool update_memory = source->loaded && source->data && offset < source->status.size;

        LARGE_INTEGER li_offset;
        li_offset.QuadPart = offset;
        SetFilePointerEx((HANDLE)source->handle, li_offset, NULL, FILE_BEGIN);

        DWORD bytes_written;
        if (!WriteFile((HANDLE)source->handle, buffer, (DWORD)size, address_of bytes_written, NULL))
                return -1;

        if (update_memory && bytes_written > 0)
        {
                positive end_offset = offset + bytes_written;
                if (end_offset > source->status.size)
                {
                        file_get_status(source);
                        file_unload(source);
                }
                else
                {
                        memory_copy((p8 address_to)source->data + offset, buffer, bytes_written);
                }
        }

        return bytes_written;
}

fn file_close(file_address source)
{
        if (!file_valid(source))
                return;

        file_unload(source);

        CloseHandle((HANDLE)source->handle);

        source->handle = -1;
        source->path = null;
}

fn library_open(file_address storage_location, string_address library_path)
{
        return LoadLibraryA(library_path);
}

address_any library_get(address_any library, string_address name)
{
        return GetProcAddress(library, name);
}

fn library_close(address_any library)
{
        FreeLibrary(library);
}

string_address working_directory_get()
{
        GetCurrentDirectoryA(sizeof(working_directory), working_directory);

        if (!raw_windows_paths)
                string_replace_all(working_directory, '\\', '/');

        return working_directory;
}

fn working_directory_set(string_address path)
{
        if (!raw_windows_paths)
                string_replace_all(path, '/', '\\');

        SetCurrentDirectoryA(path);

        working_directory_get();
}

address_any memory(positive size)
{
        return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

fn memory_free(address_any address, positive size)
{
        if (!address || size == 0)
                return;

        VirtualFree(address, 0, MEM_RELEASE);
}

#endif
