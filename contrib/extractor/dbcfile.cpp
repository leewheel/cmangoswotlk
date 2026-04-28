#define _CRT_SECURE_NO_DEPRECATE

#include "dbcfile.h"
#include "mpq_libmpq.h"
#include <cstdio>

extern char output_path[];

DBCFile::DBCFile(const std::string& filename):
    filename(filename),
    data(0)
{
}

bool DBCFile::open()
{
    std::string diskName = output_path;
    diskName += "/dbc/";
    size_t nameStart = filename.find_last_of("\\/");
    diskName += (nameStart == std::string::npos ? filename : filename.substr(nameStart + 1));

    if (FILE* disk = fopen(diskName.c_str(), "rb"))
    {
        char header[4];
        unsigned int na, nb, es, ss;
        if (fread(header, 1, 4, disk) == 4 &&
            header[0] == 'W' && header[1] == 'D' && header[2] == 'B' && header[3] == 'C' &&
            fread(&na, 4, 1, disk) == 1 &&
            fread(&nb, 4, 1, disk) == 1 &&
            fread(&es, 4, 1, disk) == 1 &&
            fread(&ss, 4, 1, disk) == 1 &&
            nb * 4 == es)
        {
            recordSize = es;
            recordCount = na;
            fieldCount = nb;
            stringSize = ss;

            size_t data_size = recordSize * recordCount + stringSize;
            data = new unsigned char[data_size];
            stringTable = data + recordSize * recordCount;
            if (fread(data, 1, data_size, disk) == data_size)
            {
                fclose(disk);
                return true;
            }

            delete [] data;
            data = 0;
        }
        fclose(disk);
    }

    MPQFile f(filename.c_str());
    char header[4];
    unsigned int na, nb, es, ss;

    if (f.read(header, 4) != 4)                             // Number of records
        return false;

    if (header[0] != 'W' || header[1] != 'D' || header[2] != 'B' || header[3] != 'C')
        return false;

    if (f.read(&na, 4) != 4)                                // Number of records
        return false;
    if (f.read(&nb, 4) != 4)                                // Number of fields
        return false;
    if (f.read(&es, 4) != 4)                                // Size of a record
        return false;
    if (f.read(&ss, 4) != 4)                                // String size
        return false;

    recordSize = es;
    recordCount = na;
    fieldCount = nb;
    stringSize = ss;
    if (fieldCount * 4 != recordSize)
        return false;

    data = new unsigned char[recordSize * recordCount + stringSize];
    stringTable = data + recordSize * recordCount;

    size_t data_size = recordSize * recordCount + stringSize;
    if (f.read(data, data_size) != data_size)
        return false;
    f.close();
    return true;
}
DBCFile::~DBCFile()
{
    delete [] data;
}

DBCFile::Record DBCFile::getRecord(size_t id)
{
    assert(data);
    return Record(*this, data + id * recordSize);
}

size_t DBCFile::getMaxId()
{
    assert(data);

    size_t maxId = 0;
    for (size_t i = 0; i < getRecordCount(); ++i)
    {
        if (maxId < getRecord(i).getUInt(0))
            maxId = getRecord(i).getUInt(0);
    }
    return maxId;
}

DBCFile::Iterator DBCFile::begin()
{
    assert(data);
    return Iterator(*this, data);
}
DBCFile::Iterator DBCFile::end()
{
    assert(data);
    return Iterator(*this, stringTable);
}
