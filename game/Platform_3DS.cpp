#include "Platform_3DS.h"

#include <3ds.h>
#include <minizip/unzip.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace {

constexpr const char* ARCHIVE_PATH = "romfs:/psydoom/disc.zip";
constexpr const char* SD_ROOT = "sdmc:/3ds/PsyDoom";
constexpr const char* LOG_PATH = "sdmc:/3ds/PsyDoom/launch.log";

void presentConsole() noexcept {
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

bool pathExists(const std::string& path) noexcept {
    struct stat pathInfo {};
    return stat(path.c_str(), &pathInfo) == 0;
}

bool writeReadyMarker(const std::string& path) noexcept {
    std::FILE* const marker = std::fopen(path.c_str(), "wb");
    if (!marker)
        return false;

    constexpr char MARKER_CONTENTS[] = "psydoom-3ds-disc-v1\n";
    const bool success = std::fwrite(MARKER_CONTENTS, 1, sizeof(MARKER_CONTENTS) - 1, marker) == sizeof(MARKER_CONTENTS) - 1;
    return (std::fclose(marker) == 0) && success;
}

bool createDirectory(const std::string& path) noexcept {
    if (path.empty() || pathExists(path))
        return true;

    const std::string::size_type separator = path.find_last_of('/');
    if ((separator != std::string::npos) && (!createDirectory(path.substr(0, separator))))
        return false;

    return (mkdir(path.c_str(), 0777) == 0) || (errno == EEXIST);
}

bool isSafeArchivePath(const char* const path) noexcept {
    if ((!path) || (!path[0]) || (path[0] == '/') || (path[0] == '\\'))
        return false;

    const std::string entryPath(path);
    return (entryPath.find("../") == std::string::npos) &&
           (entryPath.find("..\\") == std::string::npos) &&
           (entryPath.find(':') == std::string::npos);
}

bool extractCurrentFile(unzFile archive, const std::string& destinationRoot) noexcept {
    unz_file_info fileInfo {};
    std::array<char, 512> entryName {};
    if (unzGetCurrentFileInfo(archive, &fileInfo, entryName.data(), entryName.size(), nullptr, 0, nullptr, 0) != UNZ_OK)
        return false;

    if (!isSafeArchivePath(entryName.data()))
        return false;

    std::string relativePath(entryName.data());
    for (char& character : relativePath) {
        if (character == '\\')
            character = '/';
    }

    const std::string outputPath = destinationRoot + "/" + relativePath;
    if ((!relativePath.empty()) && (relativePath.back() == '/'))
        return createDirectory(outputPath.substr(0, outputPath.size() - 1));

    const std::string::size_type separator = outputPath.find_last_of('/');
    if ((separator != std::string::npos) && (!createDirectory(outputPath.substr(0, separator))))
        return false;

    struct stat outputInfo {};
    if ((stat(outputPath.c_str(), &outputInfo) == 0) &&
        S_ISREG(outputInfo.st_mode) &&
        (static_cast<unsigned long long>(outputInfo.st_size) == static_cast<unsigned long long>(fileInfo.uncompressed_size))) {
        return true;
    }

    if (unzOpenCurrentFile(archive) != UNZ_OK)
        return false;

    std::FILE* const output = std::fopen(outputPath.c_str(), "wb");
    if (!output) {
        unzCloseCurrentFile(archive);
        return false;
    }

    bool success = true;
    std::array<unsigned char, 64 * 1024> buffer {};
    while (success) {
        const int bytesRead = unzReadCurrentFile(archive, buffer.data(), static_cast<unsigned int>(buffer.size()));
        if (bytesRead < 0) {
            success = false;
        } else if (bytesRead == 0) {
            break;
        } else if (std::fwrite(buffer.data(), 1, static_cast<std::size_t>(bytesRead), output) != static_cast<std::size_t>(bytesRead)) {
            success = false;
        }
    }

    if (std::fclose(output) != 0)
        success = false;
    if (unzCloseCurrentFile(archive) != UNZ_OK)
        success = false;

    if (!success)
        std::remove(outputPath.c_str());

    return success;
}

bool extractArchive(const std::string& destinationRoot) noexcept {
    unzFile const archive = unzOpen(ARCHIVE_PATH);
    if (!archive)
        return false;

    unz_global_info archiveInfo {};
    bool success = (unzGetGlobalInfo(archive, &archiveInfo) == UNZ_OK) &&
                   (unzGoToFirstFile(archive) == UNZ_OK);

    for (uLong fileIndex = 0; success && (fileIndex < archiveInfo.number_entry); ++fileIndex) {
        std::printf("\x1b[5;1HExtracting game data...\nFile %lu of %lu      ", fileIndex + 1, archiveInfo.number_entry);
        presentConsole();
        success = extractCurrentFile(archive, destinationRoot);
        if (success && (fileIndex + 1 < archiveInfo.number_entry))
            success = (unzGoToNextFile(archive) == UNZ_OK);
    }

    if (unzClose(archive) != UNZ_OK)
        success = false;

    return success;
}

//--------------------------------------------------------------------------------------------------------------------------------------
// Brings up a console on a screen, if one is not already up.
//
// SDL owns the displays once the game is running, so anything that wants to print has to take them back first. Tracked
// so the display is only ever handed back if it was taken here.
//--------------------------------------------------------------------------------------------------------------------------------------
bool gbOwnsConsole = false;

void openConsole(const gfxScreen_t screen) noexcept {
    if (gbOwnsConsole)
        return;

    gfxInitDefault();
    consoleInit(screen, nullptr);
    gbOwnsConsole = true;
}

void closeConsole() noexcept {
    if (gbOwnsConsole) {
        gfxExit();
        gbOwnsConsole = false;
    }
}

void waitForExit() noexcept {
    std::printf("\nPress START to return to HOME Menu.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        presentConsole();
    }
}

}

