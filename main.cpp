#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <expected>
#include <system_error>
#include <atomic>
#include <csignal>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>

// ---------------------------------------------------------
// Types
// ---------------------------------------------------------

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef size_t   usize;

typedef uint8_t byte;

// ---------------------------------------------------------
// Errors
// ---------------------------------------------------------

struct AppErr {
    std::error_code code;
    std::string msg;
};

template <typename T>
using AppRes = std::expected<T, AppErr>;

// ---------------------------------------------------------
// Utils
// ---------------------------------------------------------

#define bcd_to_bin(x) ((((x) >> 4) * 10) + ((x) & 0x0F))

#define MSF_FMT "%02d:%02d:%02d"

void print_hex(byte *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        printf("%02x ", data[i]);
    }
}

template<typename... Args>
std::string fmt(const char *fmt, Args... args) {
    int size_s = std::snprintf(nullptr, 0, fmt, args...) + 1; 
    if (size_s <= 0) return "";
    
    auto size = static_cast<size_t>(size_s);

    std::string str;
    str.resize(size);

    std::snprintf(str.data(), size + 1, fmt, args...);
    
    return str;
}

std::string fmt_time(int s) {
    int h = s / 3600;
    s %= 3600;

    int m = s / 60;
    s %= 60;

    if (h) return fmt("%02d:%02d:%02d", h, m, s);
    return fmt("%02d:%02d", m, s);
}

std::string fmt_size(size_t bytes) {
    constexpr double KiB = 1024.0;
    constexpr double MiB = KiB * 1024.0;
    constexpr double GiB = MiB * 1024.0;
    constexpr double TiB = GiB * 1024.0;

    if (bytes >= TiB) return fmt("%.2f TiB", (double)bytes / TiB);
    if (bytes >= GiB) return fmt("%.2f GiB", (double)bytes / GiB);
    if (bytes >= MiB) return fmt("%.2f MiB", (double)bytes / MiB);
    if (bytes >= KiB) return fmt("%.2f KiB", (double)bytes / KiB);
    return fmt("%zu B", bytes);
}

// ---------------------------------------------------------
// Definitions
// ---------------------------------------------------------

#define RAW_SECTOR_SIZE 2352

// ---------------------------------------------------------
// [name not found]
// ---------------------------------------------------------

// --- drive capabilties

struct DriveCapabilities {
    bool can_play_audio;
    struct {
        bool cd_r;
        bool cd_rw;
        bool dvd_r;
    } read_capabilities;
    bool supports_multicession;
};

AppRes<DriveCapabilities> get_drive_capabilities(int fd) {
    if (fd < 0) throw std::invalid_argument("Invalid FD");

    int drive_cap = ioctl(fd, CDROM_GET_CAPABILITY);
    if (drive_cap < 0) {
        return std::unexpected(AppErr {
            .code = std::error_code(errno, std::generic_category()),
            .msg = "Failed to get drive capabilities."
        });
    }

    return DriveCapabilities {
        (drive_cap & CDC_PLAY_AUDIO) != 0,
        {
            (drive_cap & CDC_CD_R) != 0,
            (drive_cap & CDC_CD_RW) != 0,
            (drive_cap & CDC_DVD_R) != 0
        },
        (drive_cap & CDC_MULTI_SESSION) != 0
    };
}

// --- drive status

// enum DriveStatus : int {
//     NoInfo   = CDS_NO_INFO,
//     NoDisc   = CDS_NO_DISC,
//     TrayOpen = CDS_TRAY_OPEN,
//     NotReady = CDS_DRIVE_NOT_READY,
//     DiscOk   = CDS_DISC_OK,
//     Unknown  = -1
// };

namespace DriveStatus {
    enum type: int {
        NoInfo   = CDS_NO_INFO,
        NoDisc   = CDS_NO_DISC,
        TrayOpen = CDS_TRAY_OPEN,
        NotReady = CDS_DRIVE_NOT_READY,
        DiscOk   = CDS_DISC_OK,
        Unknown  = -1
    };

