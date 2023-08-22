cmake_minimum_required(VERSION 3.10)   #最低版本需求
project(MyAllocator)                  #项目名称

set(CMAKE_CXX_STANDARD 17)            #设定使用C++14

# 将myallocator.cpp编译为可执行文件
add_executable(MyAllocator myallocator.cpp)

# 启用 debug 模式
set(CMAKE_BUILD_TYPE Debug)



