/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "dbcfile.h"
#include "mpq_libmpq04.h"
#undef min
#undef max

#include <cstdio>
#include <cstring>

extern char output_path[];

DBCFile::DBCFile(const std::string& filename) : filename(filename)
{
    data = NULL;
}

bool DBCFile::open()
{
    std::string diskName = output_path;
    diskName += "/dbc/";
    size_t nameStart = filename.find_last_of("\\/");
    diskName += (nameStart == std::string::npos ? filename : filename.substr(nameStart + 1));

    if (FILE* disk = fopen(diskName.c_str(), "rb"))
    {
        fseek(disk, 0, SEEK_END);
        long fileSize = ftell(disk);
        fseek(disk, 0, SEEK_SET);

        if (fileSize >= 20)
        {
            unsigned char header[4];
            unsigned int na, nb, es, ss;
            if (fread(header, 1, 4, disk) == 4 &&
                header[0] == 'W' && header[1] == 'D' && header[2] == 'B' && header[3] == 'C' &&
                fread(&na, 4, 1, disk) == 1 &&
                fread(&nb, 4, 1, disk) == 1 &&
                fread(&es, 4, 1, disk) == 1 &&
                fread(&ss, 4, 1, disk) == 1)
            {
                size_t payloadSize = size_t(es) * size_t(na) + size_t(ss);
                if (payloadSize <= size_t(fileSize - 20))
                {
                    recordSize = es;
                    recordCount = na;
                    fieldCount = nb;
                    stringSize = ss;

                    data = new unsigned char[payloadSize];
                    stringTable = data + recordSize * recordCount;
                    if (fread(data, 1, payloadSize, disk) == payloadSize)
                    {
                        fclose(disk);
                        return true;
                    }

                    delete [] data;
                    data = NULL;
                }
            }
        }

        fclose(disk);
    }

    MPQFile f(filename.c_str());

    // Need some error checking, otherwise an unhandled exception error occurs
    // if people screw with the data path.
    if (f.isEof() == true)
        return false;

    unsigned char header[4];
    unsigned int na, nb, es, ss;

    f.read(header, 4); // File Header

    if (header[0] != 'W' || header[1] != 'D' || header[2] != 'B' || header[3] != 'C')
    {
        f.close();
        data = NULL;
        printf("Critical Error: An error occured while trying to read the DBCFile %s.", filename.c_str());
        return false;
    }

    //assert(header[0]=='W' && header[1]=='D' && header[2]=='B' && header[3] == 'C');

    f.read(&na, 4); // Number of records
    f.read(&nb, 4); // Number of fields
    f.read(&es, 4); // Size of a record
    f.read(&ss, 4); // String size

    recordSize = es;
    recordCount = na;
    fieldCount = nb;
    stringSize = ss;
    //assert(fieldCount*4 == recordSize);
    assert(fieldCount * 4 >= recordSize);

    data = new unsigned char[recordSize * recordCount + stringSize];
    stringTable = data + recordSize * recordCount;
    f.read(data, recordSize * recordCount + stringSize);
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
