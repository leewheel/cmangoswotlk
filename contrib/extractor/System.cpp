#define _CRT_SECURE_NO_DEPRECATE
#define __STDC_LIMIT_MACROS

#include <sstream>
#include <fstream>
#include <filesystem>
#include <string>

#include <stdio.h>
#include <deque>
#include <set>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <iomanip>
#include <exception>

#ifdef _WIN32
#include "direct.h"
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "dbcfile.h"
#include "mpq_libmpq.h"

#include "loadlib/adt.h"
#include "loadlib/wdt.h"
#include <fcntl.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#if defined( __GNUC__ )
#define _open   open
#define _close close
#ifndef O_BINARY
#define O_BINARY 0
#endif
#else
#include <io.h>
#endif

#ifdef O_LARGEFILE
#define OPEN_FLAGS  (O_RDONLY | O_BINARY | O_LARGEFILE)
#else
#define OPEN_FLAGS (O_RDONLY | O_BINARY)
#endif

#include "Maps/GridMapDefines.h"
#include "G3D/Vector3.h"
#include "Models/M2Structure.h"
extern ArchiveSet gOpenArchives;

typedef struct
{
    char name[64];
    uint32 id;
} map_id;

map_id* map_ids;
uint16* areas;
uint16* LiqType;
char output_path[1024] = ".";
char input_path[1024] = ".";
static bool s_outputPathExplicit = false;
uint32 maxAreaId = 0;
static FILE* g_adLog = nullptr;
static std::string g_adCurrentPhase = "starting";
static std::string g_adCurrentArchive;
static std::string g_adCurrentFile;
static uint32 g_missingRequiredDbc = 0;

struct ExtractSummary
{
    uint32 dbcUnique = 0;
    uint32 dbcWrites = 0;
    uint32 componentWrites = 0;
    uint32 mapFiles = 0;
    uint32 cameraFiles = 0;
    uint32 creatureModelFiles = 0;
};

static ExtractSummary g_extractSummary;

void SetAdProgress(char const* phase, char const* archive = nullptr, char const* file = nullptr)
{
    g_adCurrentPhase = phase ? phase : "";
    g_adCurrentArchive = archive ? archive : "";
    g_adCurrentFile = file ? file : "";
}

void ReportFatalAdError(char const* kind, unsigned long code = 0)
{
    fprintf(stderr, "\n[严重错误] ad.exe 异常退出。\n");
    if (code)
        fprintf(stderr, "异常代码: 0x%08lX\n", code);
    fprintf(stderr, "当前阶段: %s\n", g_adCurrentPhase.c_str());
    if (!g_adCurrentArchive.empty())
        fprintf(stderr, "当前 MPQ: %s\n", g_adCurrentArchive.c_str());
    if (!g_adCurrentFile.empty())
        fprintf(stderr, "当前文件: %s\n", g_adCurrentFile.c_str());
    fprintf(stderr, "请查看日志: %s/ad_extract.log\n", output_path);
    fprintf(stderr, "DBC 详情: %s/dbc/dbc_extract.log\n", output_path);

    if (g_adLog)
    {
        fprintf(g_adLog, "\n[严重错误] ad.exe 异常退出。\n");
        if (kind)
            fprintf(g_adLog, "错误类型: %s\n", kind);
        if (code)
            fprintf(g_adLog, "异常代码: 0x%08lX\n", code);
        fprintf(g_adLog, "当前阶段: %s\n", g_adCurrentPhase.c_str());
        if (!g_adCurrentArchive.empty())
            fprintf(g_adLog, "当前 MPQ: %s\n", g_adCurrentArchive.c_str());
        if (!g_adCurrentFile.empty())
            fprintf(g_adLog, "当前文件: %s\n", g_adCurrentFile.c_str());
        fflush(g_adLog);
    }
}

#ifdef _WIN32
LONG WINAPI AdUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    unsigned long code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    ReportFatalAdError("Windows SEH", code);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void AdTerminateHandler()
{
    ReportFatalAdError("C++ terminate");
    std::_Exit(3);
}

void LogAd(char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);

    vprintf(fmt, args);
    if (g_adLog)
    {
        vfprintf(g_adLog, fmt, copy);
        fflush(g_adLog);
    }

    va_end(copy);
    va_end(args);
}

void OpenAdLog()
{
    std::string logName = output_path;
    logName += "/ad_extract.log";
    g_adLog = fopen(logName.c_str(), "wb");
    if (g_adLog)
    {
        LogAd("ad.exe extraction log\n");
        LogAd("=====================\n");
        LogAd("Input directory : %s\n", input_path);
        LogAd("Output directory: %s\n\n", output_path);
    }
    else
        printf("Warning: cannot create ad log file '%s'\n", logName.c_str());
}

void CloseAdLog()
{
    if (!g_adLog)
        return;
    fclose(g_adLog);
    g_adLog = nullptr;
}

//**************************************************
// Extractor options
//**************************************************
enum Extract
{
    EXTRACT_MAP = 1,
    EXTRACT_DBC = 2,
    EXTRACT_CAMERA = 4,
    EXTRACT_MODELDATA = 8,
};

// Select data for extract
int   CONF_extract = EXTRACT_MAP | EXTRACT_DBC | EXTRACT_CAMERA | EXTRACT_MODELDATA;
// This option allow limit minimum height to some value (Allow save some memory)
// see contrib/mmap/src/Tilebuilder.h, INVALID_MAP_LIQ_HEIGHT
bool  CONF_allow_height_limit = true;
float CONF_use_minHeight = -500.0f;

// This option allow use float to int conversion
bool  CONF_allow_float_to_int   = true;
float CONF_float_to_int8_limit  = 2.0f;      // Max accuracy = val/256
float CONF_float_to_int16_limit = 2048.0f;   // Max accuracy = val/65536
float CONF_flat_height_delta_limit = 0.005f; // If max - min less this value - surface is flat
float CONF_flat_liquid_delta_limit = 0.001f; // If max - min less this value - liquid surface is flat

// List MPQ for extract from
static char const* CONF_mpq_list[] =
{
    "common.MPQ",
    "common-2.MPQ",
    "lichking.MPQ",
    "expansion.MPQ",
    "patch.MPQ",
    "patch-2.MPQ",
    "patch-3.MPQ",
    "patch-4.MPQ",
    "patch-5.MPQ",
};

static char const* CONF_locale_mpq_list[] =
{
    "base-%s.MPQ",
    "backup-%s.MPQ",
    "locale-%s.MPQ",
    "speech-%s.MPQ",
    "expansion-locale-%s.MPQ",
    "expansion-speech-%s.MPQ",
    "lichking-locale-%s.MPQ",
    "lichking-speech-%s.MPQ",
    "patch-%s.MPQ",
    "patch-%s-2.MPQ",
    "patch-%s-3.MPQ",
    "patch-%s-4.MPQ",
    "patch-%s-5.MPQ",
};

static char const* langs[] = {"enGB", "enUS", "deDE", "esES", "frFR", "koKR", "zhCN", "zhTW", "enCN", "enTW", "esMX", "ruRU" };
#define LANG_COUNT 12

// MPQFile checks archives in reverse opening order. Open common archives first
// and locale archives second so localized DBCs override common fallback data,
// matching Trinity/AzerothCore-style extractor behavior.
static int CONF_locale_patch_max = 5;
static int CONF_common_patch_max = 5;
static uint32 const EXPECTED_CLIENT_BUILD = 12340;

static char const* REQUIRED_SERVER_DBC_FILES[] =
{
    "DBFilesClient\\BattlemasterList.dbc",
    "DBFilesClient\\DestructibleModelData.dbc",
    "DBFilesClient\\Faction.dbc",
    "DBFilesClient\\LFGDungeons.dbc",
    "DBFilesClient\\LFGDungeonExpansion.dbc",
    "DBFilesClient\\Map.dbc",
    "DBFilesClient\\QuestFactionReward.dbc",
    "DBFilesClient\\QuestXP.dbc",
    "DBFilesClient\\PvpDifficulty.dbc",
    "DBFilesClient\\SpellDifficulty.dbc",
    "DBFilesClient\\TeamContributionPoints.dbc",
};

