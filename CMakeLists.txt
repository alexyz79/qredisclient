cmake_minimum_required(VERSION 3.1.0)

project(qredisclient)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(CMAKE_VERSION VERSION_LESS "3.7.0")
    set(CMAKE_INCLUDE_CURRENT_DIR ON)
endif()

find_package(Qt5 COMPONENTS Core Network REQUIRED)

set (
    QREDISCLIENT_CPP_SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/command.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/connection.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/connectionconfig.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/response.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/responseparser.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/scancommand.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/transporters/abstracttransporter.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/transporters/defaulttransporter.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/private/parsedresponse.cpp 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/utils/compat.cpp    
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/utils/sync.cpp    
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qredisclient/utils/text.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/hiredis/read.c
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/hiredis/sds.c
)

qt5_add_resources(RESOURCES ./lua.qrc)

add_library(
    qredisclient
    ${QREDISCLIENT_CPP_SOURCE_FILES}
    ${RESOURCES}
)

target_link_libraries(
    qredisclient Qt5::Core Qt5::Network)
target_include_directories(
    qredisclient 
    PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/asyncfuture
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/hiredis
)