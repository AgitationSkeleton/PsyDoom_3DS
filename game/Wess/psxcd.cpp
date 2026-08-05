//------------------------------------------------------------------------------------------------------------------------------------------
// Williams Entertainment Sound System (WESS): PlayStation CD-ROM handling utilities
//
// Note: this module has been almost completely rewritten for PsyDoom due to massive differences in how file I/O and CD audio playback work
// for this port. A lot of the old PsyQ 'LIBCD' code simply did not make sense anymore, hence this is all marked as 'PsyDoom modifications'.
// For the original code consult the version of this file in the 'Old' folder.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "psxcd.h"

#include "Asserts.h"
#include "FatalErrors.h"
#include "psxspu.h"
#include "PsyDoom/DiscInfo.h"
#include "PsyDoom/DiscReader.h"
#include "PsyDoom/ModMgr.h"
#include "PsyDoom/ProgArgs.h"
#include "PsyDoom/PsxVm.h"
#include "PsyDoom/Utils.h"
#include "Spu.h"

#include <cstring>
#include <mutex>

// PsyDoom: raise the open file limit
#if PSYDOOM_MODS
    static constexpr int32_t MAX_OPEN_FILES = 16;   // Maximum number of open files
#else
    static constexpr int32_t MAX_OPEN_FILES = 4;    // Maximum number of open files
#endif

static constexpr int32_t FADE_TIME_MS       = 250;      // Time it takes to fade out CD audio (milliseconds)
static constexpr int32_t CDDA_SECTOR_SIZE   = 2352;     // Size of of a CD digital audio sector

// If true then the 'psxcd' module has been initialized
static bool gbPSXCD_IsCdInit;

// Used to hold a file temporarily after opening
static PsxCd_File gPSXCD_cdfile;

// How many CDDA sectors of audio to hold in memory at once
#if PSYDOOM_3DS
    static constexpr int32_t PSXCD_AUDIO_BUFFER_SECTORS = 48;   // About 640 ms
#else
    static constexpr int32_t PSXCD_AUDIO_BUFFER_SECTORS = 1;
#endif

#if PSYDOOM_3DS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: CD audio is read ahead on the main thread rather than by the SPU.
//
// The SPU asks for CD audio a sample at a time, on the audio thread. Reading the SD card to answer that means the
// audio thread stops dead until the card comes back, and the sound stops with it - a bigger buffer only made that
// happen less often and for longer each time, which is what "cuts out every half second" was.
//
// So the audio thread now only ever takes samples out of this ring, and the main thread puts them in. The lock around
// it is held for a copy and two indices and never across a read, so the audio thread is never waiting on the card.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int32_t CD_RING_SAMPLES = (CDDA_SECTOR_SIZE * PSXCD_AUDIO_BUFFER_SECTORS) / (int32_t) sizeof(int16_t);
static constexpr int32_t CD_SECTOR_SAMPLES = CDDA_SECTOR_SIZE / (int32_t) sizeof(int16_t);

static int16_t      gCdRing[CD_RING_SAMPLES];
static int32_t      gCdRingReadPos;       // Where the audio thread is taking samples from
static int32_t      gCdRingWritePos;      // Where the main thread is putting them in
static int32_t      gCdRingCount;         // How many samples are waiting
static std::mutex   gCdRingMutex;

// Throws away whatever is buffered: for when playback stops or jumps somewhere else
static void ClearCdRing() noexcept {
    std::lock_guard<std::mutex> lock(gCdRingMutex);
    gCdRingReadPos = 0;
    gCdRingWritePos = 0;
    gCdRingCount = 0;
}
#endif

// CD audio playback related state.
// Access to all of this is controlled by the CD player mutex.
static struct {
    DiscReader  discReader          = { PsxVm::gDiscInfo };     // The disc reader used to stream the audio
    bool        bPlay               = false;                    // If 'false' then playback is either paused or stopped (stopped if the disc reader doesn't have a track)
    bool        bLoop               = false;                    // If 'true' then playback is looped upon reaching the end
    int32_t     bufferOffset        = 0;                        // Where we are in the audio buffer
    int32_t     loopTrack           = 0;                        // The track to play when looping
    int32_t     loopSectorOffset    = 0;                        // Offset (in sectors) to start at in the track when looping