static bool IsRequiredServerDbc(std::string const& name)
{
    for (char const* requiredDbc : REQUIRED_SERVER_DBC_FILES)
        if (name == requiredDbc)
            return true;
    return false;
}

static std::string MakeComponentBuildInfo(char const* locale)
{
    std::ostringstream ss;
    ss << "<componentinfo format=\"1\">\n"
       << "    <component name=\"wow-" << locale << "\" version=\"" << EXPECTED_CLIENT_BUILD << "\" />\n"
       << "</componentinfo>\n";
    return ss.str();
}

static bool WriteNormalizedComponentBuildInfo(std::string const& outputFile, char const* locale)
{
    FILE* file = fopen(outputFile.c_str(), "wb");
    if (!file)
        return false;

    std::string text = MakeComponentBuildInfo(locale);
    bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
    fclose(file);
    return ok;
}

inline void CloseMPQFiles();

void CreateDir(const std::string& Path)
{
    std::error_code ec;
    std::filesystem::create_directories(Path, ec);
}

static void WarnSuspiciousOutputPath(const char* p)
{
    if (!p || !p[0])
        return;
    if (strncmp(p, "...", 3) == 0)
        printf("Warning: -o starts with \"...\" - on Windows this is a literal folder name, not shorthand.\nUse a full path, e.g. -o \"E:\\Cmangos\\ClientData\"\n");
    if (strstr(p, "\\...\\") || strstr(p, "/.../"))
        printf("Warning: -o contains a path segment \"...\" (three dots). That is a normal folder name, not \"parent directory\" (use .. for that).\n");
}

/* Make paths absolute and ensure output root exists so fopen does not fail on missing parents. */
static void PreparePathsAfterArgs()
{
    std::error_code ec;
    std::filesystem::path in(input_path);
    if (!in.empty())
    {
        std::filesystem::path absIn = std::filesystem::absolute(in, ec);
        if (!ec)
        {
            std::string s = absIn.string();
            if (s.size() >= sizeof(input_path))
            {
                printf("Fatal: input path too long (max %zu chars).\n", sizeof(input_path) - 1);
                exit(1);
            }
            strncpy(input_path, s.c_str(), sizeof(input_path) - 1);
            input_path[sizeof(input_path) - 1] = '\0';
        }
    }

    if (!s_outputPathExplicit)
    {
        std::filesystem::path def = std::filesystem::path(input_path) / "ClientData";
        def = std::filesystem::absolute(def, ec);
        if (ec)
        {
            printf("Fatal: cannot resolve default output path (<input>/ClientData): %s\n", ec.message().c_str());
            exit(1);
        }
        std::string ds = def.string();
        if (ds.size() >= sizeof(output_path))
        {
            printf("Fatal: default output path too long (max %zu chars).\n", sizeof(output_path) - 1);
            exit(1);
        }
        strncpy(output_path, ds.c_str(), sizeof(output_path) - 1);
        output_path[sizeof(output_path) - 1] = '\0';
        printf("No -o: writing to game folder: %s\n", output_path);
        printf("    (copy this ClientData folder to your server run directory when done.)\n\n");
    }
    else
        WarnSuspiciousOutputPath(output_path);

    ec.clear();
    std::filesystem::path out(output_path);
    if (out.empty())
        return;
    std::filesystem::path absOut = std::filesystem::absolute(out, ec);
    if (ec)
    {
        printf("Fatal: invalid output path \"%s\": %s\n", output_path, ec.message().c_str());
        exit(1);
    }
    std::string os = absOut.string();
    if (os.size() >= sizeof(output_path))
    {
        printf("Fatal: output path too long (max %zu chars).\n", sizeof(output_path) - 1);
        exit(1);
    }
    strncpy(output_path, os.c_str(), sizeof(output_path) - 1);
    output_path[sizeof(output_path) - 1] = '\0';

    std::filesystem::create_directories(absOut, ec);
    if (ec)
    {
        printf("Fatal: cannot create output directory \"%s\": %s\n", output_path, ec.message().c_str());
        exit(1);
    }
    printf("Output directory: %s\n\n", output_path);
}

bool FileExists(const char* FileName)
{
    int fp = _open(FileName, OPEN_FLAGS);
    if (fp != -1)
    {
        _close(fp);
        return true;
    }

    return false;
}

void Usage(char* prg)
{
    printf(
        "Usage:\n"\
        "%s -[var] [value]\n"\
        "-i WoW install root (folder that contains Data\\)\n"\
        "-o output folder (optional; default is <input>\\ClientData next to your game)\n"\
        "-e extract only MAP(1)/DBC(2)/Camera(4)/Attachment(8) - standard: all(15)\n"\
        "-f height stored as int (less map size but lost some accuracy) 1 by default\n"\
        "Typical: %s -e 2 -i \"c:\\games\\wow-3.3.5a\"\n"\
        "Override output: %s -e 2 -i \"c:\\games\\wow\" -o \"d:\\server\\ClientData\"", prg, prg, prg);
    exit(1);
}

void HandleArgs(int argc, char* arg[])
{
    for (int c = 1; c < argc; ++c)
    {
        // i - input path
        // o - output path
        // e - extract only MAP(1)/DBC(2) - standard both(3)
        // f - use float to int conversion
        // h - limit minimum height
        if (arg[c][0] != '-')
            Usage(arg[0]);

        switch (arg[c][1])
        {
            case 'i':
                if (c + 1 < argc)
                {
                    strncpy(input_path, arg[(c++) + 1], sizeof(input_path) - 1);
                    input_path[sizeof(input_path) - 1] = '\0';
                }
                else
                    Usage(arg[0]);
                break;
            case 'o':
                if (c + 1 < argc)
                {
                    s_outputPathExplicit = true;
                    strncpy(output_path, arg[(c++) + 1], sizeof(output_path) - 1);
                    output_path[sizeof(output_path) - 1] = '\0';
                }
                else
                    Usage(arg[0]);
                break;
            case 'f':
                if (c + 1 < argc)                           // all ok
                    CONF_allow_float_to_int = atoi(arg[(c++) + 1]) != 0;
                else
                    Usage(arg[0]);
                break;
            case 'e':
                if (c + 1 < argc)                           // all ok
                {
                    CONF_extract = atoi(arg[(c++) + 1]);
                    if (!(CONF_extract > 0 && CONF_extract < 16))
                        Usage(arg[0]);
                }
                else
                    Usage(arg[0]);
                break;
        }
    }
}

uint32 ReadBuild(int locale)
{
    // include build info file also
    std::string filename  = std::string("component.wow-") + langs[locale] + ".txt";
    //printf("Read %s file... ", filename.c_str());

    MPQFile m(filename.c_str());
    if (m.isEof())
    {
        printf("Fatal error: Not found %s file!\n", filename.c_str());
        exit(1);
    }

    /* MPQ payload is not NUL-terminated; never construct std::string from getPointer(). */
    std::string text(m.getBuffer(), static_cast<size_t>(m.getSize()));
    m.close();

    size_t pos = text.find("version=\"");
    size_t pos1 = pos + strlen("version=\"");
    size_t pos2 = text.find("\"", pos1);
    if (pos == text.npos || pos2 == text.npos || pos1 >= pos2)
    {
        printf("Fatal error: Invalid  %s file format!\n", filename.c_str());
        exit(1);
    }

    std::string build_str = text.substr(pos1, pos2 - pos1);

    int build = atoi(build_str.c_str());
    if (build <= 0)
    {
        printf("Fatal error: Invalid  %s file format!\n", filename.c_str());
        exit(1);
    }

    if (uint32(build) != EXPECTED_CLIENT_BUILD)
    {
        LogAd("MPQ component build is %u; writing map/DBC build metadata as server client build %u.\n", build, EXPECTED_CLIENT_BUILD);
        LogAd("Note: component.wow-%s.txt is an MPQ component manifest, not the WoW.exe file version.\n", langs[locale]);
    }

    return EXPECTED_CLIENT_BUILD;
}