    constexpr std::string_view to_str(type status) {
        switch (status) {
            case NoInfo:   return "No info";
            case NoDisc:   return "No disc";
            case TrayOpen: return "Tray open";
            case NotReady: return "Drive not ready";
            case DiscOk:   return "Disc OK";
            case Unknown:  return "Unknown";
        }
        __builtin_unreachable();
    }
}

AppRes<DriveStatus::type> get_drive_status(int fd) {
    if (fd < 0) throw std::invalid_argument("Invalid FD");

    int stat = ioctl(fd, CDROM_DRIVE_STATUS);
    if (stat < 0) {
        return std::unexpected(AppErr {
            .code = std::error_code(errno, std::generic_category()),
            .msg = "Failed to get drive status."
        });
    }

    switch (stat) {
        case DriveStatus::NoInfo  : return DriveStatus::NoInfo  ;
        case DriveStatus::NoDisc  : return DriveStatus::NoDisc  ;
        case DriveStatus::TrayOpen: return DriveStatus::TrayOpen;
        case DriveStatus::NotReady: return DriveStatus::NotReady;
        case DriveStatus::DiscOk  : return DriveStatus::DiscOk  ;
        default                   : return DriveStatus::Unknown ;
    }
}

// --- disc status

namespace DiscType {
    enum type : int {
        Audio               = CDS_AUDIO,
        Data_Mode1          = CDS_DATA_1,
        Data_Mode2          = CDS_DATA_2,
        Data_Mode2_XA_Form1 = CDS_XA_2_1,
        Data_Mode2_XA_Form2 = CDS_XA_2_2,
        Mixed               = CDS_MIXED,
        Unknown             = -1
    };

    constexpr std::string_view to_str(type disc_type) {
        switch (disc_type) {
            case Audio:               return "Audio";
            case Data_Mode1:          return "Mode 1";
            case Data_Mode2:          return "Mode 2";
            case Data_Mode2_XA_Form1: return "Mode 2 XA (Form 1)";
            case Data_Mode2_XA_Form2: return "Mode 2 XA (Form 2)";
            case Mixed:               return "Mixed";
            case Unknown:             return "Unknown";
        }
    }
}

AppRes<DiscType::type> get_disc_type(int fd) {
    if (fd < 0) throw std::invalid_argument("Invalid FD");

    int disc_stat = ioctl(fd, CDROM_DISC_STATUS);
    if (disc_stat < 0) {
        return std::unexpected(AppErr {
            .code = std::error_code(errno, std::generic_category()),
            .msg = "Failed to get drive status."
        });
    }

    switch (disc_stat) {
        case CDS_AUDIO:  return DiscType::Audio;
        case CDS_DATA_1: return DiscType::Data_Mode1;
        case CDS_DATA_2: return DiscType::Data_Mode2;
        case CDS_XA_2_1: return DiscType::Data_Mode2_XA_Form1;
        case CDS_XA_2_2: return DiscType::Data_Mode2_XA_Form2;
        case CDS_MIXED:  return DiscType::Mixed;
        default:         return DiscType::Unknown;
    }
}

// --- tracks and TOC

// structs
struct MSF {
    u8 min, sec, frame;

    bool operator==(const MSF& other) const = default;
    
    static MSF from_sector(int sector) {
        u8 min = static_cast<u8>(sector / (60 * 75));
        sector %= 60 * 75;

        return {
            min,
            static_cast<u8>(sector / 75),
            static_cast<u8>(sector % 75)
        };
    }

    static MSF from_lba(int lba) {
        return from_sector(lba + 150);
    }

    std::string to_str() {
        return fmt("%02u:%02u:%02u", this->min, this->sec, this->frame);
    }
};

struct Track {
    u8 num;
    int start_sector, end_sector;
    u8 flags;
    MSF msf_start() const {
        return MSF::from_lba(this->start_sector);
    }
    MSF msf_end() const {
        return MSF::from_lba(this->end_sector);
    }
    bool is_data_track() const {
        return this->flags & CDROM_DATA_TRACK;
    }
    int len() const {
        return end_sector - start_sector + 1;
    }
    MSF msf_len() const {
        return MSF::from_sector(this->len());
    }

