#ifndef TOC_H
#define TOC_H

#include <string>
#include <vector>

#include <linux/cdrom.h>

#include "types.hpp"
#include "utils.hpp"

struct MSF {
    u8 min, sec, frame;

    bool operator==(const MSF& other) const = default;

    int to_sector() const {
        return min * 60 * 75 + sec * 75 + frame;
    }
    int to_lba() const {
        return min * 60 * 75 + sec * 75 + frame - 150;
    }
    
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
    int start_lba, end_lba;
    bool is_data_track;
    MSF msf_start() const {
        return MSF::from_lba(this->start_lba);
    }
    MSF msf_end() const {
        return MSF::from_lba(this->end_lba);
    }
    MSF msf_len() const {
        return MSF::from_sector(this->len());
    }
    int len() const {
        return end_lba - start_lba + 1;
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

#endif