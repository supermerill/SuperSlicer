
# Building Slic3r on UNIX/Linux

Please understand that Slic3r (& PrusaSlicer) team cannot support compilation on all possible Linux distros. Namely, we cannot help troubleshoot OpenGL driver issues or dependency issues if compiled against distro provided libraries. **We can only support Slic3r statically linked against the dependencies compiled with the `deps` scripts**, the same way we compile Slic3r for our binary builds.

If you have some reason to link dynamically to your system libraries, you are free to do so, but we can not and will not troubleshoot any issues you possibly run into.

Instead of compiling Slic3r from source code, one may also consider to install Slic3r pre-compiled by contributors, like from the distribution application manager.

## How to build, the easy way

Just use the `BuildLinux.sh` script (use the `-h` option to get the options available, and how to use them)

## Step by step guide, the hard way

This guide describes building Slic3r statically against dependencies pulled by our `deps` script. Running all the listed commands in order should result in successful build.

#### 0. Prerequisities

You need at least 8GB of RAM on your system. Linking on a 4GB RAM system will likely fail and you may need to limit the number of compiler processes with the '-j xxx' make or ninja parameter, where 'xxx' is the number of compiler processes launched if running on low RAM multi core system, for example on Raspberry PI.

GNU build tools, CMake, git and other libraries have to be installed on the build machine.
Unless that's already the case, install them as usual from your distribution packages.
E.g. on Ubuntu 20.10, run
```shell
sudo apt-get install  -y \
git \
build-essential \
autoconf \
cmake \
libglu1-mesa-dev \
libgtk-3-dev \
libdbus-1-dev \

```
The names of the packages may be different on different distros.

#### 1. Cloning the repository


Cloning the repository is simple thanks to git and Github. Simply `cd` into wherever you want to clone Slic3r code base and run (with Slic3r / supermerill or prusa3D as REPO_NAME and Slic3r / SuperSlicer or PrusaSlicer as SLIC3R_NAME)
```
git clone https://www.github.com/REPO_NAME/SLIC3R_NAME
cd SLIC3R_NAME
```

This will download the source code into a new directory and `cd` into it. You can now optionally select a tag/branch/commit to build using `git checkout`. Otherwise, `master` branch will be built.


#### 2. Building dependencies

Slic3r uses CMake and the build is quite simple, the only tricky part is resolution of dependencies. The supported and recommended way is to build the dependencies first and link to them statically. Slic3r source base contains a CMake script that automatically downloads and builds the required dependencies. All that is needed is to run the following (from the top of the cloned repository):

    cd deps
    mkdir build
    cd build
    cmake .. -DDEP_WX_GTK3=ON
    make
    cd ../..


**Warning**: Once the dependency bundle is installed in a destdir, the destdir cannot be moved elsewhere. This is because wxWidgets hardcode the installation path.


#### 3. Building Slic3r

Now when the dependencies are compiled, all that is needed is to tell CMake that we are interested in static build and point it to the dependencies. From the top of the repository, run

    mkdir build
    cd build
    cmake .. -DSLIC3R_STATIC=1 -DSLIC3R_GTK=3 -DSLIC3R_PCH=OFF -DCMAKE_PREFIX_PATH=$(pwd)/../deps/build/destdir/usr/local
    make -j4

And that's it. It is now possible to run the freshly built Slic3r binary:

    cd src
    ./superslicer




## Useful CMake flags when building dependencies

- `-DDESTDIR=<target destdir>` allows to specify a directory where the dependencies will be installed. When not provided, the script creates and uses `destdir` directory where cmake is run.

- `-DDEP_DOWNLOAD_DIR=<download cache dir>` specifies a directory to cache the downloaded source packages for each library. Can be useful for repeated builds, to avoid unnecessary network traffic.

- `-DDEP_WX_GTK3=ON` builds wxWidgets (one of the dependencies) against GTK3 (defaults to OFF)


## Useful CMake flags when building Slic3r
- `-DSLIC3R_ASAN=ON` enables gcc/clang address sanitizer (defaults to `OFF`, requires gcc>4.8 or clang>3.1)
- `-DSLIC3R_GTK=3` to use GTK3 (defaults to `2`). Note that wxWidgets must be built against the same GTK version.
- `-DSLIC3R_STATIC=ON` for static build (defaults to `OFF`)
- `-DSLIC3R_WX_STABLE=ON` to look for wxWidgets 3.0 (defaults to `OFF`)
- `-DCMAKE_BUILD_TYPE=Debug` to build in debug mode (defaults to `Release`)
- `-DSLIC3R_GUI=no` to build the console variant of Slic3r

See the CMake files to get the complete list.



## Building dynamically

As already mentioned above, dynamic linking of dependencies is possible, but Slic3r (& PrusaSlicer) team is unable to troubleshoot (Linux world is way too complex). Feel free to do so, but you are on your own. Several remarks though:

The list of dependencies can be easily obtained by inspecting the CMake scripts in the `deps/` directory. Some of the dependencies don't have to be as recent as the versions listed - generally versions available on conservative Linux distros such as Debian stable, Ubuntu LTS releases or Fedora are likely sufficient. If you decide to build this way, it is your responsibility to make sure that CMake finds all required dependencies. It is possible to look at your distribution Slic3r package to see how the package maintainers solved the dependency issues.

#### wxWidgets
By default, Slic3r looks for wxWidgets 3.1. Our build script in fact downloads specific patched version of wxWidgets. If you want to link against wxWidgets 3.0 (which are still provided by most distributions because wxWidgets 3.1 have not yet been declared stable), you must set `-DSLIC3R_WX_STABLE=ON` when running CMake. Note that while Slic3r can be linked against wWidgets 3.0, the combination is not well tested and there might be bugs in the resulting application. 

When building on ubuntu 20.04 focal fossa, the package libwxgtk3.0-gtk3-dev needs to be installed instead of libwxgtk3.0-dev and you should use:
```
-DSLIC3R_WX_STABLE=1 -DSLIC3R_GTK=3
``` 

## Miscellaneous

### Installation

At runtime, Slic3r needs a way to access its resource files. By default, it looks for a `resources` directory relative to its binary.

If you instead want Slic3r installed in a structure according to the File System Hierarchy Standard, use the `SLIC3R_FHS` flag

    cmake .. -DSLIC3R_FHS=1

This will make Slic3r look for a fixed-location `share/slic3r-prusa3d` directory instead (note that the location becomes hardcoded).

You can then use the `make install` target to install Slic3r.

### Desktop Integration (Slic3r 2.4 and newer)

If Slic3r is to be distributed as an AppImage or a binary blob (.tar.gz and similar), then a desktop integration support is compiled in by default: Slic3r will offer to integrate with desktop by manually copying the desktop file and application icon into user's desktop configuration. The built-in desktop integration is also handy on Crosstini (Linux on Chrome OS).

If Slic3r is compiled with `SLIC3R_FHS` enabled, then a desktop integration support will not be integrated. One may want to disable desktop integration by running
    
    cmake .. -DSLIC3R_DESKTOP_INTEGRATION=0
    
when building Slic3r for flatpack or snap, where the desktop integration is performed by the installer.

## Raspberry pi

Some hints to be able to build on raspberry pi:

I think that the most important thing is preparing Raspberry Pi. I'm using overclocked to 2GHz RPi 4B 2GB with active cooling(it starts when the temperature is too high). 2GB of RAM is not enough and I turned swap on(+2GB), but i use SSD instead of memory card so it's not that bad. Swap is only needed during compilation, built-in memory is enough to run the GUI.

first, you have to install all the dependencies  (you can also try to build them from the deps directory):
`sudo apt-get install -y git cmake libboost-dev libboost-regex-dev libboost-filesystem-dev libboost-thread-dev libboost-log-dev libboost-locale-dev libcurl4-openssl-dev libwxgtk3.0-dev build-essential pkg-config libtbb-dev zlib1g-dev libcereal-dev libeigen3-dev libnlopt-cxx-dev libudev-dev libopenvdb-dev libboost-iostreams-dev libnlopt-dev libdbus-1-dev`

Running cmake will give information about the remaining missing dependencies. (to be run in a newly created build directory)
`cmake .. -DCMAKE_BUILD_TYPE=Release -DSLIC3R_BUILD_TESTS=OFF -DSLIC3R_GTK = 2`
Note that you can also use different parameters, like
`cmake .. -DSLIC3R_STATIC=0 -DCMAKE_BUILD_TYPE=Debug -DSLIC3R_FHS=1 -DSLIC3R_GTK=2 -DSLIC3R_PCH=0 -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib`

Then, when cmake end without errors, you can compile with the make command.

I think it is a good idea to run this command in screen, because it takes a long time to complete and the connection may time out. screen will remain running in the background.

Example:
```
screen -S superclicer
#new terminal will open
 make -j4
 ```

and after disconnect:
`screen -x`

Finally, it remains to run `sudo make install`

May also be useful to run
`sudo systemctl set-environment LC_ALL=en_US.UTF-8`

I'm sure there is some steps missing, please open an issue to let us know or open a pull request by editing this document.

To get this running / updated on an RPi running Raspberry Pi OS - Bullseye (64bit) you can run the ./BuildLinux.sh script, however it will not make the AppImage, but it will generate the application in the package folder that you can them move to somewhere convenient.

e.g. navigate to clone repository
    sudo ./BuildLinux.sh -s
    sudo ./BuildLinux.sh -dsi

It will fail, but (for me) ONLY when it tries making the AppImage.  If you then copy the contents of the 'package' folder you should have a nice fully working instance.  

Move the package folder to a location of your choice e.g.
    cp -R build/package /home/pi/SuperSlicer/
Ensure the executable is runnable as default user pi.
    cd /home/pi
    sudo chown -R pi:pi SuperSlicer
    Finally make the sure the program is executable
sudo chomod +x ~/SuperSlicer/superslicer

Then run by calling 
./home/pi/SuperSlicer/superslicer