    // struct {
    //     size_t sync_err_count;
    //     size_t msf_err_count;
    //     size_t edc_err_count;
    //     size_t other_err_count;

    //     size_t mode1_count;
    //     size_t mode2_count;
    //     size_t mode2_plain_count;
    //     size_t mode2_xa1_count;
    //     size_t mode2_xa2_count;
    // } diagnostics;
};

struct TOC {
    u8 first_track;
    u8 last_track;
    std::vector<Track> tracks;
    
    u8 number_of_tracks() const {
        return last_track - first_track + 1;
    }
};

// functions
AppRes<TOC> get_toc(int fd) {
    if (fd < 0) throw std::invalid_argument("Invalid FD");  // should be an exception because this is huge logical error, not expected

    TOC toc;

    struct cdrom_tochdr toc_header;
    if (ioctl(fd, CDROMREADTOCHDR, &toc_header) < 0) {
        return std::unexpected(AppErr {
            .code = std::error_code(errno, std::generic_category()),
            .msg = "Failed to read TOC header."
        });
    }

    toc.first_track = toc_header.cdth_trk0;
    toc.last_track = toc_header.cdth_trk1;

    struct cdrom_tocentry toc_entry;
    toc_entry.cdte_format = CDROM_LBA;
    for (u8 i = toc.first_track; i <= toc.last_track; i++) {
        toc_entry.cdte_track = i;
        if (ioctl(fd, CDROMREADTOCENTRY, &toc_entry) < 0) {
            return std::unexpected(AppErr {
                .code = std::error_code(errno, std::generic_category()),
                .msg = fmt("Failed to read TOC entry for track %d.", i)
            });
        }
        
        if (i > toc.first_track) toc.tracks.back().end_sector = toc_entry.cdte_addr.lba - 1;

        toc.tracks.emplace_back(
            i,
            toc_entry.cdte_addr.lba, -1,
            (u8)toc_entry.cdte_ctrl
        );
    }

    if (!toc.tracks.empty()) {
        toc_entry.cdte_track = CDROM_LEADOUT;
        if (ioctl(fd, CDROMREADTOCENTRY, &toc_entry) < 0) {
            return std::unexpected(AppErr {
                .code = std::error_code(errno, std::generic_category()),
                .msg = "Failed to read TOC entry for leadout."
            });
        }
        toc.tracks.back().end_sector = toc_entry.cdte_addr.lba - 1;
    }

    return toc;
}

// ---------------------------------------------------------
// Main
// ---------------------------------------------------------

// ---
std::atomic<bool> g_run(true);

void signal_handler(int sig) {
    switch (sig) {
        case SIGINT: {
            g_run = false;
            break;
        }
    }
}

enum class AppAction {
    Probe,
    RawDump,
    Extract,
    Help
};

// ---
struct SectorReader {
    std::array<byte, RAW_SECTOR_SIZE> raw_data;
};

inline AppRes<std::span<const byte>> read_sector(int fd, int sector, SectorReader &r) {
    if (fd < 0) throw std::invalid_argument("Invalid FD");

    const MSF msf = MSF::from_lba(sector);

    struct cdrom_msf msf_;
    msf_.cdmsf_min0   = msf.min;
    msf_.cdmsf_sec0   = msf.sec;
    msf_.cdmsf_frame0 = msf.frame;
    std::memcpy(r.raw_data.data(), &msf_, sizeof(struct cdrom_msf));

    if (ioctl(fd, CDROMREADRAW, r.raw_data.data()) < 0) {
        return std::unexpected(AppErr {
            .code = std::error_code(errno, std::generic_category()),
            .msg = fmt("Failed to read sector '%d'.", sector)
        });
    }

    return std::span(r.raw_data);
}

