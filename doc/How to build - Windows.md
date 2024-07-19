# Building PrusaSlicer on Windows


## 0. Prerequisities

The following tools need to be installed on your computer:
- Microsoft Visual Studio version 16 2019 or 17 2022
- CMake
- git

Install git for Windows from [gitforwindows.org](https://gitforwindows.org/)
If you haven't, I advise to use a gui like tortoisegit.
Download and run the exe accepting all defaults


Clone the respository.  To place it in C:\local\Slic3r, run:


## 1. Download sources

Clone the respository. Use a directory relatively close to the drive root, so the path is not too long. Avoid spaces and non-ASCII characters. To place it in `C:\local\Slic3r`, run:
```
c:> mkdir src
c:> cd src
c:\src> git clone https://github.com/Slic3r/Slic3r.git
```


## 2.A Manual Build Instructions

c:\src>cd c:\local\Slic3r
c:\local\Slic3r>build_win.bat -d=..\PrusaSlicer-deps -r=console
_As an alternative, you can go to 2.B to compile PrusaSlicer using one automatic script._

1. Configure and build [deps](#compile-the-dependencies) as RelWithDebInfo with `c:\local\Slic3r-deps` as the destination directory
### Compile the dependencies.
Dependencies are updated seldomly, thus they are compiled out of the Slic3r source tree.
Open the MSVC x64 Native Tools Command Prompt and run the following:
```
cd c:\local\Slic3r\deps
mkdir build
cd build
cmake ..
cmake --build .
```
Expect this to take some time. Note that both _Debug_ and _Release_ variants are built.
If you need to compile with a specific version of visual studio, add this option to the first CMake call `-G "Visual Studio 16 2022"`
If you want to compile in another place than C:\local\Slic3r\deps\usr\local, add this òption to the first CMake call: ` -DDESTDIR="c:\local\Slic3-deps"`
 You can force only the _Release_ build by passing `-DDEP_DEBUG=OFF` to the first CMake call.

note: if you have visual studio 2022 installed alongside 2017/2019, you may have to comment/remove the lines 20->31 in `dep_Boost-prefix/src/dep_Boosttools/build/src/engine/vswhere_usability_wrapper.cmd` (after first failing to compile) to force it to ignore vs2022.

### Generate Visual Studio project file for Slic3r, referencing the precompiled dependencies.
Open the MSVC x64 Native Tools Command Prompt and run the following:
```
cd c:\local\Slic3r\
cd c:\src\REPO_NAME\
mkdir build
cd build
cmake .. -DCMAKE_PREFIX_PATH="c:\local\Slic3r\deps\build\destdir\usr\local"
```

Note that `CMAKE_PREFIX_PATH` must be absolute path. A relative path will not work.
If you set yourself the DESTDIR for the deps, write your location instead of `c:\local\Slic3r\deps\build\destdir\usr\local`
If you need to compile with a specific version of visual studio, add this option to the first CMake call `-G "Visual Studio 16 2022"`. Use the same version as the deps.

If it complains about not finding PSAPI, you cans et yourself the value in your cmakcache. Search your computer for 'psapi.lib'. For exemple, mine is at "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.22621.0/um/x64/psapi.lib".

### Compile Slic3r. 

Double-click c:\local\Slic3r\build\PrusaSlicer.sln to open in Visual Studio 2019.
Double-click c:\local\Slic3r\build\Slic3r.sln to open in Visual Studio and select `Slic3r_app_gui` as your startup project (right-click->Set as Startup Project).

Run Build->Rebuild Solution once to populate all required dependency modules. This is NOT done automatically when you Build/Run. If you run both Debug and Release variants, you will need to do this once for each.

Debug->Start Debugging or press F5

Slic3r should start. You're up and running!




## 2.B Run the automatic build script

_This is an alternative to the manual build described above. It relies on `build_win.bat` script which is part of the repository and which will find the MSVC installation, set up the build environment and build the dependencies and Slic3r itself. The script was provided by @jschuh and PrusaSlicer team does not maintain it._

Just run the following command to get everything going with the default configs:

```
c:\src>cd c:\local\Slic3r
c:\local\Slic3r>build_win.bat -d=..\Slic3r-deps -r=console
```

The build script will run for a while and automatically perform the following steps:
1. Configure and build [deps](#compile-the-dependencies) as RelWithDebInfo with `c:\local\Slic3r-deps` as the destination directory
2. Configure and build all [application targets](#compile-slic3r) as RelWithDebInfo
3. Launch the resulting `slic3r-console.exe` binary

You can change the above command line options to do things like:
* Change the destination for the dependencies by pointing `-d` to a different directory such as: `build_win.bat -d=s:\Slic3rDeps`
* Open the solution in Visual Studio after the build completes by changing the `-r` switch to `-r=ide`
* Generate a release build without debug info by adding `-c=Release` or a full debug build with `-c=Debug`
* Perform an incremental application build (the default) with: `build_win.bat -s=app-dirty`
* Clean and rebuild the application: `build_win.bat -s=app`
* Clean and rebuild the dependencies: `build_win.bat -s=deps`
* Clean and rebuild everything (app and deps): `build_win.bat -s=all`
* _The full list of build script options can be listed by running:_ `build_win.bat -?`

#### Troubleshooting

You're best off initiating builds from within Visual Studio for day-to-day development. However, the `build_win.bat` script can be very helpful if you run into build failures after updating your source tree. Here are some tips to keep in mind:
* The last several lines of output from `build_win.bat` will usually have the most helpful error messages.
* If CMake complains about missing binaries or paths (e.g. after updating Visual Studio), building with `build_win.bat` will force CMake to regenerate its cache on an error.
* After a deps change, you may just need to rebuild everything with the `-s=all` switch.
* Reading through the instructions in the next section may help diagnose more complex issues.