static std::string ParseComponentVersion(std::string const& text)
{
    size_t pos = text.find("version=\"");
    if (pos == text.npos)
        return "";

    size_t pos1 = pos + strlen("version=\"");
    size_t pos2 = text.find("\"", pos1);
    if (pos2 == text.npos || pos1 >= pos2)
        return "";

    return text.substr(pos1, pos2 - pos1);
}

static std::string ReadComponentVersionFromDisk(std::string const& filename)
{
    std::ifstream file(filename.c_str(), std::ios::in | std::ios::binary);
    if (!file)
        return "";

    std::ostringstream ss;
    ss << file.rdbuf();
    return ParseComponentVersion(ss.str());
}

uint32 ReadMapDBC()
{
    printf("Read Map.dbc file... ");
    DBCFile dbc("DBFilesClient\\Map.dbc");

    if (!dbc.open())
    {
        printf("Fatal error: Invalid Map.dbc file format!\n");
        exit(1);
    }

    size_t map_count = dbc.getRecordCount();
    map_ids = new map_id[map_count];
    for (uint32 x = 0; x < map_count; ++x)
    {
        map_ids[x].id = dbc.getRecord(x).getUInt(0);
        strcpy(map_ids[x].name, dbc.getRecord(x).getString(1));
    }
    printf("Done! (%u maps loaded)\n", uint32(map_count));
    return map_count;
}

void ReadAreaTableDBC()
{
    printf("Read AreaTable.dbc file...");
    DBCFile dbc("DBFilesClient\\AreaTable.dbc");

    if (!dbc.open())
    {
        printf("Fatal error: Invalid AreaTable.dbc file format!\n");
        exit(1);
    }

    size_t area_count = dbc.getRecordCount();
    size_t maxid = dbc.getMaxId();
    areas = new uint16[maxid + 1];
    memset(areas, 0xff, (maxid + 1) * sizeof(uint16));

    for (uint32 x = 0; x < area_count; ++x)
        areas[dbc.getRecord(x).getUInt(0)] = dbc.getRecord(x).getUInt(3);

    maxAreaId = dbc.getMaxId();

    printf("Done! (%u areas loaded)\n", uint32(area_count));
}

void ReadLiquidTypeTableDBC()
{
    printf("Read LiquidType.dbc file...");
    DBCFile dbc("DBFilesClient\\LiquidType.dbc");
    if (!dbc.open())
    {
        printf("Fatal error: Invalid LiquidType.dbc file format!\n");
        exit(1);
    }

    size_t LiqType_count = dbc.getRecordCount();
    size_t LiqType_maxid = dbc.getMaxId();
    LiqType = new uint16[LiqType_maxid + 1];
    memset(LiqType, 0xff, (LiqType_maxid + 1) * sizeof(uint16));

    for (uint32 x = 0; x < LiqType_count; ++x)
        LiqType[dbc.getRecord(x).getUInt(0)] = dbc.getRecord(x).getUInt(3);

    printf("Done! (%u LiqTypes loaded)\n", uint32(LiqType_count));
}

//
// Adt file convertor function and data
//

// Map file format data
static char const* MAP_MAGIC         = "MAPS";
static char const* MAP_VERSION_MAGIC = "v1.4";
static char const* MAP_AREA_MAGIC    = "AREA";
static char const* MAP_HEIGHT_MAGIC  = "MHGT";
static char const* MAP_LIQUID_MAGIC  = "MLIQ";

float selectUInt8StepStore(float maxDiff)
{
    return 255 / maxDiff;
}

float selectUInt16StepStore(float maxDiff)
{
    return 65535 / maxDiff;
}
// Temporary grid data store
uint16 area_flags[ADT_CELLS_PER_GRID][ADT_CELLS_PER_GRID];

float V8[ADT_GRID_SIZE][ADT_GRID_SIZE];
float V9[ADT_GRID_SIZE + 1][ADT_GRID_SIZE + 1];
uint16 uint16_V8[ADT_GRID_SIZE][ADT_GRID_SIZE];
uint16 uint16_V9[ADT_GRID_SIZE + 1][ADT_GRID_SIZE + 1];
uint8  uint8_V8[ADT_GRID_SIZE][ADT_GRID_SIZE];
uint8  uint8_V9[ADT_GRID_SIZE + 1][ADT_GRID_SIZE + 1];

uint16 liquid_entry[ADT_CELLS_PER_GRID][ADT_CELLS_PER_GRID];
uint8 liquid_flags[ADT_CELLS_PER_GRID][ADT_CELLS_PER_GRID];
bool  liquid_show[ADT_GRID_SIZE][ADT_GRID_SIZE];
float liquid_height[ADT_GRID_SIZE + 1][ADT_GRID_SIZE + 1];

