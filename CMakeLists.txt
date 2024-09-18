cmake_minimum_required(VERSION 3.5)
project(rio)
find_package(rosidl_default_generators REQUIRED)
find_package(ament_cmake REQUIRED)
find_package(rosbag REQUIRED)
find_package(rclcpp REQUIRED)
find_package(lpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(roslib REQUIRED)
find_package(tf2_eigen REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(nodelet REQUIRED)
find_package(gtsam_catkin REQUIRED)
find_package(std_srvs REQUIRED)

find_package(Eigen3 REQUIRED)
find_package(GTSAM REQUIRED)
find_package(GTSAM_UNSTABLE REQUIRED)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
set(CMAKE_BUILD_TYPE Release)

rosidl_generate_interfaces(
    ${PROJECT_NAME}
        msg/DopplerResidual.msg
        msg/Timing.msg
    DEPENDENCIES
        
)

include_directories(include)
add_library(${PROJECT_NAME}
        src/gtsam/landmark_tracker.cpp
        src/gtsam/optimization.cpp
        src/gtsam/propagation.cpp
        src/gtsam/state.cpp
        src/common.cpp
        src/rio.cpp
        )
ament_target_dependencies(${PROJECT_NAME} gtsam gtsam_unstable Eigen3::Eigen "rclcpp" "nodelet" "rosbag" "pluginlib" "sensor_msgs" "nav_msgs" "rclpy" "geometry_msgs" "std_msgs" "tf2_eigen" "tf2_ros")

add_library(${PROJECT_NAME}_nodelet
        src/rio_nodelet.cpp
        )
ament_target_dependencies(${PROJECT_NAME}_nodelet ${PROJECT_NAME})

add_executable(rio_node
        src/rio_node.cpp
        )
ament_target_dependencies(rio_node ${PROJECT_NAME})

add_executable(rio_calibration_node
        src/rio_calibration_node.cpp
)
ament_target_dependencies(rio_calibration_node ${PROJECT_NAME})
ament_export_include_directories(include ${catkin_INCLUDE_DIRS})
ament_export_libraries(${PROJECT_NAME} ${PROJECT_NAME}_nodelet)
ament_export_dependencies(message_runtime rclcpp lpp sensor_msgs std_msgs roslib tf2_eigen tf2_ros nodelet gtsam_catkin std_srvs)
ament_package()

###########
# INSTALL #
###########

install(TARGETS ${PROJECT_NAME}
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
)

install(FILES nodelet_plugins.xml
        DESTINATION share/${PROJECT_NAME}
)

###########
## Build ##
###########

add_compile_definitions(MODE_ROSLOG)
add_compile_definitions(GTSAM_USE_BOOST_FEATURES)

#########
# TESTS #
#########
if (BUILD_TESTING)
        find_package(rostest REQUIRED)
        find_package(GTest REQUIRED)
        include_directories(${GTEST_INCLUDE_DIRS})
        target_link_libraries(test_adjoint ${PROJECT_NAME})

        ament_add_gtest(test_adjoint
        test/adjoint-test.cpp
        
)
endif()