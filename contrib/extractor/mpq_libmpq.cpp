#include "mpq_libmpq.h"
#include <deque>
#include <cstdio>
#include <string>

ArchiveSet gOpenArchives;

static const int LIBMPQ_SAFE_EXCEPTION = -9999;
static const libmpq__off_t MPQ_EXTRACT_MAX_SIZE = 256 * 1024 * 1024;
static const libmpq__off_t MPQ_EXTRACT_GUARD_SIZE = 1024 * 1024;

static int SafeMpqFileNumber(mpq_archive_s* archive, char const* filename, uint32* filenum)
{
#ifdef _WIN32
    __try
    {
        return libmpq__file_number(archive, filename, filenum);
    }
    __except (1)
    {
        return LIBMPQ_SAFE_EXCEPTION;
    }
#else
    return libmpq__file_number(archive, filename, filenum);
#endif
}

static int SafeMpqFileUnpackedSize(mpq_archive_s* archive, uint32 filenum, libmpq__off_t* size)
{
#ifdef _WIN32
    __try
    {
        return libmpq__file_unpacked_size(archive, filenum, size);
    }
    __except (1)
    {
        return LIBMPQ_SAFE_EXCEPTION;
    }
#else
    return libmpq__file_unpacked_size(archive, filenum, size);
#endif
}

static int SafeMpqFileRead(mpq_archive_s* archive, uint32 filenum, unsigned char* buffer, libmpq__off_t size, libmpq__off_t* transferred)
{
#ifdef _WIN32
    __try
    {
        return libmpq__file_read(archive, filenum, buffer, size, transferred);
    }
    __except (1)
    {
        return LIBMPQ_SAFE_EXCEPTION;
    }
#else
    return libmpq__file_read(archive, filenum, buffer, size, transferred);
#endif
}

MPQArchive::MPQArchive(const char* filename)
    : mpq_a(nullptr),
      filename(filename)
{
    int result = libmpq__archive_open(&mpq_a, filename, -1);
    printf("Opening %s\n", filename);
    if (result)
    {
        switch (result)
        {
            case LIBMPQ_ERROR_OPEN :
                printf("Error opening archive '%s': Does file really exist?\n", filename);
                break;
            case LIBMPQ_ERROR_FORMAT :            /* bad file format */
                printf("Error opening archive '%s': Bad file format\n", filename);
                break;
            case LIBMPQ_ERROR_SEEK :         /* seeking in file failed */
                printf("Error opening archive '%s': Seeking in file failed\n", filename);
                break;
            case LIBMPQ_ERROR_READ :              /* Read error in archive */
                printf("Error opening archive '%s': Read error in archive\n", filename);
                break;
            case LIBMPQ_ERROR_MALLOC :               /* maybe not enough memory? :) */
                printf("Error opening archive '%s': Maybe not enough memory\n", filename);
                break;
            default:
                printf("Error opening archive '%s': Unknown error\n", filename);
                break;
        }
        return;
    }
    gOpenArchives.push_front(this);
}

MPQArchive::~MPQArchive()
{
    close();
}

void MPQArchive::close()
{
    if (!mpq_a)
        return;
    /* libmpq__archive_close frees the archive; keep a dangling pointer otherwise. */
    mpq_archive_s* a = mpq_a;
    mpq_a = nullptr;
    if (libmpq__archive_close(a) != 0)
    {
        /* Rare: fclose failed; libmpq did not free - restore handle (retry or leak on exit). */
        mpq_a = a;
    }
}

void MPQArchive::GetFileListTo(vector<string>& filelist)
{
    if (!mpq_a)
        return;
    uint32 filenum;
    int result = SafeMpqFileNumber(mpq_a, "(listfile)", &filenum);
    if (result == LIBMPQ_SAFE_EXCEPTION)
        printf("libmpq exception while locating (listfile) in '%s'\n", filename.c_str());
    if (result)
        return;
    libmpq__off_t size, transferred;
    result = SafeMpqFileUnpackedSize(mpq_a, filenum, &size);
    if (result == LIBMPQ_SAFE_EXCEPTION)
        printf("libmpq exception while reading (listfile) size in '%s'\n", filename.c_str());
    if (result != 0 || size <= 0 || size > MPQ_EXTRACT_MAX_SIZE)
        return;

    char* buffer = new char[static_cast<size_t>(size + MPQ_EXTRACT_GUARD_SIZE)];
    result = SafeMpqFileRead(mpq_a, filenum, (unsigned char*)buffer, size, &transferred);
    if (result == LIBMPQ_SAFE_EXCEPTION)
        printf("libmpq exception while reading (listfile) from '%s'\n", filename.c_str());
    if (result != 0 || transferred != size)
    {
        delete[] buffer;
        return;
    }

    std::string content(buffer, static_cast<size_t>(size));
    delete[] buffer;

    size_t start = 0;
    while (start < content.size())
    {
        size_t end = content.find('\n', start);
        if (end == std::string::npos)
            end = content.size();

        std::string line = content.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty())
            filelist.push_back(line);

        start = end + 1;
    }
}