bool ConvertADT(char* filename, char* filename2, int cell_y, int cell_x, uint32 build)
{
    ADT_file adt;

    if (!adt.loadFile(filename))
        return false;

    adt_MCIN* cells = adt.a_grid->getMCIN();
    if (!cells)
    {
        printf("Can't find cells in '%s'\n", filename);
        return false;
    }

    memset(liquid_show, 0, sizeof(liquid_show));
    memset(liquid_flags, 0, sizeof(liquid_flags));
    memset(liquid_entry, 0, sizeof(liquid_entry));

    // Prepare map header
    GridMapFileHeader map;
    map.mapMagic = *(uint32 const*)MAP_MAGIC;
    map.versionMagic = *(uint32 const*)MAP_VERSION_MAGIC;
    map.buildMagic = build;

    // Get area flags data
    for (int i = 0; i < ADT_CELLS_PER_GRID; i++)
    {
        for (int j = 0; j < ADT_CELLS_PER_GRID; j++)
        {
            adt_MCNK* cell = cells->getMCNK(i, j);
            uint32 areaid = cell->areaid;
            if (areaid && areaid <= maxAreaId)
            {
                if (areas[areaid] != 0xffff)
                {
                    area_flags[i][j] = areas[areaid];
                    continue;
                }
                printf("File: %s\nCan't find area flag for areaid %u [%d, %d].\n", filename, areaid, cell->ix, cell->iy);
            }
            area_flags[i][j] = 0xffff;
        }
    }
    //============================================
    // Try pack area data
    //============================================
    bool fullAreaData = false;
    uint32 areaflag = area_flags[0][0];
    for (int y = 0; y < ADT_CELLS_PER_GRID; y++)
    {
        for (int x = 0; x < ADT_CELLS_PER_GRID; x++)
        {
            if (area_flags[y][x] != areaflag)
            {
                fullAreaData = true;
                break;
            }
        }
    }

    map.areaMapOffset = sizeof(map);
    map.areaMapSize   = sizeof(GridMapAreaHeader);

    GridMapAreaHeader areaHeader;
    areaHeader.fourcc = *(uint32 const*)MAP_AREA_MAGIC;
    areaHeader.flags = 0;
    if (fullAreaData)
    {
        areaHeader.gridArea = 0;
        map.areaMapSize += sizeof(area_flags);
    }
    else
    {
        areaHeader.flags |= MAP_AREA_NO_AREA;
        areaHeader.gridArea = (uint16)areaflag;
    }

    //
    // Get Height map from grid
    //
    for (int i = 0; i < ADT_CELLS_PER_GRID; i++)
    {
        for (int j = 0; j < ADT_CELLS_PER_GRID; j++)
        {
            adt_MCNK* cell = cells->getMCNK(i, j);
            if (!cell)
                continue;
            // Height values for triangles stored in order:
            // 1     2     3     4     5     6     7     8     9
            //    10    11    12    13    14    15    16    17
            // 18    19    20    21    22    23    24    25    26
            //    27    28    29    30    31    32    33    34
            // . . . . . . . .
            // For better get height values merge it to V9 and V8 map
            // V9 height map:
            // 1     2     3     4     5     6     7     8     9
            // 18    19    20    21    22    23    24    25    26
            // . . . . . . . .
            // V8 height map:
            //    10    11    12    13    14    15    16    17
            //    27    28    29    30    31    32    33    34
            // . . . . . . . .

            // Set map height as grid height
            for (int y = 0; y <= ADT_CELL_SIZE; y++)
            {
                int cy = i * ADT_CELL_SIZE + y;
                for (int x = 0; x <= ADT_CELL_SIZE; x++)
                {
                    int cx = j * ADT_CELL_SIZE + x;
                    V9[cy][cx] = cell->ypos;
                }
            }
            for (int y = 0; y < ADT_CELL_SIZE; y++)
            {
                int cy = i * ADT_CELL_SIZE + y;
                for (int x = 0; x < ADT_CELL_SIZE; x++)
                {
                    int cx = j * ADT_CELL_SIZE + x;
                    V8[cy][cx] = cell->ypos;
                }
            }
            // Get custom height
            adt_MCVT* v = cell->getMCVT();
            if (!v)
                continue;
            // get V9 height map
            for (int y = 0; y <= ADT_CELL_SIZE; y++)
            {
                int cy = i * ADT_CELL_SIZE + y;
                for (int x = 0; x <= ADT_CELL_SIZE; x++)
                {
                    int cx = j * ADT_CELL_SIZE + x;
                    V9[cy][cx] += v->height_map[y * (ADT_CELL_SIZE * 2 + 1) + x];
                }
            }
            // get V8 height map
            for (int y = 0; y < ADT_CELL_SIZE; y++)
            {
                int cy = i * ADT_CELL_SIZE + y;
                for (int x = 0; x < ADT_CELL_SIZE; x++)
                {
                    int cx = j * ADT_CELL_SIZE + x;
                    V8[cy][cx] += v->height_map[y * (ADT_CELL_SIZE * 2 + 1) + ADT_CELL_SIZE + 1 + x];
                }
            }
        }
    }
    //============================================
    // Try pack height data
    //============================================
    float maxHeight = -20000;
    float minHeight =  20000;
    for (int y = 0; y < ADT_GRID_SIZE; y++)
    {
        for (int x = 0; x < ADT_GRID_SIZE; x++)
        {
            float h = V8[y][x];
            if (maxHeight < h) maxHeight = h;
            if (minHeight > h) minHeight = h;
        }
    }
    for (int y = 0; y <= ADT_GRID_SIZE; y++)
    {
        for (int x = 0; x <= ADT_GRID_SIZE; x++)
        {
            float h = V9[y][x];
            if (maxHeight < h) maxHeight = h;
            if (minHeight > h) minHeight = h;
        }
    }

    // Check for allow limit minimum height (not store height in deep ochean - allow save some memory)
    if (CONF_allow_height_limit && minHeight < CONF_use_minHeight)
    {
        for (int y = 0; y < ADT_GRID_SIZE; y++)
            for (int x = 0; x < ADT_GRID_SIZE; x++)
                if (V8[y][x] < CONF_use_minHeight)
                    V8[y][x] = CONF_use_minHeight;
        for (int y = 0; y <= ADT_GRID_SIZE; y++)
            for (int x = 0; x <= ADT_GRID_SIZE; x++)
                if (V9[y][x] < CONF_use_minHeight)
                    V9[y][x] = CONF_use_minHeight;
        if (minHeight < CONF_use_minHeight)
            minHeight = CONF_use_minHeight;
        if (maxHeight < CONF_use_minHeight)
            maxHeight = CONF_use_minHeight;
    }

    map.heightMapOffset = map.areaMapOffset + map.areaMapSize;
    map.heightMapSize = sizeof(GridMapHeightHeader);

    GridMapHeightHeader heightHeader;
    heightHeader.fourcc = *(uint32 const*)MAP_HEIGHT_MAGIC;
    heightHeader.flags = 0;
    heightHeader.gridHeight    = minHeight;
    heightHeader.gridMaxHeight = maxHeight;

    if (maxHeight == minHeight)
        heightHeader.flags |= MAP_HEIGHT_NO_HEIGHT;

    // Not need store if flat surface
    if (CONF_allow_float_to_int && (maxHeight - minHeight) < CONF_flat_height_delta_limit)
        heightHeader.flags |= MAP_HEIGHT_NO_HEIGHT;

    // Try store as packed in uint16 or uint8 values
    if (!(heightHeader.flags & MAP_HEIGHT_NO_HEIGHT))
    {
        float step;
        // Try Store as uint values
        if (CONF_allow_float_to_int)
        {
            float diff = maxHeight - minHeight;
            if (diff < CONF_float_to_int8_limit)      // As uint8 (max accuracy = CONF_float_to_int8_limit/256)
            {
                heightHeader.flags |= MAP_HEIGHT_AS_INT8;
                step = selectUInt8StepStore(diff);
            }
            else if (diff < CONF_float_to_int16_limit) // As uint16 (max accuracy = CONF_float_to_int16_limit/65536)
            {
                heightHeader.flags |= MAP_HEIGHT_AS_INT16;
                step = selectUInt16StepStore(diff);
            }
        }

        // Pack it to int values if need
        if (heightHeader.flags & MAP_HEIGHT_AS_INT8)
        {
            for (int y = 0; y < ADT_GRID_SIZE; y++)
                for (int x = 0; x < ADT_GRID_SIZE; x++)
                    uint8_V8[y][x] = uint8((V8[y][x] - minHeight) * step + 0.5f);
            for (int y = 0; y <= ADT_GRID_SIZE; y++)
                for (int x = 0; x <= ADT_GRID_SIZE; x++)
                    uint8_V9[y][x] = uint8((V9[y][x] - minHeight) * step + 0.5f);
            map.heightMapSize += sizeof(uint8_V9) + sizeof(uint8_V8);
        }
        else if (heightHeader.flags & MAP_HEIGHT_AS_INT16)
        {
            for (int y = 0; y < ADT_GRID_SIZE; y++)
                for (int x = 0; x < ADT_GRID_SIZE; x++)
                    uint16_V8[y][x] = uint16((V8[y][x] - minHeight) * step + 0.5f);
            for (int y = 0; y <= ADT_GRID_SIZE; y++)
                for (int x = 0; x <= ADT_GRID_SIZE; x++)
                    uint16_V9[y][x] = uint16((V9[y][x] - minHeight) * step + 0.5f);
            map.heightMapSize += sizeof(uint16_V9) + sizeof(uint16_V8);
        }
        else
            map.heightMapSize += sizeof(V9) + sizeof(V8);
    }

    // Get from MCLQ chunk (old)
    for (int i = 0; i < ADT_CELLS_PER_GRID; i++)
    {
        for (int j = 0; j < ADT_CELLS_PER_GRID; j++)
        {
            adt_MCNK* cell = cells->getMCNK(i, j);
            if (!cell)
                continue;

            adt_MCLQ* liquid = cell->getMCLQ();
            int count = 0;
            if (!liquid || cell->sizeMCLQ <= 8)
                continue;

            for (int y = 0; y < ADT_CELL_SIZE; y++)
            {
                int cy = i * ADT_CELL_SIZE + y;
                for (int x = 0; x < ADT_CELL_SIZE; x++)
                {
                    int cx = j * ADT_CELL_SIZE + x;
                    if (liquid->flags[y][x] != 0x0F)
                    {
                        liquid_show[cy][cx] = true;
                        if (liquid->flags[y][x] & (1 << 7))
                            liquid_flags[i][j] |= MAP_LIQUID_TYPE_DEEP_WATER;
                        ++count;
                    }
                }
            }

            uint32 c_flag = cell->flags;
            if (c_flag & (1 << 2))
            {
                liquid_entry[i][j] = 1;
                liquid_flags[i][j] |= MAP_LIQUID_TYPE_WATER;            // water
            }
            if (c_flag & (1 << 3))
            {
                liquid_entry[i][j] = 2;
                liquid_flags[i][j] |= MAP_LIQUID_TYPE_OCEAN;            // ocean
            }
            if (c_flag & (1 << 4))
            {
                liquid_entry[i][j] = 3;
                liquid_flags[i][j] |= MAP_LIQUID_TYPE_MAGMA;            // magma/slime
            }

            if (!count && liquid_flags[i][j])
                fprintf(stderr, "Wrong liquid detect in MCLQ chunk");

            for (int y = 0; y <= ADT_CELL_SIZE; y++)
            {
                int cy = i * ADT_CELL_SIZE + y;
                for (int x = 0; x <= ADT_CELL_SIZE; x++)
                {
                    int cx = j * ADT_CELL_SIZE + x;
                    liquid_height[cy][cx] = liquid->liquid[y][x].height;
                }
            }
        }
    }

    // Get liquid map for grid (in WOTLK used MH2O chunk)
    adt_MH2O* h2o = adt.a_grid->getMH2O();
    if (h2o)
    {
        for (int i = 0; i < ADT_CELLS_PER_GRID; i++)
        {
            for (int j = 0; j < ADT_CELLS_PER_GRID; j++)
            {
                adt_liquid_header* h = h2o->getLiquidData(i, j);
                if (!h)
                    continue;

                int count = 0;
                uint64 show = h2o->getLiquidShowMap(h);
                for (int y = 0; y < h->height; y++)
                {
                    int cy = i * ADT_CELL_SIZE + y + h->yOffset;
                    for (int x = 0; x < h->width; x++)
                    {
                        int cx = j * ADT_CELL_SIZE + x + h->xOffset;
                        if (show & 1)
                        {
                            liquid_show[cy][cx] = true;
                            ++count;
                        }
                        show >>= 1;
                    }
                }

                liquid_entry[i][j] = h->liquidType;
                switch (LiqType[h->liquidType])
                {
                    case LIQUID_TYPE_WATER: liquid_flags[i][j] |= MAP_LIQUID_TYPE_WATER; break;
                    case LIQUID_TYPE_OCEAN: liquid_flags[i][j] |= MAP_LIQUID_TYPE_OCEAN; break;
                    case LIQUID_TYPE_MAGMA: liquid_flags[i][j] |= MAP_LIQUID_TYPE_MAGMA; break;
                    case LIQUID_TYPE_SLIME: liquid_flags[i][j] |= MAP_LIQUID_TYPE_SLIME; break;
                    default:
                        printf("\nCan't find Liquid type %u for map %s\nchunk %d,%d\n", h->liquidType, filename, i, j);
                        break;
                }
                // Dark water detect
                if (LiqType[h->liquidType] == LIQUID_TYPE_OCEAN)
                {
                    uint8* lm = h2o->getLiquidLightMap(h);
                    if (!lm)
                        liquid_flags[i][j] |= MAP_LIQUID_TYPE_DEEP_WATER;
                }

                if (!count && liquid_flags[i][j])
                    printf("Wrong liquid detect in MH2O chunk");

                float* height = h2o->getLiquidHeightMap(h);
                int pos = 0;
                for (int y = 0; y <= h->height; y++)
                {
                    int cy = i * ADT_CELL_SIZE + y + h->yOffset;
                    for (int x = 0; x <= h->width; x++)
                    {
                        int cx = j * ADT_CELL_SIZE + x + h->xOffset;
                        if (height)
                            liquid_height[cy][cx] = height[pos];
                        else
                            liquid_height[cy][cx] = h->heightLevel1;
                        pos++;
                    }
                }
            }
        }
    }
    //============================================
    // Pack liquid data
    //============================================
    uint16 firstLiquidEntry = liquid_entry[0][0];
    uint8 firstLiquidFlag = liquid_flags[0][0];
    bool fullType = false;
    for (int y = 0; y < ADT_CELLS_PER_GRID; y++)
    {
        for (int x = 0; x < ADT_CELLS_PER_GRID; x++)
        {
            if (liquid_entry[y][x] != firstLiquidEntry || liquid_flags[y][x] != firstLiquidFlag)
            {
                fullType = true;
                y = ADT_CELLS_PER_GRID;
                break;
            }
        }
    }

    GridMapLiquidHeader liquidHeader;

    // no water data (if all grid have 0 liquid type)
    if (firstLiquidFlag == 0 && !fullType)
    {
        // No liquid data
        map.liquidMapOffset = 0;
        map.liquidMapSize   = 0;
    }
    else
    {
        int minX = 255, minY = 255;
        int maxX = 0, maxY = 0;
        maxHeight = -20000;
        minHeight = 20000;
        for (int y = 0; y < ADT_GRID_SIZE; y++)
        {
            for (int x = 0; x < ADT_GRID_SIZE; x++)
            {
                if (liquid_show[y][x])
                {
                    if (minX > x) minX = x;
                    if (maxX < x) maxX = x;
                    if (minY > y) minY = y;
                    if (maxY < y) maxY = y;
                    float h = liquid_height[y][x];
                    if (maxHeight < h) maxHeight = h;
                    if (minHeight > h) minHeight = h;
                }
                else
                {
                    liquid_height[y][x] = CONF_use_minHeight;
                    if (minHeight > CONF_use_minHeight) minHeight = CONF_use_minHeight;
                }
            }
        }
        map.liquidMapOffset = map.heightMapOffset + map.heightMapSize;
        map.liquidMapSize = sizeof(GridMapLiquidHeader);
        liquidHeader.fourcc = *(uint32 const*)MAP_LIQUID_MAGIC;
        liquidHeader.flags = 0;
        liquidHeader.liquidFlags = 0;
        liquidHeader.liquidType = 0;
        liquidHeader.offsetX = minX;
        liquidHeader.offsetY = minY;
        liquidHeader.width   = maxX - minX + 1 + 1;
        liquidHeader.height  = maxY - minY + 1 + 1;
        liquidHeader.liquidLevel = minHeight;

        if (maxHeight == minHeight)
            liquidHeader.flags |= MAP_LIQUID_NO_HEIGHT;

        // Not need store if flat surface
        if (CONF_allow_float_to_int && (maxHeight - minHeight) < CONF_flat_liquid_delta_limit)
            liquidHeader.flags |= MAP_LIQUID_NO_HEIGHT;

        if (!fullType)
            liquidHeader.flags |= MAP_LIQUID_NO_TYPE;

        if (liquidHeader.flags & MAP_LIQUID_NO_TYPE)
        {
            liquidHeader.liquidFlags = firstLiquidFlag;
            liquidHeader.liquidType = firstLiquidEntry;
        }
        else
            map.liquidMapSize += sizeof(liquid_entry) + sizeof(liquid_flags);

        if (!(liquidHeader.flags & MAP_LIQUID_NO_HEIGHT))
            map.liquidMapSize += sizeof(float) * liquidHeader.width * liquidHeader.height;
    }

    // map hole info
    uint16 holes[ADT_CELLS_PER_GRID][ADT_CELLS_PER_GRID];

    if (map.liquidMapOffset)
        map.holesOffset = map.liquidMapOffset + map.liquidMapSize;
    else
        map.holesOffset = map.heightMapOffset + map.heightMapSize;

    map.holesSize = sizeof(holes);
    memset(holes, 0, map.holesSize);

    for (int i = 0; i < ADT_CELLS_PER_GRID; ++i)
    {
        for (int j = 0; j < ADT_CELLS_PER_GRID; ++j)
        {
            adt_MCNK* cell = cells->getMCNK(i, j);
            if (!cell)
                continue;
            holes[i][j] = cell->holes;
        }
    }

    // Ok all data prepared - store it
    FILE* output = fopen(filename2, "wb");
    if (!output)
    {
        printf("Can't create the output file '%s'\n", filename2);
        return false;
    }
    fwrite(&map, sizeof(map), 1, output);
    // Store area data
    fwrite(&areaHeader, sizeof(areaHeader), 1, output);
    if (!(areaHeader.flags & MAP_AREA_NO_AREA))
        fwrite(area_flags, sizeof(area_flags), 1, output);

    // Store height data
    fwrite(&heightHeader, sizeof(heightHeader), 1, output);
    if (!(heightHeader.flags & MAP_HEIGHT_NO_HEIGHT))
    {
        if (heightHeader.flags & MAP_HEIGHT_AS_INT16)
        {
            fwrite(uint16_V9, sizeof(uint16_V9), 1, output);
            fwrite(uint16_V8, sizeof(uint16_V8), 1, output);
        }
        else if (heightHeader.flags & MAP_HEIGHT_AS_INT8)
        {
            fwrite(uint8_V9, sizeof(uint8_V9), 1, output);
            fwrite(uint8_V8, sizeof(uint8_V8), 1, output);
        }
        else
        {
            fwrite(V9, sizeof(V9), 1, output);
            fwrite(V8, sizeof(V8), 1, output);
        }
    }

    // Store liquid data if need
    if (map.liquidMapOffset)
    {
        fwrite(&liquidHeader, sizeof(liquidHeader), 1, output);
        if (!(liquidHeader.flags & MAP_LIQUID_NO_TYPE))
        {
            fwrite(liquid_entry, sizeof(liquid_entry), 1, output);
            fwrite(liquid_flags, sizeof(liquid_flags), 1, output);
        }
        if (!(liquidHeader.flags & MAP_LIQUID_NO_HEIGHT))
        {
            for (int y = 0; y < liquidHeader.height; y++)
                fwrite(&liquid_height[y + liquidHeader.offsetY][liquidHeader.offsetX], sizeof(float), liquidHeader.width, output);
        }
    }

    // store hole data
    fwrite(holes, map.holesSize, 1, output);

    fclose(output);

    return true;
}