namespace Platform3DS {

bool isNew3DS() noexcept {
    static bool bQueried = false;
    static bool bIsNew3DS = false;

    if (!bQueried) {
        bQueried = true;

        // Note: this needs the APT service, which 'SDL_n3ds_main' brings up before 'main' runs.
        // If the query fails for any reason then assume the weaker hardware.
        bool result = false;
        bIsNew3DS = (R_SUCCEEDED(APT_CheckNew3DS(&result)) && result);
    }

    return bIsNew3DS;
}

//--------------------------------------------------------------------------------------------------------------------------------------
// Startup logging: see the header for why every line is flushed and closed on its own
//--------------------------------------------------------------------------------------------------------------------------------------
const char* logPath() noexcept {
    return LOG_PATH;
}

void logOpen() noexcept {
    // The log lives beside the extracted disc data, so the folder may not exist yet on a first run
    createDirectory(SD_ROOT);

    std::FILE* const log = std::fopen(LOG_PATH, "wb");

    if (log) {
        std::fclose(log);
    }

    logf("PsyDoom 3DS startup log");
    logf("Build: %s %s, variant '%s'", __DATE__, __TIME__, PSYDOOM_3DS_VARIANT_DIR);
    logf("Environment: envIsHomebrew=%d, systemLanguage handled by libctru", (int) envIsHomebrew());
    logf("Startup %s", memoryStatusString());
    logf("Console: %s", (isNew3DS()) ? "New 3DS/2DS" : "Old 3DS/2DS");

    // How the process carved up its memory. An installed title gets whatever arrangement its exheader asked for, which
    // can be a good deal less than homebrew inherits, so these two numbers decide whether the game can fit at all.
    logf(
        "Heaps: application heap %lu KiB, linear heap %lu KiB",
        (unsigned long)(envGetHeapSize() / 1024),
        (unsigned long)(envGetLinearHeapSize() / 1024)
    );
}

const char* memoryStatusString() noexcept {
    static char buffer[128];

    std::snprintf(
        buffer,
        sizeof(buffer),
        "memory: %lu KiB free of %lu KiB application, %lu KiB linear free",
        (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
        (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024),
        (unsigned long)(linearSpaceFree() / 1024)
    );

    return buffer;
}

void logf(const char* const format, ...) noexcept {
    std::FILE* const log = std::fopen(LOG_PATH, "ab");

    if (!log)
        return;

    std::va_list args;
    va_start(args, format);
    std::vfprintf(log, format, args);
    va_end(args);

    std::fputc('\n', log);
    std::fflush(log);
    std::fclose(log);
}

//--------------------------------------------------------------------------------------------------------------------------------------
// Puts a fatal error on the screen and waits for the player, instead of dropping them back to the HOME Menu with
// nothing to go on. Anything that got as far as the log is named here too, so the player knows where to look.
//--------------------------------------------------------------------------------------------------------------------------------------
void reportFatalError(const char* const msg) noexcept {
    const char* const message = (msg) ? msg : "An unspecified error has occurred.";
    logf("FATAL ERROR: %s", message);

    openConsole(GFX_TOP);
    std::printf("\x1b[2J\x1b[1;1H%s\n\n", PSYDOOM_3DS_APP_NAME);
    std::printf("A fatal error has occurred:\n\n%s\n\n", message);
    std::printf("A log of this startup was written to:\n%s\n", LOG_PATH);
    waitForExit();
    closeConsole();
}

bool prepareDisc(std::string& cuePath) noexcept {
    const std::string variantRoot = std::string(SD_ROOT) + "/" + PSYDOOM_3DS_VARIANT_DIR;
    const std::string discRoot = variantRoot + "/disc";
    const std::string readyPath = variantRoot + "/disc.ready";
    cuePath = discRoot + "/" + PSYDOOM_3DS_DISC_CUE;

    logf("Disc: variant root '%s'", variantRoot.c_str());
    logf("Disc: cue path '%s'", cuePath.c_str());

    if (pathExists(readyPath) && pathExists(cuePath)) {
        logf("Disc: already extracted, skipping preparation");
        return true;
    }

    logf(
        "Disc: needs extracting (ready marker %s, cue %s)",
        (pathExists(readyPath)) ? "present" : "missing",
        (pathExists(cuePath)) ? "present" : "missing"
    );

    std::remove(readyPath.c_str());

    openConsole(GFX_BOTTOM);
    std::printf("\x1b[1;1H%s\n\n", PSYDOOM_3DS_APP_NAME);

    // Whether there is anything to extract decides what to say next, so check before promising a long wait
    const bool bHaveArchive = pathExists(ARCHIVE_PATH);
    logf("Disc: archive '%s' %s", ARCHIVE_PATH, (bHaveArchive) ? "present" : "NOT IN THIS PACKAGE");

    if (!bHaveArchive) {
        // A data-less package: the small build, meant for a console that already has the disc extracted.
        // The console is 40 columns wide, so the lines are kept short enough not to wrap.
        logf("Disc: this package carries no game data and none is on the SD card");
        std::printf("This package has no game data in it,\n");
        std::printf("and none was found on the SD card.\n\n");
        std::printf("Install the full package once to\n");
        std::printf("extract the data. After that this\n");
        std::printf("smaller one can be used instead.\n\n");
        std::printf("Looked for:\n%s\n", cuePath.c_str());
        waitForExit();
        closeConsole();
        return false;
    }

    std::printf("Preparing PlayStation game data on SD.\n");
    std::printf("This is needed once and may take\n");
    std::printf("several minutes.\n");
    presentConsole();

    const bool bMadeDir = createDirectory(discRoot);
    logf("Disc: create '%s' %s", discRoot.c_str(), (bMadeDir) ? "ok" : "FAILED");

    const bool bExtracted = bMadeDir && extractArchive(discRoot);
    logf("Disc: extract archive '%s' %s", ARCHIVE_PATH, (bExtracted) ? "ok" : "FAILED");

    const bool bHaveCue = bExtracted && pathExists(cuePath);
    logf("Disc: cue file after extract %s", (bHaveCue) ? "present" : "MISSING");

    const bool success = bHaveCue && writeReadyMarker(readyPath);
    logf("Disc: preparation %s", (success) ? "succeeded" : "FAILED");

    if (!success) {
        std::printf("\n\nCould not prepare game data.\n");
        std::printf("Check SD free space and reinstall this build.\n");
        std::printf("Expected archive: %s\n", ARCHIVE_PATH);
        waitForExit();
    } else {
        std::printf("\n\nGame data ready. Starting PsyDoom...\n");
        presentConsole();
    }

    closeConsole();
    return success;
}

}

