#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef uint8_t byte;

#define bcd_to_bin(x) ((((x) >> 4) * 10) + ((x) & 0x0F))

void print_hex(byte *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        printf("%02x ", data[i]);
    }
}

struct MSF {
    u8 min, sec, frame;

    bool operator==(const MSF& other) const = default;

    static MSF from_lba(int lba) {
        lba += 150;

        u8 min = static_cast<u8>(lba / (60 * 75));
        lba %= 60 * 75;

        return {
            min,
            static_cast<u8>(lba / 75),
            static_cast<u8>(lba % 75)
        };
    }
};

struct Track {
    u8 num;
    int start_sector;  // lba
    u8 flags;

    struct {
        size_t sync_err_count;
        size_t msf_err_count;
        size_t edc_err_count;
        size_t other_err_count;

        size_t mode1_count;
        size_t mode2_count;
        size_t mode2_plain_count;
        size_t mode2_xa1_count;
        size_t mode2_xa2_count;
    } diagnostics;

    MSF msf() const {
        return MSF::from_lba(this->start_sector);
    }

    bool is_data_track() const {
        return this->flags & CDROM_DATA_TRACK;
    }

    bool is_lead_out() const {
        return this->num == CDROM_LEADOUT;
    }
};

Track get_track(int fd, u8 num) {
    Track t;
    t.num = num;

    struct cdrom_tocentry toc_entry;
    toc_entry.cdte_track = num;
    toc_entry.cdte_format = CDROM_LBA;
    ioctl(fd, CDROMREADTOCENTRY, &toc_entry);

    t.start_sector = toc_entry.cdte_addr.lba;
    t.flags = toc_entry.cdte_ctrl;

    // toc_entry.cdte_format = CDROM_MSF;
    // ioctl(fd, CDROMREADTOCENTRY, &toc_entry);
    // t.msf.min   = toc_entry.cdte_addr.msf.minute;
    // t.msf.sec   = toc_entry.cdte_addr.msf.second;
    // t.msf.frame = toc_entry.cdte_addr.msf.frame;

    return t;
}