// not used anywhere yet
// inline AppRes<std::span<const byte>> get_logical_sector(int fd, const Track &track, int sector, SectorReader &r) {  // NOTE: doesn't do diagnostics!
//     if (fd < 0) throw std::invalid_argument("Invalid FD");
// 
//     const MSF msf = MSF::from_lba(sector);
// 
//     struct cdrom_msf msf_;
//     msf_.cdmsf_min0   = msf.min;
//     msf_.cdmsf_sec0   = msf.sec;
//     msf_.cdmsf_frame0 = msf.frame;
//     std::memcpy(r.raw_data.data(), &msf_, sizeof(struct cdrom_msf));
//
//     if (ioctl(fd, CDROMREADRAW, r.raw_data.data()) < 0) {
//         return std::unexpected(AppErr {
//             .code = std::error_code(errno, std::generic_category()),
//             .msg = fmt("Failed to read sector '%d'.", sector)
//         });
//     }
//
//     if (track.is_data_track()) {
//         const u8 mode = r.raw_data[15];
//         switch (mode) {
//             case 1: {
//                 return std::span(r.raw_data.data() + 16, 2048);
//             }
//             case 2: {  // assuming only XA
//                 struct {
//                     u8 file_num;
//                     u8 channel_num;
//                     u8 submode; // eor, video, audio, data, trigger, form, real-time, eof
//                     u8 coding_info;
//                 } xa_header = {
//                     r.raw_data[16],
//                     r.raw_data[17],
//                     r.raw_data[18],
//                     r.raw_data[19],
//                 };
//
//                 u8 xa_form = (xa_header.submode >> 5) & 1;
//                 switch (xa_form) {
//                     case 0:  return std::span(r.raw_data.data() + 24, 2048);
//                     case 1:  return std::span(r.raw_data.data() + 24, 2324);
//                     default: throw std::runtime_error("TODO: handle this properly. error 2.");
//                 }
//             }
//             default: {
//                 throw std::runtime_error("TODO: handle this properly. error 1.");
//             }
//         }
//     } else {
//         return std::span(r.raw_data);
//     }
// }

// ---
AppRes<void> probe(int fd) {
    if (fd < 0) throw std::invalid_argument("Invalid FD");

    const auto toc = get_toc(fd);
    if (!toc) return std::unexpected(toc.error());

    printf("First track: %d\n", (*toc).first_track);
    printf("Last track : %d\n\n",  (*toc).last_track);

    usize n_data_sectors = 0;
    usize n_audio_sectors = 0;

    for (const Track &track : (*toc).tracks) {
        const MSF start = track.msf_start();
        const MSF end   = track.msf_end();
        const MSF len   = track.msf_len();

        const usize n_sectors = track.len();

        if (track.is_data_track()) n_data_sectors += n_sectors;
        else n_audio_sectors += n_sectors;

        printf("Track %2d: %5s | " MSF_FMT " (%06d) -> " MSF_FMT " (%06d) | " "Duration " MSF_FMT " (%6zu sectors, %8s)\n",
            track.num,
            track.is_data_track() ? "data" : "audio",
            start.min, start.sec, start.frame,
            track.start_sector,
            end.min, end.sec, end.frame,
            track.end_sector,
            len.min, len.sec, len.frame,
            n_sectors,
            fmt_size(n_sectors * RAW_SECTOR_SIZE).c_str()
        );
    }

    printf("\n");
    printf("========== Totals ==========\n");
    printf("Tracks       : %zu\n", toc->tracks.size());
    printf("Data sectors : %zu (%s)\n",
        n_data_sectors,
        fmt_size(n_data_sectors * RAW_SECTOR_SIZE).c_str()
    );
    printf("Audio sectors: %zu (%s)\n",
        n_audio_sectors,
        fmt_size(n_audio_sectors * RAW_SECTOR_SIZE).c_str()
    );
    printf("Total sectors: %zu (%s)\n",
        n_data_sectors + n_audio_sectors,
        fmt_size((n_data_sectors + n_audio_sectors) * RAW_SECTOR_SIZE).c_str()
    );

    return {};
}

