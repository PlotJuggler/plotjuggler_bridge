# Session Context

## User Prompts

### Prompt 1

I want to use bloom to create debian packages for ROS Humble / Jazzy / Rolling / Kilted. search the web to determine how to do this

### Prompt 2

try building the humble deb for pj_ros_bridge

### Prompt 3

sudo apt install -y debhelper dh-python

### Prompt 4

it's installed now, retry the build

### Prompt 5

[Request interrupted by user for tool use]

### Prompt 6

can I try using conan to fetch that dependency, instead? will that work?

### Prompt 7

2

### Prompt 8

websocketpp is already available in ubuntu and rosdistro, wouldi tbe worthy transitioning to that, or not?

### Prompt 9

where is the debina file?

### Prompt 10

try "catkin_prepare_release --bump=minor"

### Prompt 11

try that

### Prompt 12

now, I want you to create a github CI workflow that can be trigged manually or automatically when a new tag is created. This should create debian packages fro Humble / Jazzy / Kilted and Rolling, and upload them as artifact in a Release. is it clear? any question?

### Prompt 13

use manual trigger to test yourself that everything works, using "gh"

### Prompt 14

yes, commit and push

### Prompt 15

[Request interrupted by user]

### Prompt 16

continue monitoring and fix any error, if the occurs

### Prompt 17

review my package.xml and create the CHANGELOG

