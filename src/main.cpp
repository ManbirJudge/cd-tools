#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>

#include "cue.hpp"
#include "error.hpp"
#include "toc.hpp"

namespace fs = std::filesystem;

// ---------------------------------------------------------
// Definitions
// ---------------------------------------------------------

#define RAW_SECTOR_SIZE 2352

// ---------------------------------------------------------
// [name not found]
// ---------------------------------------------------------

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

// ---------------------------------------------------------
// Reading
// ---------------------------------------------------------

// --- sector reader
struct SectorReader {
    std::array<byte, RAW_SECTOR_SIZE> raw_data;
};

// --- sector source
class SectorSource {
protected:
    TOC m_toc;

    bool m_is_ready;
    std::string m_reason;

    SectorSource(bool raw, bool logical) : can_read_raw(raw), can_read_logical(logical) {}

    void set_toc(const TOC toc) {
        m_toc = toc;
    }

public:
    const bool can_read_raw;
    const bool can_read_logical;

    std::pair<bool, std::string> is_ready() const noexcept {
       return { m_is_ready, m_reason };
    };

    TOC get_toc() const noexcept { return m_toc; };

    virtual AppRes<std::span<const byte>> read_sector(int lba, SectorReader &r) = 0;
    virtual AppRes<std::span<const byte>> read_logical_sector(int lba, const Track &t, SectorReader &r) = 0;

    virtual ~SectorSource() = default;

    virtual MSF lba_2_msf(int lba) = 0;
    virtual int msf_2_lba(const MSF msf) = 0;
};

// CD source
class CDSource : public SectorSource {
    int fd = -1;

