# ZetaRay
[![MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)

A hobby real-time Direct3D 12 ray tracer. Mainly developed for learning and experimenting with the latest research in real-time rendering. 

To achieve real-time frame rates, this renderer utilizes recent developments such as hardware-accelerated ray tracing (DXR), advanced sampling (ReSTIR), denoising, and smart upscaling (AMD FSR2).

<div align="center">
  <img src="Assets/Images/8.png?raw=true" alt="Sponza" style="zoom:60%"/>
  <p style="text-align: center;"><i>Subway Station</i> based on the NYC R46 subway, rendered by ZetaRay. <i>(<a href="https://sketchfab.com/3d-models/free-subway-station-r46-subway-ae5aadde1c6f48a19b32b309417a669b">Scene</a> by <a href="https://sketchfab.com/Xlay3D">Alex Murias</a>.)</i></p>
</div>

## Highlight Features

 - ReSTIR DI for many-light sampling from emissive meshes and sky [[4](#references)]. Sampling efficiency is increased by
   - Power sampling using the alias method
   - A world-space voxel grid with stochastic light reservoirs per each voxel [[5](#references)]
 - Multi-bounce indirect lighting using ReSTIR GI [[1](#references)]
 - Ray differentials for texture MIP selection with camera rays, ray cones for secondary rays
 - Physically-based BSDF with roughness, metallic mask, normal, and emissive maps
 - Single scattering sky and atmosphere [[2, 3](#references)]
 - Reference path tracer
 - Render graph for automatic resource barrier placement and multi-threaded GPU command list recording and submission ([more details below](#render-graph))
 - AMD FSR2 upscaling
 - Temporal anti-aliasing (TAA)
 - Auto exposure using luminance histogram
 - glTF scene loading with the metalness-roughness workflow. Following extensions are also supported:
   - KHR_materials_emissive_strength
   - KHR_materials_ior
   - KHR_materials_transmission

### Render Graph

Modern APIs such as Vulkan and Direct3D 12 require resource barriers to be placed manually by the programmer. These barriers control synchronization, memory visibility, and additionally for textures, resource layout. As program complexity grows, manual placement can lead to issues such as:
   - Becoming highly error-prone and limiting extensibility and prototyping.
   - GPU command buffers can be recorded in multiple threads; correct usage and prevention of data-race issues may be hard to achieve.
    
Furthermore, using multiple command buffers requires a correct submission order—e.g., command list A writes to a resource that is read by command list B, so A has to be submitted before B. 

A render graph can help with all of the above; by analyzing resource dependencies, a directed acyclic graph (DAG) is formed from which submission order is derived and barriers are automatically placed. A visualization is shown below.

<div align="center">
  <img src="Assets/Images/6.png?raw=true" alt="Render graph" style="zoom:50%"/>
  <p style="text-align: center;"><i>A sample frame render graph.</i></p>
</div>

## Requirements

1. GPU with hardware-accelerated ray tracing support (Nvidia RTX 2000 series or later, AMD RX 6000 series or later). Tested on RTX 3070 desktop and RTX 3060 laptop.
2. Windows 10 1909 or later (required by [Agility SDK](https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/)).
3. [Visual Studio 2019 or later](https://visualstudio.microsoft.com/downloads/).
4. [CMake 3.21 or later](https://cmake.org/download/).

## Build

Standard CMake build. Make a build directory somewhere you like, then call CMake from inside that directory and point it to ([`./CMakeLists`](./CMakeLists.txt)) in the top-level project directory (`./build` is preferred as it's included in [`.gitignore`](./.gitignore)).

## Sample App

The main sample application ([`Samples/ZetaLab/`](./Samples/ZetaLab/)) works by loading a glTF scene and then proceeding to rendering that scene while exposing various settings through the UI window. 

Note that glTF scenes need to be preprocessed first by generating mipmaps and converting textures to DDS format. A command-line utility app ([`Tools/BCnCompressglTF`](./Tools/BCnCompressglTF/)) is provided for that purpose. It can be used as follows:

```bash
> cd bin
> .\BCnCompressglTF <path-to-gltf>
```
The outputs are:
1. The converted glTF scene file in the same directory with a `_zeta` suffix (e.g., `myscene.gltf` -> `myscene_zeta.gltf`) 
2. The compressed textures in the `<path-to-gltf-directory>/compressed` directory.

For convenience, a preprocessed Cornell Box scene is provided ([`Assets/CornellBox/cornell9.gltf`](./Assets/CornellBox/cornell9.gltf)). After building the project, you can run it as follows:
```bash
> cd bin
> .\ZetaLab ..\Assets\CornellBox\cornell9.gltf
```

Currently, unicode paths are not supported. Support is planned for a future release.

## Screenshots

<div align="center">
  <img src="Assets/Images/2.png?raw=true" alt="Subway Station" style="zoom:60%"/>
  <p style="text-align: center;"><i>Subway Station. (<a href="https://sketchfab.com/3d-models/free-subway-station-r46-subway-ae5aadde1c6f48a19b32b309417a669b">Scene</a> by <a href="https://sketchfab.com/Xlay3D">Alex Murias</a>.)</i></p>

  </br>

  <img src="Assets/Images/12.png?raw=true" alt="Subway Station" style="zoom:60%"/>
  <p style="text-align: center;"><i>Subway Station. (<a href="https://sketchfab.com/3d-models/free-subway-station-r46-subway-ae5aadde1c6f48a19b32b309417a669b">Scene</a> by <a href="https://sketchfab.com/Xlay3D">Alex Murias</a>.)</i></p>

  </br>

  <img src="Assets/Images/5.png?raw=true" alt="Bistro" style="zoom:50%"/>
  <p style="text-align: center;"><i>Amazon Lumberyard Bistro. (Scene from <a href="http://developer.nvidia.com/orca/amazon-lumberyard-bistro">Open Research Content Archive (ORCA)</a>.)</i></p>

  </br>

  <img src="Assets/Images/10.png?raw=true" alt="San Miguel" style="zoom:50%"/>
  <p style="text-align: center;"><i>San Miguel 2.0. (Scene from <a href="https://casual-effects.com/data">Morgan McGuire's Computer Graphics Archive</a>.)</i></p>

  </br>

  <img src="Assets/Images/7.png?raw=true" alt="San Miguel" style="zoom:50%"/>
  <p style="text-align: center;"><i>San Miguel 2.0. (Scene from <a href="https://casual-effects.com/data">Morgan McGuire's Computer Graphics Archive</a>.)</i></p>

  </br>

  <img src="Assets/Images/4.jpg?raw=true" alt="San Miguel" style="zoom:50%"/>
  <p style="text-align: center;"><i>San Miguel 2.0. (Scene from <a href="https://casual-effects.com/data">Morgan McGuire's Computer Graphics Archive</a>.)</i></p>

  </br>

  <img src="Assets/Images/9.png?raw=true" alt="Junk Shop" style="zoom:50%"/>
  <p style="text-align: center;"><i>Modified Blender 2.81 Splash Screen. (<a href="https://cloud.blender.org/p/gallery/5dd6d7044441651fa3decb56">Original scene</a> by <a href="http://www.aendom.com/">Alex Treviño</a>, <a href="https://sketchfab.com/3d-models/ported-hair-e8eb76ad3c8f46f8aee7ceb076f05972">hair model</a> by <a href="https://sketchfab.com/bbb59149">bbb59149</a>.)</i></p>

  </br>

  <img src="Assets/Images/3.png?raw=true" alt="Sponza" style="zoom:50%"/>
  <p style="text-align: center;"><i>Sponza Atrium. (Scene from <a href="https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html">Intel Samples Library</a>.)</i></p>

  </br>

  <img src="Assets/Images/11.png?raw=true" style="zoom:50%"/>
  <p style="text-align: center;"><i>Custom scene made in Blender, rendered by ZetaRay. (<a href="https://www.deviantart.com/mangotangofox/art/CubeD-188764994">Reference art</a>)</i></p>

</div>

## References
[**1**] Y. Ouyang, S. Liu, M. Kettunen, M. Pharr and J. Pantaleoni, "ReSTIR GI: Path Resampling for Real-Time Path Tracing," *Computer Graphics Forum*, 2021.

[**2**] S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique," *Computer Graphics Forum*, 2020.

[**3**] MinimalAtmosphere, [https://github.com/Fewes/MinimalAtmosphere](https://github.com/Fewes/MinimalAtmosphere)

[**4**] B. Bitterli, C. Wyman, M. Pharr, P. Shirley, A. Lefohn and W. Jarosz, "Spatiotemporal reservoir resampling for real-time ray tracing with 
dynamic direct lighting," *ACM Transactions on Graphics*, 2020.

[**5**] J. Boksansky, P. Jukarainen, and C. Wyman, "Rendering Many Lights With Grid-Based Reservoirs," in Ray Tracing Gems 2, 2021.

## External Libraries

- [AMD FSR 2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2)
- [AMD FidelityFX Denoiser](https://github.com/GPUOpen-Effects/FidelityFX-Denoiser)
- [cgltf](https://github.com/jkuhlmann/cgltf)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [doctest](https://github.com/doctest/doctest)
- [FastDelegate](https://www.codeproject.com/Articles/7150/Member-Function-Pointers-and-the-Fastest-Possible)
- [ImPlot](https://github.com/epezent/implot)
- [imnodes](https://github.com/Nelarius/imnodes)
- [json](https://github.com/nlohmann/json)
- [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue)
- [stb](https://github.com/nothings/stb)
- [xxHash](https://github.com/Cyan4973/xxHash)

## License

MIT license—see [`LICENSE`](./LICENSE) for more details.