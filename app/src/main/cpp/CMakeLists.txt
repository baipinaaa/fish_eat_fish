cmake_minimum_required(VERSION 3.22.1)

project(fishcrack CXX)

# 单个 .so：把 il2cpp 的 C API 当作“充电桩”，用它在 Unity 主线程里
# 调用热更 DLL（Logic.dll）里的 C# 方法，实现“免登陆”触发。
#
# 注意：build.gradle 里传了 -DANDROID_STL=none（NDK 会据此加 -nostdinc++，
# 最终 .so 不链接 libc++_static），所以 fishcrack.cpp 只能包含 C 头，
# 不能出现 <cstddef> 这类 C++ 头，也不能用 std:: 前缀。
add_library(fishcrack SHARED fishcrack.cpp)

target_compile_features(fishcrack PRIVATE cxx_std_17)

target_compile_options(fishcrack PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Oz
        -fvisibility=hidden
        -fno-exceptions
        -fno-rtti)

find_library(log-lib log)
target_link_libraries(fishcrack PRIVATE ${log-lib} dl)