    AppRes<DriveStatus::type> read_drive_status() {
        int stat = ioctl(this->fd, CDROM_DRIVE_STATUS);
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

    AppRes<TOC> read_toc() {
        TOC toc;

        struct cdrom_tochdr toc_header;
        if (ioctl(this->fd, CDROMREADTOCHDR, &toc_header) < 0) {
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
            if (ioctl(this->fd, CDROMREADTOCENTRY, &toc_entry) < 0) {
                return std::unexpected(AppErr {
                    .code = std::error_code(errno, std::generic_category()),
                    .msg = fmt("Failed to read TOC entry for track %d.", i)
                });
            }
            
            if (i > toc.first_track) toc.tracks.back().end_lba = toc_entry.cdte_addr.lba - 1;

            toc.tracks.emplace_back(
                i,
                toc_entry.cdte_addr.lba, -1,
                toc_entry.cdte_ctrl & CDROM_DATA_TRACK
            );
        }

        if (!toc.tracks.empty()) {
            toc_entry.cdte_track = CDROM_LEADOUT;
            if (ioctl(this->fd, CDROMREADTOCENTRY, &toc_entry) < 0) {
                return std::unexpected(AppErr {
                    .code = std::error_code(errno, std::generic_category()),
                    .msg = "Failed to read TOC entry for leadout."
                });
            }
            toc.tracks.back().end_lba = toc_entry.cdte_addr.lba - 1;
        }

        return toc;
    }

public:
    explicit CDSource(const std::string_view dev_path) : SectorSource(true, false) {
        this->fd = open(dev_path.data(), O_RDONLY | O_NONBLOCK);
        if (this->fd < 0) {
            m_is_ready = false;
            m_reason = std::string("Failed to open device: ") + dev_path.data();
            return;
        }

        auto drive_status = this->read_drive_status();
        if (!drive_status) {
            const auto err = drive_status.error();

            m_is_ready = false;
            m_reason = fmt("Error: %s\n  Code %d: %s\n",
                err.msg.c_str(),
                err.code.value(), err.code.message().c_str()
            );

            return;
        }
        if (drive_status != DriveStatus::DiscOk) {
            m_is_ready = false;
            m_reason = fmt("Disc not readable. Current status: %s\n", DriveStatus::to_str(*drive_status).data());

            return;
        }

        const auto toc = this->read_toc();
        if (!toc) {
            const auto err = toc.error();

            m_is_ready = false;
            m_reason = fmt("Error: %s\n  Code %d: %s\n",
                err.msg.c_str(),
                err.code.value(), err.code.message().c_str()
            );

            return;
        }
        m_toc = toc.value();

        m_is_ready = true;
    }

    CDSource(const CDSource&) = delete;
    CDSource(CDSource&& other) noexcept : SectorSource(true, false), fd(other.fd) { other.fd = -1; }
    CDSource& operator=(const CDSource&) = delete;
    CDSource& operator=(CDSource&& other) noexcept {
        if (this != &other) {
            if (this->fd >= 0) ::close(fd);
            this->fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    AppRes<std::span<const byte>> read_sector(int lba, SectorReader &r) override {
        const MSF msf = MSF::from_sector(lba + 150);

        struct cdrom_msf msf_;
        msf_.cdmsf_min0   = msf.min;
        msf_.cdmsf_sec0   = msf.sec;
        msf_.cdmsf_frame0 = msf.frame;
        std::memcpy(r.raw_data.data(), &msf_, sizeof(struct cdrom_msf));  // mimicking union memory layout

        if (ioctl(fd, CDROMREADRAW, r.raw_data.data()) < 0) {
            return std::unexpected(AppErr {
                .code = std::error_code(errno, std::generic_category()),
                .msg = fmt("Failed to read sector LBA = %d.", lba)
            });
        }

        return std::span(r.raw_data);
    }
    
    AppRes<std::span<const byte>> read_logical_sector(int lba, const Track &t, SectorReader& r) override {
        const auto raw = this->read_sector(lba, r);
        if (!raw) return std::unexpected(raw.error());

        if (t.is_data_track) {
            const u8 mode = r.raw_data[15];
            switch (mode) {
                case 1: {
                    return std::span(r.raw_data.data() + 16, 2048);
                }
                case 2: {
                    // struct {
                    //     u8 file_num;
                    //     u8 channel_num;
                    //     u8 submode; // eor, video, audio, data, trigger, form, real-time, eof
                    //     u8 coding_info;
                    // } xa_header = {
                    //     r.raw_data[16],
                    //     r.raw_data[17],
                    //     r.raw_data[18],
                    //     r.raw_data[19],
                    // };

                    const u8 xa_form = (r.raw_data[18] >> 5) & 1;
                    switch (xa_form) {
                        case 0:  return std::span(r.raw_data.data() + 24, 2048);
                        case 1:  return std::span(r.raw_data.data() + 24, 2324);
                        default: throw std::runtime_error("messi!");
                    }
                }
                default: throw std::runtime_error("penaldo!");
            }
        } else {
            return std::span(r.raw_data);
        }
    }

    ~CDSource() override {
        if (this->fd >= 0) ::close(this->fd);
        this->fd = -1;
    }

    MSF lba_2_msf(int lba) override {
        return MSF::from_sector(lba + 150);
    }
    int msf_2_lba(const MSF msf) override {
        return msf.to_sector() - 150;
    } 
};

// .bin+.cue/.toc source
class BinSource : public SectorSource {  // !!!!!! WIP !!!!!!
    FILE *f = nullptr;
    Cue cue;

public:
    explicit BinSource(const std::string_view cue_path) : SectorSource(true, false) {
        const auto cue = read_cue(cue_path);
        if (!cue) {
            const auto err = cue.error();

            m_is_ready = false;
            m_reason = fmt("Error: %s\n  Code %d: %s\n",
                err.msg.c_str(),
                err.code.value(), err.code.message().c_str()
            );

            return;
        }

        this->cue = cue.value();

        if (this->cue.files.empty())  {
            m_is_ready = false;
            m_reason = "CUE contains no files.";
            return;
        } else if (this->cue.files.size() > 1) {
            m_is_ready = false;
            m_reason = "CUE contains multiple files.";
            return;
        }
        if (this->cue.files.front().type != CueFileType::Binary) {
            m_is_ready = false;
            m_reason = "Only binary files are supported.";
            return;
        }

        const fs::path cue_path_ = cue_path;
        const fs::path bin_path = fs::weakly_canonical(cue_path_.parent_path() / cue->files.front().filename);
        f = fopen(bin_path.c_str(), "rb");
        if (!f) {
            m_is_ready = false;
            m_reason = fmt("Failed to open file: %s", bin_path.c_str());
            return;
        }
    
        // TODO: maybe a file size check?

        m_toc.first_track = this->cue.files.front().tracks.front().num;
        m_toc.last_track  = this->cue.files.front().tracks.back() .num;

        for (const auto &t : this->cue.files.front().tracks) {
            if (t._sector_size != RAW_SECTOR_SIZE) {
                m_is_ready = false;
                m_reason = "Only fully raw disc images are supported.";
                return;
            }

            const int start_lba = t.indices.front().msf.to_sector(); // NOTE: NOT USING msf.to_lba() because convention in CUEs is different

            if (!m_toc.tracks.empty()) m_toc.tracks.back().end_lba = start_lba - 1;
            m_toc.tracks.emplace_back(
                t.num,
                start_lba, -1,
                t.type != CueTrackType::Audio
            );
        }
        if (!m_toc.tracks.empty()) {
            fseek(f, 0, SEEK_END);
            size_t bin_size = (size_t)ftell(f);
            if (bin_size % RAW_SECTOR_SIZE != 0) {
                m_is_ready = false;
                m_reason = fmt("Binary file size isn't valid; it should be a multiple of %d", RAW_SECTOR_SIZE);
                return;
            } 
            m_toc.tracks.back().end_lba = (bin_size / RAW_SECTOR_SIZE) - 1;
        }

        m_is_ready = true;
    }

    BinSource(const BinSource&) = delete;
    BinSource(BinSource&& other) noexcept : SectorSource(true, false), f(other.f) { other.f = nullptr; }
    BinSource& operator=(const BinSource&) = delete;
    BinSource& operator=(BinSource&& other) noexcept {
        if (this != &other) {
            if (this->f) fclose(f);
            this->f = other.f;
            other.f = nullptr;
        }
        return *this;
    }

    AppRes<std::span<const byte>> read_sector(int lba, SectorReader& r) override {
        size_t off = (size_t)lba * RAW_SECTOR_SIZE;
        fseek(f, off, SEEK_SET);

        if (fread(r.raw_data.data(), 1, RAW_SECTOR_SIZE, this->f) != RAW_SECTOR_SIZE) {
            return std::unexpected(AppErr {
                .code = std::error_code(errno, std::generic_category()),
                .msg = fmt("Failed to read sector LBA = '%d'.", lba)
            });;
        }

        return std::span(r.raw_data);
    }
    
    AppRes<std::span<const byte>> read_logical_sector(int, const Track&, SectorReader&) override {
        throw std::logic_error("Not implemented yet.");
    }

    ~BinSource() override {
        if (this->f) fclose(this->f);
        this->f = nullptr;
    }
    
    MSF lba_2_msf(int lba) override {
        return MSF::from_sector(lba);
    }
    int msf_2_lba(const MSF msf) override {
        return msf.to_sector();
    } 
};

// ---------------------------------------------------------
// Main
// ---------------------------------------------------------

std::atomic<bool> g_run(true);

void on_signal(int sig) {
    switch (sig) {
        case SIGINT: {
            g_run = false;
            break;
        }
    }
}

// actions
enum class AppAction {
    Probe,
    RawDump,
    Extract,
    Help
};

AppRes<void> probe(SectorSource &src) {
    const TOC toc = src.get_toc();

    printf("First track: %d\n", toc.first_track);
    printf("Last track : %d\n\n",  toc.last_track);

    usize n_data_sectors = 0;
    usize n_audio_sectors = 0;

    for (const Track &track : toc.tracks) {
        const MSF start = src.lba_2_msf(track.start_lba);
        const MSF end   = src.lba_2_msf(track.end_lba  );
        const MSF len   = track.msf_len();

        const usize n_sectors = track.len();

        if (track.is_data_track) n_data_sectors += n_sectors;
        else n_audio_sectors += n_sectors;

        printf("Track %2d: %5s | " MSF_FMT " (%06d) -> " MSF_FMT " (%06d) | " "Duration " MSF_FMT " (%6zu sectors, %8s)\n",
            track.num,
            track.is_data_track ? "data" : "audio",
            start.min, start.sec, start.frame,
            track.start_lba,
            end.min, end.sec, end.frame,
            track.end_lba,
            len.min, len.sec, len.frame,
            n_sectors,
            fmt_size(n_sectors * RAW_SECTOR_SIZE).c_str()
        );
    }

    printf("\n");
    printf("========== Totals ==========\n");
    printf("Tracks       : %zu\n", toc.tracks.size());
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

AppRes<void> raw_dump(SectorSource &src, bool seperate) {
    using clock = std::chrono::steady_clock;

    const TOC toc = src.get_toc();

    const auto _start = clock::now();

    size_t _total_sectors = 0, _sectors_read = 0;
    for (const Track &t : toc.tracks)
        _total_sectors += t.end_lba - t.start_lba + 1;

    FILE *f = nullptr;
    if (!seperate) {
        f = fopen("disc.bin", "wb");
        if (!f) {
            return std::unexpected(AppErr {
                .code = std::error_code(errno, std::generic_category()),
                .msg = "Failed to open 'disc.bin'."
            });
        }

        byte buf[RAW_SECTOR_SIZE] = {0};
        for (int i = 0; i < 150; i++)
            fwrite(buf, RAW_SECTOR_SIZE, 1, f);
    }

    SectorReader reader;
    bool complete = true;
    for (const Track &t : toc.tracks) {
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

        for (int i = t.start_lba; i <= t.end_lba; i++) {
            if (!g_run.load()) {
                complete = false;
                printf("\nStopping safely.\n");
                break;
            }

            const auto data = src.read_sector(i, reader);
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

            const MSF msf = src.lba_2_msf(i);
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
            auto res = write_cue(toc, "disc.bin", "disc.cue");
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

AppRes<void> extract(SectorSource &src) {
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

    const TOC toc = src.get_toc();
    
    FILE *f = nullptr;

    SectorReader reader;
    bool complete = true;
    for (const Track &t : toc.tracks) {
        if (!g_run.load()) {
            complete = false;
            printf("\nStopping safely.\n");
            break;
        }

        const auto f_name = fmt("track%02d.%s", t.num, t.is_data_track ? "data.bin" : "cda.wav");
        f = fopen(f_name.c_str(), "wb");
        if (!f) {
            return std::unexpected(AppErr {
                .code = std::error_code(errno, std::generic_category()),
                .msg = fmt("Failed to open '%s'.", f_name.c_str())
            });
        }

        const auto& extract_sector = t.is_data_track
            ? extract_data_sector
            : extract_audio_sector;

        for (int i = t.start_lba; i <= t.end_lba; i++) {
            if (!g_run.load()) {
                complete = false;
                printf("\nStopping safely.\n");
                break;
            }

            const auto data = src.read_sector(i, reader);
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

// entry point
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
    // CDSource src("/dev/sr0");
    BinSource src("assets/a6/disc.cue");

    const auto status = src.is_ready();
    if (!status.first) {
        printf("Source not ready:\n%s\n", status.second.data());
        return 67;
    }
    
    // ---
    signal(SIGINT, on_signal);

    AppRes<void> res;
    switch (action) {
        case AppAction::Probe: {
            res = probe(src);
            break;
        }
        case AppAction::RawDump: {
            res = raw_dump(src, _raw_seperate);
            break;
        }
        case AppAction::Extract: {
            res = extract(src);
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
    return 0;
}