# VASTClient C++ Library

This is a C++ library for interacting with the VAST Data REST APIs.

## Features
- Support for various HTTP methods (GET, POST, PUT, DELETE, etc.)
- Authentication via token or username/password
- SSL certificate handling

## Build Instructions
1. Install CMake (version 3.10 or higher).
2. Create a build directory: `mkdir build && cd build`
3. Run CMake: `cmake ..`
4. Build the project: `make`

## Installation

To install the library, follow these steps:

1. Build the project:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```

2. Install the library:
   ```bash
   sudo make install
   ```

By default, the library will be installed to `/usr/local/lib` and the header files to `/usr/local/include`. You can customize the installation path by setting the `CMAKE_INSTALL_PREFIX` variable during the `cmake` step:

```bash
cmake -DCMAKE_INSTALL_PREFIX=/your/custom/path ..
```

## Usage
Include the `vastclient` library in your project and use the `VASTClient` class to interact with the VAST Data REST APIs.

## Using the Library

To use the installed library in your project:

1. Include the header file in your code:
   ```cpp
   #include <VASTClient.h>
   ```

2. Compile and link your code with the library. For example:
   ```bash
   g++ -std=c++17 -I/usr/local/include -L/usr/local/lib -lvastclient -o my_program main.cpp
   ```

   Here:
   - `-I/usr/local/include`: Specifies the path to the header files.
   - `-L/usr/local/lib`: Specifies the path to the library files.
   - `-lvastclient`: Links the `vastclient` library.

3. Run your program. If the library is not in the default library path, set the `DYLD_LIBRARY_PATH` environment variable on macOS:
   ```bash
   export DYLD_LIBRARY_PATH=/usr/local/lib:$DYLD_LIBRARY_PATH
   ./my_program
   ```