    // The CD audio buffer: we read CD audio in chunks.
    //
    // PsyDoom 3DS: read many sectors at a time rather than one.
    //
    // This buffer is refilled from inside the SPU's callback, which runs on the audio thread, and refilling it means
    // reading from the SD card. A single sector is a thirteenth of a second of audio, so the audio thread was stopping
    // to wait on the card about seventy five times a second - several times within a single callback - and the music
    // broke up accordingly. Sound effects were unaffected because they play from emulated SPU RAM and touch no I/O.
    //
    // Holding half a second means waiting on the card roughly twice a second instead, for a read that costs little
    // more than a small one does: on this hardware the cost is mostly in making the request at all, not in the size of
    // it. Seventy five kilobytes is a small price next to a sixteen megabyte zone heap.
    int16_t buffer[(CDDA_SECTOR_SIZE * PSXCD_AUDIO_BUFFER_SECTORS) / sizeof(int16_t)];
} gCdPlayer;

// The lock for the CD player and a helper to lock/unlock via RAII.
// N.B: this *CANNOT* be held the same time as the SPU lock, otherwise deadlock MIGHT occur!
// The SPU can request audio from the CD and thus needs access to the cd player lock also.
static std::recursive_mutex gCdPlayerMutex;

struct LockCdPlayer {
    LockCdPlayer() noexcept { gCdPlayerMutex.lock(); }
    ~LockCdPlayer() noexcept { gCdPlayerMutex.unlock(); }
};

// Disc readers used for each open file
static DiscReader gFileDiscReaders[MAX_OPEN_FILES] = {
    PsxVm::gDiscInfo, PsxVm::gDiscInfo, PsxVm::gDiscInfo, PsxVm::gDiscInfo,
    PsxVm::gDiscInfo, PsxVm::gDiscInfo, PsxVm::gDiscInfo, PsxVm::gDiscInfo,
    PsxVm::gDiscInfo, PsxVm::gDiscInfo, PsxVm::gDiscInfo, PsxVm::gDiscInfo,
    PsxVm::gDiscInfo, PsxVm::gDiscInfo, PsxVm::gDiscInfo, PsxVm::gDiscInfo,
};

//------------------------------------------------------------------------------------------------------------------------------------------
// A callback invoked by the SPU when it wants audio from the CD player - returns a single sample.
//------------------------------------------------------------------------------------------------------------------------------------------
static Spu::StereoSample SpuAudioCallback([[maybe_unused]] void* pUserData) noexcept {
#if PSYDOOM_3DS
    // PsyDoom 3DS: take what has been read ahead and nothing else. No file access, no waiting on the main thread:
    // if the read ahead has not kept up then a moment of silence is far better than stalling the audio thread.
    std::lock_guard<std::mutex> lock(gCdRingMutex);

    if (gCdRingCount < 2)
        return Spu::StereoSample{};

    const Spu::StereoSample sample = { gCdRing[gCdRingReadPos], gCdRing[gCdRingReadPos + 1] };
    gCdRingReadPos = (gCdRingReadPos + 2) % CD_RING_SAMPLES;
    gCdRingCount -= 2;
    return sample;
#else
    // Lock the CD player while we are doing this.
    // Note that this thread also has the SPU lock at this point too.
    // Therefore the main thread must NOT lock both the CD player and the SPU at the same time, or otherwise a deadlock might occur.
    LockCdPlayer cdPlayerLock;

    // If the CD player is not currently active then return silence
    if ((!gCdPlayer.bPlay) || (!gCdPlayer.discReader.isTrackOpen()))
        return Spu::StereoSample{};

    // Check if we have any data left in the buffer firstly
    constexpr int16_t SAMPLE_SIZE = sizeof(int16_t);
    constexpr int32_t NUM_BUFFER_SAMPLES = sizeof(gCdPlayer.buffer) / SAMPLE_SIZE;
    static_assert(NUM_BUFFER_SAMPLES % 2 == 0);

    DiscReader& disc = gCdPlayer.discReader;

    if (gCdPlayer.bufferOffset + 1 >= NUM_BUFFER_SAMPLES) {
        // Get the size of the track and where we are at in it
        const DiscTrack* pTrack = disc.getOpenTrack();
        int32_t trackSize = pTrack->trackPayloadSize;
        int32_t trackOffset = disc.tell();

        // See if there is any data left in the track to read
        if (trackOffset >= trackSize) {
            // We reached the end, do we loop back around again?
            if (gCdPlayer.bLoop) {
                // Looping: rewind back to the start plus any additional offset.
                // Change tracks also if we need to.
                if (disc.getTrackNum() != gCdPlayer.loopTrack) {
                    disc.setTrackNum(gCdPlayer.loopTrack);

                    // Need to re-fetch this info when changing tracks
                    pTrack = disc.getOpenTrack();
                    trackSize = pTrack->trackPayloadSize;
                }

                if (gCdPlayer.loopSectorOffset > 0) {
                    disc.trackSeekAbs(CDDA_SECTOR_SIZE * gCdPlayer.loopSectorOffset);
                } else {
                    disc.trackSeekAbs(0);
                }

                trackOffset = disc.tell();
            }
            else {
                // No looping, mark the CD player as no longer playing and return an empty sample
                gCdPlayer.bPlay = false;
                return Spu::StereoSample{};
            }
        }

        // Read what we can and zero anything we can't (in case the last sector is short for some reason)
        const int32_t samplesToRead = std::min<int32_t>((trackSize - trackOffset) / SAMPLE_SIZE, NUM_BUFFER_SAMPLES);
        const int32_t samplesToZero = NUM_BUFFER_SAMPLES - samplesToRead;
        disc.read(gCdPlayer.buffer, samplesToRead * SAMPLE_SIZE);

        if (samplesToZero > 0) {
            std::memset(gCdPlayer.buffer + samplesToRead * SAMPLE_SIZE, 0, (size_t) samplesToZero * SAMPLE_SIZE);
        }

        gCdPlayer.bufferOffset = 0;
    }

    // Should have samples in the buffer at this point, return the requested samples
    ASSERT(gCdPlayer.bufferOffset + 2 <= NUM_BUFFER_SAMPLES);

    Spu::StereoSample sample = { gCdPlayer.buffer[gCdPlayer.bufferOffset], gCdPlayer.buffer[gCdPlayer.bufferOffset + 1] };
    gCdPlayer.bufferOffset += 2;
    return sample;
#endif
}

