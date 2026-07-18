#include "cue.hpp"

#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>

AppRes<Cue> read_cue(const std::string_view path) {
    const auto tokenize = [](const char *line) {
        std::vector<std::string_view> tokens;

        const char *cur = line;
        while (*cur) {
            while (isspace(*cur)) cur++;
            if (!*cur) break;
            
            if (*cur == '"') {
                cur++;                                   // move one char forward
                const char *begin = cur;                 // store the beginning
                while (*cur && *cur != '"') cur++;       // increment current till next double-quote or null-terminator
                tokens.emplace_back(begin, cur - begin); // store the view
                if (*cur == '"') cur++;                  // if double-quote (other possibity is null-terminator), move one char forward
            } else {
                const char *begin = cur;
                while (*cur && !isspace(*cur)) cur++;
                tokens.emplace_back(begin, cur - begin);
            }
        }

        return tokens;
    };

    static const std::unordered_map<std::string_view, CueCmd> cmd_map {
        {"FILE"  , CueCmd::File },
        {"TRACK" , CueCmd::Track},
        {"INDEX" , CueCmd::Index},
        {"REM"   , CueCmd::KnownButUnhandled},
        {"PREGAP", CueCmd::KnownButUnhandled},
        {"TITLE" , CueCmd::KnownButUnhandled},
    };
    static const std::unordered_map<std::string_view, CueFileType> file_type_map {
        {"BINARY"  , CueFileType::Binary  },
        {"WAVE"    , CueFileType::Wave    },
        {"AIFF"    , CueFileType::AIFF    },
        {"MP3"     , CueFileType::MP3     },
        {"MOTOROLO", CueFileType::Motorola}
    };
    static const std::unordered_map<std::string_view, CueTrackType> track_type_map {
        {"AUDIO"     , CueTrackType::Audio     },
        {"MODE1/2352", CueTrackType::Mode1_2352},
        {"MODE1/2048", CueTrackType::Mode1_2048},
        {"MODE2/2352", CueTrackType::Mode2_2352},
        {"MODE2/2336", CueTrackType::Mode2_2336},
        {"CDI/2352"  , CueTrackType::CDI_2352  },
        {"CDI/2336"  , CueTrackType::CDI_2336  },
    };

    FILE *f = fopen(path.data(), "r");
    if (!f) return std::unexpected(AppErr {
        .code = std::error_code(errno, std::generic_category()),
        .msg = fmt("Failed to open file: %s", path.data())
    });

    Cue cue;

    char *line = nullptr;
    size_t line_len = 0;
    while (::getline(&line, &line_len, f) >= 0) {
        const auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        const auto it = cmd_map.find(tokens[0]);
        if (it == cmd_map.end()) {
            free(line);
            fclose(f);
            return std::unexpected(AppErr {
                .code = std::error_code(0, std::generic_category()),
                .msg = fmt("Failed to open file: %s", path.data())
            }); 
        }

        switch (it->second) {
            case CueCmd::KnownButUnhandled: break;

            case CueCmd::File: {
                if (tokens.size() != 3) throw std::runtime_error("A");

                const auto it2 = file_type_map.find(tokens[2]);
                if (it2 == file_type_map.end()) throw std::runtime_error("B");

                cue.files.push_back(CueFile {
                    std::string(tokens[1]),
                    it2->second,
                    std::vector<CueTrack>()
                });

                break;
            }

            case CueCmd::Track: {
                if (tokens.size() != 3) throw std::runtime_error("C");

                if (cue.files.empty()) throw std::runtime_error("F");

                int track_num = -1;
                try {
                    track_num = std::stoi(tokens[1].data());
                } catch (std::exception&) {
                    throw std::runtime_error("D");
                }

                const auto it2 = track_type_map.find(tokens[2]);
                if (it2 == track_type_map.end()) throw std::runtime_error("E");

                size_t sector_size = 0;  // initialized to 0 to silence gcc
                switch (it2->second) {
                    case CueTrackType::Audio:
                    case CueTrackType::Mode1_2352:
                    case CueTrackType::Mode2_2352:
                    case CueTrackType::CDI_2352:
                        sector_size = 2352;
                        break;
                    
                    case CueTrackType::Mode2_2336:
                    case CueTrackType::CDI_2336:
                        sector_size = 2336;
                        break;

                    case CueTrackType::Mode1_2048:
                        sector_size = 2048;
                        break;
                }

                // printf("%d %zu\n", track_num, sector_size);

                auto &tracks = cue.files.back().tracks;
                tracks.push_back(CueTrack {
                    track_num,
                    it2->second,
                    std::vector<CueIndex>(),
                    sector_size,
                    0  // TODO: calculate 
                });

                break;
            }
            
            case CueCmd::Index: {
                if (tokens.size() != 3) throw std::runtime_error("G");
                
                if (cue.files.empty()) throw std::runtime_error("I");
                if (cue.files.back().tracks.empty()) throw std::runtime_error("J");

                int index_num = -1;
                try {
                    index_num = std::stoi(tokens[1].data());
                } catch (std::exception&) {
                    throw std::runtime_error("H");
                }

                MSF msf;
                if (sscanf(
                    tokens[2].data(),
                    "%hhu:%hhu:%hhu",
                    &msf.min, &msf.sec, &msf.frame
                ) != 3) throw std::runtime_error("K");

                cue.files.back().tracks.back().indices.push_back({
                    index_num,
                    msf
                });

                break;
            }
        }

        free(line); line = nullptr;
    }
    if (line) free(line);

    return cue;
}

AppRes<void> write_cue(const TOC &toc, const std::string &bin_name, const std::string &cue_name) {
    FILE *f = fopen(cue_name.c_str(), "w");
    if (!f) return std::unexpected(AppErr{
        .code = std::error_code(errno, std::generic_category()),
        .msg = fmt("Failed to open '%s'.", cue_name.c_str())
    });

    fprintf(f, "FILE \"%s\" BINARY\n\n", bin_name.c_str());
    for (const Track &t : toc.tracks) {
        if (t.is_data_track) fprintf(f, "TRACK %02u MODE2/2352\n", t.num); // TODO: FIX: MODE2/2352 shouldn't be hardcoded!!!
        else                 fprintf(f, "TRACK %02u AUDIO\n"     , t.num);
        fprintf(f, "INDEX 01 %s\n\n", MSF::from_sector(t.start_lba).to_str().c_str());
    }

    fclose(f);
    return {};
}