uint32 ExtractMapsFromMpq(uint32 build)
{
    char mpq_filename[2048];
    char output_filename[2048];
    char mpq_map_name[2048];
    uint32 extractedMapFiles = 0;

    printf("Extracting maps...\n");

    uint32 map_count = ReadMapDBC();

    ReadAreaTableDBC();
    ReadLiquidTypeTableDBC();

    std::string path = output_path;
    path += "/maps/";
    CreateDir(path);

    printf("Convert map files\n");
    for (uint32 z = 0; z < map_count; ++z)
    {
        printf("Extract %s (%d/%d)                  \n", map_ids[z].name, z + 1, map_count);
        // Loadup map grid data
        sprintf(mpq_map_name, "World\\Maps\\%s\\%s.wdt", map_ids[z].name, map_ids[z].name);
        WDT_file wdt;
        if (!wdt.loadFile(mpq_map_name, false))
        {
//            printf("Error loading %s map wdt data\n", map_ids[z].name);
            continue;
        }

        for (uint32 y = 0; y < WDT_MAP_SIZE; ++y)
        {
            for (uint32 x = 0; x < WDT_MAP_SIZE; ++x)
            {
                if (!wdt.main->adt_list[y][x].exist)
                    continue;
                sprintf(mpq_filename, "World\\Maps\\%s\\%s_%u_%u.adt", map_ids[z].name, map_ids[z].name, x, y);
                sprintf(output_filename, "%s/maps/%03u%02u%02u.map", output_path, map_ids[z].id, y, x);
                if (ConvertADT(mpq_filename, output_filename, y, x, build))
                    ++extractedMapFiles;
            }
            // draw progress bar
            printf("Processing........................%d%%\r", (100 * (y + 1)) / WDT_MAP_SIZE);
        }
    }
    delete [] areas;
    delete [] map_ids;
    printf("Extracted %u map grid files\n", extractedMapFiles);
    g_extractSummary.mapFiles += extractedMapFiles;
    return extractedMapFiles;
}

