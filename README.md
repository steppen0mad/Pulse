# 3D Sandbox

A minimal 3D first-person sandbox environment built with C, OpenGL, and GLFW. Navigate through a simple 3D world with free camera movement.

## Features

- **First-person camera control** with mouse look
- **Free movement** in all directions (WASD + Space/Shift)
- **OpenGL rendering** with immediate mode graphics
- **Grid floor** for spatial reference
- **Colored cubes** as environment objects

## Controls

| Input | Action |
|-------|--------|
| `W` `A` `S` `D` | Move forward/left/backward/right |
| `Space` | Move up |
| `Left Shift` | Move down |
| `Mouse` | Look around (first-person) |
| `ESC` | Exit |

## Requirements

- **GLFW 3.3+** - Window and input handling
- **OpenGL 4.5+** - Graphics rendering (Mesa compatible)
- **GLU** - OpenGL utility functions
- **GCC** - C compiler with C11 support

## Installation

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install -y libglfw3 libglfw3-dev libgl1-mesa-dev libglu1-mesa-dev mesa-utils
```

### Build from source

```bash
git clone <repository-url>
cd sandbox
gcc -Wall -Wextra -std=gnu11 -Iinclude `pkg-config --cflags glfw3` src/main.c -o build/sandbox `pkg-config --libs glfw3` -lGL -lGLU -lm
```

## Usage

```bash
./build/sandbox
```

## Project Structure

```
sandbox/
├── src/          # Source files
│   └── main.c    # Main application entry point
├── include/      # Header files (for future expansion)
├── build/        # Compiled binaries
├── Makefile      # Build configuration
└── README.md     # Documentation
```

## Technical Details

- **Language**: C (GNU11)
- **Graphics API**: OpenGL 4.5 (Compatibility Profile)
- **Window/Input**: GLFW 3.3
- **Rendering**: Immediate mode (legacy OpenGL for simplicity)

## Development

The project uses a straightforward architecture:
- Camera system with yaw/pitch orientation
- Delta-time based movement for frame-independent speed
- Mouse input captured for seamless look control

## Known Issues

- Runs on software renderer (llvmpipe) in WSL2 without GPU passthrough
- Uses legacy immediate mode OpenGL (not modern shader-based pipeline)

## Future Enhancements

- [ ] Modern OpenGL shader pipeline
- [ ] Texture support
- [ ] Collision detection
- [ ] Block placement/removal (voxel editing)
- [ ] Lighting system
- [ ] Multiple terrain types

## License

MIT License - See LICENSE file for details

## Contributing

Contributions welcome! Open an issue or submit a pull request.