// TODO: written by AI, check and refactor
AppRes<void> write_cue(const TOC &toc, const std::string &bin_name, const std::string &cue_name) {
    FILE *f = fopen(cue_name.c_str(), "w");
    if (!f) {
        return std::unexpected(AppErr{
            .code = std::error_code(errno, std::generic_category()),
            .msg = fmt("Failed to open '%s'.", cue_name.c_str())
        });
    }

    fprintf(f, "FILE \"%s\" BINARY\n\n", bin_name.c_str());

    for (const Track &t : toc.tracks) {
        if (t.is_data_track())
            fprintf(f, "  TRACK %02u MODE2/2352\n", t.num);
        else
            fprintf(f, "  TRACK %02u AUDIO\n", t.num);

        fprintf(f,
                "    INDEX 01 %s\n\n",
               t.msf_start().to_str().c_str());
    }

    fclose(f);
    return {};
}

AppRes<void> write_toc(const TOC &toc, const std::string &bin_name, const std::string &toc_name) {
    FILE *f = fopen(toc_name.c_str(), "w");
    if (!f) {
        return std::unexpected(AppErr{
            .code = std::error_code(errno, std::generic_category()),
            .msg = fmt("Failed to open '%s'.", toc_name.c_str())
        });
    }

    fprintf(f, "CD_ROM\n\n");

    for (const Track &t : toc.tracks) {
        if (t.is_data_track())
            fprintf(f, "TRACK MODE2_RAW\n");
        else
            fprintf(f, "TRACK AUDIO\n");

        fprintf(f,
                "    FILE \"%s\" %s\n\n",
                bin_name.c_str(),
                t.msf_len().to_str().c_str());
    }

    fclose(f);
    return {};
}

AppRes<void> raw_dump(int fd, bool seperate) {
    using clock = std::chrono::steady_clock;

    if (fd < 0) throw std::invalid_argument("Invalid FD");

    const auto toc = get_toc(fd);
    if (!toc) return std::unexpected(toc.error());

    const auto _start = clock::now();

    size_t _total_sectors = 0, _sectors_read = 0;
    for (const Track &t : toc->tracks)
        _total_sectors += t.end_sector - t.start_sector + 1;

    FILE *f = nullptr;
    if (!seperate) {
        f = fopen("disc.bin", "wb");
        if (!f) {
            return std::unexpected(AppErr {
                .code = std::error_code(errno, std::generic_category()),
                .msg = "Failed to open 'disc.bin'."
            });
        }
    }

    SectorReader reader;
    bool complete = true;
    for (const Track &t : toc->tracks) {
        if (!g_run.load()) {
            complete = false;
            printf("\nStopping safely.\n");
            break;
        }

        if (seperate) {
            const auto f_name = fmt("track%02d.bin", t.num);
            f = fopen(f_name.c_str(), "wb");
            if (!f) {
                return std::unexpected(AppErr {
                    .code = std::error_code(errno, std::generic_category()),
                    .msg = fmt("Failed to open '%s'.", f_name.c_str())
                });
            }
        }

        for (int i = t.start_sector; i <= t.end_sector; i++) {
            if (!g_run.load()) {
                complete = false;
                printf("\nStopping safely.\n");
                break;
            }

            const MSF msf = MSF::from_lba(i);

            const auto data = read_sector(fd, i, reader);
            if (!data) {
                fclose(f);
                return std::unexpected(data.error());
            }

            if (fwrite(data->data(), 1, RAW_SECTOR_SIZE, f) != RAW_SECTOR_SIZE) {
                fclose(f);
                return std::unexpected(AppErr {
                    .code = std::error_code(errno, std::generic_category()),
                    .msg = fmt("Failed to write sector '%d' to file.", i)
                });
            }

            // output diagnostics
            ++_sectors_read;

            const auto   _now = clock::now();
            const double _elapsed = std::chrono::duration<double>(_now - _start).count();
            const double _sectors_per_sec = _elapsed > 0.0 ? _sectors_read / _elapsed : 0.0;
            const double _bytes_per_sec = _sectors_per_sec * RAW_SECTOR_SIZE;
            const size_t _sectors_left = _total_sectors - _sectors_read;
            const double _remaining = (_sectors_per_sec) > 0.0 ? _sectors_left / _sectors_per_sec : 0.0;
            
            printf(
                "\33[2K\r" MSF_FMT " (%06d)  %zu/%zu  %.1f sec/s  %s/s  Elapsed %s  ETA %s",
                msf.min, msf.sec, msf.frame,
                i,
                _sectors_read,
                _total_sectors,
                _sectors_per_sec,
                fmt_size(_bytes_per_sec).c_str(),
                fmt_time(_elapsed).c_str(),
                fmt_time(_remaining).c_str()
            );
            fflush(stdout);
        }

        if (seperate) {
            fclose(f);
            f = nullptr;
        }
    }

    if (!seperate) fclose(f);

    if (complete) {
        if (!seperate) {
            auto res = write_cue(*toc, "disc.bin", "disc.cue");
            if (!res) return std::unexpected(res.error());
            res = write_toc(*toc, "disc.bin", "disc.toc");
            if (!res) return std::unexpected(res.error());
        }

        printf("\nRaw dump is done.\n");
        const auto   _end = clock::now();
        const double _total = std::chrono::duration<double>(_end - _start).count();
        printf("\n");
        printf("Completed in %s\n", fmt_time(_total).c_str());
        printf("Read %zu sectors (%.2f MiB)\n",
            _sectors_read,
            _sectors_read * RAW_SECTOR_SIZE / 1024.0 / 1024.0
        );
        printf("Stats: %0.2f sec/s %s/s \n",
            (double)_sectors_read / _total,
            fmt_size(_sectors_read * RAW_SECTOR_SIZE / _total).c_str()
        );
    }

    return {};
}

