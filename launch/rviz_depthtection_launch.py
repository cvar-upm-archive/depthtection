from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare



# find the path to the launch file

def generate_launch_description():
    rviz_config = PathJoinSubstitution([
        FindPackageShare('depthtection'),
        'rviz', 'depthtection_tests.rviz'
    ])

    return LaunchDescription([
        DeclareLaunchArgument('namespace', default_value='quadrotor_1'),
        DeclareLaunchArgument('use_sim_time', default_value='false'),
        DeclareLaunchArgument('camera_topic', default_value='slot0'),
        DeclareLaunchArgument('detection_topic', default_value='detector_node/detections'),
        DeclareLaunchArgument('ground_truth_topic', default_value='""'),
        DeclareLaunchArgument('base_frame', default_value='quadrotor_1'),
        DeclareLaunchArgument('show_detection', default_value='true'),
        DeclareLaunchArgument('target_object', default_value='small_blue_box'),
        DeclareLaunchArgument('rviz_config', default_value=rviz_config),
        DeclareLaunchArgument('same_object_distance_threshold', default_value='1.0'),
        Node(
            package='depthtection',
            executable='depthtection_node',
            namespace=LaunchConfiguration('namespace'),
            parameters=[{'use_sim_time': LaunchConfiguration('use_sim_time')},
                        {'camera_topic': LaunchConfiguration('camera_topic')},
                        {'detection_topic': LaunchConfiguration('detection_topic')},
                        {'ground_truth_topic': LaunchConfiguration('ground_truth_topic')},
                        {'base_frame': LaunchConfiguration('base_frame')},
                        {'show_detection': LaunchConfiguration('show_detection')},
                        {'target_object': LaunchConfiguration('target_object')},
                        {'same_object_distance_threshold': LaunchConfiguration('same_object_distance_threshold')}],
            output='screen',
            emulate_tty=True
        ),
        Node(
            package='rviz2',
            executable='rviz2',
            arguments=['-d', LaunchConfiguration('rviz_config')],
            output={
                'stdout': 'log',
                'stderr': 'log',
            }

        )
    ])