bool ExtractFile(char const* mpq_name, std::string const& filename)
{
    MPQFile m(mpq_name);
    if (m.isEof())
        return false;

    FILE* output = fopen(filename.c_str(), "wb");
    if (!output)
    {
        printf("Can't create the output file '%s'\n", filename.c_str());
        return false;
    }

    fwrite(m.getPointer(), 1, m.getSize(), output);

    fclose(output);
    return true;
}

bool ExtractMinimizedModelFile(char const* mpq_name, std::string const& filename)
{
    FILE* output = fopen(filename.c_str(), "wb");
    if (!output)
    {
        printf("Can't create the output file '%s'\n", filename.c_str());
        return false;
    }

    MPQFile m(mpq_name);
    if (!m.isEof())
    {
        std::stringstream m2file;
        m2file.write(m.getPointer(), m.getSize());
        m2file.seekg(0, std::ios::end);
        std::streamoff const fileSize = m2file.tellg();

        if (static_cast<uint32_t const>(fileSize) < sizeof(M2Header))
        {
            printf("Creature Model file %s is damaged. File is smaller than header size\n", filename.c_str());
            m2file.clear();
            return false;
        }
        m2file.seekg(0, std::ios::beg);
        char fileCheck[5];
        m2file.read(fileCheck, 4);
        fileCheck[4] = 0;

        // Check file has correct magic (MD20)
        if (strcmp(fileCheck, "MD20"))
        {
            printf("Creature Model file %s is damaged. File identifier not found\n", filename.c_str());
            m2file.clear();
            return false;
        }
        M2Array<M2Attachment> attachments;

        m2file.seekg(0x0F0);
        m2file.read(reinterpret_cast<char*>(&attachments), sizeof(attachments));

        m2file.seekg(attachments.offset);
        std::vector<M2Attachment> attachmentData(attachments.size);
        m2file.read(reinterpret_cast<char*>(attachmentData.data()), attachments.size * sizeof(M2Attachment));

        for (M2Attachment& attachment : attachmentData)
        {
            fwrite(&attachment.id, sizeof(uint32_t), 1, output);
            fwrite(&attachment.position, sizeof(G3D::Vector3), 1, output);
        }
    }

    fclose(output);
    return true;
}

