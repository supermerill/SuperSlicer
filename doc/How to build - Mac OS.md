
# Building Slic3r on Mac OS

To build Slic3r on Mac OS, you will need the following software:

- XCode
- CMake
- git
- gettext

XCode is available through Apple's App Store, the other three tools are available on
[brew](https://brew.sh/) (use `brew install cmake git gettext` to install them).

### How to get the source code

You have to gitclone  the repository

Open 'terminal' and enter each line, one at a time.
```
git clone https://github.com/supermerill/SuperSlicer.git
```

and then you have to clone the profiles submodules

```
cd SuperSlicer
cd resources/profiles
git submodule update
```

## How to build, the easy way

After cloning, execute `./BuildMacOS.sh -i`
The executable & dmg should be somewhere in the build directory. 

### How to build

You can follow the [script](https://github.com/supermerill/Slic3r/blob/master/.github/workflows/ccpp_mac.yml) the build server use to create the ubuntu release.

You have to execute each command at the right of the 'run: ' tags, in the directory that is at the right of the previous 'working-directory:' tag.

You can stop after the `make slic3r` as the rest of the commands are for building the .dmg

# Old doc, not up-to-date:


### Dependencies

Slic3r comes with a set of CMake scripts to build its dependencies, it lives in the `deps` directory.
Open a terminal window and navigate to Slic3r sources directory and then to `deps`.
Use the following commands to build the dependencies:

    mkdir build
    cd build
    cmake ..
    make

This will create a dependencies bundle inside the `build/destdir` directory.
You can also customize the bundle output path using the `-DDESTDIR=<some path>` option passed to `cmake`.

**Warning**: Once the dependency bundle is installed in a destdir, the destdir cannot be moved elsewhere.
(This is because wxWidgets hardcodes the installation path.)

FIXME The Cereal serialization library needs a tiny patch on some old OSX clang installations
https://github.com/USCiLab/cereal/issues/339#issuecomment-246166717


### Building Slic3r

If dependencies are built without errors, you can proceed to build Slic3r itself.
Go back to top level Slic3r sources directory and use these commands:

    mkdir build
    cd build
    cmake .. -DCMAKE_PREFIX_PATH="$PWD/../deps/build/destdir/usr/local"

The `CMAKE_PREFIX_PATH` is the path to the dependencies bundle but with `/usr/local` appended - if you set a custom path
using the `DESTDIR` option, you will need to change this accordingly. **Warning:** the `CMAKE_PREFIX_PATH` needs to be an absolute path.

The CMake command above prepares Slic3r for building from the command line.
To start the build, use

    make -jN

where `N` is the number of CPU cores, so, for example `make -j4` for a 4-core machine.

Alternatively, if you would like to use XCode GUI, modify the `cmake` command to include the `-GXcode` option:

    cmake .. -GXcode -DCMAKE_PREFIX_PATH="$PWD/../deps/build/destdir/usr/local"

and then open the `Slic3r.xcodeproj` file.
This should open up XCode where you can perform build using the GUI or perform other tasks.

### Note on Mac OS X SDKs

By default Slic3r builds against whichever SDK is the default on the current system.

This can be customized. The `CMAKE_OSX_SYSROOT` option sets the path to the SDK directory location
and the `CMAKE_OSX_DEPLOYMENT_TARGET` option sets the target OS X system version (eg. `10.14` or similar).
Note you can set just one value and the other will be guessed automatically.
In case you set both, the two settings need to agree with each other. (Building with a lower deployment target
is currently unsupported because some of the dependencies don't support this, most notably wxWidgets.)

Please note that the `CMAKE_OSX_DEPLOYMENT_TARGET` and `CMAKE_OSX_SYSROOT` options need to be set the same
on both the dependencies bundle as well as Slic3r itself.

Official Mac Slic3r builds are currently built against SDK 10.9 to ensure compatibility with older Macs.

_Warning:_ XCode may be set such that it rejects SDKs bellow some version (silently, more or less).
This is set in the property list file

    /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Info.plist

To remove the limitation, simply delete the key `MinimumSDKVersion` from that file.


# TL; DR

Works on a fresh installation of MacOS Catalina 10.15.6

- Install [brew](https://brew.sh/):
- Open Terminal
    
- Enter:

```brew install cmake git gettext
brew update
brew upgrade
git clone https://github.com/prusa3d/PrusaSlicer/
cd PrusaSlicer/deps
mkdir build
cd build
cmake ..
make
cd ../..
mkdir build
cd build
cmake .. -DCMAKE_PREFIX_PATH="$PWD/../deps/build/destdir/usr/local"
make
src/prusa-slicer
