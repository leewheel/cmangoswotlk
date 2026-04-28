#define _CRT_SECURE_NO_DEPRECATE
#define _CRT_SECURE_NO_WARNINGS

#ifndef MPQ_H
#define MPQ_H

#include "loadlib/loadlib.h"
#include "libmpq/mpq.h"
#include <string.h>
#include <ctype.h>
#include <vector>
#include <iostream>
#include <deque>

using namespace std;

class MPQArchive
{

    public:
        mpq_archive_s* mpq_a;
        std::string filename;
        std::string lastError;

        MPQArchive(const char* filename);
        ~MPQArchive();
        void close();
        bool ExtractFileTo(char const* mpq_name, std::string const& output_filename);
        char const* LastError() const { return lastError.c_str(); }

        void GetFileListTo(vector<string>& filelist);
};
typedef std::deque<MPQArchive*> ArchiveSet;

class MPQFile
{
        //MPQHANDLE handle;
        bool eof;
        char* buffer;
        libmpq__off_t pointer, size;

        // disable copying
        MPQFile(const MPQFile& f) {}
        void operator=(const MPQFile& f) {}

    public:
        MPQFile(const char* filename);    // filenames are not case sensitive
        ~MPQFile() { close(); }
        size_t read(void* dest, size_t bytes);
        size_t getSize() { return size; }
        size_t getPos() { return pointer; }
        char* getBuffer() { return buffer; }
        char* getPointer() { return buffer + pointer; }
        bool isEof() { return eof; }
        void seek(int offset);
        void seekRelative(int offset);
        void close();
};

inline void flipcc(char* fcc)
{
    char t;
    t = fcc[0];
    fcc[0] = fcc[3];
    fcc[3] = t;
    t = fcc[1];
    fcc[1] = fcc[2];
    fcc[2] = t;
}

#endif
