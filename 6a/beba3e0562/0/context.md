# Session Context

## User Prompts

### Prompt 1

read README.md

### Prompt 2

make the default port 9090 in all the files and documentations

### Prompt 3

back to the readme. in "usage" add in order the case of pixi, AppImage, ros2 run, dastdds

### Prompt 4

in pixi, how to I change my parameters (port numebr for instance)?

### Prompt 5

to keep it more organize, move together the "how to build" and "how to run in the same sections

### Prompt 6

we just released this: https://prefix.dev/channels/plotjuggler. let create two different sections instead (I crated them): "Download and run" that describe how to download with pixi and run it, or download the AppImage from the release page and run it, and the section "Build instructions" that focus on building on pixi, colcon or fastdds with conan

### Prompt 7

add the instructions after # Run with custom port

### Prompt 8

davide@davide-Katana:~/ws_plotjuggler/src/pj_ros_bridge$ pixi global install pj-bridge-ros2-humble -c https://prefix.dev/plotjuggler -c robostack-staging -c conda-forge
└── pj-bridge-ros2-humble: 0.4.0 (installed)
    └─ exposes: Nothing
davide@davide-Katana:~/ws_plotjuggler/src/pj_ros_bridge$ pj_bridge_ros2 --ros-args -p port:=8080

### Prompt 9

pj_bridge_ros2 --ros-args -p port:=8080
/home/davide/.pixi/envs/pj-bridge-ros2-humble/lib/pj_bridge/pj_bridge_ros2: error while loading shared libraries: libsensor_msgs__rosidl_typesupport_cpp.so: cannot open shared object file: No such file or directory

### Prompt 10

can I fix my package to solve this problem?

### Prompt 11

search the internet. what is the recommended way in ros2?

### Prompt 12

fix the root cause, we will create a new package

### Prompt 13

read the file. how do I add the argument "port" to the appimage?

### Prompt 14

double check the current version of the document. any suggestion?

### Prompt 15

fix

### Prompt 16

review again

### Prompt 17

keep 9090

### Prompt 18

git-cola

### Prompt 19

this failed now https://github.REDACTED

### Prompt 20

commit and retag 0.4.1

