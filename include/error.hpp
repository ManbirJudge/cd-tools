#ifndef ERROR_H
#define ERROR_H

#include <expected>
#include <string>
#include <system_error>
#include <type_traits>

// --- i have no idea wtf this is but it should work to create a custom cateogory

enum class LogicErr {
    Err
};

class LogicErrCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "Logic"; }

    std::string message(int ev) const override {
        switch (static_cast<LogicErr>(ev)) {
            case LogicErr::Err: return "Logic error.";
            default:            return "Unknown logic error.";
        }
    }
};

inline const std::error_category& logic_category() {
    static LogicErrCategory inst;
    return inst;
}

inline std::error_code make_error_code(LogicErr e) {
    return { static_cast<int>(e), logic_category() };
}

namespace std {
    template<>
    struct is_error_code_enum<LogicErr> : true_type {};
}

// ---

struct AppErr {
    std::error_code code;
    std::string msg;
};

template <typename T>
using AppRes = std::expected<T, AppErr>;

#endif