AppRes<void> extract(int fd) {
    constexpr byte SYNC[12] = {
        0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0
    };

    struct {
        size_t n_sync_errors;
        size_t n_msf_mismatch_errors;
        size_t n_mode1;
        size_t n_mode2_xa_form1;
        size_t n_mode2_xa_form2;
    } diagnostics;

    const std::function<std::span<byte>(byte*)> extract_audio_sector = [&diagnostics](byte *data) {
        return std::span(data, RAW_SECTOR_SIZE);
    };

    const std::function<std::span<byte>(byte*)> extract_data_sector = [SYNC, &diagnostics](byte *data) {
        if (memcmp(data, SYNC, 12) != 0)
            diagnostics.n_sync_errors++;

        // TODO: msf match check

        const u8 mode = data[15];

        switch (mode) {
            case 1: {
                diagnostics.n_mode1++;
                return std::span(data + 16, 2048);
            }
            case 2: {  // assuming only XA mode
                struct {
                    u8 file_num;
                    u8 channel_num;
                    u8 submode; // eor, video, audio, data, trigger, form, real-time, eof
                    u8 coding_info;
                } xa_header = {
                    data[16],
                    data[17],
                    data[18],
                    data[19],
                };

                u8 xa_form = (xa_header.submode >> 5) & 1;

                switch (xa_form) {
                    case 0:  {
                        diagnostics.n_mode2_xa_form1++;
                        return std::span(data + 24, 2048);
                    }
                    case 1:  {
                        diagnostics.n_mode2_xa_form2++;
                        return std::span(data + 24, 2324);
                    }
                    default: throw std::runtime_error("TODO: handle this properly. error 2.");
                }
            }
            default: {
                throw std::runtime_error("TODO: handle this properly. error 1.");
            }
        }

        return std::span<byte>(data, RAW_SECTOR_SIZE);
    };

    if (fd < 0) throw std::invalid_argument("Invalid FD");

    const auto toc = get_toc(fd);
    if (!toc) return std::unexpected(toc.error());
    
    FILE *f = nullptr;

    SectorReader reader;
    bool complete = true;
    for (const Track &t : toc->tracks) {
        if (!g_run.load()) {
            complete = false;
            printf("\nStopping safely.\n");
            break;
        }

        const auto f_name = fmt("track%02d.%s", t.num, t.is_data_track() ? "data.bin" : "cda.wav");
        f = fopen(f_name.c_str(), "wb");
        if (!f) {
            return std::unexpected(AppErr {
                .code = std::error_code(errno, std::generic_category()),
                .msg = fmt("Failed to open '%s'.", f_name.c_str())
            });
        }

        const auto& extract_sector = t.is_data_track()
            ? extract_data_sector
            : extract_audio_sector;

        for (int i = t.start_sector; i <= t.end_sector; i++) {
            if (!g_run.load()) {
                complete = false;
                printf("\nStopping safely.\n");
                break;
            }

            const auto data = read_sector(fd, i, reader);
            if (!data) {
                fclose(f);
                return std::unexpected(data.error());
            }

            const auto extracted_data = extract_sector(const_cast<byte*>(data->data()));

            if (fwrite(extracted_data.data(), 1, extracted_data.size(), f) != extracted_data.size()) {
                fclose(f);
                return std::unexpected(AppErr {
                    .code = std::error_code(errno, std::generic_category()),
                    .msg = fmt("Failed to write sector '%d' to file.", i)
                });
            }

            printf("\r%d", i);
            fflush(stdout);
        }

        fclose(f);
        f = nullptr;
    }

    if (complete) {
        printf("\nDiagnostics:\n");
        printf("n_sync_errors: %zu\n", diagnostics.n_sync_errors);
        printf("n_msf_mismatch_errors: %zu\n", diagnostics.n_msf_mismatch_errors);
        printf("n_mode1: %zu\n", diagnostics.n_mode1);
        printf("n_mode2_xa_form1: %zu\n", diagnostics.n_mode2_xa_form1);
        printf("n_mode2_xa_form2: %zu\n", diagnostics.n_mode2_xa_form2);
    }

    return {};
}

