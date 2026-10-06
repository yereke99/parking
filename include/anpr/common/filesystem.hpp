#pragma once

// JetPack 4.x ships GCC 7, where the C++17 filesystem implementation still lives under the
// experimental header and namespace. Keep that compatibility detail in one place.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 8
#include <experimental/filesystem>
namespace anpr {
namespace filesystem = std::experimental::filesystem;
}
#else
#include <filesystem>
namespace anpr {
namespace filesystem = std::filesystem;
}
#endif