#if PSYDOOM_3DS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: tops the CD audio ring back up. Must be called regularly from the main thread, and never from the
// audio thread, since this is where the SD card is actually read.
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_update_audio_buffer() noexcept {
    LockCdPlayer cdPlayerLock;

    if ((!gCdPlayer.bPlay) || (!gCdPlayer.discReader.isTrackOpen()))
        return;

    DiscReader& disc = gCdPlayer.discReader;

    // Fill whatever room there is, a sector at a time. Reading happens outside the ring's lock so the audio thread is
    // never held up by it; only the copy in afterwards takes the lock.
    //
    // Only so many sectors per visit though. Filling all the room there is meant the first visit after music started
    // read the whole buffer - a hundred kilobytes off the card in one go, with everything else waiting on it. That is
    // felt worst in a network game, where the two consoles run in lockstep and a stall on one holds up both, which is
    // what made Club Doom crawl the moment its music began.
    //
    // Eight sectors is about 18 KB a visit, and still supplies more than playback consumes even if this is only
    // reached ten times a second - so the read ahead keeps filling, it just gets there over several visits instead of
    // stopping the world once. Worth the margin: a console struggling enough to update this rarely is exactly the one
    // that must not also lose its music.
    constexpr int32_t MAX_SECTORS_PER_UPDATE = 8;

    // ...and nothing at all until there is that much room to fill, so most visits cost nothing.
    //
    // Without this the ring is topped up on every single visit, which on the menus and the title screen - where this is
    // reached far more often than the game needs - turns into a constant trickle of small reads off the card. Batching
    // them is the same bytes in far fewer trips.
    {
        std::lock_guard<std::mutex> lock(gCdRingMutex);

        if (CD_RING_SAMPLES - gCdRingCount < CD_SECTOR_SAMPLES * MAX_SECTORS_PER_UPDATE)
            return;
    }

    for (int32_t sectorsRead = 0; sectorsRead < MAX_SECTORS_PER_UPDATE; ++sectorsRead) {
        int32_t freeSamples;

        {
            std::lock_guard<std::mutex> lock(gCdRingMutex);
            freeSamples = CD_RING_SAMPLES - gCdRingCount;
        }

        if (freeSamples < CD_SECTOR_SAMPLES)
            break;

        const DiscTrack* pTrack = disc.getOpenTrack();
        int32_t trackSize = pTrack->trackPayloadSize;
        int32_t trackOffset = disc.tell();

        if (trackOffset >= trackSize) {
            if (!gCdPlayer.bLoop) {
                gCdPlayer.bPlay = false;
                return;
            }

            if (disc.getTrackNum() != gCdPlayer.loopTrack) {
                disc.setTrackNum(gCdPlayer.loopTrack);
                pTrack = disc.getOpenTrack();
                trackSize = pTrack->trackPayloadSize;
            }

            disc.trackSeekAbs((gCdPlayer.loopSectorOffset > 0) ? CDDA_SECTOR_SIZE * gCdPlayer.loopSectorOffset : 0);
            trackOffset = disc.tell();
        }

        int16_t staging[CD_SECTOR_SAMPLES];
        const int32_t samplesToRead = std::min<int32_t>((trackSize - trackOffset) / (int32_t) sizeof(int16_t), CD_SECTOR_SAMPLES);
        const int32_t samplesToZero = CD_SECTOR_SAMPLES - samplesToRead;

        if (samplesToRead > 0) {
            disc.read(staging, samplesToRead * (int32_t) sizeof(int16_t));
        }

        if (samplesToZero > 0) {
            std::memset(staging + samplesToRead, 0, (size_t) samplesToZero * sizeof(int16_t));
        }

        {
            std::lock_guard<std::mutex> lock(gCdRingMutex);

            for (int32_t i = 0; i < CD_SECTOR_SAMPLES; ++i) {
                gCdRing[gCdRingWritePos] = staging[i];
                gCdRingWritePos = (gCdRingWritePos + 1) % CD_RING_SAMPLES;
            }

            gCdRingCount += CD_SECTOR_SAMPLES;
        }
    }
}
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Initialize the WESS (Williams Entertainment Sound System) CD handling module.
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_init() noexcept {
    // If we've already done this then just no-op
    if (gbPSXCD_IsCdInit)
        return;

    gbPSXCD_IsCdInit = true;

    // Initialize the SPU and install the CD player as an external input to the SPU
    psxspu_init();

    {
        PsxVm::LockSpu spuLock;
        PsxVm::gSpu.pExtInputCallback = SpuAudioCallback;
        PsxVm::gSpu.pExtInputUserData = nullptr;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Shut down the WESS (Williams Entertainment Sound System) CD handling module
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_exit() noexcept {
    // Uninstall the CD player as an external input to the SPU
    {
        PsxVm::LockSpu spuLock;
        PsxVm::gSpu.pExtInputCallback = nullptr;
        PsxVm::gSpu.pExtInputUserData = nullptr;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Open a specified CD file for reading
//------------------------------------------------------------------------------------------------------------------------------------------
PsxCd_File* psxcd_open(const CdFileId discFile) noexcept {
    // Zero init the temporary file structure
    gPSXCD_cdfile = {};

    // Modding mechanism: allow files to be overridden with user files in a specified directory.
    // Note that we do this check BEFORE validating if the file exists on-disc because PsyDoom now allows Doom format maps (.WAD)
    // to override maps in Final Doom (.ROM files). The .WAD map files of course won't exist on-disc in the case of Final Doom.
    if (ModMgr::areOverridesAvailableForFile(discFile))
        return (ModMgr::openOverridenFile(discFile, gPSXCD_cdfile)) ? &gPSXCD_cdfile : nullptr;

    // Figure out where the file is on disc and sanity check the file is valid
    const PsxCd_MapTblEntry fileTableEntry = CdMapTbl_GetEntry(discFile);

    if (fileTableEntry == PsxCd_MapTblEntry{}) {
        FatalErrors::raiseF("psxcd_open: attempt to open non-existing file '%s'!", discFile.c_str().data());
    }

    // Find a free disc reader slot to accomodate this file
    int32_t discReaderIdx = -1;

    for (int32_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!gFileDiscReaders[i].isTrackOpen()) {
            discReaderIdx = i;
            break;
        }
    }

    if (discReaderIdx < 0) {
        FatalErrors::raise("psxcd_open: out of file handles!");
    }

    // Open up the disc reader for it and save it's details
    DiscReader& discReader = gFileDiscReaders[discReaderIdx];

    if (!discReader.setTrackNum(1)) {
        FatalErrors::raise("psxcd_open: failed to open a disc reader for the data track!");
    }

    if (!discReader.trackSeekAbs(fileTableEntry.startSector * CDROM_SECTOR_SIZE)) {
        FatalErrors::raise("psxcd_open: failed to seek to the specified file!");
    }

    gPSXCD_cdfile.size = fileTableEntry.size;
    gPSXCD_cdfile.startSector = fileTableEntry.startSector;
    gPSXCD_cdfile.fileHandle = discReaderIdx + 1;
    return &gPSXCD_cdfile;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Tells if the cdrom is currently seeking to a location for audio playback; the answer for this is always 'false' now for PsyDoom
//------------------------------------------------------------------------------------------------------------------------------------------
bool psxcd_seeking_for_play() noexcept { return false; }

//------------------------------------------------------------------------------------------------------------------------------------------
// Read the specified number of bytes synchronously from the given CD file and returns the number of bytes read
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t psxcd_read(void* const pDest, int32_t numBytes, PsxCd_File& file) noexcept {
    // Modding mechanism: allow files to be overriden with user files in a specified directory
    if (ModMgr::isFileOverriden(file))
        return ModMgr::readFromOverridenFile(pDest, numBytes, file);

    // If the file does not have a valid handle then the read fails
    if ((file.fileHandle <= 0) || (file.fileHandle > MAX_OPEN_FILES))
        return -1;

    // Verify that the read is in bounds for the file and fail if it isn't
    DiscReader& reader = gFileDiscReaders[file.fileHandle - 1];

    const int32_t fileBegByteIdx = file.startSector * CDROM_SECTOR_SIZE;
    const int32_t fileEndByteIdx = fileBegByteIdx + file.size;
    const int32_t curByteIdx = reader.tell();

    if ((curByteIdx < fileBegByteIdx) || (curByteIdx + numBytes > fileEndByteIdx))
        return -1;

    // Do the actual read and return the number of bytes read
    return (reader.read(pDest, numBytes)) ? numBytes : -1;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Seek to a specified position in a file, relatively or absolutely.
// Returns '0' on success, any other value on failure.
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t psxcd_seek(PsxCd_File& file, int32_t offset, const PsxCd_SeekMode mode) noexcept {
    // Modding mechanism: allow files to be overriden with user files in a specified directory
    if (ModMgr::isFileOverriden(file))
        return ModMgr::seekForOverridenFile(file, offset, mode);

    // If the file handle is invalid then the seek fails
    if ((file.fileHandle <= 0) || (file.fileHandle > MAX_OPEN_FILES))
        return -1;

    DiscReader& reader = gFileDiscReaders[file.fileHandle - 1];

    if (mode == PsxCd_SeekMode::SET) {
        // Seek to an absolute position in the file: make sure the offset is valid and try to go to it
        if ((offset < 0) || (offset > file.size))
            return -1;

        return (reader.trackSeekAbs(file.startSector * CDROM_SECTOR_SIZE + offset)) ? 0 : -1;
    }
    else if (mode == PsxCd_SeekMode::CUR) {
        // Seek relative to the current IO position: make sure the offset is valid and try to go to it
        const int32_t curOffset = reader.tell() - file.startSector * CDROM_SECTOR_SIZE;
        const int32_t newOffset = curOffset + offset;

        if ((newOffset < 0) || (newOffset > file.size))
            return -1;

        return (reader.trackSeekRel(offset)) ? 0 : -1;
    }
    else if (mode == PsxCd_SeekMode::END) {
        // Seek relative to the end: make sure the offset is valid and try to go to it
        const int32_t newOffset = file.size - offset;

        if ((newOffset < 0) || (newOffset > file.size))
            return -1;

        return (reader.trackSeekAbs(file.startSector * CDROM_SECTOR_SIZE + newOffset)) ? 0 : -1;
    }

    return -1;  // Bad seek mode!
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Returns the current IO offset within the given file
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t psxcd_tell(const PsxCd_File& file) noexcept {
    // Modding mechanism: allow files to be overriden with user files in a specified directory
    if (ModMgr::isFileOverriden(file))
        return ModMgr::tellForOverridenFile(file);

    // If the file handle is invalid then the tell fails
    if ((file.fileHandle <= 0) || (file.fileHandle > MAX_OPEN_FILES))
        return -1;

    // Tell where we are in the file
    DiscReader& reader = gFileDiscReaders[file.fileHandle - 1];
    return reader.tell() - file.startSector * CDROM_SECTOR_SIZE;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Close a CD file and free up the file slot
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_close([[maybe_unused]] PsxCd_File& file) noexcept {
    // Modding mechanism: allow files to be overriden with user files in a specified directory
    if (ModMgr::isFileOverriden(file)) {
        ModMgr::closeOverridenFile(file);
        return;
    }

    // If it's a file on the game CD then close out any open disc readers it has and then zero the struct
    if ((file.fileHandle > 0) && (file.fileHandle <= MAX_OPEN_FILES)) {
        gFileDiscReaders[file.fileHandle - 1].closeTrack();
    }

    file = {};
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Internal helper to eliminate the redundancy between 'psxcd_play_at_andloop' and 'psxcd_play_at'.
// This is a new addition for PsyDoom.
//------------------------------------------------------------------------------------------------------------------------------------------
static void psxcd_play_internal(
    const int32_t track,
    const int32_t vol,
    const int32_t sectorOffset,
    const int32_t fadeUpTime,
    const bool bLoop,
    const int32_t loopTrack,
    const int32_t loopSectorOffset
) noexcept {
    // Ignore the command in headless mode
    if (ProgArgs::gbHeadlessMode)
        return;

    // Switch to the specified track and temporarily pause: if it fails then stop and abort
    bool setTrackOk;

    {
        // N.B: don't hold this lock in the main thread at the same time as the SPU lock - otherwise deadlock might occur!
        LockCdPlayer cdPlayerLock;
        gCdPlayer.bPlay = false;
        setTrackOk = gCdPlayer.discReader.setTrackNum(track);
    }

    if (!setTrackOk) {
        psxcd_stop();
        return;
    }

    // Start mixing in CD audio set the volume level and start fading (if requested)
    psxspu_setcdmixon();

    if (fadeUpTime <= 0) {
        psxspu_set_cd_vol(vol);
        psxspu_stop_cd_fade();
    } else {
        psxspu_set_cd_vol(0);
        psxspu_start_cd_fade(fadeUpTime, vol);
    }

    // Skip the requested number of sectors
    {
        // N.B: don't hold this lock in the main thread at the same time as the SPU lock - otherwise deadlock might occur!
        LockCdPlayer cdPlayerLock;

        if (sectorOffset > 0) {
            gCdPlayer.discReader.trackSeekAbs(CDDA_SECTOR_SIZE * sectorOffset);
        }

        // Mark the player as playing and save loop parameters
        gCdPlayer.bPlay = true;
        gCdPlayer.bufferOffset = CDDA_SECTOR_SIZE / sizeof(int16_t);    // Need to read a sector

        // PsyDoom 3DS: whatever was buffered belongs to wherever playback used to be
        #if PSYDOOM_3DS
            ClearCdRing();
        #endif
        gCdPlayer.bLoop = bLoop;
        gCdPlayer.loopTrack = loopTrack;
        gCdPlayer.loopSectorOffset = loopSectorOffset;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Play the given cd track and loop another track afterwards using the specified parameters.
//
//  track:              The track to play
//  vol:                Track volume
//  sectorOffset:       To start past the normal track start
//  fadeUpTime:         Milliseconds to fade in the track, or '0' if instant play.
//  loopTrack:          What track to play in loop after this track ends
//  loopVol:            What volume to play that looped track at.
//                          NOTE: This is IGNORED by PsyDoom and was always the same as the original volume in PSX DOOM.
//  loopSectorOffset:   What sector offset to use for the looped track
//  loopFadeUpTime:     Fade up time for the looped track.
//                          NOTE: This is IGNORED by PsyDoom and was always '0' in PSX Doom.
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_play_at_andloop(
    const int32_t track,
    const int32_t vol,
    const int32_t sectorOffset,
    const int32_t fadeUpTime,
    const int32_t loopTrack,
    [[maybe_unused]] const int32_t loopVol,
    const int32_t loopSectorOffset,
    [[maybe_unused]] const int32_t loopFadeUpTime
) noexcept {
    // PsyDoom: to simplify threading and very messy synchronization in the CD audio callback these fields are no longer supported.
    // Setting them to values other than this will no longer work! That's OK because Doom always followed these usage patterns:
    ASSERT(loopVol == vol);
    ASSERT(loopFadeUpTime == 0);

    psxcd_play_internal(track, vol, sectorOffset, fadeUpTime, true, loopTrack, loopSectorOffset);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Begin playint the specified cd track at the given volume level.
// A sector offset can also be specified to begin from a certain location in the track.
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_play_at(const int32_t track, const int32_t vol, const int32_t sectorOffset) noexcept {
    psxcd_play_internal(track, vol, sectorOffset, 0, false, 0, 0);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Play the given audio track at the specified volume level
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_play(const int32_t track, const int32_t vol) noexcept {
    psxcd_play_at(track, vol, 0);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Stop playback of cd audio; unlike 'psxcd_pause' playback CANNOT be resumed by calling 'psxcd_restart' afterwards
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_stop() noexcept {
    // Quickly fade out cd audio if playing
    bool bMightNeedFade = false;

    {
        // N.B: don't hold this lock in the main thread at the same time as the SPU lock - otherwise deadlock might occur!
        LockCdPlayer cdPlayerLock;
        bMightNeedFade = (gCdPlayer.discReader.isTrackOpen() && gCdPlayer.bPlay);
    }

    if (bMightNeedFade) {
        const int32_t startCdVol = psxspu_get_cd_vol();

        if (startCdVol != 0) {
            psxspu_start_cd_fade(FADE_TIME_MS, 0);
            Utils::waitForCdAudioFadeOut();
        }
    }

    // Close the disc and zero out everything
    {
        // N.B: don't hold this lock in the main thread at the same time as the SPU lock - otherwise deadlock might occur!
        LockCdPlayer cdPlayerLock;

        gCdPlayer.discReader.closeTrack();
        gCdPlayer.bPlay = false;
        gCdPlayer.bLoop = false;
        gCdPlayer.bufferOffset = 0;
        gCdPlayer.loopSectorOffset = 0;

        #if PSYDOOM_3DS
            ClearCdRing();
        #endif
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Pause cd audio playback and make a note of where we paused at
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_pause() noexcept {
    // Quickly fade out cd audio if playing
    bool bMightNeedFade = false;

    {
        // N.B: don't hold this lock in the main thread at the same time as the SPU lock - otherwise deadlock might occur!
        LockCdPlayer cdPlayerLock;
        bMightNeedFade = (gCdPlayer.discReader.isTrackOpen() && gCdPlayer.bPlay);
    }

    if (bMightNeedFade) {
        const int32_t startCdVol = psxspu_get_cd_vol();

        if (startCdVol != 0) {
            psxspu_start_cd_fade(FADE_TIME_MS, 0);
            Utils::waitForCdAudioFadeOut();
        }
    }

    // Mark as no longer playing
    {
        // N.B: don't hold this lock in the main thread at the same time as the SPU lock - otherwise deadlock might occur!
        LockCdPlayer cdPlayerLock;
        gCdPlayer.bPlay = false;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Restart cd audio playback: playback resumes from where the cd was last paused
//------------------------------------------------------------------------------------------------------------------------------------------
void psxcd_restart(const int32_t vol) noexcept {
    // Only Do this if we are actually playing a track
    {
        // N.B: don't hold this lock in the main thread at the same time as the SPU lock - otherwise deadlock might occur!
        LockCdPlayer cdPlayerLock;

        if (!gCdPlayer.discReader.isTrackOpen())
            return;

        // Begin playing again
        gCdPlayer.bPlay = true;
    }

    // Set the audio volume
    psxspu_set_cd_vol(vol);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Tells how many sectors have elapsed during cd playback
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t psxcd_elapsed_sectors() noexcept {
    // N.B: don't hold this lock in the main thread at the same time as the SPU lock - otherwise deadlock might occur!
    LockCdPlayer cdPlayerLock;
    return (gCdPlayer.discReader.isTrackOpen()) ? gCdPlayer.discReader.tell() / CDDA_SECTOR_SIZE : 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the size of a file
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t psxcd_get_file_size(const CdFileId discFile) noexcept {
    // Modding mechanism: allow files to be overriden with user files in a specified directory
    if (ModMgr::areOverridesAvailableForFile(discFile))
        return ModMgr::getOverridenFileSize(discFile);

    return CdMapTbl_GetEntry(discFile).size;
}

int32_t psxcd_get_playing_track() noexcept {
    LockCdPlayer cdPlayerLock;
    const DiscTrack* const pTrack = gCdPlayer.discReader.getOpenTrack();
    return (pTrack) ? pTrack->trackNum : -1;
}