int main() {
    // ---
    int fd = open("/dev/sr0", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("Failed to open drive");
        return 1;
    }

    // ---
    int drive_stat = ioctl(fd, CDROM_DRIVE_STATUS);

    printf("Drive status: ");
    switch (drive_stat) {
        case CDS_NO_INFO:
            printf("No info\n");
            close(fd);
            return 0;
        case CDS_NO_DISC:
            printf("Disc absent\n");
            break;
        case CDS_TRAY_OPEN:
            printf("Tray open\n");
            break;
        case CDS_DRIVE_NOT_READY:
            printf("Not ready\n");
            close(fd);
            return 0;
        case CDS_DISC_OK:
            printf("Disc present\n");
            break;
        default:
            printf("\nError: Unknown drive status: %i.\n", drive_stat);
            close(fd);
            return 2;
    }
 
    // --
    int drive_cap = ioctl(fd, CDROM_GET_CAPABILITY);

    printf("Drive capablities:\n");
    if (drive_cap & CDC_PLAY_AUDIO) printf("  Can play audio.\n");
    if (drive_cap & CDC_CD_R) printf("  Can read CD-R.\n");
    if (drive_cap & CDC_CD_RW) printf("  Can read CD-RW.\n");
    if (drive_cap & CDC_DVD_R) printf("  Can read DVD-R.\n");
    if (drive_cap & CDC_MULTI_SESSION) printf("  Supports multisession.\n");

    // ---  
    int disc_stat = ioctl(fd, CDROM_DISC_STATUS);

    printf("Reported disc status (don't trust): ");
    switch (disc_stat) {
        case CDS_AUDIO:
            printf("Audio CD\n");
            break;
        case CDS_DATA_1:
            printf("Data CD (Mode 1)\n");
            break;
        case CDS_DATA_2:
            printf("Data CD (Mode 2)\n");
            break;
        case CDS_XA_2_1:
            printf("CD-ROM XA (Form 1)\n");
            break;
        case CDS_XA_2_2:
            printf("CD-ROM XA (Form 2)\n");
            break;
        case CDS_MIXED:
            printf("Mixed-mode CD\n");
            break;
        default:
            printf("Unknown (%i)\n", disc_stat);
    }

    // ---
    std::vector<Track> tracks;

    struct cdrom_tochdr toc_header;
    if (ioctl(fd, CDROMREADTOCHDR, &toc_header) < 0) {
        perror("ioctl CDROMREADTOCHDR failed");
        close(fd);
        return 4;
    }

    for (int i = toc_header.cdth_trk0; i <= toc_header.cdth_trk1; i++)
        tracks.push_back(get_track(fd, i));
    tracks.push_back(get_track(fd, CDROM_LEADOUT));

    for (const Track &t : tracks) {
        if (t.is_lead_out()) printf("Leadout:");
        else printf("Track %d:", t.num);
        const MSF msf = t.msf();
        printf(" %02d:%02d:%02d (%06d) | %s\n",
            msf.min, msf.sec, msf.frame,
            t.start_sector,
            t.is_data_track() ? "data" : "audio"
        );
    }

    // ---
    if (!(tracks.size() > 0)) {
        close(fd);
        return 0;
    }

    const byte sync[] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};

    for (u8 n = 0, N = tracks.size() - 1; n < N; n++) {
        Track &t = tracks[n];

        if (t.is_data_track()) {
            char file_name[128];
            snprintf(file_name, 128, "track%d.bin", n);
            FILE *f = fopen("test.bin", "wb");
            FILE *f2 = fopen("test.iso", "wb");

            union {
                struct cdrom_msf msf;
                byte data[2352];
            } arg;
            for (size_t i = t.start_sector, l = tracks[n + 1].start_sector; i < l; i++) {
                const MSF msf = MSF::from_lba(i);
                
                printf("Doing %02d:%02d:%02d (%0zu).\r", msf.min, msf.sec, msf.frame, i);
                fflush(stdout);
                
                arg.msf.cdmsf_min0   = msf.min;
                arg.msf.cdmsf_sec0   = msf.sec;
                arg.msf.cdmsf_frame0 = msf.frame;
                if (ioctl(fd, CDROMREADRAW, &arg) < 0) {
                    perror("ioctl CDROMREADRAW failed");
                    fclose(f);
                    close(fd);
                    return 1;
                }

                bool synced = memcmp(arg.data, sync, 12) == 0;
                const MSF msf_read {
                    .min   = (u8)bcd_to_bin(arg.data[12]),
                    .sec   = (u8)bcd_to_bin(arg.data[13]),
                    .frame = (u8)bcd_to_bin(arg.data[14])
                };
                const u8 mode = arg.data[15];

                if (!synced) {
                    printf("Sector %zu not synced!", i);
                    t.diagnostics.sync_err_count++;
                }
                if (msf != msf_read) {
                    printf("Sector %zu MSF doesn't match!", i);
                    t.diagnostics.msf_err_count++;
                }

                // printf("Sector %zu (%02d:%02d:%02d):\n", i, msf.min, msf.sec, msf.frame);
                // printf("  Synced: %s\n", synced ? "yes" : "no");
                // printf("  Read MSF: ");
                // if (msf == msf_read) printf("same");
                // else printf("mismatch! %02d:%02d:%02d", msf_read.min, msf_read.sec, msf_read.frame);
                // printf("\n  Mode: %d\n", mode);
                
                switch (mode) {
                    case 1: {
                        break;
                    }
                    case 2: {  // assuming always XA
                        struct {
                            u8 file_num;
                            u8 channel_num;
                            u8 submode; // eor, video, audio, data, trigger, form, real-time, eof
                            u8 coding_info;
                        } xa_header = {
                            arg.data[16],
                            arg.data[17],
                            arg.data[18],
                            arg.data[19],
                        };

                        u8 xa_form = (xa_header.submode > 2) & 1;
                        
                        // printf("  XA Subheader:\n");
                        // printf("    File number: %d\n", xa_header.file_num);
                        // printf("    Channel number: %d\n", xa_header.channel_num);
                        // printf("    Submode:\n");
                        // printf("      Form:     Form %d\n", xa_form + 1);
                        // printf("      Data:     %s\n", ((xa_header.submode > 4) & 1) ? "yes" : "no");
                        // printf("      Audio:    %s\n", ((xa_header.submode > 5) & 1) ? "yes" : "no");
                        // printf("      Video:    %s\n", ((xa_header.submode > 6) & 1) ? "yes" : "no");
                        // printf("      Realtime: %s\n", ((xa_header.submode > 1) & 1) ? "yes" : "no");
                        // printf("      EOF:      %s\n", ((xa_header.submode    ) & 1) ? "yes" : "no");
                        // printf("      EOR:      %s\n", ((xa_header.submode > 7) & 1) ? "yes" : "no");
                        // printf("    Coding information: 0x%02x\n", xa_header.coding_info);

                        switch (xa_form) {
                            case 0: {
                                fwrite(arg.data + 24, 1, 2048, f2);
                                break;
                            }
                            case 1: {
                                fwrite(arg.data + 24, 1, 2324, f2);
                                break;
                            }
                            default:
                                printf("Error: Unknown XA form: %d.\n", xa_form);
                        }
                    }
                    default: {
                        printf("Unknown sector mode: %d\n", mode);
                        t.diagnostics.other_err_count++;
                    }
                }

                fwrite(arg.data, 1, 2352, f);

                n++;
            }

            fclose(f);
            fclose(f2);
            printf("\33[2K\rDone.\n");
        } else {
            size_t track_len_sectors = t.start_sector - tracks[n + 1].start_sector;
            size_t pcm_len_bytes = track_len_sectors * 2352;

            char wav_file_name[128];
            snprintf(wav_file_name, 128, "track%d.cda.wav", n);
            FILE *f = fopen(wav_file_name, "wb");

            fwrite("RIFF", 1, 4, f);
            u32 _ = 36 + pcm_len_bytes; fwrite(&_, 4, 1, f);
            fwrite("WAVE", 1, 4, f);

            fwrite("fmt ", 1, 4, f);  // chunk id = 'fmt '
            _ = 16; fwrite(&_, 4, 1, f);  // chunk size = 16 bytes
            u16 __ = 1; fwrite(&__, 2, 1, f);  // audio format = pcm
            __ = 2; fwrite(&__, 2, 1, f);  // number of channels = 2 (for stereo)
            _ = 44100; fwrite(&_, 4, 1, f); // sample rate = 44.1 kHz
            _ = 176400; fwrite(&_, 4, 1, f); // byte rate (caculated)
            __ = 4; fwrite(&__, 2, 1, f);  // block align = 4 (for 16-bit stereo)
            __ = 16; fwrite(&__, 2, 1, f);  // bits per sample

            fwrite("data", 1, 4, f);
            fwrite(&pcm_len_bytes, 4, 1, f);

            union {
                struct cdrom_msf msf;
                byte data[2352];
            } arg;
            for (size_t i = t.start_sector, l = tracks[n + 1].start_sector; i < l; i++) {
                const MSF msf = MSF::from_lba(i);
                
                printf("Doing %02d:%02d:%02d (%0zu).\r", msf.min, msf.sec, msf.frame, i);
                fflush(stdout);

                arg.msf.cdmsf_min0   = msf.min;
                arg.msf.cdmsf_sec0   = msf.sec;
                arg.msf.cdmsf_frame0 = msf.frame;
                if (ioctl(fd, CDROMREADRAW, &arg) < 0) {
                    perror("ioctl CDROMREADRAW failed");
                    fclose(f);
                    close(fd);
                    return 1;
                }

                fwrite(arg.data, 1, 2352, f);
            }

            fclose(f);
            printf("\33[2K\rDone.\n");
        }
    }

    // ---
    close(fd);
    return 0;
}