void help(std::optional<std::string_view>) {
    printf("Sorry. Help not implemented yet.\n");
}

int main(int argc, char* argv[]) {
    // command line args parsing
    std::vector<std::string_view> args(argv, argv + argc);

    if (args.size() == 1) {
        printf("Usage: %s [command] <arguments>\n", args[0].data());
        printf("Commands: probe, raw, help\n");
        return 1;
    }

    AppAction action;
    bool _raw_seperate = false;

    if (args[1] == "probe") {  // get toc info
        action = AppAction::Probe;
    } else if (args[1] == "raw") {
        action = AppAction::RawDump;

        if (args.size() == 3) _raw_seperate = true;
        else if (!(args.size() == 2)) {
            printf("Usage: %s raw <flags>\nFlags:\n  -s\t Dump each track seperately.", args[0].data());
            return 1;
        }
    } else if (args[1] == "extract") {
        action = AppAction::Extract;
    } else if (args[1] == "help") {
        action = AppAction::Help;
        if (argc > 3) {
            printf("Usage: %s help <command>\n", args[0].data());
            return 1;
        }
    } else {
        printf("Error: Unknown command: %s\n", args[1].data());
        return 1;
    }

    if (action == AppAction::Help) {
        if (args.size() == 3) help(args[2]);
        else help(std::nullopt);

        return 0;
    }

    // ---
    int fd = open("/dev/sr0", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("Failed to open drive"); // definitely goes in stderr
        return 2;
    }

    // ---
    auto drive_status = get_drive_status(fd);
    if (!drive_status) {
        const auto err = drive_status.error();
        printf("Error: %s\n  Code %d: %s\n",
            err.msg.c_str(),
            err.code.value(), err.code.message().c_str()
        );
        close(fd);
        return 10;
    }

    if (drive_status != DriveStatus::DiscOk) {
        fprintf(stderr, "Error: Disc not readable. Current status: %s\n", DriveStatus::to_str(*drive_status).data()); // should this go in stdout or stderr - when the cd/dvd drive isn't ready - whether its open, initalizing or anything except ready
        close(fd);
        return 3;
    }
    
    // ---
    std::signal(SIGINT, signal_handler);

    AppRes<void> res;
    switch (action) {
        case AppAction::Probe: {
            res = probe(fd);
            break;
        }
        case AppAction::RawDump: {
            res = raw_dump(fd, _raw_seperate);
            break;
        }
        case AppAction::Extract: {
            res = extract(fd);
            break;
        }
        case AppAction::Help:
            __builtin_unreachable();
    }

    if (!res.has_value()) {
        const auto err = res.error();
        printf("Error: %s\n  Code %d: %s\n",
            err.msg.c_str(),
            err.code.value(), err.code.message().c_str()
        );
    }

    // ---
    close(fd);
    return 0;
}