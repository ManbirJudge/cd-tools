#include "utils.hpp"

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