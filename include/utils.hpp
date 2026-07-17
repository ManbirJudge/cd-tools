#ifndef COMMON_H
#define COMMON_H

#include <cstdio>
#include <string>

#define bcd_to_bin(x) ((((x) >> 4) * 10) + ((x) & 0x0F))

// --- string formating

#define MSF_FMT "%02d:%02d:%02d"

template<typename... Args>
std::string fmt(const char *fmt, Args... args) {
    int size_s = snprintf(nullptr, 0, fmt, args...) + 1; 
    if (size_s <= 0) return "";
    
    auto size = static_cast<size_t>(size_s);

    std::string str;
    str.resize(size);

    snprintf(str.data(), size + 1, fmt, args...);
    
    return str;
}

std::string fmt_time(int s);
std::string fmt_size(size_t bytes);

#endif