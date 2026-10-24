#include <vector>
extern "C" int stremio_native_toolchain_check() { std::vector<int> values{1,2,3}; return static_cast<int>(values.size()); }
