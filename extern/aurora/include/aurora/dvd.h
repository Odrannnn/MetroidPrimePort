#ifndef AURORA_DVD_H
#define AURORA_DVD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <dolphin/types.h>

/**
 * Open a GC/Wii disc image for use by the DVD API.
 * Must be called before DVDInit().
 * Returns true on success, false on failure.
 */
bool aurora_dvd_open(const char* disc_path);

/**
 * Close the disc image and free all resources.
 */
void aurora_dvd_close(void);

/**
 * OVERLAY FILES!
 *
 * Overlay files allow you to replace and add ("overlay") files that are present in the loaded DVD.
 * The way this works is pretty simple: you provide some callbacks and a list of files.
 * When an overlaid file gets read, your callbacks get called instead of pulling from the underlying DVD.
 *
 * Original disc EntryNums are not touched by the overlay system. New files and directories
 * are assigned stable EntryNums by Aurora.
 */

/**
 * \brief A single file to be overlaid over the DVD files.
 *
 * You do not need to concern yourself with providing entries for directories. They are automatically merged
 * and created where necessary.
 */
typedef struct AuroraOverlayFile {
  /**
   * \brief Absolute file path of this file.
   *
   * Must be in the form "/foo/bar/baz.txt", note the leading slash.
   */
  const char* fileName;

  /**
   * \brief Userdata pointer that will be passed to the callback when this file is opened.
   */
  void* userData;

  /**
   * \brief Size of this file, in bytes.
   *
   * While this is of type size_t, file sizes larger than u32 are not currently supported.
   */
  size_t size;

} AuroraOverlayFile;

/**
 * \brief Callbacks to implement overlay files.
 *
 * Callbacks may be ran from any thread at any time. Make sure they're thread safe!
 */
typedef struct AuroraOverlayCallbacks {
  /**
   * Called when a new file has been opened.
   *
   * Returns an opaque handle that will be passed to the remaining callbacks. Receives the userdata specified in
   * the AuroraOverlayFile.
   */
  void* (*open)(void* userdata);

  /**
   * Close a file handle previously returned from the open callback.
   */
  void (*close)(void* handle);

  /**
   * Read data from a file handle.
   *
   * Returns the amount of data read, or -1 on error.
   */
  int64_t (*read)(void* handle, uint8_t* buf, size_t len);

  /**
   * Seek to a position in a file handle.
   *
   * Returns the resulting position, or -1 on error.
   */
  int64_t (*seek)(void* handle, int64_t offset, int32_t whence);
} AuroraOverlayCallbacks;

/**
 * \brief Specify callbacks for overlaid files.
 */
void aurora_dvd_overlay_callbacks(const AuroraOverlayCallbacks* callbacks);

/**
 * \brief Specify a set of overlay files to be used by the DVD layer.
 *
 * Calling this function immediately applies the new files and rebuilds the FST, replacing any
 * previously specified set. It may be called again at runtime; FST reads and overlay opens are
 * serialized against the rebuild, and previously assigned EntryNums are stable per path. Files
 * removed by a re-registration fail to open from then on (or revert to the underlying DVD file);
 * already-open handles are unaffected. Only call from one thread at a time.
 *
 * This function must be called *after* aurora_dvd_overlay_callbacks.
 *
 * @param files Array of AuroraOverlayFiles, one for every file being overlaid.
 * @param nFiles Amount of files in the array.
 * @param outEntryNums Optional output array receiving one EntryNum per input file. Unaccepted files receive -1.
 */
void aurora_dvd_overlay_files(const AuroraOverlayFile* files, size_t nFiles, s32* outEntryNums);

/**
 * \brief Gets the amount of FST entries present on the loaded game disc.
 *
 * This does not take overlay files into account.
 */
s32 aurora_dvd_base_entry_count();

/**
 * \brief Opens a file of the loaded disc by its base EntryNum, ignoring overlays.
 *
 * Lets an overlay serve a patched version of the file it replaces. Does not take the FST lock, so it may be called
 * from overlay callbacks. Returns null for directories, EntryNums past aurora_dvd_base_entry_count(), or with no disc
 * open. The handle must be closed with aurora_dvd_base_close before the disc is closed.
 */
void* aurora_dvd_base_open(s32 entrynum);

/** \brief Reads from a handle from aurora_dvd_base_open. Returns the amount read, or -1 on error. */
int64_t aurora_dvd_base_read(void* handle, uint8_t* buf, size_t len);

/** \brief Seeks a handle from aurora_dvd_base_open. Returns the resulting position, or -1 on error. */
int64_t aurora_dvd_base_seek(void* handle, int64_t offset, int32_t whence);

/** \brief Closes a handle from aurora_dvd_base_open. */
void aurora_dvd_base_close(void* handle);

/** \brief Disc offset of a base file's data, or -1 for directories and invalid EntryNums. */
int64_t aurora_dvd_base_offset(s32 entrynum);

/**
 * \brief Called the first time a read of the disc image itself fails (overlay files don't count).
 *
 * Runs on whichever thread did the read, so it must be thread safe. Pass null to remove it.
 */
void aurora_dvd_set_read_error_callback(void (*callback)(void));

/**
 * \brief A second disc image, read-only and separate from the game's disc (e.g. another region's, for its text).
 */
typedef struct AuroraDiscImage AuroraDiscImage;

/** \brief Opens an image; writes its 6-character game id, disc number and version. Returns null on failure. */
AuroraDiscImage* aurora_disc_image_open(const char* path, char gameId[6], u8* discNumber, u8* discVersion);

/** \brief Calls `callback` for every file (not directory) of the image's data partition, with its FST index. */
void aurora_disc_image_list(AuroraDiscImage* image, void (*callback)(u32 index, const char* name, u32 size, void* user),
                            void* user);

/**
 * \brief Opens a file of the image by FST index. Read, seek and close it with the aurora_dvd_base_* calls (its
 * failures aren't reported to the read error callback). Close it before the image.
 */
void* aurora_disc_image_file_open(AuroraDiscImage* image, u32 index);

/** \brief Closes an image from aurora_disc_image_open. */
void aurora_disc_image_close(AuroraDiscImage* image);

#ifdef __cplusplus
}
#endif

#endif
