#ifndef CUE_H
#define CUE_H

#include <string_view>
#include <string>
#include <vector>

#include "error.hpp"
#include "toc.hpp"

// ---
enum class CueCmd {
    File,
    Track,
    Index,
    KnownButUnhandled
};

// ---
struct CueIndex {
    int num;
    MSF msf;
};

enum class CueTrackType {
    Audio     , // audio
    Mode1_2352, // mode1 raw
    Mode1_2048, // mode1 user data
    Mode2_2352, // mode2 raw
    Mode2_2336, // mode2 payload (without sync and EDC; with XA sub-header)
    CDI_2352  , // CD-i raw
    CDI_2336  , // CD-i payload

};
struct CueTrack {
    int num;
    CueTrackType type;

    std::vector<CueIndex> indices;

    size_t _sector_size;  // in bytes
    size_t _file_off;  // in bytes
};

enum class CueFileType {
    Binary,
    Wave,
    AIFF,
    MP3,
    Motorola
};
struct CueFile {
    std::string filename;
    CueFileType type;

    std::vector<CueTrack> tracks;
};

struct Cue {
    std::vector<CueFile> files;
};

// ---
AppRes<Cue> read_cue(const std::string_view path);
AppRes<void> write_cue(const TOC &toc, const std::string &bin_name, const std::string &cue_name);

#endif