bool MPQArchive::ExtractFileTo(char const* mpq_name, std::string const& output_filename)
{
    lastError.clear();
    if (!mpq_a)
    {
        lastError = "MPQ archive is not open";
        return false;
    }
    uint32 filenum;
    int result = SafeMpqFileNumber(mpq_a, mpq_name, &filenum);
    if (result == LIBMPQ_SAFE_EXCEPTION)
    {
        printf("libmpq exception while locating '%s' in '%s'\n", mpq_name, filename.c_str());
        lastError = "libmpq exception while locating file";
    }
    if (result)
    {
        if (lastError.empty())
            lastError = "file not found in this MPQ";
        return false;
    }

    libmpq__off_t size, transferred;
    result = SafeMpqFileUnpackedSize(mpq_a, filenum, &size);
    if (result == LIBMPQ_SAFE_EXCEPTION)
    {
        printf("libmpq exception while reading size for '%s' in '%s'\n", mpq_name, filename.c_str());
        lastError = "libmpq exception while reading unpacked size";
    }
    if (result != 0 || size <= 1 || size > MPQ_EXTRACT_MAX_SIZE)
    {
        if (lastError.empty())
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "invalid unpacked size or size read failed: result=%d size=%lld", result, static_cast<long long>(size));
            lastError = msg;
        }
        return false;
    }

    char* buffer = new char[static_cast<size_t>(size + MPQ_EXTRACT_GUARD_SIZE)];
    transferred = 0;
    result = SafeMpqFileRead(mpq_a, filenum, (unsigned char*)buffer, size, &transferred);
    if (result == LIBMPQ_SAFE_EXCEPTION)
    {
        printf("libmpq exception while reading '%s' from '%s'\n", mpq_name, filename.c_str());
        lastError = "libmpq exception while reading file data";
    }
    if (result != 0 || transferred != size)
    {
        if (lastError.empty())
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "file read failed: result=%d transferred=%lld expected=%lld", result, static_cast<long long>(transferred), static_cast<long long>(size));
            lastError = msg;
        }
        delete[] buffer;
        return false;
    }

    FILE* output = fopen(output_filename.c_str(), "wb");
    if (!output)
    {
        printf("Can't create the output file '%s'\n", output_filename.c_str());
        lastError = "cannot create output file";
        delete[] buffer;
        return false;
    }

    size_t w = fwrite(buffer, 1, static_cast<size_t>(size), output);
    fclose(output);
    delete[] buffer;
    if (w != static_cast<size_t>(size))
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "output write failed: wrote=%zu expected=%lld", w, static_cast<long long>(size));
        lastError = msg;
        return false;
    }
    lastError = "ok";
    return w == static_cast<size_t>(size);
}

MPQFile::MPQFile(const char* filename):
    eof(false),
    buffer(0),
    pointer(0),
    size(0)
{
    for (ArchiveSet::iterator i = gOpenArchives.begin(); i != gOpenArchives.end(); ++i)
    {
        mpq_archive* mpq_a = (*i)->mpq_a;

        uint32 filenum;
        int result = SafeMpqFileNumber(mpq_a, filename, &filenum);
        if (result == LIBMPQ_SAFE_EXCEPTION)
        {
            printf("libmpq exception while locating '%s' in '%s'\n", filename, (*i)->filename.c_str());
            continue;
        }
        if (result || size > MPQ_EXTRACT_MAX_SIZE)
            continue;
        libmpq__off_t transferred;
        result = SafeMpqFileUnpackedSize(mpq_a, filenum, &size);
        if (result == LIBMPQ_SAFE_EXCEPTION)
        {
            printf("libmpq exception while reading size for '%s' in '%s'\n", filename, (*i)->filename.c_str());
            continue;
        }
        if (result)
            continue;

        // HACK: in patch.mpq some files don't want to open and give 1 for filesize
        if (size <= 1)
        {
            // printf("info: file %s has size %d; considered dummy file.\n", filename, size);
            continue;
        }
        buffer = new char[static_cast<size_t>(size + MPQ_EXTRACT_GUARD_SIZE)];

        //libmpq_file_getdata
        result = SafeMpqFileRead(mpq_a, filenum, (unsigned char*)buffer, size, &transferred);
        if (result == LIBMPQ_SAFE_EXCEPTION)
            printf("libmpq exception while reading '%s' from '%s'\n", filename, (*i)->filename.c_str());
        if (result != 0 || transferred != size)
        {
            delete[] buffer;
            buffer = 0;
            continue;
        }
        /*libmpq_file_getdata(&mpq_a, hash, fileno, (unsigned char*)buffer);*/
        return;

    }
    eof = true;
    buffer = 0;
}

size_t MPQFile::read(void* dest, size_t bytes)
{
    if (eof) return 0;

    libmpq__off_t rpos = pointer + bytes;
    if (rpos > size)
    {
        bytes = size - pointer;
        eof = true;
    }

    memcpy(dest, &(buffer[pointer]), bytes);

    pointer = rpos;

    return bytes;
}

void MPQFile::seek(int offset)
{
    pointer = offset;
    eof = (pointer >= size);
}

void MPQFile::seekRelative(int offset)
{
    pointer += offset;
    eof = (pointer >= size);
}

void MPQFile::close()
{
    if (buffer) delete[] buffer;
    buffer = 0;
    eof = true;
}