void ExtractDBCFiles(int locale, bool basicLocale)
{
    SetAdProgress("提取 DBC", nullptr, nullptr);
    LogAd("Extracting dbc files...\n");

    std::string path = output_path;
    path += "/dbc/";
    CreateDir(path);
    if (!basicLocale)
    {
        path += langs[locale];
        path += "/";
        CreateDir(path);
    }

    std::string dbcLogName = path + "dbc_extract.log";
    FILE* dbcLog = fopen(dbcLogName.c_str(), "wb");
    if (dbcLog)
    {
        fprintf(dbcLog, "DBC extraction log\n");
        fprintf(dbcLog, "==================\n");
        fprintf(dbcLog, "Locale         : %s\n", langs[locale]);
        fprintf(dbcLog, "Output path    : %s\n", path.c_str());
        fprintf(dbcLog, "Archive order  : common first, locale later, patch-chain overwrite enabled\n\n");
        fflush(dbcLog);
    }
    else
        LogAd("Warning: cannot create DBC extraction log '%s'\n", dbcLogName.c_str());

    std::set<std::string> finalDbcFiles;
    int componentCount = 0;
    int totalWrites = 0;

    // Extract in MPQ open order, not from one global resolved view. Later MPQs
    // overwrite earlier MPQs exactly like the WoW client patch chain.
    string componentName = std::string("component.wow-") + langs[locale] + ".txt";
    string componentOutput = path + componentName;
    for (ArchiveSet::reverse_iterator archive = gOpenArchives.rbegin(); archive != gOpenArchives.rend(); ++archive)
    {
        SetAdProgress("提取 DBC: 读取 MPQ", (*archive)->filename.c_str(), nullptr);
        LogAd("DBC archive: %s\n", (*archive)->filename.c_str());
        if (dbcLog)
        {
            fprintf(dbcLog, "[Archive] %s\n", (*archive)->filename.c_str());
            fflush(dbcLog);
        }

        SetAdProgress("提取 DBC: component", (*archive)->filename.c_str(), componentName.c_str());
        if ((*archive)->ExtractFileTo(componentName.c_str(), componentOutput))
        {
            std::string componentVersion = ReadComponentVersionFromDisk(componentOutput);
            LogAd("Extracted component build info from %s", (*archive)->filename.c_str());
            if (!componentVersion.empty())
                LogAd(" (version=%s)", componentVersion.c_str());
            LogAd("\n");
            if (dbcLog)
            {
                fprintf(dbcLog, "  component: %s -> %s", componentName.c_str(), componentOutput.c_str());
                if (!componentVersion.empty())
                    fprintf(dbcLog, " version=%s", componentVersion.c_str());
                fprintf(dbcLog, "\n");
                fflush(dbcLog);
            }
            ++componentCount;
        }

        vector<string> files;
        SetAdProgress("提取 DBC: 读取 listfile", (*archive)->filename.c_str(), "(listfile)");
        (*archive)->GetFileListTo(files);
        for (vector<string>::iterator iter = files.begin(); iter != files.end(); ++iter)
        {
            if (iter->length() <= strlen(".dbc") || iter->rfind(".dbc") != iter->length() - strlen(".dbc"))
                continue;
            if (iter->find("DBFilesClient\\") != 0)
                continue;

            string filename = path;
            filename += (iter->c_str() + strlen("DBFilesClient\\"));
            SetAdProgress("提取 DBC: 写入文件", (*archive)->filename.c_str(), iter->c_str());
            if (dbcLog)
            {
                fprintf(dbcLog, "  try: %s\n", iter->c_str());
                fflush(dbcLog);
            }
            if ((*archive)->ExtractFileTo(iter->c_str(), filename))
            {
                finalDbcFiles.insert(*iter);
                ++totalWrites;
                if (dbcLog)
                {
                    fprintf(dbcLog, "  write: %s -> %s\n", iter->c_str(), filename.c_str());
                    fflush(dbcLog);
                }
            }
            else if (IsRequiredServerDbc(*iter) && dbcLog)
            {
                fprintf(dbcLog, "  fail: %s -> %s\n", iter->c_str(), (*archive)->LastError());
                fflush(dbcLog);
            }
        }

        if (dbcLog)
        {
            fprintf(dbcLog, "\n");
            fflush(dbcLog);
        }
    }

    if (WriteNormalizedComponentBuildInfo(componentOutput, langs[locale]))
    {
        LogAd("Wrote normalized CMaNGOS build info: %s (version=%u)\n", componentOutput.c_str(), EXPECTED_CLIENT_BUILD);
        if (dbcLog)
        {
            fprintf(dbcLog, "  normalized-component: %s version=%u\n", componentOutput.c_str(), EXPECTED_CLIENT_BUILD);
            fflush(dbcLog);
        }
    }
    else
        LogAd("[错误] 无法写入规范化 build 信息文件: %s\n", componentOutput.c_str());

    for (char const* requiredDbc : REQUIRED_SERVER_DBC_FILES)
    {
        if (finalDbcFiles.find(requiredDbc) == finalDbcFiles.end())
        {
            LogAd("[错误] 必需 DBC 未从 MPQ listfile 成功提取: %s\n", requiredDbc);
            if (dbcLog)
                fprintf(dbcLog, "[Missing required] %s\n", requiredDbc);
            ++g_missingRequiredDbc;
        }
    }

    if (dbcLog)
    {
        fprintf(dbcLog, "\nSummary\n");
        fprintf(dbcLog, "-------\n");
        fprintf(dbcLog, "Unique DBC files     : %u\n", uint32(finalDbcFiles.size()));
        fprintf(dbcLog, "Patch-chain writes   : %u\n", uint32(totalWrites));
        fprintf(dbcLog, "Component writes     : %u\n", uint32(componentCount));
        fclose(dbcLog);
    }

    LogAd("DBC detail log: %s\n", dbcLogName.c_str());
    LogAd("Extracted %u unique DBC files with %u patch-chain writes; component writes=%u\n\n", uint32(finalDbcFiles.size()), uint32(totalWrites), uint32(componentCount));
    g_extractSummary.dbcUnique = uint32(finalDbcFiles.size());
    g_extractSummary.dbcWrites += uint32(totalWrites);
    g_extractSummary.componentWrites += uint32(componentCount);
}

bool EnsureDBCFilesForCurrentTask(int locale)
{
    std::vector<std::string> required;
    if (CONF_extract & EXTRACT_MAP)
    {
        required.push_back("Map.dbc");
        required.push_back("AreaTable.dbc");
        required.push_back("LiquidType.dbc");
    }
    if (CONF_extract & EXTRACT_CAMERA)
        required.push_back("CinematicCamera.dbc");
    if (CONF_extract & EXTRACT_MODELDATA)
        required.push_back("CreatureModelData.dbc");

    for (std::string const& dbcName : required)
    {
        std::string filename = output_path;
        filename += "/dbc/";
        filename += dbcName;
        if (!FileExists(filename.c_str()))
        {
            printf("Required DBC dependency '%s' is missing; extracting DBCs first.\n", dbcName.c_str());
            ExtractDBCFiles(locale, true);
            return true;
        }
    }

    return false;
}

uint32 ExtractCameraFiles(int locale, bool basicLocale)
{
    printf("Extracting camera files...\n");
    DBCFile camdbc("DBFilesClient\\CinematicCamera.dbc");

    if (!camdbc.open())
    {
        printf("Unable to open CinematicCamera.dbc. Camera extract aborted.\n");
        return 0;
    }

    // get camera file list from DBC
    std::vector<std::string> camerafiles;
    size_t cam_count = camdbc.getRecordCount();

    for (uint32 i = 0; i < cam_count; ++i)
    {
        std::string camFile(camdbc.getRecord(i).getString(1));
        size_t loc = camFile.find(".mdx");
        if (loc != std::string::npos)
            camFile.replace(loc, 4, ".m2");
        camerafiles.push_back(std::string(camFile));
    }

    std::string path = output_path;
    path += "/Cameras/";
    CreateDir(path);
    if (!basicLocale)
    {
        path += langs[locale];
        path += "/";
        CreateDir(path);
    }

    // extract M2s
    uint32 count = 0;
    for (std::string thisFile : camerafiles)
    {
        std::string filename = path;
        filename += (thisFile.c_str() + strlen("Cameras\\"));

        if (FileExists(filename.c_str()))
            continue;

        if (ExtractFile(thisFile.c_str(), filename))
            ++count;
    }
    printf("Extracted %u camera files\n", count);
    g_extractSummary.cameraFiles += count;
    return count;
}

uint32 ExtractCreatureModelFiles(int locale, bool basicLocale)
{
    printf("Extracting Creature Model files...\n");
    DBCFile modeldbc("DBFilesClient\\CreatureModelData.dbc");

    if (!modeldbc.open())
    {
        printf("Unable to open CreatureModelData.dbc. Creature Model extract aborted.\n");
        return 0;
    }

    // get camera file list from DBC
    std::vector<std::string> modelfiles;
    size_t model_count = modeldbc.getRecordCount();

    for (uint32 i = 0; i < model_count; ++i)
    {
        std::string modelFile(modeldbc.getRecord(i).getString(2));
        size_t loc = modelFile.find(".mdx");
        if (loc != std::string::npos)
            modelFile.replace(loc, 4, ".m2");
        modelfiles.push_back(std::string(modelFile));
    }

    std::string path = output_path;
    path += "/CreatureModels/";
    CreateDir(path);
    if (!basicLocale)
    {
        path += langs[locale];
        path += "/";
        CreateDir(path);
    }

    // extract M2s
    uint32 count = 0;
    for (std::string thisFile : modelfiles)
    {
        std::string filename = path;

        auto pos = thisFile.find_last_of('\\');
        std::string pureName = thisFile;
        std::replace(pureName.begin(), pureName.end(), '\\', '_');
        filename += pureName;

        if (FileExists(filename.c_str()))
            continue;

        if (ExtractMinimizedModelFile(thisFile.c_str(), filename))
            ++count;
    }
    printf("Extracted %u CreatureModel files\n", count);
    g_extractSummary.creatureModelFiles += count;
    return count;
}

void PrintExtractionSummary()
{
    LogAd("\nExtraction summary\n");
    LogAd("==================\n");
    LogAd("Output directory : %s\n", output_path);
    if ((CONF_extract & EXTRACT_DBC) || g_extractSummary.dbcUnique || g_extractSummary.dbcWrites || g_extractSummary.componentWrites)
    {
        LogAd("DBC files        : %u unique files\n", g_extractSummary.dbcUnique);
        LogAd("DBC writes       : %u patch-chain writes\n", g_extractSummary.dbcWrites);
        LogAd("Component files  : %u writes\n", g_extractSummary.componentWrites);
    }
    if (CONF_extract & EXTRACT_MAP)
        LogAd("Map grid files   : %u .map files\n", g_extractSummary.mapFiles);
    if (CONF_extract & EXTRACT_CAMERA)
        LogAd("Camera files     : %u files\n", g_extractSummary.cameraFiles);
    if (CONF_extract & EXTRACT_MODELDATA)
        LogAd("Creature models  : %u files\n", g_extractSummary.creatureModelFiles);
    LogAd("Extraction complete.\n");
    LogAd("Main log         : %s/ad_extract.log\n", output_path);
    LogAd("DBC detail log   : %s/dbc/dbc_extract.log\n\n", output_path);
}

void LoadLocaleMPQFiles(int const locale, int const maxPatch = CONF_locale_patch_max)
{
    char filename[2048];
    int count = sizeof(CONF_locale_mpq_list) / sizeof(char*);
    for (int i = 0; i < count; ++i)
    {
        int patchNumber = 0;
        if (sscanf(CONF_locale_mpq_list[i], "patch-%*[^-]-%d.MPQ", &patchNumber) == 1)
        {
            if (patchNumber > maxPatch)
                continue;
        }
        else if (strstr(CONF_locale_mpq_list[i], "patch-%s.MPQ") == CONF_locale_mpq_list[i])
        {
            patchNumber = 1;
            if (patchNumber > maxPatch)
                continue;
        }

        char archiveName[128];
        sprintf(archiveName, CONF_locale_mpq_list[i], langs[locale], langs[locale]);
        sprintf(filename, "%s/Data/%s/%s", input_path, langs[locale], archiveName);
        if (FileExists(filename))
        {
            MPQArchive* arch = new MPQArchive(filename);
            if (!arch->mpq_a)
                delete arch;
        }
    }
}

void LoadCommonMPQFiles(int const maxPatch = CONF_common_patch_max)
{
    char filename[2048];
    int count = sizeof(CONF_mpq_list) / sizeof(char*);
    for (int i = 0; i < count; ++i)
    {
        int patchNumber = 0;
        if (sscanf(CONF_mpq_list[i], "patch-%d.MPQ", &patchNumber) == 1)
        {
            if (patchNumber > maxPatch)
                continue;
        }
        else if (_stricmp(CONF_mpq_list[i], "patch.MPQ") == 0)
        {
            patchNumber = 1;
            if (patchNumber > maxPatch)
                continue;
        }

        sprintf(filename, "%s/Data/%s", input_path, CONF_mpq_list[i]);
        if (FileExists(filename))
        {
            MPQArchive* arch = new MPQArchive(filename);
            if (!arch->mpq_a)
                delete arch;
        }
    }
}

inline void CloseMPQFiles()
{
    for (ArchiveSet::iterator j = gOpenArchives.begin(); j != gOpenArchives.end(); ++j)
    {
        (*j)->close();
        delete *j;
    }
    gOpenArchives.clear();
}

int main(int argc, char* arg[])
{
    printf("Map & DBC Extractor\n");
    printf("===================\n\n");

    HandleArgs(argc, arg);
    PreparePathsAfterArgs();
    OpenAdLog();
    std::set_terminate(AdTerminateHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(AdUnhandledExceptionFilter);
#endif

    int FirstLocale = -1;
    uint32 build = 0;

    for (int i = 0; i < LANG_COUNT; i++)
    {
        char tmp1[1536];
        sprintf(tmp1, "%s/Data/%s/locale-%s.MPQ", input_path, langs[i], langs[i]);
        if (FileExists(tmp1))
        {
            SetAdProgress("检测客户端语言", nullptr, langs[i]);
            LogAd("Detected locale: %s (reopening all MPQs for this locale)\n", langs[i]);

            if (FirstLocale < 0)
            {
                FirstLocale = i;
            }

            // Open common first, then locale. Locale archives must have priority
            // for DBC files with localized layouts and strings.
            LoadCommonMPQFiles();
            LoadLocaleMPQFiles(i);
            if (FirstLocale == i)
            {
                build = ReadBuild(i);
                LogAd("Detected MPQ component build: %u\n", build);
            }

            if ((CONF_extract & EXTRACT_DBC) == 0)
            {
                CloseMPQFiles();
                break;
            }

            //Extract DBC files
            if (FirstLocale == i)
            {
                ExtractDBCFiles(i, true);
            }
            else
                ExtractDBCFiles(i, false);

            //Close MPQs
            CloseMPQFiles();
        }
    }

    if (FirstLocale < 0)
    {
        LogAd("[错误] 未检测到客户端语言目录。请确认输入目录包含 Data\\zhCN\\locale-zhCN.MPQ 等文件。\n");
        CloseAdLog();
        return 2;
    }

    if (CONF_extract & EXTRACT_CAMERA)
    {
        LogAd("Using locale: %s\n", langs[FirstLocale]);

        LoadCommonMPQFiles();
        LoadLocaleMPQFiles(FirstLocale);

        EnsureDBCFilesForCurrentTask(FirstLocale);
        ExtractCameraFiles(FirstLocale, true);
        // Close MPQs
        CloseMPQFiles();
    }

    if (CONF_extract & EXTRACT_MODELDATA)
    {
        LogAd("Using locale: %s\n", langs[FirstLocale]);

        LoadCommonMPQFiles();
        LoadLocaleMPQFiles(FirstLocale);

        EnsureDBCFilesForCurrentTask(FirstLocale);
        ExtractCreatureModelFiles(FirstLocale, true);
        // Close MPQs
        CloseMPQFiles();
    }

    if (CONF_extract & EXTRACT_MAP)
    {
        LogAd("Using locale: %s\n", langs[FirstLocale]);

        LoadCommonMPQFiles();
        LoadLocaleMPQFiles(FirstLocale);

        EnsureDBCFilesForCurrentTask(FirstLocale);
        // Extract maps
        ExtractMapsFromMpq(build);

        // Close MPQs
        CloseMPQFiles();
    }

    PrintExtractionSummary();
    bool failed = false;
    if ((CONF_extract & EXTRACT_DBC) && g_extractSummary.dbcUnique == 0)
    {
        LogAd("[错误] DBC 提取没有生成任何 DBC 文件。\n");
        failed = true;
    }
    if ((CONF_extract & EXTRACT_DBC) && g_missingRequiredDbc != 0)
    {
        LogAd("[错误] 有 %u 个必需 DBC 缺失，提取结果不可用于服务端。\n", g_missingRequiredDbc);
        failed = true;
    }
    if ((CONF_extract & EXTRACT_MAP) && g_extractSummary.mapFiles == 0)
    {
        LogAd("[错误] 地图提取没有生成任何 .map 文件。\n");
        failed = true;
    }
    if (failed)
    {
        LogAd("ad.exe 提取失败，请查看上面的错误和日志。\n");
        CloseAdLog();
        return 2;
    }
    CloseAdLog();
    return 0;
}
